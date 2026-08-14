#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadweap.hpp>

#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvcraftingruntime.hpp"
#include "apps/openmw/mwworld/fnvplayerruntimestate.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"

namespace
{
    constexpr std::uint32_t sWorkbench = 0x00075005;
    constexpr std::uint32_t sWorkbenchScript = 0x0013e11c;
    constexpr std::uint32_t sWorkbenchCategory = 0x0013b2c1;
    constexpr std::uint32_t sReloadingBench = 0x0015361f;
    constexpr std::uint32_t sReloadingBenchScript = 0x0015361e;
    constexpr std::uint32_t sReloadingBenchCategory = 0x00153621;
    constexpr std::uint32_t sSubCategory = 0x0013b2c2;
    constexpr std::uint32_t sRecipe = 0x01010000;
    constexpr std::uint32_t sMisc = 0x01020001;
    constexpr std::uint32_t sPotion = 0x01020002;
    constexpr std::uint32_t sAmmo = 0x01020003;
    constexpr std::uint32_t sWeapon = 0x01020004;
    constexpr std::uint32_t sArmor = 0x01020005;
    constexpr std::uint32_t sBook = 0x01020006;
    constexpr std::uint32_t sCoinShotCurrency = 0x00176ac4;

    ESM::RefId refId(std::uint32_t value)
    {
        return ESM::RefId(ESM::FormId::fromUint32(value));
    }

    template <class Record>
    Record makeItem(std::uint32_t id)
    {
        Record result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mEditorId = "Item" + std::to_string(id);
        result.mFullName = "Item " + std::to_string(id);
        return result;
    }

    ESM4::Activator makeStation(std::uint32_t id = sWorkbench, std::uint32_t script = sWorkbenchScript)
    {
        ESM4::Activator result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mScriptId = ESM::FormId::fromUint32(script);
        result.mEditorId = "CraftingStation";
        result.mFullName = "Crafting Station";
        return result;
    }

    ESM4::RecipeCategory makeCategory(std::uint32_t id)
    {
        ESM4::RecipeCategory result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mEditorId = "RecipeCategory" + std::to_string(id);
        result.mFullName = "Recipe Category " + std::to_string(id);
        return result;
    }

    ESM4::Recipe makeRecipe(std::uint32_t category = sWorkbenchCategory, std::uint32_t id = sRecipe)
    {
        ESM4::Recipe result{};
        result.mId = ESM::FormId::fromUint32(id);
        result.mEditorId = "Recipe" + std::to_string(id);
        result.mFullName = "Recipe " + std::to_string(id);
        result.mData.mRequiredSkill = 39; // Repair
        result.mData.mRequiredSkillLevel = 40;
        result.mData.mCategory = ESM::FormId::fromUint32(category);
        result.mData.mSubCategory = ESM::FormId::fromUint32(sSubCategory);
        result.mIngredients = {
            { ESM::FormId::fromUint32(sMisc), 2 },
            { ESM::FormId::fromUint32(sAmmo), 3 },
        };
        result.mOutputs = {
            { ESM::FormId::fromUint32(sWeapon), 1 },
            { ESM::FormId::fromUint32(sPotion), 2 },
            { ESM::FormId::fromUint32(sArmor), 1 },
        };
        return result;
    }

    MWWorld::FalloutPlayerState makePlayerBase(std::uint8_t repairOffset = 0)
    {
        MWWorld::FalloutPlayerState result;
        result.mBaseRecord = ESM::FormId::fromUint32(0x00000007);
        result.mHealth = 100;
        result.mSpecial.fill(5);
        result.mSkillValues.fill(50);
        result.mSkillOffsets.fill(0);
        result.mSkillOffsets[7] = repairOffset;
        return result;
    }

    struct ActorHandle
    {
        ESM4::MiscItem mBase;
        ESM::CellRef mCellRef;
        MWWorld::LiveCellRef<ESM4::MiscItem> mLive;
        MWWorld::Ptr mPtr;

        static ESM4::MiscItem makeBase(std::uint32_t id)
        {
            static const bool sClassesRegistered = [] {
                MWClass::registerClasses();
                return true;
            }();
            static_cast<void>(sClassesRegistered);
            return makeItem<ESM4::MiscItem>(id);
        }

        static ESM::CellRef makeCellRef(std::uint32_t id)
        {
            ESM::CellRef result = ESM::makeBlankCellRef();
            result.mRefID = refId(id);
            return result;
        }

        explicit ActorHandle(std::uint32_t id)
            : mBase(makeBase(id))
            , mCellRef(makeCellRef(id))
            , mLive(mCellRef, &mBase)
            , mPtr(&mLive)
        {
        }
    };

    class FakeInventory final : public MWWorld::FnvCraftingInventory
    {
    public:
        struct Call
        {
            std::string mOperation;
            ESM::RefId mItem;
            int mCount = 0;
            bool mAllowAutoEquip = true;
        };

        MWWorld::Ptr mOwner;
        std::map<ESM::RefId, std::int64_t> mCounts;
        std::vector<Call> mCalls;
        int mPrepareCalls = 0;
        bool mCountInvalid = false;
        bool mRemoveSucceeds = true;
        bool mAddSucceeds = true;

        explicit FakeInventory(const MWWorld::Ptr& owner)
            : mOwner(owner)
        {
        }

        bool prepareForActor(const MWWorld::Ptr& actor) noexcept override
        {
            ++mPrepareCalls;
            return !actor.isEmpty() && actor == mOwner;
        }

        bool stillBelongsTo(const MWWorld::Ptr& actor) const noexcept override
        {
            return !actor.isEmpty() && actor == mOwner;
        }

        std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept override
        {
            if (mCountInvalid)
                return std::nullopt;
            const auto found = mCounts.find(item);
            return found != mCounts.end() ? found->second : 0;
        }

        bool remove(const ESM::RefId& item, int count) noexcept override
        {
            mCalls.push_back({ "remove", item, count, false });
            if (!mRemoveSucceeds || count <= 0 || mCounts[item] < count)
                return false;
            mCounts[item] -= count;
            return true;
        }

        bool add(const MWWorld::Ptr& item, int count, bool allowAutoEquip) noexcept override
        {
            const ESM::RefId id = item.getCellRef().getRefId();
            mCalls.push_back({ "add", id, count, allowAutoEquip });
            if (!mAddSucceeds || count <= 0)
                return false;
            mCounts[id] += count;
            return true;
        }
    };

    struct CraftingFixture
    {
        MWWorld::ESMStore mStore;
        ActorHandle mPlayer{ 0x01030000 };
        ActorHandle mOther{ 0x01030001 };
        FakeInventory mInventory{ mPlayer.mPtr };
        MWWorld::FalloutPlayerRuntimeState mPlayerState;
        const ESM4::Activator* mStation = nullptr;
        const ESM4::RecipeCategory* mCategory = nullptr;
        const ESM4::RecipeCategory* mSubCategory = nullptr;
        const ESM4::Recipe* mRecipe = nullptr;

        CraftingFixture()
        {
            mStation = mStore.overrideRecord(makeStation());
            mCategory = mStore.overrideRecord(makeCategory(sWorkbenchCategory));
            mSubCategory = mStore.overrideRecord(makeCategory(sSubCategory));
            mStore.overrideRecord(makeItem<ESM4::MiscItem>(sMisc));
            mStore.overrideRecord(makeItem<ESM4::Potion>(sPotion));
            mStore.overrideRecord(makeItem<ESM4::Ammunition>(sAmmo));
            mStore.overrideRecord(makeItem<ESM4::Weapon>(sWeapon));
            mStore.overrideRecord(makeItem<ESM4::Armor>(sArmor));
            mStore.overrideRecord(makeItem<ESM4::Book>(sBook));
            mRecipe = mStore.overrideRecord(makeRecipe());
            mPlayerState.initialize(makePlayerBase());
            mInventory.mCounts[refId(sMisc)] = 5;
            mInventory.mCounts[refId(sAmmo)] = 3;
        }

        MWWorld::FnvCraftingTransactionSource source() const
        {
            return { MWWorld::ESM4Game::FalloutNewVegas, &mStore, mStation, mCategory, mRecipe, mPlayer.mPtr,
                mPlayer.mPtr, const_cast<FakeInventory*>(&mInventory), &mPlayerState };
        }

        void replaceRecipe(ESM4::Recipe value) { mRecipe = mStore.overrideRecord(value); }

        template <class F>
        void mutateRecipe(F&& mutate)
        {
            ESM4::Recipe value = *mRecipe;
            std::forward<F>(mutate)(value);
            replaceRecipe(std::move(value));
        }
    };

    MWWorld::FnvCraftingPreparationError preparationError(const MWWorld::FnvCraftingTransactionSource& source)
    {
        MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::None;
        EXPECT_FALSE(MWWorld::prepareFnvCraftingTransaction(source, &error));
        return error;
    }
}

static_assert(!std::is_copy_constructible_v<MWWorld::PreparedFnvCraftingPlan>);
static_assert(!std::is_copy_assignable_v<MWWorld::PreparedFnvCraftingPlan>);
static_assert(std::is_nothrow_move_constructible_v<MWWorld::PreparedFnvCraftingPlan>);
static_assert(!std::is_copy_assignable_v<MWWorld::PreparedFnvCraftingCatalog>);
static_assert(!std::is_move_assignable_v<MWWorld::PreparedFnvCraftingCatalog>);

TEST(FnvCraftingRuntimeTest, PublicStationMappingAcceptsOnlyTheTwoExactBaseScriptPairs)
{
    EXPECT_EQ(MWWorld::getFnvCraftingStationCategory(makeStation()), ESM::FormId::fromUint32(sWorkbenchCategory));
    EXPECT_EQ(MWWorld::getFnvCraftingStationCategory(makeStation(sReloadingBench, sReloadingBenchScript)),
        ESM::FormId::fromUint32(sReloadingBenchCategory));
    EXPECT_FALSE(MWWorld::getFnvCraftingStationCategory(makeStation(sWorkbench, sWorkbenchScript + 1)));
    EXPECT_FALSE(MWWorld::getFnvCraftingStationCategory(makeStation(sWorkbench + 1, sWorkbenchScript)));
}

TEST(FnvCraftingRuntimeTest, MapsCanonicalAuthoredRecipeMenuScriptWithoutStationFormIdKnowledge)
{
    constexpr std::uint32_t stationId = 0x02000001;
    constexpr std::uint32_t scriptId = 0x02000002;
    constexpr std::uint32_t categoryId = 0x02000003;

    MWWorld::ESMStore store;
    ESM4::Activator station = makeStation(stationId, scriptId);
    ESM4::Script script;
    script.mId = ESM::FormId::fromUint32(scriptId);
    script.mEditorId = "CraftingCampfireRecipesScript";
    script.mScript.scriptSource = "scn CraftingCampfireRecipesScript\n"
                                  "Ref User\n"
                                  "Begin OnActivate\n"
                                  "Set User to GetActionRef\n"
                                  "If GetActionRef != Player\n"
                                  "User.Activate\n"
                                  "Elseif GetActionRef == Player\n"
                                  "player.showrecipemenu CampfireRecipes\n"
                                  "Endif\n"
                                  "End\n";
    ESM4::RecipeCategory category = makeCategory(categoryId);
    category.mEditorId = "CampfireRecipes";
    store.overrideRecord(station);
    store.overrideRecord(script);
    store.overrideRecord(category);

    EXPECT_EQ(MWWorld::getFnvCraftingStationCategory(store, station), ESM::FormId::fromUint32(categoryId));

    script.mScript.scriptSource = "scn VCG03CraftingCampfireRecipesScript\n"
                                  "Ref User\n"
                                  "Begin OnActivate\n"
                                  "if GetQuestRunning VCG03\n"
                                  "else\n"
                                  "Set User to GetActionRef\n"
                                  "If GetActionRef != Player\n"
                                  "User.Activate\n"
                                  "Elseif GetActionRef == Player\n"
                                  "player.showrecipemenu CampfireRecipes\n"
                                  "Endif\n"
                                  "endif\n"
                                  "End\n";
    store.overrideRecord(script);
    EXPECT_FALSE(MWWorld::getFnvCraftingStationCategory(store, station))
        << "quest-gated recipe-menu scripts must remain fail-closed";
}

TEST(FnvCraftingRuntimeTest, CatalogRetainsEveryLiveMatchingRecipeWithExplicitStaticBlockers)
{
    CraftingFixture fixture;
    ESM4::Recipe conditional = makeRecipe(sWorkbenchCategory, sRecipe + 1);
    conditional.mConditions.emplace_back();
    fixture.mStore.overrideRecord(conditional);
    ESM4::Recipe currency = makeRecipe(sWorkbenchCategory, sRecipe + 2);
    currency.mIngredients[0].mItem = ESM::FormId::fromUint32(sCoinShotCurrency);
    fixture.mStore.overrideRecord(currency);

    MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::MissingRecipe;
    std::optional<MWWorld::PreparedFnvCraftingCatalog> catalog = MWWorld::prepareFnvCraftingCatalog(
        { MWWorld::ESM4Game::FalloutNewVegas, &fixture.mStore, fixture.mStation }, &error);

    ASSERT_TRUE(catalog);
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::None);
    EXPECT_EQ(catalog->getStation(), ESM::FormId::fromUint32(sWorkbench));
    EXPECT_EQ(catalog->getCategory(), ESM::FormId::fromUint32(sWorkbenchCategory));
    EXPECT_EQ(catalog->getEntries().size(), 3u);
    EXPECT_EQ(fixture.mInventory.mPrepareCalls, 0);

    const auto findEntry = [&](std::uint32_t id) -> const MWWorld::PreparedFnvCraftingCatalogEntry* {
        for (const auto& entry : catalog->getEntries())
        {
            if (entry.getRecipe() == ESM::FormId::fromUint32(id))
                return &entry;
        }
        return nullptr;
    };
    const auto* supported = findEntry(sRecipe);
    const auto* blockedConditional = findEntry(sRecipe + 1);
    const auto* blockedCurrency = findEntry(sRecipe + 2);
    ASSERT_NE(supported, nullptr);
    ASSERT_NE(blockedConditional, nullptr);
    ASSERT_NE(blockedCurrency, nullptr);
    EXPECT_TRUE(supported->isStaticallySupported());
    EXPECT_EQ(blockedConditional->getStaticBlocker(), MWWorld::FnvCraftingPreparationError::ConditionalRecipe);
    EXPECT_EQ(blockedCurrency->getStaticBlocker(), MWWorld::FnvCraftingPreparationError::UnsupportedCurrency);
    ASSERT_EQ(supported->getIngredients().size(), 2u);
    EXPECT_EQ(supported->getIngredients()[0].getDelta().mQuantity, 2);
    EXPECT_FALSE(supported->getIngredients()[0].getName().empty());
    EXPECT_FALSE(supported->getSubCategoryName().empty());
}

TEST(FnvCraftingRuntimeTest, CatalogRejectsDetachedDeletedAndIncompleteStationSources)
{
    CraftingFixture fixture;
    MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::None;
    EXPECT_FALSE(
        MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::Fallout3, &fixture.mStore, fixture.mStation }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::NotFalloutNewVegas);

    EXPECT_FALSE(
        MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, nullptr, fixture.mStation }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::MissingStore);

    EXPECT_FALSE(
        MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, &fixture.mStore, nullptr }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::MissingStation);

    ESM4::Activator detached = *fixture.mStation;
    EXPECT_FALSE(
        MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, &fixture.mStore, &detached }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::StationNotInStore);

    ESM4::Activator deleted = *fixture.mStation;
    deleted.mFlags |= ESM4::Rec_Deleted;
    fixture.mStation = fixture.mStore.overrideRecord(deleted);
    EXPECT_FALSE(MWWorld::prepareFnvCraftingCatalog(
        { MWWorld::ESM4Game::FalloutNewVegas, &fixture.mStore, fixture.mStation }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::DeletedRecord);

    MWWorld::ESMStore missingCategoryStore;
    const ESM4::Activator* stationWithoutCategory = missingCategoryStore.overrideRecord(makeStation());
    EXPECT_FALSE(MWWorld::prepareFnvCraftingCatalog(
        { MWWorld::ESM4Game::FalloutNewVegas, &missingCategoryStore, stationWithoutCategory }, &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::CategoryNotInStore);

    CraftingFixture deletedCategoryFixture;
    ESM4::RecipeCategory deletedCategory = *deletedCategoryFixture.mCategory;
    deletedCategory.mFlags |= ESM4::Rec_Deleted;
    deletedCategoryFixture.mStore.overrideRecord(deletedCategory);
    EXPECT_FALSE(MWWorld::prepareFnvCraftingCatalog(
        { MWWorld::ESM4Game::FalloutNewVegas, &deletedCategoryFixture.mStore, deletedCategoryFixture.mStation },
        &error));
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::DeletedRecord);
}

TEST(FnvCraftingRuntimeTest, FrozenWorkbenchAndReloadingCatalogAnchorsStayCompleteAndFailClosed)
{
    constexpr std::uint32_t coinShotRecipe = 0x00165e7d;
    constexpr std::array<std::uint32_t, 6> blockedFunctions{
        ESM4::FUN_GetItemCount,
        ESM4::FUN_GetStage,
        ESM4::FUN_GetDeadCount,
        ESM4::FUN_GetHasNote,
        ESM4::FUN_HasPerk,
        ESM4::FUN_GetMapMarkerVisible,
    };

    MWWorld::ESMStore store;
    store.overrideRecord(makeStation());
    store.overrideRecord(makeStation(sReloadingBench, sReloadingBenchScript));
    store.overrideRecord(makeCategory(sWorkbenchCategory));
    store.overrideRecord(makeCategory(sReloadingBenchCategory));
    store.overrideRecord(makeCategory(sSubCategory));
    store.overrideRecord(makeItem<ESM4::MiscItem>(sMisc));
    store.overrideRecord(makeItem<ESM4::Weapon>(sWeapon));

    for (std::size_t index = 0; index < 96; ++index)
    {
        ESM4::Recipe recipe = makeRecipe(sWorkbenchCategory, 0x02010000 + static_cast<std::uint32_t>(index));
        recipe.mData.mRequiredSkill = -1;
        recipe.mData.mRequiredSkillLevel = 0;
        recipe.mIngredients = { { ESM::FormId::fromUint32(sMisc), 1 } };
        recipe.mOutputs = { { ESM::FormId::fromUint32(sWeapon), 1 } };
        if (index < 59)
        {
            ESM4::TargetCondition condition;
            condition.functionIndex = blockedFunctions[index % blockedFunctions.size()];
            recipe.mConditions.push_back(condition);
        }
        store.overrideRecord(recipe);
    }
    for (std::size_t index = 0; index < 63; ++index)
    {
        const std::uint32_t id = index == 26 ? coinShotRecipe : 0x02020000 + static_cast<std::uint32_t>(index);
        ESM4::Recipe recipe = makeRecipe(sReloadingBenchCategory, id);
        recipe.mData.mRequiredSkill = -1;
        recipe.mData.mRequiredSkillLevel = 0;
        recipe.mIngredients = { { ESM::FormId::fromUint32(sMisc), 1 } };
        recipe.mOutputs = { { ESM::FormId::fromUint32(sWeapon), 1 } };
        if (index < 26)
        {
            ESM4::TargetCondition condition;
            condition.functionIndex = blockedFunctions[index % blockedFunctions.size()];
            recipe.mConditions.push_back(condition);
        }
        else if (index == 26)
            recipe.mIngredients[0].mItem = ESM::FormId::fromUint32(sCoinShotCurrency);
        store.overrideRecord(recipe);
    }

    const ESM4::Activator* workbench = store.get<ESM4::Activator>().search(refId(sWorkbench));
    const ESM4::Activator* reloadingBench = store.get<ESM4::Activator>().search(refId(sReloadingBench));
    ASSERT_NE(workbench, nullptr);
    ASSERT_NE(reloadingBench, nullptr);
    std::optional<MWWorld::PreparedFnvCraftingCatalog> workbenchCatalog
        = MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, &store, workbench });
    std::optional<MWWorld::PreparedFnvCraftingCatalog> reloadingCatalog
        = MWWorld::prepareFnvCraftingCatalog({ MWWorld::ESM4Game::FalloutNewVegas, &store, reloadingBench });
    ASSERT_TRUE(workbenchCatalog);
    ASSERT_TRUE(reloadingCatalog);
    ASSERT_EQ(workbenchCatalog->getEntries().size(), 96u);
    ASSERT_EQ(reloadingCatalog->getEntries().size(), 63u);

    const auto blockerCount
        = [](const MWWorld::PreparedFnvCraftingCatalog& catalog, MWWorld::FnvCraftingPreparationError blocker) {
              std::size_t result = 0;
              for (const auto& entry : catalog.getEntries())
              {
                  if (entry.getStaticBlocker() == blocker)
                      ++result;
              }
              return result;
          };
    EXPECT_EQ(blockerCount(*workbenchCatalog, MWWorld::FnvCraftingPreparationError::ConditionalRecipe), 59u);
    EXPECT_EQ(blockerCount(*workbenchCatalog, MWWorld::FnvCraftingPreparationError::None), 37u);
    EXPECT_EQ(blockerCount(*reloadingCatalog, MWWorld::FnvCraftingPreparationError::ConditionalRecipe), 26u);
    EXPECT_EQ(blockerCount(*reloadingCatalog, MWWorld::FnvCraftingPreparationError::UnsupportedCurrency), 1u);
    EXPECT_EQ(blockerCount(*reloadingCatalog, MWWorld::FnvCraftingPreparationError::None), 36u);

    for (std::size_t index = 0; index < blockedFunctions.size(); ++index)
    {
        const ESM::FormId recipe = ESM::FormId::fromUint32(0x02010000 + static_cast<std::uint32_t>(index));
        bool found = false;
        for (const auto& entry : workbenchCatalog->getEntries())
        {
            if (entry.getRecipe() == recipe)
            {
                found = true;
                EXPECT_EQ(entry.getStaticBlocker(), MWWorld::FnvCraftingPreparationError::ConditionalRecipe);
            }
        }
        EXPECT_TRUE(found) << "missing CTDA blocker recipe for function " << blockedFunctions[index];
    }

    bool foundCoinShot = false;
    for (const auto& entry : reloadingCatalog->getEntries())
    {
        if (entry.getRecipe() == ESM::FormId::fromUint32(coinShotRecipe))
        {
            foundCoinShot = true;
            EXPECT_EQ(entry.getStaticBlocker(), MWWorld::FnvCraftingPreparationError::UnsupportedCurrency);
        }
    }
    EXPECT_TRUE(foundCoinShot);
}

TEST(FnvCraftingRuntimeTest, PreflightsAndAppliesAllSupportedTypesInRemoveThenAddOrder)
{
    CraftingFixture fixture;
    MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::MissingItem;
    std::optional<MWWorld::PreparedFnvCraftingPlan> plan
        = MWWorld::prepareFnvCraftingTransaction(fixture.source(), &error);

    ASSERT_TRUE(plan);
    EXPECT_EQ(error, MWWorld::FnvCraftingPreparationError::None);
    EXPECT_EQ(plan->getStation(), ESM::FormId::fromUint32(sWorkbench));
    EXPECT_EQ(plan->getCategory(), ESM::FormId::fromUint32(sWorkbenchCategory));
    EXPECT_EQ(plan->getRecipe(), ESM::FormId::fromUint32(sRecipe));
    ASSERT_EQ(plan->getIngredients().size(), 2u);
    ASSERT_EQ(plan->getOutputs().size(), 3u);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());

    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::Applied);
    ASSERT_EQ(fixture.mInventory.mCalls.size(), 5u);
    EXPECT_EQ(fixture.mInventory.mCalls[0].mOperation, "remove");
    EXPECT_EQ(fixture.mInventory.mCalls[1].mOperation, "remove");
    EXPECT_EQ(fixture.mInventory.mCalls[2].mOperation, "add");
    EXPECT_EQ(fixture.mInventory.mCalls[3].mOperation, "add");
    EXPECT_EQ(fixture.mInventory.mCalls[4].mOperation, "add");
    for (std::size_t index = 2; index < fixture.mInventory.mCalls.size(); ++index)
        EXPECT_FALSE(fixture.mInventory.mCalls[index].mAllowAutoEquip);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sMisc)], 3);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sAmmo)], 0);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sWeapon)], 1);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sPotion)], 2);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sArmor)], 1);

    // Commit consumes the plan even when the caller retains the moved-from wrapper.
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::InvalidPlan);
}

TEST(FnvCraftingRuntimeTest, OverlapRecipeConsumesBeforeItCreatesTheOutput)
{
    CraftingFixture fixture;
    fixture.mutateRecipe([](ESM4::Recipe& recipe) {
        recipe.mIngredients = { { ESM::FormId::fromUint32(sMisc), 3 } };
        recipe.mOutputs = { { ESM::FormId::fromUint32(sMisc), 2 } };
    });

    std::optional<MWWorld::PreparedFnvCraftingPlan> plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sMisc)], 5);
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::Applied);
    ASSERT_EQ(fixture.mInventory.mCalls.size(), 2u);
    EXPECT_EQ(fixture.mInventory.mCalls[0].mOperation, "remove");
    EXPECT_EQ(fixture.mInventory.mCalls[0].mCount, 3);
    EXPECT_EQ(fixture.mInventory.mCalls[1].mOperation, "add");
    EXPECT_EQ(fixture.mInventory.mCalls[1].mCount, 2);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(sMisc)], 4);
}

TEST(FnvCraftingRuntimeTest, AcceptsOnlyTheTwoAuditedStationScriptCategoryMappings)
{
    CraftingFixture fixture;

    ESM4::Activator wrongScript = *fixture.mStation;
    wrongScript.mScriptId = ESM::FormId::fromUint32(0x0013e11d);
    fixture.mStation = fixture.mStore.overrideRecord(wrongScript);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedStation);

    fixture.mStation = fixture.mStore.overrideRecord(makeStation(sReloadingBench, sReloadingBenchScript));
    fixture.mCategory = fixture.mStore.overrideRecord(makeCategory(sReloadingBenchCategory));
    fixture.replaceRecipe(makeRecipe(sReloadingBenchCategory));
    EXPECT_TRUE(MWWorld::prepareFnvCraftingTransaction(fixture.source()));

    fixture.mCategory = fixture.mStore.overrideRecord(makeCategory(sWorkbenchCategory));
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::StationCategoryMismatch);
}

TEST(FnvCraftingRuntimeTest, RejectsEveryMissingOrUnresolvedTransactionSource)
{
    CraftingFixture fixture;
    MWWorld::FnvCraftingTransactionSource source = fixture.source();
    source.mStore = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingStore);

    source = fixture.source();
    source.mStation = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingStation);

    source = fixture.source();
    ESM4::Activator detachedStation = *fixture.mStation;
    source.mStation = &detachedStation;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::StationNotInStore);

    source = fixture.source();
    source.mStationCategory = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingStationCategory);

    source = fixture.source();
    source.mRecipe = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingRecipe);

    source = fixture.source();
    source.mInventory = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingInventory);

    source = fixture.source();
    source.mGame = MWWorld::ESM4Game::Fallout3;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::NotFalloutNewVegas);
}

TEST(FnvCraftingRuntimeTest, RejectsConditionalRecipesAndTreatsSubCategoryOnlyAsARequiredLiveGrouping)
{
    CraftingFixture fixture;
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mConditions.emplace_back(); });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::ConditionalRecipe);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mData.mSubCategory = {}; });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::MissingSubCategory);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mData.mSubCategory = ESM::FormId::fromUint32(0x0100ffff); });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::SubCategoryNotInStore);
}

TEST(FnvCraftingRuntimeTest, RequiresTheExactPlayerActorInventoryAndInitializedNativeState)
{
    CraftingFixture fixture;
    MWWorld::FnvCraftingTransactionSource source = fixture.source();
    source.mActor = {};
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingActor);

    source = fixture.source();
    source.mActor = fixture.mOther.mPtr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::ActorIsNotPlayer);

    source = fixture.source();
    fixture.mInventory.mOwner = fixture.mOther.mPtr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::InventoryMismatch);
    fixture.mInventory.mOwner = fixture.mPlayer.mPtr;

    MWWorld::FalloutPlayerRuntimeState uninitialized;
    source = fixture.source();
    source.mPlayerState = &uninitialized;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::PlayerStateUninitialized);

    MWWorld::FalloutPlayerRuntimeState wrongPlayerState;
    MWWorld::FalloutPlayerState wrongPlayerBase = makePlayerBase();
    wrongPlayerBase.mBaseRecord = ESM::FormId::fromUint32(0x00000008);
    wrongPlayerState.initialize(wrongPlayerBase);
    source = fixture.source();
    source.mPlayerState = &wrongPlayerState;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::PlayerStateMismatch);
}

TEST(FnvCraftingRuntimeTest, SkillGateRejectsUnsupportedLowAndUnresolvedOffsetValues)
{
    CraftingFixture fixture;
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mData.mRequiredSkill = 16; });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedSkill);

    fixture.replaceRecipe(makeRecipe());
    fixture.mPlayerState.setCurrentActorValue(39, 39.f);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::InsufficientSkill);

    fixture.mPlayerState.initialize(makePlayerBase(1));
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnresolvedSkillOffset);

    fixture.mPlayerState.initialize(makePlayerBase());
    fixture.mutateRecipe([](ESM4::Recipe& recipe) {
        recipe.mData.mRequiredSkill = -1;
        recipe.mData.mRequiredSkillLevel = 1;
    });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::InvalidNoSkillGate);
}

TEST(FnvCraftingRuntimeTest, RejectsNullMissingUnsupportedCurrencyAndScriptedItemRecords)
{
    CraftingFixture fixture;
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mIngredients[0].mItem = {}; });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::MissingItem);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe(
        [](ESM4::Recipe& recipe) { recipe.mIngredients[0].mItem = ESM::FormId::fromUint32(0x0100ffff); });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::MissingItem);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mIngredients[0].mItem = ESM::FormId::fromUint32(sBook); });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedItemType);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe(
        [](ESM4::Recipe& recipe) { recipe.mIngredients[0].mItem = ESM::FormId::fromUint32(sCoinShotCurrency); });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedCurrency);

    fixture.replaceRecipe(makeRecipe());
    ESM4::MiscItem scripted = makeItem<ESM4::MiscItem>(sMisc);
    scripted.mScriptId = ESM::FormId::fromUint32(0x0102f000);
    fixture.mStore.overrideRecord(scripted);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::ScriptedItem);
}

TEST(FnvCraftingRuntimeTest, DecodesQuantitiesAsSignedAndRejectsEveryOverflowBeforeMutation)
{
    CraftingFixture fixture;
    fixture.mutateRecipe([](ESM4::Recipe& recipe) { recipe.mIngredients[0].mQuantity = 0xffffffffu; });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::InvalidQuantity);

    fixture.replaceRecipe(makeRecipe());
    fixture.mutateRecipe([](ESM4::Recipe& recipe) {
        recipe.mIngredients = {
            { ESM::FormId::fromUint32(sMisc), static_cast<std::uint32_t>(std::numeric_limits<int>::max()) },
            { ESM::FormId::fromUint32(sMisc), 1 },
        };
    });
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::QuantityOverflow);

    fixture.replaceRecipe(makeRecipe());
    fixture.mInventory.mCounts[refId(sWeapon)] = std::numeric_limits<int>::max();
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::QuantityOverflow);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());
}

TEST(FnvCraftingRuntimeTest, RejectsInvalidOrInsufficientInventoryBeforeConstructingAMutation)
{
    CraftingFixture fixture;
    fixture.mInventory.mCounts[refId(sMisc)] = 1;
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::InsufficientIngredients);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());

    fixture.mInventory.mCounts[refId(sMisc)] = 5;
    fixture.mInventory.mCountInvalid = true;
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::InvalidInventory);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());
}

TEST(FnvCraftingRuntimeTest, InventoryDriftOrActorChangeConsumesPlanWithoutMutating)
{
    CraftingFixture fixture;
    std::optional<MWWorld::PreparedFnvCraftingPlan> plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    fixture.mInventory.mCounts[refId(sMisc)] += 1;
    EXPECT_EQ(
        MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::InventoryChanged);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());

    fixture.mInventory.mCounts[refId(sMisc)] -= 1;
    plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    fixture.mInventory.mOwner = fixture.mOther.mPtr;
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)),
        MWWorld::FnvCraftingCommitResult::ActorOrInventoryChanged);
    EXPECT_TRUE(fixture.mInventory.mCalls.empty());
}

TEST(FnvCraftingRuntimeTest, ErrorNamesAreStableForHeadlessDiagnostics)
{
    EXPECT_EQ(MWWorld::getFnvCraftingPreparationErrorName(MWWorld::FnvCraftingPreparationError::UnsupportedCurrency),
        "unsupported-currency");
    EXPECT_EQ(MWWorld::getFnvCraftingPreparationErrorName(MWWorld::FnvCraftingPreparationError::UnresolvedSkillOffset),
        "unresolved-skill-offset");
    EXPECT_EQ(MWWorld::getFnvCraftingCommitResultName(MWWorld::FnvCraftingCommitResult::Applied), "applied");
}
