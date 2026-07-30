#include "occlusionstorage.hpp"

#include <components/debug/debuglog.hpp>

#include <sqlite3.h>

#include <cstring>
#include <vector>

namespace OcclusionCulling
{
    namespace
    {
        template <class T>
        bool readPod(const std::byte*& cursor, std::size_t& remaining, T& out)
        {
            if (remaining < sizeof(T))
                return false;
            std::memcpy(&out, cursor, sizeof(T));
            cursor += sizeof(T);
            remaining -= sizeof(T);
            return true;
        }
    }

    OcclusionStorage::OcclusionStorage(const std::string& databasePath)
    {
        if (databasePath.empty())
            return;

        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
        if (sqlite3_open_v2(databasePath.c_str(), &mDb, flags, nullptr) != SQLITE_OK)
        {
            Log(Debug::Warning) << "Failed to open occlusion cache database \"" << databasePath << "\": "
                                << sqlite3_errmsg(mDb);
            if (mDb)
            {
                sqlite3_close(mDb);
                mDb = nullptr;
            }
            return;
        }

        constexpr const char* schema = R"(
            CREATE TABLE IF NOT EXISTS occlusion_meshes (
                model_path  TEXT    NOT NULL,
                mesh_res    INTEGER NOT NULL,
                shrink_key  INTEGER NOT NULL,
                mesh_blob   BLOB    NOT NULL,
                PRIMARY KEY (model_path, mesh_res, shrink_key)
            );
        )";
        char* errMsg = nullptr;
        if (sqlite3_exec(mDb, schema, nullptr, nullptr, &errMsg) != SQLITE_OK)
        {
            Log(Debug::Warning) << "Failed to create occlusion cache schema: " << errMsg;
            sqlite3_free(errMsg);
        }

        sqlite3_exec(mDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(mDb, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

        int rowCount = 0;
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(mDb, "SELECT COUNT(*) FROM occlusion_meshes", -1, &countStmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(countStmt) == SQLITE_ROW)
                rowCount = sqlite3_column_int(countStmt, 0);
            sqlite3_finalize(countStmt);
        }
        Log(Debug::Info) << "OcclusionStorage: opened cache at \"" << databasePath
                         << "\" — " << rowCount << " precomputed meshes";
    }

    OcclusionStorage::~OcclusionStorage()
    {
        if (mDb)
            sqlite3_close(mDb);
    }

    bool OcclusionStorage::isOpen() const
    {
        return mDb != nullptr;
    }

    int OcclusionStorage::makeShrinkKey(float shrinkFactor)
    {
        return static_cast<int>(shrinkFactor * 1000000.0f + (shrinkFactor >= 0.f ? 0.5f : -0.5f));
    }

    std::string OcclusionStorage::makeKey(std::string_view modelPath, int meshRes, int shrinkKey) const
    {
        return std::string(modelPath) + "|" + std::to_string(meshRes) + "|" + std::to_string(shrinkKey);
    }

    bool OcclusionStorage::deserialize(const void* data, int bytes, OccluderMesh& out) const
    {
        const auto* cursor = reinterpret_cast<const std::byte*>(data);
        std::size_t remaining = static_cast<std::size_t>(bytes);

        float mins[3];
        float maxs[3];
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;

        if (!readPod(cursor, remaining, mins)
            || !readPod(cursor, remaining, maxs)
            || !readPod(cursor, remaining, vertexCount)
            || !readPod(cursor, remaining, indexCount))
            return false;

        out = {};
        out.aabb.set(mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]);
        out.vertices.resize(vertexCount);
        out.indices.resize(indexCount);

        const std::size_t vertexBytes = sizeof(osg::Vec3f) * out.vertices.size();
        const std::size_t indexBytes = sizeof(unsigned int) * out.indices.size();
        if (remaining < vertexBytes + indexBytes)
            return false;

        if (vertexBytes)
        {
            std::memcpy(out.vertices.data(), cursor, vertexBytes);
            cursor += vertexBytes;
            remaining -= vertexBytes;
        }
        if (indexBytes)
            std::memcpy(out.indices.data(), cursor, indexBytes);

        return true;
    }

    bool OcclusionStorage::get(std::string_view modelPath, int meshRes, int shrinkKey, OccluderMesh& out) const
    {
        if (!mDb)
            return false;

        const std::string key = makeKey(modelPath, meshRes, shrinkKey);
        {
            const std::lock_guard lock(mMutex);
            const auto it = mCache.find(key);
            if (it != mCache.end())
            {
                out = *it->second;
                ++mMemHits;
                return true;
            }
        }

        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql = "SELECT mesh_blob FROM occlusion_meshes WHERE model_path = ?1 AND mesh_res = ?2 AND shrink_key = ?3";
        if (sqlite3_prepare_v2(mDb, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return false;

        sqlite3_bind_text(stmt, 1, modelPath.data(), static_cast<int>(modelPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, meshRes);
        sqlite3_bind_int(stmt, 3, shrinkKey);

        bool found = false;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(stmt, 0);
            const int blobSize = sqlite3_column_bytes(stmt, 0);
            found = blob != nullptr && blobSize > 0 && deserialize(blob, blobSize, out);
        }
        sqlite3_finalize(stmt);

        if (found)
        {
            ++mDbHits;
            const auto cached = std::make_shared<OccluderMesh>(out);
            const std::lock_guard lock(mMutex);
            mCache.emplace(key, cached);
        }

        return found;
    }

    OcclusionStorage::Stats OcclusionStorage::getAndResetStats()
    {
        Stats s;
        s.memHits = mMemHits.exchange(0);
        s.dbHits  = mDbHits.exchange(0);
        s.misses  = mMisses.exchange(0);
        s.writes  = mWrites.exchange(0);
        return s;
    }

    std::vector<std::byte> OcclusionStorage::serialize(const OccluderMesh& mesh) const
    {
        const float mins[3] = { mesh.aabb.xMin(), mesh.aabb.yMin(), mesh.aabb.zMin() };
        const float maxs[3] = { mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax() };
        const std::uint32_t vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        const std::uint32_t indexCount  = static_cast<std::uint32_t>(mesh.indices.size());

        const std::size_t totalBytes = sizeof(mins) + sizeof(maxs)
            + sizeof(vertexCount) + sizeof(indexCount)
            + sizeof(osg::Vec3f) * mesh.vertices.size()
            + sizeof(unsigned int) * mesh.indices.size();

        std::vector<std::byte> buf(totalBytes);
        std::byte* p = buf.data();

        auto write = [&](const void* src, std::size_t n) {
            std::memcpy(p, src, n);
            p += n;
        };

        write(mins, sizeof(mins));
        write(maxs, sizeof(maxs));
        write(&vertexCount, sizeof(vertexCount));
        write(&indexCount, sizeof(indexCount));
        if (!mesh.vertices.empty())
            write(mesh.vertices.data(), sizeof(osg::Vec3f) * mesh.vertices.size());
        if (!mesh.indices.empty())
            write(mesh.indices.data(), sizeof(unsigned int) * mesh.indices.size());

        return buf;
    }

    void OcclusionStorage::put(std::string_view modelPath, int meshRes, int shrinkKey, const OccluderMesh& mesh)
    {
        if (!mDb)
            return;

        const std::vector<std::byte> blob = serialize(mesh);
        if (blob.empty())
            return;

        constexpr const char* sql = R"(
            INSERT OR REPLACE INTO occlusion_meshes (model_path, mesh_res, shrink_key, mesh_blob)
            VALUES (?1, ?2, ?3, ?4)
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(mDb, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return;

        sqlite3_bind_text(stmt, 1, modelPath.data(), static_cast<int>(modelPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, meshRes);
        sqlite3_bind_int(stmt, 3, shrinkKey);
        sqlite3_bind_blob(stmt, 4, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        ++mWrites;

        const std::string key = makeKey(modelPath, meshRes, shrinkKey);
        const auto cached = std::make_shared<OccluderMesh>(mesh);
        const std::lock_guard lock(mMutex);
        mCache.emplace(key, cached);
    }
}
