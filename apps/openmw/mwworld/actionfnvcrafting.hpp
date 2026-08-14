#ifndef GAME_MWWORLD_ACTIONFNVCRAFTING_H
#define GAME_MWWORLD_ACTIONFNVCRAFTING_H

#include <cstddef>
#include <string_view>
#include <vector>

#include "action.hpp"
#include "fnvcraftingruntime.hpp"

namespace MWWorld
{
    inline constexpr std::size_t FnvCraftingCatalogPageSize = 6;

    class FnvCraftingSessionPresenter
    {
    public:
        virtual ~FnvCraftingSessionPresenter() = default;
        virtual int show(std::string_view message, const std::vector<std::string>& buttons) = 0;
    };

    struct FnvCraftingAttemptResult
    {
        bool mPrepared = false;
        FnvCraftingPreparationError mPreparationError = FnvCraftingPreparationError::None;
        FnvCraftingCommitResult mCommitResult = FnvCraftingCommitResult::InvalidPlan;
    };

    class FnvCraftingSessionBackend
    {
    public:
        virtual ~FnvCraftingSessionBackend() = default;
        virtual FnvCraftingAttemptResult craft(ESM::FormId recipe) = 0;
    };

    enum class FnvCraftingSessionRunResult
    {
        Closed,
        Cancelled,
        InvalidSelection,
        RedrawLimitExceeded,
    };

    /// Drive the immutable paged catalog. A transaction is requested exactly
    /// once only after a supported recipe receives explicit confirmation.
    [[nodiscard]] FnvCraftingSessionRunResult runPreparedFnvCraftingSession(const PreparedFnvCraftingCatalog& catalog,
        FnvCraftingSessionPresenter& presenter, FnvCraftingSessionBackend& backend);

    class ActionFnvCraftingStation final : public Action
    {
        const PreparedFnvCraftingCatalog mCatalog;

        void executeImp(const Ptr& actor) override;

    public:
        ActionFnvCraftingStation(const Ptr& target, PreparedFnvCraftingCatalog catalog);
    };
}

#endif
