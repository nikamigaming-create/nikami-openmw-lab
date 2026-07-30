#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONDB_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONDB_H

#include "types.hpp"

#include <components/esm/refid.hpp>
#include <components/sqlite3/db.hpp>
#include <components/sqlite3/statement.hpp>
#include <components/sqlite3/transaction.hpp>
#include <components/sqlite3/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace OcclusionCulling
{
    // ---------------------------------------------------------------------------
    // DB query descriptors (same pattern as DetourNavigator::DbQueries)
    // ---------------------------------------------------------------------------
    namespace DbQueries
    {
        struct GetMaxCellId
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3&, sqlite3_stmt&) {}
        };

        struct FindCell
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3& db, sqlite3_stmt& statement,
                std::string_view worldspace, int cellX, int cellY,
                const std::vector<std::byte>& contentHash);
        };

        struct GetCellPvs
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3& db, sqlite3_stmt& statement,
                std::string_view worldspace, int cellX, int cellY,
                const std::vector<std::byte>& contentHash);
        };

        struct InsertCell
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3& db, sqlite3_stmt& statement,
                CellId cellId, std::string_view worldspace, int cellX, int cellY,
                CellVersion version, const std::vector<std::byte>& contentHash,
                const std::vector<std::byte>& pvsData);
        };

        struct UpdateCell
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3& db, sqlite3_stmt& statement,
                CellId cellId, CellVersion version,
                const std::vector<std::byte>& pvsData);
        };

        struct DeleteCellsOutsideWorldspace
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3& db, sqlite3_stmt& statement,
                std::string_view worldspace);
        };

        struct Vacuum
        {
            static std::string_view text() noexcept;
            static void bind(sqlite3&, sqlite3_stmt&) {}
        };
    }

    // ---------------------------------------------------------------------------
    // A resolved PVS entry returned from the DB
    // ---------------------------------------------------------------------------
    struct CellPvsEntry
    {
        CellId  mCellId;
        CellVersion mVersion;
        // Compressed blob of sorted int64 visible-cell IDs.
        // Decompress with Misc::decompress() to get raw bytes,
        // then interpret as std::vector<int64_t>.
        std::vector<std::byte> mPvsData;
    };

    // ---------------------------------------------------------------------------
    // OcclusionDb
    // ---------------------------------------------------------------------------
    class OcclusionDb
    {
    public:
        explicit OcclusionDb(std::string_view path, std::uint64_t maxFileSize);

        Sqlite3::Transaction startTransaction(
            Sqlite3::TransactionMode mode = Sqlite3::TransactionMode::Default);

        CellId getMaxCellId();

        /// Returns the CellId+Version if the cell with the given content hash
        /// already exists (used to skip re-generation).
        std::optional<CellId> findCell(
            ESM::RefId worldspace, int cellX, int cellY,
            const std::vector<std::byte>& contentHash);

        /// Retrieves the full PVS blob for a cell (used at runtime by the engine).
        std::optional<CellPvsEntry> getCellPvs(
            ESM::RefId worldspace, int cellX, int cellY,
            const std::vector<std::byte>& contentHash);

        int insertCell(
            CellId cellId, ESM::RefId worldspace, int cellX, int cellY,
            CellVersion version, const std::vector<std::byte>& contentHash,
            const std::vector<std::byte>& pvsData);

        int updateCell(
            CellId cellId, CellVersion version,
            const std::vector<std::byte>& pvsData);

        /// Removes all entries that no longer belong to the current content list.
        int deleteCellsOutsideWorldspace(ESM::RefId worldspace);

        void vacuum();

    private:
        Sqlite3::Db mDb;
        Sqlite3::Statement<DbQueries::GetMaxCellId>               mGetMaxCellId;
        Sqlite3::Statement<DbQueries::FindCell>                   mFindCell;
        Sqlite3::Statement<DbQueries::GetCellPvs>                 mGetCellPvs;
        Sqlite3::Statement<DbQueries::InsertCell>                 mInsertCell;
        Sqlite3::Statement<DbQueries::UpdateCell>                 mUpdateCell;
        Sqlite3::Statement<DbQueries::DeleteCellsOutsideWorldspace> mDeleteCells;
        Sqlite3::Statement<DbQueries::Vacuum>                     mVacuum;
    };

} // namespace OcclusionCulling

#endif
