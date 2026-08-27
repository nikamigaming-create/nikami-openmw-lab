#ifndef OPENMW_MWWORLD_FNVCRAFTINGSESSION_H
#define OPENMW_MWWORLD_FNVCRAFTINGSESSION_H

#include <cstddef>
#include <span>

#include "fnvcraftingruntime.hpp"

namespace MWWorld
{
    struct FnvCraftingAttemptResult
    {
        bool mPrepared = false;
        FnvCraftingPreparationError mPreparationError = FnvCraftingPreparationError::None;
        FnvCraftingCommitResult mCommitResult = FnvCraftingCommitResult::InvalidPlan;
    };

    enum class FnvCraftingSessionPageSelection
    {
        Entry,
        Previous,
        Next,
        Close,
        Cancel,
        Invalid,
    };

    struct FnvCraftingSessionPageRequest
    {
        FnvCraftingSessionPageSelection mSelection = FnvCraftingSessionPageSelection::Invalid;
        std::size_t mEntry{};
    };

    enum class FnvCraftingSessionEntryDecision
    {
        Craft,
        Back,
        Close,
        Cancel,
        Invalid,
    };

    enum class FnvCraftingSessionNoticeResult
    {
        Continue,
        Cancel,
        Invalid,
    };

    /// The presenter is an engine-facing seam. A MyGUI, headless, or test
    /// adapter can implement it without making the crafting controller know
    /// about widgets, localization, or input devices.
    class FnvCraftingSessionPresenter
    {
    public:
        virtual ~FnvCraftingSessionPresenter() = default;

        virtual FnvCraftingSessionPageRequest selectPage(const PreparedFnvCraftingCatalog& catalog,
            std::size_t page, std::size_t pageCount, std::span<const PreparedFnvCraftingCatalogEntry> entries)
            = 0;
        virtual FnvCraftingSessionEntryDecision decideEntry(const PreparedFnvCraftingCatalogEntry& entry) = 0;
        virtual FnvCraftingSessionNoticeResult showBlocked(const PreparedFnvCraftingCatalogEntry& entry) = 0;
        virtual FnvCraftingSessionNoticeResult showResult(
            const PreparedFnvCraftingCatalogEntry& entry, const FnvCraftingAttemptResult& result)
            = 0;
    };

    class FnvCraftingSessionBackend
    {
    public:
        virtual ~FnvCraftingSessionBackend() = default;
        virtual FnvCraftingAttemptResult craft(ESM::FormId recipe) = 0;
    };

    struct FnvCraftingSessionPolicy
    {
        std::size_t mPageSize;
        std::size_t mMaxRedraws;
    };

    enum class FnvCraftingSessionRunResult
    {
        Closed,
        Cancelled,
        InvalidPolicy,
        InvalidSelection,
        RedrawLimitExceeded,
    };

    /// Drive a prepared catalog through an injected presentation/backend seam.
    /// The controller owns navigation and confirmation ordering only; it does
    /// not choose a UI toolkit, input source, localization string, or policy
    /// number.
    [[nodiscard]] FnvCraftingSessionRunResult runPreparedFnvCraftingSession(
        const PreparedFnvCraftingCatalog& catalog, const FnvCraftingSessionPolicy& policy,
        FnvCraftingSessionPresenter& presenter, FnvCraftingSessionBackend& backend);
}

#endif
