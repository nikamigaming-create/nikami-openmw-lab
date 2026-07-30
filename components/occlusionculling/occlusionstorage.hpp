#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H

#include "occludermesh.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace OcclusionCulling
{
    /// SQLite-backed cache of precomputed occluder meshes.
    /// Keyed by (model_path, mesh_res, shrink_key).
    /// Thread-safe for concurrent get() calls; put() is also guarded by a mutex.
    class OcclusionStorage
    {
    public:
        explicit OcclusionStorage(const std::string& databasePath);
        ~OcclusionStorage();

        OcclusionStorage(const OcclusionStorage&) = delete;
        OcclusionStorage& operator=(const OcclusionStorage&) = delete;

        bool isOpen() const;
        bool get(std::string_view modelPath, int meshRes, int shrinkKey, OccluderMesh& out) const;
        void put(std::string_view modelPath, int meshRes, int shrinkKey, const OccluderMesh& mesh);

        struct Stats
        {
            int memHits = 0; // served from in-memory map
            int dbHits  = 0; // loaded from SQLite
            int misses  = 0; // not in cache — buildSimplifiedMesh was called
            int writes  = 0; // written to SQLite
        };
        /// Returns accumulated stats since last call and resets counters.
        Stats getAndResetStats();

        void recordMiss() { ++mMisses; }

        static int makeShrinkKey(float shrinkFactor);

    private:
        bool deserialize(const void* data, int bytes, OccluderMesh& out) const;
        std::vector<std::byte> serialize(const OccluderMesh& mesh) const;
        std::string makeKey(std::string_view modelPath, int meshRes, int shrinkKey) const;

        sqlite3* mDb = nullptr;
        mutable std::mutex mMutex;
        mutable std::unordered_map<std::string, std::shared_ptr<OccluderMesh>> mCache;

        mutable std::atomic<int> mMemHits{ 0 };
        mutable std::atomic<int> mDbHits{ 0 };
        mutable std::atomic<int> mMisses{ 0 };
        mutable std::atomic<int> mWrites{ 0 };
    };
}

#endif
