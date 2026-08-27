#include "fnvcraftingsession.hpp"

#include <algorithm>

namespace MWWorld
{
    FnvCraftingSessionRunResult runPreparedFnvCraftingSession(
        const PreparedFnvCraftingCatalog& catalog, const FnvCraftingSessionPolicy& policy,
        FnvCraftingSessionPresenter& presenter, FnvCraftingSessionBackend& backend)
    {
        if (policy.mPageSize == std::size_t{} || policy.mMaxRedraws == std::size_t{})
            return FnvCraftingSessionRunResult::InvalidPolicy;
        if (catalog.mEntries.empty())
            return FnvCraftingSessionRunResult::InvalidSelection;

        const std::size_t pageCount
            = catalog.mEntries.size() / policy.mPageSize
            + (catalog.mEntries.size() % policy.mPageSize != 0 ? std::size_t{ 1 } : std::size_t{});
        std::size_t page{};
        std::size_t redraws{};
        while (true)
        {
            if (redraws >= policy.mMaxRedraws)
                return FnvCraftingSessionRunResult::RedrawLimitExceeded;
            ++redraws;

            const std::size_t first = page * policy.mPageSize;
            const std::size_t count = std::min(policy.mPageSize, catalog.mEntries.size() - first);
            const FnvCraftingSessionPageRequest request
                = presenter.selectPage(catalog, page, pageCount,
                    std::span<const PreparedFnvCraftingCatalogEntry>(catalog.mEntries).subspan(first, count));
            switch (request.mSelection)
            {
                case FnvCraftingSessionPageSelection::Cancel:
                    return FnvCraftingSessionRunResult::Cancelled;
                case FnvCraftingSessionPageSelection::Close:
                    return FnvCraftingSessionRunResult::Closed;
                case FnvCraftingSessionPageSelection::Previous:
                    if (page == std::size_t{})
                        return FnvCraftingSessionRunResult::InvalidSelection;
                    --page;
                    continue;
                case FnvCraftingSessionPageSelection::Next:
                    if (page + std::size_t{ 1 } >= pageCount)
                        return FnvCraftingSessionRunResult::InvalidSelection;
                    ++page;
                    continue;
                case FnvCraftingSessionPageSelection::Entry:
                    if (request.mEntry >= count)
                        return FnvCraftingSessionRunResult::InvalidSelection;
                    break;
                case FnvCraftingSessionPageSelection::Invalid:
                    return FnvCraftingSessionRunResult::InvalidSelection;
            }

            const PreparedFnvCraftingCatalogEntry& entry = catalog.mEntries[first + request.mEntry];
            if (!entry.isStaticallySupported())
            {
                switch (presenter.showBlocked(entry))
                {
                    case FnvCraftingSessionNoticeResult::Continue:
                        continue;
                    case FnvCraftingSessionNoticeResult::Cancel:
                        return FnvCraftingSessionRunResult::Cancelled;
                    case FnvCraftingSessionNoticeResult::Invalid:
                        return FnvCraftingSessionRunResult::InvalidSelection;
                }
            }
            else
            {
                switch (presenter.decideEntry(entry))
                {
                    case FnvCraftingSessionEntryDecision::Back:
                        continue;
                    case FnvCraftingSessionEntryDecision::Close:
                        return FnvCraftingSessionRunResult::Closed;
                    case FnvCraftingSessionEntryDecision::Cancel:
                        return FnvCraftingSessionRunResult::Cancelled;
                    case FnvCraftingSessionEntryDecision::Invalid:
                        return FnvCraftingSessionRunResult::InvalidSelection;
                    case FnvCraftingSessionEntryDecision::Craft:
                        break;
                }

                const FnvCraftingAttemptResult result = backend.craft(entry.mRecipe);
                switch (presenter.showResult(entry, result))
                {
                    case FnvCraftingSessionNoticeResult::Continue:
                        continue;
                    case FnvCraftingSessionNoticeResult::Cancel:
                        return FnvCraftingSessionRunResult::Cancelled;
                    case FnvCraftingSessionNoticeResult::Invalid:
                        return FnvCraftingSessionRunResult::InvalidSelection;
                }
            }
        }
    }
}
