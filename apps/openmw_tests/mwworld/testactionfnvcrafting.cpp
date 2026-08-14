#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/loadweap.hpp>

#include "apps/openmw/mwworld/actionfnvcrafting.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    constexpr std::uint32_t sWorkbench = 0x00075005;
    constexpr std::uint32_t sWorkbenchScript = 0x0013e11c;
    constexpr std::uint32_t sWorkbenchCategory = 0x0013b2c1;
    constexpr std::uint32_t sSubCategory = 0x0013b2c2;
    constexpr std::uint32_t sFirstRecipe = 0x01010000;
    constexpr std::uint32_t sIngredient = 0x01020000;
    constexpr std::uint32_t sOutput = 0x01020001;

    template <class Record>
    Record makeItem(std::uint32_t id, std::string name)
    {
        Record result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mEditorId = name;
        result.mFullName = std::move(name);
        return result;
    }

    ESM4::RecipeCategory makeCategory(std::uint32_t id, std::string name)
    {
        ESM4::RecipeCategory result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mEditorId = name;
        result.mFullName = std::move(name);
        return result;
    }

    MWWorld::PreparedFnvCraftingCatalog makeCatalog(
        std::size_t recipeCount, std::optional<std::size_t> conditional = std::nullopt)
    {
        MWWorld::ESMStore store;
        ESM4::Activator station{};
        station.mId = ESM::FormId::fromUint32(sWorkbench);
        station.mScriptId = ESM::FormId::fromUint32(sWorkbenchScript);
        station.mEditorId = "WorkBench";
        station.mFullName = "Workbench";
        const ESM4::Activator* storedStation = store.overrideRecord(station);
        store.overrideRecord(makeCategory(sWorkbenchCategory, "Workbench Recipes"));
        store.overrideRecord(makeCategory(sSubCategory, "Utility"));
        store.overrideRecord(makeItem<ESM4::MiscItem>(sIngredient, "Scrap Metal"));
        store.overrideRecord(makeItem<ESM4::Weapon>(sOutput, "Crafted Weapon"));

        for (std::size_t index = 0; index < recipeCount; ++index)
        {
            ESM4::Recipe recipe{};
            recipe.mId = ESM::FormId::fromUint32(sFirstRecipe + static_cast<std::uint32_t>(index));
            recipe.mEditorId = "Recipe" + std::to_string(index);
            recipe.mFullName = "Recipe " + std::to_string(index);
            recipe.mData.mCategory = ESM::FormId::fromUint32(sWorkbenchCategory);
            recipe.mData.mSubCategory = ESM::FormId::fromUint32(sSubCategory);
            recipe.mIngredients = { { ESM::FormId::fromUint32(sIngredient), 2 } };
            recipe.mOutputs = { { ESM::FormId::fromUint32(sOutput), 1 } };
            if (conditional == index)
                recipe.mConditions.emplace_back();
            store.overrideRecord(recipe);
        }

        std::optional<MWWorld::PreparedFnvCraftingCatalog> result
            = MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, &store, storedStation });
        if (!result)
            throw std::runtime_error("failed to construct test crafting catalog");
        return std::move(*result);
    }

    class ScriptedPresenter final : public MWWorld::FnvCraftingSessionPresenter
    {
        std::vector<int> mSelections;
        std::size_t mNext = 0;

    public:
        struct Call
        {
            std::string mMessage;
            std::vector<std::string> mButtons;
        };

        std::vector<Call> mCalls;

        explicit ScriptedPresenter(std::vector<int> selections)
            : mSelections(std::move(selections))
        {
        }

        int show(std::string_view message, const std::vector<std::string>& buttons) override
        {
            mCalls.push_back({ std::string(message), buttons });
            return mNext < mSelections.size() ? mSelections[mNext++] : -1;
        }
    };

    class FakeBackend final : public MWWorld::FnvCraftingSessionBackend
    {
    public:
        MWWorld::FnvCraftingAttemptResult mResult{ true, MWWorld::FnvCraftingPreparationError::None,
            MWWorld::FnvCraftingCommitResult::Applied };
        std::vector<ESM::FormId> mRecipes;

        MWWorld::FnvCraftingAttemptResult craft(ESM::FormId recipe) override
        {
            mRecipes.push_back(recipe);
            return mResult;
        }
    };
}

TEST(ActionFnvCraftingTest, PagesForwardBacksOutOfDetailAndPagesBackwardWithoutMutation)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(7);
    ScriptedPresenter presenter({ 6, 0, 1, 1, 7 });
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    EXPECT_TRUE(backend.mRecipes.empty());
    ASSERT_EQ(presenter.mCalls.size(), 5u);
    EXPECT_EQ(presenter.mCalls[0].mButtons[6], "Next");
    EXPECT_EQ(presenter.mCalls[1].mButtons[1], "Previous");
    EXPECT_EQ(presenter.mCalls[2].mButtons, (std::vector<std::string>{ "Craft", "Back" }));
}

TEST(ActionFnvCraftingTest, BlockedEntryIsVisibleButNeverReachesBackend)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1, 0);
    ScriptedPresenter presenter({ 0, 0, 1 });
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    EXPECT_TRUE(backend.mRecipes.empty());
    ASSERT_GE(presenter.mCalls.size(), 2u);
    EXPECT_NE(presenter.mCalls[0].mButtons[0].find("[blocked]"), std::string::npos);
    EXPECT_NE(presenter.mCalls[1].mMessage.find("conditional-recipe"), std::string::npos);
}

TEST(ActionFnvCraftingTest, ConfirmationRequestsExactlyOneSuccessfulTransactionThenRefreshes)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1);
    ScriptedPresenter presenter({ 0, 0, 0, 1 });
    FakeBackend backend;

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    ASSERT_EQ(backend.mRecipes.size(), 1u);
    EXPECT_EQ(backend.mRecipes[0], ESM::FormId::fromUint32(sFirstRecipe));
    ASSERT_EQ(presenter.mCalls.size(), 4u);
    EXPECT_NE(presenter.mCalls[2].mMessage.find("Crafted Recipe 0"), std::string::npos);
    EXPECT_NE(presenter.mCalls[3].mMessage.find("Page 1 of 1"), std::string::npos);
}

TEST(ActionFnvCraftingTest, PreparationFailureReportsAndRefreshesWithoutRetrying)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1);
    ScriptedPresenter presenter({ 0, 0, 0, 1 });
    FakeBackend backend;
    backend.mResult = { false, MWWorld::FnvCraftingPreparationError::InsufficientIngredients,
        MWWorld::FnvCraftingCommitResult::InvalidPlan };

    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, presenter, backend),
        MWWorld::FnvCraftingSessionRunResult::Closed);
    EXPECT_EQ(backend.mRecipes.size(), 1u);
    ASSERT_GE(presenter.mCalls.size(), 3u);
    EXPECT_NE(presenter.mCalls[2].mMessage.find("insufficient-ingredients"), std::string::npos);
}

TEST(ActionFnvCraftingTest, RejectsInvalidSelectionAndTreatsDismissalAsCancellation)
{
    const MWWorld::PreparedFnvCraftingCatalog catalog = makeCatalog(1);
    FakeBackend backend;
    ScriptedPresenter invalid({ 99 });
    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, invalid, backend),
        MWWorld::FnvCraftingSessionRunResult::InvalidSelection);

    ScriptedPresenter cancelled({ -1 });
    EXPECT_EQ(MWWorld::runPreparedFnvCraftingSession(catalog, cancelled, backend),
        MWWorld::FnvCraftingSessionRunResult::Cancelled);
    EXPECT_TRUE(backend.mRecipes.empty());
}
