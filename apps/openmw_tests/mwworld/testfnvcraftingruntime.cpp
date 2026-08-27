#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/loadweap.hpp>

#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvcraftingruntime.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"

namespace
{
    constexpr std::uint32_t kWorkbench = 0x01010000;
    constexpr std::uint32_t kWorkbenchScript = 0x01010001;
    constexpr std::uint32_t kWorkbenchCategory = 0x01010002;
    constexpr std::uint32_t kSubCategory = 0x01010003;
    constexpr std::uint32_t kRecipe = 0x01010004;
    constexpr std::uint32_t kIngredient = 0x01010005;
    constexpr std::uint32_t kOutput = 0x01010006;
    constexpr std::uint32_t kOther = 0x01010007;
    constexpr std::uint32_t kRepairSkill = 39;

    ESM::RefId refId(std::uint32_t value)
    {
        return ESM::RefId(ESM::FormId::fromUint32(value));
    }

    template <class Record>
    Record makeItem(std::uint32_t id)
    {
        Record result{};
        result.mId = ESM::FormId::fromUint32(id);
        return result;
    }

    ESM4::Activator makeStation()
    {
        ESM4::Activator result{};
        result.mId = ESM::FormId::fromUint32(kWorkbench);
        result.mScriptId = ESM::FormId::fromUint32(kWorkbenchScript);
        return result;
    }

    ESM4::RecipeCategory makeCategory(std::uint32_t id)
    {
        ESM4::RecipeCategory result{};
        result.mId = ESM::FormId::fromUint32(id);
        return result;
    }

    ESM4::Recipe makeRecipe()
    {
        ESM4::Recipe result{};
        result.mId = ESM::FormId::fromUint32(kRecipe);
        result.mData.mRequiredSkill = kRepairSkill;
        result.mData.mRequiredSkillLevel = 40;
        result.mData.mCategory = ESM::FormId::fromUint32(kWorkbenchCategory);
        result.mData.mSubCategory = ESM::FormId::fromUint32(kSubCategory);
        result.mIngredients = {
            { ESM::FormId::fromUint32(kIngredient), 2 },
            { ESM::FormId::fromUint32(kIngredient), 1 },
        };
        result.mOutputs = { { ESM::FormId::fromUint32(kOutput), 2 } };
        return result;
    }

    struct ActorHandle
    {
        ESM4::MiscItem mBase;
        ESM::CellRef mCellRef;
        MWWorld::LiveCellRef<ESM4::MiscItem> mLive;
        MWWorld::Ptr mPtr;

        explicit ActorHandle(std::uint32_t id)
            : mBase(makeItem<ESM4::MiscItem>(id))
            , mCellRef(ESM::makeBlankCellRef())
            , mLive(mCellRef, &mBase)
            , mPtr(&mLive)
        {
            mCellRef.mRefID = refId(id);
        }
    };

    class FakeSkillProvider final : public MWWorld::FnvCraftingSkillProvider
    {
    public:
        std::map<std::uint32_t, double> mValues;

        std::optional<double> getSkill(std::uint32_t actorValue) const noexcept override
        {
            const auto found = mValues.find(actorValue);
            return found == mValues.end() ? std::nullopt : std::optional<double>(found->second);
        }
    };

    class FakeInventory final : public MWWorld::FnvCraftingInventory
    {
    public:
        MWWorld::Ptr mOwner;
        std::map<ESM::RefId, std::int64_t> mCounts;
        bool mApplySucceeds = true;
        int mApplyCalls = 0;

        explicit FakeInventory(const MWWorld::Ptr& owner)
            : mOwner(owner)
        {
        }

        bool prepareForActor(const MWWorld::Ptr& actor) noexcept override
        {
            return !actor.isEmpty() && actor == mOwner;
        }

        bool stillBelongsTo(const MWWorld::Ptr& actor) const noexcept override
        {
            return !actor.isEmpty() && actor == mOwner;
        }

        std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept override
        {
            const auto found = mCounts.find(item);
            return found == mCounts.end() ? std::optional<std::int64_t>(0) : found->second;
        }

        bool apply(std::span<const MWWorld::FnvCraftingItemDelta> ingredients,
            std::span<const MWWorld::FnvCraftingItemDelta> outputs) noexcept override
        {
            ++mApplyCalls;
            if (!mApplySucceeds)
                return false;

            auto next = mCounts;
            for (const auto& ingredient : ingredients)
            {
                if (next[ingredient.mItem] < ingredient.mQuantity)
                    return false;
                next[ingredient.mItem] -= ingredient.mQuantity;
            }
            for (const auto& output : outputs)
                next[output.mItem] += output.mQuantity;
            mCounts = std::move(next);
            return true;
        }
    };

    struct CraftingFixture
    {
        MWWorld::ESMStore mStore;
        ActorHandle mPlayer{ 0x01020000 };
        ActorHandle mOther{ 0x01020001 };
        FakeInventory mInventory{ mPlayer.mPtr };
        FakeSkillProvider mSkills;
        std::vector<MWWorld::FnvCraftingStationRule> mRules;
        const ESM4::Activator* mStation;
        const ESM4::RecipeCategory* mCategory;
        const ESM4::Recipe* mRecipe;

        CraftingFixture()
        {
            static const bool classesRegistered = [] {
                MWClass::registerClasses();
                return true;
            }();
            static_cast<void>(classesRegistered);

            mStation = mStore.overrideRecord(makeStation());
            mCategory = mStore.overrideRecord(makeCategory(kWorkbenchCategory));
            mStore.overrideRecord(makeCategory(kSubCategory));
            mRecipe = mStore.overrideRecord(makeRecipe());
            mStore.overrideRecord(makeItem<ESM4::MiscItem>(kIngredient));
            mStore.overrideRecord(makeItem<ESM4::MiscItem>(kOutput));
            mSkills.mValues[kRepairSkill] = 50;
            mInventory.mCounts[refId(kIngredient)] = 5;
            mRules = { { ESM::FormId::fromUint32(kWorkbench), ESM::FormId::fromUint32(kWorkbenchScript),
                ESM::FormId::fromUint32(kWorkbenchCategory) } };
        }

        MWWorld::FnvCraftingTransactionSource source() const
        {
            return { MWWorld::ESM4Game::FalloutNewVegas, &mStore, mStation, mCategory, mRecipe, mRules,
                mPlayer.mPtr, mPlayer.mPtr, const_cast<FakeInventory*>(&mInventory), &mSkills };
        }
    };

    MWWorld::FnvCraftingPreparationError preparationError(const MWWorld::FnvCraftingTransactionSource& source)
    {
        MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::None;
        EXPECT_FALSE(MWWorld::prepareFnvCraftingTransaction(source, &error));
        return error;
    }
}

TEST(FnvCraftingRuntimeTest, PlansAndAppliesUsingInjectedStationAndSkillPolicies)
{
    CraftingFixture fixture;
    auto plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->getStation(), ESM::FormId::fromUint32(kWorkbench));
    EXPECT_EQ(plan->getCategory(), ESM::FormId::fromUint32(kWorkbenchCategory));
    EXPECT_EQ(plan->getRecipe(), ESM::FormId::fromUint32(kRecipe));
    ASSERT_EQ(plan->getIngredients().size(), 1u);
    EXPECT_EQ(plan->getIngredients()[0], (MWWorld::FnvCraftingItemDelta{ refId(kIngredient), 3 }));
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::Applied);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(kIngredient)], 2);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(kOutput)], 2);
}

TEST(FnvCraftingRuntimeTest, RequiresTheInjectedStationRuleAndSkillProvider)
{
    CraftingFixture fixture;
    fixture.mRules.clear();
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedStation);

    fixture.mRules = { { ESM::FormId::fromUint32(kWorkbench), ESM::FormId::fromUint32(kWorkbenchScript),
        ESM::FormId::fromUint32(kOther) } };
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::StationCategoryMismatch);

    fixture.mRules = { { ESM::FormId::fromUint32(kWorkbench), ESM::FormId::fromUint32(kWorkbenchScript),
        ESM::FormId::fromUint32(kWorkbenchCategory) } };
    fixture.mSkills.mValues.clear();
    auto source = fixture.source();
    source.mSkills = nullptr;
    EXPECT_EQ(preparationError(source), MWWorld::FnvCraftingPreparationError::MissingSkillProvider);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::UnsupportedSkill);
}

TEST(FnvCraftingRuntimeTest, RejectsUnsupportedAndConditionedRecipesBeforeMutation)
{
    CraftingFixture fixture;
    auto recipe = *fixture.mRecipe;
    recipe.mConditions.emplace_back();
    fixture.mRecipe = fixture.mStore.overrideRecord(recipe);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::ConditionalRecipe);
    EXPECT_EQ(fixture.mInventory.mApplyCalls, 0);

    recipe = *fixture.mRecipe;
    recipe.mConditions.clear();
    recipe.mIngredients[0].mItem = ESM::FormId{};
    fixture.mRecipe = fixture.mStore.overrideRecord(recipe);
    EXPECT_EQ(preparationError(fixture.source()), MWWorld::FnvCraftingPreparationError::MissingItem);
}

TEST(FnvCraftingRuntimeTest, RevalidatesSnapshotAndConsumesPlanExactlyOnce)
{
    CraftingFixture fixture;
    auto plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    fixture.mInventory.mCounts[refId(kIngredient)] = 4;
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::InventoryChanged);
    EXPECT_EQ(fixture.mInventory.mApplyCalls, 0);

    plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::Applied);
    EXPECT_EQ(fixture.mInventory.mApplyCalls, 1);
}

TEST(FnvCraftingRuntimeTest, MutationBoundaryIsAllOrNoneAndPlanCannotBeReused)
{
    CraftingFixture fixture;
    auto plan = MWWorld::prepareFnvCraftingTransaction(fixture.source());
    ASSERT_TRUE(plan);
    fixture.mInventory.mApplySucceeds = false;
    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::MutationFailed);
    EXPECT_EQ(fixture.mInventory.mCounts[refId(kIngredient)], 5);

    EXPECT_EQ(MWWorld::commitFnvCraftingTransaction(std::move(*plan)), MWWorld::FnvCraftingCommitResult::InvalidPlan);
}
