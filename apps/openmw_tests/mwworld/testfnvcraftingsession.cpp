#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <components/esm/defs.hpp>

#include "apps/openmw/mwworld/fnvcraftingsession.hpp"

namespace
{
    constexpr std::size_t kPageSize = 2;
    constexpr std::size_t kRedrawLimit = 8;
    constexpr std::uint32_t kFirstRecipe = 0x01010000;
    constexpr std::size_t kNoBlockedIndex = std::numeric_limits<std::size_t>::max();

    MWWorld::PreparedFnvCraftingCatalog makeCatalog(std::size_t count, std::size_t blockedIndex)
    {
        MWWorld::PreparedFnvCraftingCatalog catalog;
        catalog.mCategoryName = "Workbench Recipes";
        for (std::size_t index{}; index < count; ++index)
        {
            MWWorld::PreparedFnvCraftingCatalogEntry entry;
            entry.mRecipe = ESM::FormId::fromUint32(kFirstRecipe + static_cast<std::uint32_t>(index));
            entry.mName = "Recipe " + std::to_string(index);
            entry.mStaticBlocker = index == blockedIndex ? MWWorld::FnvCraftingPreparationError::ConditionalRecipe
                                                         : MWWorld::FnvCraftingPreparationError::None;
            catalog.mEntries.push_back(std::move(entry));
        }
        return catalog;
    }

    class ScriptedPresenter final : public MWWorld::FnvCraftingSessionPresenter
    {
    public:
        std::vector<MWWorld::FnvCraftingSessionPageRequest> mPages;
        std::vector<MWWorld::FnvCraftingSessionEntryDecision> mDecisions;
        std::vector<MWWorld::FnvCraftingSessionNoticeResult> mNotices;
        std::vector<std::size_t> mPageNumbers;
        std::vector<std::size_t> mPageCounts;
        std::vector<std::size_t> mShownPageSizes;
        std::vector<ESM::FormId> mBlocked;
        std::vector<ESM::FormId> mResults;
        std::size_t mPage{};
        std::size_t mDecision{};
        std::size_t mNotice{};

        FnvCraftingSessionPageRequest selectPage(const MWWorld::PreparedFnvCraftingCatalog&, std::size_t page,
            std::size_t pageCount, std::span<const MWWorld::PreparedFnvCraftingCatalogEntry> entries) override
        {
            mPageNumbers.push_back(page);
            mPageCounts.push_back(pageCount);
            mShownPageSizes.push_back(entries.size());
            return mPage < mPages.size() ? mPages[mPage++]
                                         : MWWorld::FnvCraftingSessionPageRequest{};
        }

        MWWorld::FnvCraftingSessionEntryDecision decideEntry(
            const MWWorld::PreparedFnvCraftingCatalogEntry&) override
        {
            return mDecision < mDecisions.size() ? mDecisions[mDecision++]
                                                  : MWWorld::FnvCraftingSessionEntryDecision::Invalid;
        }

        MWWorld::FnvCraftingSessionNoticeResult showBlocked(
            const MWWorld::PreparedFnvCraftingCatalogEntry& entry) override
        {
            mBlocked.push_back(entry.mRecipe);
            return mNotice < mNotices.size() ? mNotices[mNotice++]
                                              : MWWorld::FnvCraftingSessionNoticeResult::Invalid;
        }

        MWWorld::FnvCraftingSessionNoticeResult showResult(
            const MWWorld::PreparedFnvCraftingCatalogEntry& entry, const MWWorld::FnvCraftingAttemptResult&) override
        {
            mResults.push_back(entry.mRecipe);
            return mNotice < mNotices.size() ? mNotices[mNotice++]
                                              : MWWorld::FnvCraftingSessionNoticeResult::Invalid;
        }
    };

    class FakeBackend final : public MWWorld::FnvCraftingSessionBackend
    {
    public:
        std::vector<ESM::FormId> mRecipes;

        MWWorld::FnvCraftingAttemptResult craft(ESM::FormId recipe) override
        {
            mRecipes.push_back(recipe);
            return { true, MWWorld::FnvCraftingPreparationError::None, MWWorld::FnvCraftingCommitResult::Applied };
        }
    };
}

TEST(FnvCraftingSessionTest, NavigatesInjectedPagesAndCraftsOnlyAfterConfirmation)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(3, kNoBlockedIndex);
    ScriptedPresenter presenter;
    presenter.mPages = { { MWWorld::FnvCraftingSessionPageSelection::Next },
        { MWWorld::FnvCraftingSessionPageSelection::Entry, 0 },
        { MWWorld::FnvCraftingSessionPageSelection::Close } };
    presenter.mDecisions = { MWWorld::FnvCraftingSessionEntryDecision::Craft };
    presenter.mNotices = { MWWorld::FnvCraftingSessionNoticeResult::Continue };
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(
                  catalog, { kPageSize, kRedrawLimit }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    ASSERT_EQ(backend.mRecipes.size(), 1u);
    EXPECT_EQ(backend.mRecipes.front(), ESM::FormId::fromUint32(kFirstRecipe + 2));
    EXPECT_EQ(presenter.mPageNumbers, (std::vector<std::size_t>{ 0, 1, 1 }));
    EXPECT_EQ(presenter.mPageCounts, (std::vector<std::size_t>{ 2, 2, 2 }));
    EXPECT_EQ(presenter.mShownPageSizes, (std::vector<std::size_t>{ 2, 1, 1 }));
}

TEST(FnvCraftingSessionTest, ShowsBlockedEntriesWithoutCallingBackend)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1, 0);
    ScriptedPresenter presenter;
    presenter.mPages = { { MWWorld::FnvCraftingSessionPageSelection::Entry, 0 },
        { MWWorld::FnvCraftingSessionPageSelection::Close } };
    presenter.mNotices = { MWWorld::FnvCraftingSessionNoticeResult::Continue };
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(
                  catalog, { kPageSize, kRedrawLimit }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    EXPECT_TRUE(backend.mRecipes.empty());
    ASSERT_EQ(presenter.mBlocked.size(), 1u);
    EXPECT_EQ(presenter.mBlocked.front(), ESM::FormId::fromUint32(kFirstRecipe));
}

TEST(FnvCraftingSessionTest, RejectsInvalidPoliciesAndSelections)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1, 99);
    ScriptedPresenter presenter;
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, { 0, kRedrawLimit }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::InvalidPolicy);
    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, { kPageSize, 0 }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::InvalidPolicy);

    presenter.mPages = { { MWWorld::FnvCraftingSessionPageSelection::Entry, 2 } };
    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, { kPageSize, kRedrawLimit }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::InvalidSelection);
}

TEST(FnvCraftingSessionTest, StopsAtInjectedRedrawLimit)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1, 99);
    ScriptedPresenter presenter;
    presenter.mPages = { { MWWorld::FnvCraftingSessionPageSelection::Next } };
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, { kPageSize, 1 }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::InvalidSelection);

    presenter.mPages = { { MWWorld::FnvCraftingSessionPageSelection::Entry, 0 },
        { MWWorld::FnvCraftingSessionPageSelection::Entry, 0 } };
    presenter.mDecisions = { MWWorld::FnvCraftingSessionEntryDecision::Back,
        MWWorld::FnvCraftingSessionEntryDecision::Back };
    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, { kPageSize, 2 }, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::RedrawLimitExceeded);
}
