#include "occlusiondb.hpp"

#include <components/sqlite3/db.hpp>
#include <components/sqlite3/request.hpp>

#include <sqlite3.h>

#include <cstddef>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace OcclusionCulling
{
    namespace
    {
        // -----------------------------------------------------------------
        // Schema
        // -----------------------------------------------------------------
        constexpr const char schema[] = R"(
            BEGIN TRANSACTION;

            CREATE TABLE IF NOT EXISTS cells (
                cell_id       INTEGER PRIMARY KEY,
                revision      INTEGER NOT NULL DEFAULT 1,
                worldspace    TEXT    NOT NULL,
                cell_x        INTEGER NOT NULL,
                cell_y        INTEGER NOT NULL,
                version       INTEGER NOT NULL,
                content_hash  BLOB,
                pvs_data      BLOB
            );

            -- Primary lookup: worldspace + position + content hash (mod-aware invalidation)
            CREATE UNIQUE INDEX IF NOT EXISTS idx_cells_unique
                ON cells (worldspace, cell_x, cell_y, content_hash);

            -- Secondary: position only (for bulk range queries)
            CREATE INDEX IF NOT EXISTS idx_cells_position
                ON cells (worldspace, cell_x, cell_y);

            COMMIT;
        )";

        // -----------------------------------------------------------------
        // Query text
        // -----------------------------------------------------------------
        constexpr std::string_view getMaxCellIdQuery = R"(
            SELECT max(cell_id) FROM cells
        )";

        constexpr std::string_view findCellQuery = R"(
            SELECT cell_id
              FROM cells
             WHERE worldspace   = :worldspace
               AND cell_x       = :cell_x
               AND cell_y       = :cell_y
               AND content_hash = :content_hash
        )";

        constexpr std::string_view getCellPvsQuery = R"(
            SELECT cell_id, version, pvs_data
              FROM cells
             WHERE worldspace   = :worldspace
               AND cell_x       = :cell_x
               AND cell_y       = :cell_y
               AND content_hash = :content_hash
        )";

        constexpr std::string_view insertCellQuery = R"(
            INSERT INTO cells
                    ( cell_id,  worldspace,  cell_x,  cell_y,  version,  content_hash,  pvs_data)
             VALUES (:cell_id, :worldspace, :cell_x, :cell_y, :version, :content_hash, :pvs_data)
        )";

        constexpr std::string_view updateCellQuery = R"(
            UPDATE cells
               SET version  = :version,
                   pvs_data = :pvs_data,
                   revision = revision + 1
             WHERE cell_id  = :cell_id
        )";

        constexpr std::string_view deleteCellsOutsideWorldspaceQuery = R"(
            DELETE FROM cells
             WHERE worldspace != :worldspace
        )";

        constexpr std::string_view vacuumQuery = "VACUUM";
    }

    // -----------------------------------------------------------------
    // DbQueries implementations
    // -----------------------------------------------------------------
    std::string_view DbQueries::GetMaxCellId::text() noexcept
    {
        return getMaxCellIdQuery;
    }

    std::string_view DbQueries::FindCell::text() noexcept
    {
        return findCellQuery;
    }

    void DbQueries::FindCell::bind(sqlite3& db, sqlite3_stmt& statement,
        std::string_view worldspace, int cellX, int cellY,
        const std::vector<std::byte>& contentHash)
    {
        Sqlite3::bindParameter(db, statement, ":worldspace",   worldspace);
        Sqlite3::bindParameter(db, statement, ":cell_x",       cellX);
        Sqlite3::bindParameter(db, statement, ":cell_y",       cellY);
        Sqlite3::bindParameter(db, statement, ":content_hash", contentHash);
    }

    std::string_view DbQueries::GetCellPvs::text() noexcept
    {
        return getCellPvsQuery;
    }

    void DbQueries::GetCellPvs::bind(sqlite3& db, sqlite3_stmt& statement,
        std::string_view worldspace, int cellX, int cellY,
        const std::vector<std::byte>& contentHash)
    {
        Sqlite3::bindParameter(db, statement, ":worldspace",   worldspace);
        Sqlite3::bindParameter(db, statement, ":cell_x",       cellX);
        Sqlite3::bindParameter(db, statement, ":cell_y",       cellY);
        Sqlite3::bindParameter(db, statement, ":content_hash", contentHash);
    }

    std::string_view DbQueries::InsertCell::text() noexcept
    {
        return insertCellQuery;
    }

    void DbQueries::InsertCell::bind(sqlite3& db, sqlite3_stmt& statement,
        CellId cellId, std::string_view worldspace, int cellX, int cellY,
        CellVersion version, const std::vector<std::byte>& contentHash,
        const std::vector<std::byte>& pvsData)
    {
        Sqlite3::bindParameter(db, statement, ":cell_id",      cellId.mValue);
        Sqlite3::bindParameter(db, statement, ":worldspace",   worldspace);
        Sqlite3::bindParameter(db, statement, ":cell_x",       cellX);
        Sqlite3::bindParameter(db, statement, ":cell_y",       cellY);
        Sqlite3::bindParameter(db, statement, ":version",      version.mValue);
        Sqlite3::bindParameter(db, statement, ":content_hash", contentHash);
        Sqlite3::bindParameter(db, statement, ":pvs_data",     pvsData);
    }

    std::string_view DbQueries::UpdateCell::text() noexcept
    {
        return updateCellQuery;
    }

    void DbQueries::UpdateCell::bind(sqlite3& db, sqlite3_stmt& statement,
        CellId cellId, CellVersion version,
        const std::vector<std::byte>& pvsData)
    {
        Sqlite3::bindParameter(db, statement, ":cell_id", cellId.mValue);
        Sqlite3::bindParameter(db, statement, ":version",  version.mValue);
        Sqlite3::bindParameter(db, statement, ":pvs_data", pvsData);
    }

    std::string_view DbQueries::DeleteCellsOutsideWorldspace::text() noexcept
    {
        return deleteCellsOutsideWorldspaceQuery;
    }

    void DbQueries::DeleteCellsOutsideWorldspace::bind(sqlite3& db, sqlite3_stmt& statement,
        std::string_view worldspace)
    {
        Sqlite3::bindParameter(db, statement, ":worldspace", worldspace);
    }

    std::string_view DbQueries::Vacuum::text() noexcept
    {
        return vacuumQuery;
    }

    // -----------------------------------------------------------------
    // OcclusionDb
    // -----------------------------------------------------------------
    OcclusionDb::OcclusionDb(std::string_view path, std::uint64_t maxFileSize)
        : mDb(Sqlite3::makeDb(path, schema))
        , mGetMaxCellId(*mDb)
        , mFindCell(*mDb)
        , mGetCellPvs(*mDb)
        , mInsertCell(*mDb)
        , mUpdateCell(*mDb)
        , mDeleteCells(*mDb)
        , mVacuum(*mDb)
    {
        // Apply file size limit via PRAGMA (SQLite page limit approach)
        if (maxFileSize > 0)
        {
            const std::string pragma = std::format(
                "PRAGMA max_page_count = {};", maxFileSize / 4096);
            sqlite3_exec(mDb.get(), pragma.c_str(), nullptr, nullptr, nullptr);
        }
        sqlite3_exec(mDb.get(), "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(mDb.get(), "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    }

    Sqlite3::Transaction OcclusionDb::startTransaction(Sqlite3::TransactionMode mode)
    {
        return Sqlite3::Transaction(*mDb, mode);
    }

    CellId OcclusionDb::getMaxCellId()
    {
        std::vector<CellId> results;
        Sqlite3::request(*mDb, mGetMaxCellId, std::back_inserter(results), 1);
        return results.empty() ? CellId{ 0 } : results[0];
    }

    std::optional<CellId> OcclusionDb::findCell(
        ESM::RefId worldspace, int cellX, int cellY,
        const std::vector<std::byte>& contentHash)
    {
        const std::string ws = worldspace.toString();
        std::vector<CellId> results;
        Sqlite3::request(*mDb, mFindCell, std::back_inserter(results), 1, ws, cellX, cellY, contentHash);
        return results.empty() ? std::optional<CellId>{} : results[0];
    }

    std::optional<CellPvsEntry> OcclusionDb::getCellPvs(
        ESM::RefId worldspace, int cellX, int cellY,
        const std::vector<std::byte>& contentHash)
    {
        const std::string ws = worldspace.toString();
        using Row = std::tuple<CellId, CellVersion, std::vector<std::byte>>;
        std::vector<Row> results;
        Sqlite3::request(*mDb, mGetCellPvs, std::back_inserter(results), 1, ws, cellX, cellY, contentHash);
        if (results.empty())
            return std::nullopt;
        auto& [id, ver, data] = results[0];
        return CellPvsEntry{ id, ver, std::move(data) };
    }

    int OcclusionDb::insertCell(
        CellId cellId, ESM::RefId worldspace, int cellX, int cellY,
        CellVersion version, const std::vector<std::byte>& contentHash,
        const std::vector<std::byte>& pvsData)
    {
        const std::string ws = worldspace.toString();
        return Sqlite3::execute(*mDb, mInsertCell,
            cellId, ws, cellX, cellY, version, contentHash, pvsData);
    }

    int OcclusionDb::updateCell(
        CellId cellId, CellVersion version,
        const std::vector<std::byte>& pvsData)
    {
        return Sqlite3::execute(*mDb, mUpdateCell, cellId, version, pvsData);
    }

    int OcclusionDb::deleteCellsOutsideWorldspace(ESM::RefId worldspace)
    {
        const std::string ws = worldspace.toString();
        return Sqlite3::execute(*mDb, mDeleteCells, ws);
    }

    void OcclusionDb::vacuum()
    {
        Sqlite3::execute(*mDb, mVacuum);
    }

} // namespace OcclusionCulling
