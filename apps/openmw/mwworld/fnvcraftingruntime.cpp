#include "fnvcraftingruntime.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <utility>

#include <components/esm/defs.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/loadweap.hpp>

#include "containerstore.hpp"
#include "esmstore.hpp"
#include "fnvplayerruntimestate.hpp"
#include "manualref.hpp"

namespace
{
    constexpr std::uint32_t sWorkbenchBase = 0x00075005;
    constexpr std::uint32_t sWorkbenchScript = 0x0013e11c;
    constexpr std::uint32_t sWorkbenchCategory = 0x0013b2c1;
    constexpr std::uint32_t sReloadingBenchBase = 0x0015361f;
    constexpr std::uint32_t sReloadingBenchScript = 0x0015361e;
    constexpr std::uint32_t sReloadingBenchCategory = 0x00153621;
    constexpr std::uint32_t sPlayerBase = 0x00000007;

    // Frozen blocker: FalloutNV.esm CMNY 00176AC4 is consumed by RCPE
    // 00165E7D (Recipe12GaCoinShot). CMNY has no typed runtime store in this
    // slice and is deliberately not projected onto a MISC object or caps.
    constexpr std::uint32_t sCoinShotCurrency = 0x00176ac4;

    struct StationMapping
    {
        ESM::FormId mCategory;
    };

    std::optional<StationMapping> getStationMapping(const ESM4::Activator& station)
    {
        if (station.mId == ESM::FormId::fromUint32(sWorkbenchBase)
            && station.mScriptId == ESM::FormId::fromUint32(sWorkbenchScript))
            return StationMapping{ ESM::FormId::fromUint32(sWorkbenchCategory) };
        if (station.mId == ESM::FormId::fromUint32(sReloadingBenchBase)
            && station.mScriptId == ESM::FormId::fromUint32(sReloadingBenchScript))
            return StationMapping{ ESM::FormId::fromUint32(sReloadingBenchCategory) };
        return std::nullopt;
    }

    bool isDeleted(std::uint32_t flags)
    {
        return (flags & ESM4::Rec_Deleted) != 0;
    }

    std::optional<int> decodeQuantity(std::uint32_t raw)
    {
        const std::int32_t exact = std::bit_cast<std::int32_t>(raw);
        if (exact <= 0)
            return std::nullopt;
        return exact;
    }

    template <class Record>
    const Record* findExactRecord(const MWWorld::ESMStore& store, ESM::FormId id)
    {
        if (id.isZeroOrUnset())
            return nullptr;
        return store.get<Record>().search(ESM::RefId(id));
    }

    enum class ItemValidation
    {
        Supported,
        Missing,
        Currency,
        Unsupported,
        Deleted,
        Scripted,
    };

    template <class Record, class ScriptMember>
    ItemValidation validateTypedItem(const MWWorld::ESMStore& store, const ESM::RefId& id, ScriptMember scriptMember)
    {
        const Record* record = store.get<Record>().search(id);
        if (record == nullptr)
            return ItemValidation::Missing;
        if (isDeleted(record->mFlags))
            return ItemValidation::Deleted;
        if (!(record->*scriptMember).isZeroOrUnset())
            return ItemValidation::Scripted;
        return ItemValidation::Supported;
    }

    ItemValidation validateItem(const MWWorld::ESMStore& store, ESM::FormId form)
    {
        if (form.isZeroOrUnset())
            return ItemValidation::Missing;

        const ESM::RefId id(form);
        if (form == ESM::FormId::fromUint32(sCoinShotCurrency) || store.find(id) == ESM::REC_CMNY4)
            return ItemValidation::Currency;

        switch (store.find(id))
        {
            case ESM::REC_MISC4:
                return validateTypedItem<ESM4::MiscItem>(store, id, &ESM4::MiscItem::mScriptId);
            case ESM::REC_ALCH4:
                return validateTypedItem<ESM4::Potion>(store, id, &ESM4::Potion::mScriptId);
            case ESM::REC_AMMO4:
                return validateTypedItem<ESM4::Ammunition>(store, id, &ESM4::Ammunition::mScript);
            case ESM::REC_WEAP4:
                return validateTypedItem<ESM4::Weapon>(store, id, &ESM4::Weapon::mScriptId);
            case ESM::REC_ARMO4:
                return validateTypedItem<ESM4::Armor>(store, id, &ESM4::Armor::mScriptId);
            case 0:
                return ItemValidation::Missing;
            default:
                return ItemValidation::Unsupported;
        }
    }

    std::optional<MWWorld::PreparedFnvCraftingPlan> fail(
        MWWorld::FnvCraftingPreparationError value, MWWorld::FnvCraftingPreparationError* output)
    {
        if (output != nullptr)
            *output = value;
        return std::nullopt;
    }

    MWWorld::FnvCraftingPreparationError itemError(ItemValidation value)
    {
        switch (value)
        {
            case ItemValidation::Missing:
                return MWWorld::FnvCraftingPreparationError::MissingItem;
            case ItemValidation::Currency:
                return MWWorld::FnvCraftingPreparationError::UnsupportedCurrency;
            case ItemValidation::Unsupported:
                return MWWorld::FnvCraftingPreparationError::UnsupportedItemType;
            case ItemValidation::Deleted:
                return MWWorld::FnvCraftingPreparationError::DeletedRecord;
            case ItemValidation::Scripted:
                return MWWorld::FnvCraftingPreparationError::ScriptedItem;
            case ItemValidation::Supported:
                break;
        }
        return MWWorld::FnvCraftingPreparationError::None;
    }

    bool checkedAdd(std::map<ESM::RefId, std::int64_t>& totals, const ESM::RefId& id, int value)
    {
        std::int64_t& total = totals[id];
        if (value <= 0 || total > std::numeric_limits<int>::max() - static_cast<std::int64_t>(value))
            return false;
        total += value;
        return true;
    }
}

namespace MWWorld
{
    struct PreparedFnvCraftingPlan::Impl
    {
        struct Output
        {
            FnvCraftingItemDelta mDelta;
            std::unique_ptr<ManualRef> mConstructed;
        };

        ESM::FormId mStation;
        ESM::FormId mCategory;
        ESM::FormId mRecipe;
        Ptr mActor;
        FnvCraftingInventory* mInventory = nullptr;
        std::vector<FnvCraftingItemDelta> mIngredients;
        std::vector<FnvCraftingItemDelta> mPublicOutputs;
        std::vector<Output> mOutputs;
        std::map<ESM::RefId, std::int64_t> mSnapshot;
    };

    class FnvCraftingPlanBuilder
    {
    public:
        static PreparedFnvCraftingPlan make(std::unique_ptr<PreparedFnvCraftingPlan::Impl> impl)
        {
            return PreparedFnvCraftingPlan(std::move(impl));
        }
    };

    ContainerStoreCraftingInventory::ContainerStoreCraftingInventory(ContainerStore& store)
        : mStore(&store)
    {
    }

    bool ContainerStoreCraftingInventory::prepareForActor(const Ptr& actor) noexcept
    {
        try
        {
            if (actor.isEmpty() || mStore == nullptr || mStore->getPtr() != actor)
                return false;
            mStore->resolve();
            return mStore->getPtr() == actor;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ContainerStoreCraftingInventory::stillBelongsTo(const Ptr& actor) const noexcept
    {
        try
        {
            return !actor.isEmpty() && mStore != nullptr && mStore->getPtr() == actor;
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<std::int64_t> ContainerStoreCraftingInventory::getCount(const ESM::RefId& item) const noexcept
    {
        try
        {
            std::int64_t result = 0;
            for (const ConstPtr candidate : *mStore)
            {
                if (candidate.getCellRef().getRefId() != item)
                    continue;
                const int count = candidate.getCellRef().getCount(false);
                if (count < 0 || result > std::numeric_limits<int>::max() - static_cast<std::int64_t>(count))
                    return std::nullopt;
                result += count;
            }
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    bool ContainerStoreCraftingInventory::remove(const ESM::RefId& item, int count) noexcept
    {
        try
        {
            return count > 0 && mStore->remove(item, count, false, false) == count;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ContainerStoreCraftingInventory::add(const Ptr& item, int count, bool allowAutoEquip) noexcept
    {
        try
        {
            return count > 0 && mStore->add(item, count, allowAutoEquip, false) != mStore->end();
        }
        catch (...)
        {
            return false;
        }
    }

    PreparedFnvCraftingPlan::PreparedFnvCraftingPlan(std::unique_ptr<Impl> impl)
        : mImpl(std::move(impl))
    {
    }

    PreparedFnvCraftingPlan::~PreparedFnvCraftingPlan() = default;
    PreparedFnvCraftingPlan::PreparedFnvCraftingPlan(PreparedFnvCraftingPlan&&) noexcept = default;
    PreparedFnvCraftingPlan& PreparedFnvCraftingPlan::operator=(PreparedFnvCraftingPlan&&) noexcept = default;

    ESM::FormId PreparedFnvCraftingPlan::getStation() const
    {
        return mImpl != nullptr ? mImpl->mStation : ESM::FormId{};
    }

    ESM::FormId PreparedFnvCraftingPlan::getCategory() const
    {
        return mImpl != nullptr ? mImpl->mCategory : ESM::FormId{};
    }

    ESM::FormId PreparedFnvCraftingPlan::getRecipe() const
    {
        return mImpl != nullptr ? mImpl->mRecipe : ESM::FormId{};
    }

    const std::vector<FnvCraftingItemDelta>& PreparedFnvCraftingPlan::getIngredients() const
    {
        static const std::vector<FnvCraftingItemDelta> sEmpty;
        return mImpl != nullptr ? mImpl->mIngredients : sEmpty;
    }

    const std::vector<FnvCraftingItemDelta>& PreparedFnvCraftingPlan::getOutputs() const
    {
        static const std::vector<FnvCraftingItemDelta> sEmpty;
        return mImpl != nullptr ? mImpl->mPublicOutputs : sEmpty;
    }

    std::optional<PreparedFnvCraftingPlan> prepareFnvCraftingTransaction(
        const FnvCraftingTransactionSource& source, FnvCraftingPreparationError* error)
    {
        if (error != nullptr)
            *error = FnvCraftingPreparationError::None;

        if (source.mGame != ESM4Game::FalloutNewVegas)
            return fail(FnvCraftingPreparationError::NotFalloutNewVegas, error);
        if (source.mStore == nullptr)
            return fail(FnvCraftingPreparationError::MissingStore, error);
        const ESMStore& store = *source.mStore;

        if (source.mStation == nullptr || source.mStation->mId.isZeroOrUnset())
            return fail(FnvCraftingPreparationError::MissingStation, error);
        const ESM4::Activator* storedStation = findExactRecord<ESM4::Activator>(store, source.mStation->mId);
        if (storedStation == nullptr || storedStation != source.mStation)
            return fail(FnvCraftingPreparationError::StationNotInStore, error);
        if (isDeleted(source.mStation->mFlags))
            return fail(FnvCraftingPreparationError::DeletedRecord, error);
        const std::optional<StationMapping> stationMapping = getStationMapping(*source.mStation);
        if (!stationMapping)
            return fail(FnvCraftingPreparationError::UnsupportedStation, error);

        if (source.mStationCategory == nullptr || source.mStationCategory->mId.isZeroOrUnset())
            return fail(FnvCraftingPreparationError::MissingStationCategory, error);
        const ESM4::RecipeCategory* storedCategory
            = findExactRecord<ESM4::RecipeCategory>(store, source.mStationCategory->mId);
        if (storedCategory == nullptr || storedCategory != source.mStationCategory)
            return fail(FnvCraftingPreparationError::CategoryNotInStore, error);
        if (isDeleted(source.mStationCategory->mFlags))
            return fail(FnvCraftingPreparationError::DeletedRecord, error);
        if (source.mStationCategory->mId != stationMapping->mCategory)
            return fail(FnvCraftingPreparationError::StationCategoryMismatch, error);

        if (source.mRecipe == nullptr || source.mRecipe->mId.isZeroOrUnset())
            return fail(FnvCraftingPreparationError::MissingRecipe, error);
        const ESM4::Recipe* storedRecipe = findExactRecord<ESM4::Recipe>(store, source.mRecipe->mId);
        if (storedRecipe == nullptr || storedRecipe != source.mRecipe)
            return fail(FnvCraftingPreparationError::RecipeNotInStore, error);
        const ESM4::Recipe& recipe = *source.mRecipe;
        if (isDeleted(recipe.mFlags))
            return fail(FnvCraftingPreparationError::DeletedRecord, error);
        if (!recipe.mConditions.empty())
            return fail(FnvCraftingPreparationError::ConditionalRecipe, error);
        if (recipe.mData.mCategory.isZeroOrUnset() || recipe.mData.mCategory != source.mStationCategory->mId)
            return fail(FnvCraftingPreparationError::RecipeCategoryMismatch, error);
        if (recipe.mData.mSubCategory.isZeroOrUnset())
            return fail(FnvCraftingPreparationError::MissingSubCategory, error);
        const ESM4::RecipeCategory* subCategory
            = findExactRecord<ESM4::RecipeCategory>(store, recipe.mData.mSubCategory);
        if (subCategory == nullptr)
            return fail(FnvCraftingPreparationError::SubCategoryNotInStore, error);
        if (isDeleted(subCategory->mFlags))
            return fail(FnvCraftingPreparationError::DeletedRecord, error);

        if (source.mActor.isEmpty() || source.mPlayer.isEmpty())
            return fail(FnvCraftingPreparationError::MissingActor, error);
        if (source.mActor != source.mPlayer)
            return fail(FnvCraftingPreparationError::ActorIsNotPlayer, error);
        if (source.mInventory == nullptr)
            return fail(FnvCraftingPreparationError::MissingInventory, error);
        if (!source.mInventory->prepareForActor(source.mActor))
            return fail(FnvCraftingPreparationError::InventoryMismatch, error);
        if (source.mPlayerState == nullptr || !source.mPlayerState->isInitialized())
            return fail(FnvCraftingPreparationError::PlayerStateUninitialized, error);
        if (source.mPlayerState->getBaseState()->mBaseRecord != ESM::FormId::fromUint32(sPlayerBase))
            return fail(FnvCraftingPreparationError::PlayerStateMismatch, error);

        if (recipe.mData.mRequiredSkill == -1)
        {
            if (recipe.mData.mRequiredSkillLevel != 0)
                return fail(FnvCraftingPreparationError::InvalidNoSkillGate, error);
        }
        else
        {
            if (recipe.mData.mRequiredSkill < 0)
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            const std::uint32_t actorValue = static_cast<std::uint32_t>(recipe.mData.mRequiredSkill);
            if (actorValue < FalloutPlayerRuntimeState::SkillActorValueBegin
                || actorValue > FalloutPlayerRuntimeState::SkillActorValueEnd)
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            const std::optional<FalloutRuntimeActorValue> skill = source.mPlayerState->getCurrentActorValue(actorValue);
            if (!skill || !std::isfinite(skill->mValue))
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            if (skill->mRawSkillOffset && *skill->mRawSkillOffset != 0)
                return fail(FnvCraftingPreparationError::UnresolvedSkillOffset, error);
            if (static_cast<double>(skill->mValue) < static_cast<double>(recipe.mData.mRequiredSkillLevel))
                return fail(FnvCraftingPreparationError::InsufficientSkill, error);
        }

        if (recipe.mIngredients.empty() || recipe.mOutputs.empty())
            return fail(FnvCraftingPreparationError::MissingItem, error);

        std::map<ESM::RefId, std::int64_t> ingredientTotals;
        std::map<ESM::RefId, std::int64_t> outputTotals;
        std::vector<FnvCraftingItemDelta> authoredOutputs;
        authoredOutputs.reserve(recipe.mOutputs.size());

        const auto validateAuthoredItem
            = [&](const ESM4::Recipe::Item& item, std::map<ESM::RefId, std::int64_t>& totals,
                  std::vector<FnvCraftingItemDelta>* authored) -> FnvCraftingPreparationError {
            const ItemValidation validation = validateItem(store, item.mItem);
            if (validation != ItemValidation::Supported)
                return itemError(validation);
            const std::optional<int> quantity = decodeQuantity(item.mQuantity);
            if (!quantity)
                return FnvCraftingPreparationError::InvalidQuantity;
            const ESM::RefId id(item.mItem);
            if (!checkedAdd(totals, id, *quantity))
                return FnvCraftingPreparationError::QuantityOverflow;
            if (authored != nullptr)
                authored->push_back({ id, *quantity });
            return FnvCraftingPreparationError::None;
        };

        for (const ESM4::Recipe::Item& item : recipe.mIngredients)
        {
            const FnvCraftingPreparationError itemFailure = validateAuthoredItem(item, ingredientTotals, nullptr);
            if (itemFailure != FnvCraftingPreparationError::None)
                return fail(itemFailure, error);
        }
        for (const ESM4::Recipe::Item& item : recipe.mOutputs)
        {
            const FnvCraftingPreparationError itemFailure = validateAuthoredItem(item, outputTotals, &authoredOutputs);
            if (itemFailure != FnvCraftingPreparationError::None)
                return fail(itemFailure, error);
        }

        auto impl = std::make_unique<PreparedFnvCraftingPlan::Impl>();
        impl->mStation = source.mStation->mId;
        impl->mCategory = source.mStationCategory->mId;
        impl->mRecipe = recipe.mId;
        impl->mActor = source.mActor;
        impl->mInventory = source.mInventory;
        impl->mPublicOutputs = authoredOutputs;
        impl->mIngredients.reserve(ingredientTotals.size());
        for (const auto& [item, quantity] : ingredientTotals)
            impl->mIngredients.push_back({ item, static_cast<int>(quantity) });

        std::map<ESM::RefId, bool> relevantItems;
        for (const auto& [item, _] : ingredientTotals)
            relevantItems[item] = true;
        for (const auto& [item, _] : outputTotals)
            relevantItems[item] = true;

        for (const auto& [item, _] : relevantItems)
        {
            const std::optional<std::int64_t> count = source.mInventory->getCount(item);
            if (!count || *count < 0 || *count > std::numeric_limits<int>::max())
                return fail(FnvCraftingPreparationError::InvalidInventory, error);
            impl->mSnapshot[item] = *count;

            const std::int64_t ingredients = ingredientTotals.contains(item) ? ingredientTotals.at(item) : 0;
            const std::int64_t outputs = outputTotals.contains(item) ? outputTotals.at(item) : 0;
            if (*count < ingredients)
                return fail(FnvCraftingPreparationError::InsufficientIngredients, error);
            const std::int64_t finalCount = *count - ingredients + outputs;
            if (finalCount < 0 || finalCount > std::numeric_limits<int>::max())
                return fail(FnvCraftingPreparationError::QuantityOverflow, error);
        }

        // ManualRef construction performs typed base-record resolution without
        // registering a global pointer. Every output is built before the first
        // ingredient can be removed.
        try
        {
            impl->mOutputs.reserve(authoredOutputs.size());
            for (const FnvCraftingItemDelta& output : authoredOutputs)
            {
                auto constructed = std::make_unique<ManualRef>(store, output.mItem, output.mQuantity);
                if (constructed->getPtr().isEmpty() || constructed->getPtr().getCellRef().getRefId() != output.mItem
                    || constructed->getPtr().getCellRef().getCount(false) != output.mQuantity)
                    return fail(FnvCraftingPreparationError::OutputConstructionFailed, error);
                ContainerStore::getType(constructed->getPtr());
                impl->mOutputs.push_back({ output, std::move(constructed) });
            }
        }
        catch (...)
        {
            return fail(FnvCraftingPreparationError::OutputConstructionFailed, error);
        }

        return FnvCraftingPlanBuilder::make(std::move(impl));
    }

    FnvCraftingCommitResult commitFnvCraftingTransaction(PreparedFnvCraftingPlan&& plan) noexcept
    {
        std::unique_ptr<PreparedFnvCraftingPlan::Impl> impl = std::move(plan.mImpl);
        if (impl == nullptr || impl->mInventory == nullptr)
            return FnvCraftingCommitResult::InvalidPlan;
        if (!impl->mInventory->stillBelongsTo(impl->mActor))
            return FnvCraftingCommitResult::ActorOrInventoryChanged;

        // Exact snapshot equality prevents a stale plan from consuming a
        // later inventory state. This check completes before any mutation.
        for (const auto& [item, expected] : impl->mSnapshot)
        {
            const std::optional<std::int64_t> actual = impl->mInventory->getCount(item);
            if (!actual || *actual != expected)
                return FnvCraftingCommitResult::InventoryChanged;
        }

        // Preserve FNV overlap semantics: every ingredient is consumed before
        // any result is created. Duplicate ingredient rows were aggregated in
        // preflight; output rows retain their authored one-call-per-row shape.
        for (const FnvCraftingItemDelta& ingredient : impl->mIngredients)
        {
            if (!impl->mInventory->remove(ingredient.mItem, ingredient.mQuantity))
                return FnvCraftingCommitResult::MutationFailed;
        }
        for (const PreparedFnvCraftingPlan::Impl::Output& output : impl->mOutputs)
        {
            if (!impl->mInventory->add(
                    output.mConstructed->getPtr(), output.mDelta.mQuantity, false /* allowAutoEquip */))
                return FnvCraftingCommitResult::MutationFailed;
        }
        return FnvCraftingCommitResult::Applied;
    }

    std::string_view getFnvCraftingPreparationErrorName(FnvCraftingPreparationError error)
    {
        switch (error)
        {
            case FnvCraftingPreparationError::None:
                return "none";
            case FnvCraftingPreparationError::NotFalloutNewVegas:
                return "not-fallout-new-vegas";
            case FnvCraftingPreparationError::MissingStore:
                return "missing-store";
            case FnvCraftingPreparationError::MissingStation:
                return "missing-station";
            case FnvCraftingPreparationError::StationNotInStore:
                return "station-not-in-store";
            case FnvCraftingPreparationError::UnsupportedStation:
                return "unsupported-station";
            case FnvCraftingPreparationError::MissingStationCategory:
                return "missing-station-category";
            case FnvCraftingPreparationError::CategoryNotInStore:
                return "category-not-in-store";
            case FnvCraftingPreparationError::StationCategoryMismatch:
                return "station-category-mismatch";
            case FnvCraftingPreparationError::MissingRecipe:
                return "missing-recipe";
            case FnvCraftingPreparationError::RecipeNotInStore:
                return "recipe-not-in-store";
            case FnvCraftingPreparationError::DeletedRecord:
                return "deleted-record";
            case FnvCraftingPreparationError::ConditionalRecipe:
                return "conditional-recipe";
            case FnvCraftingPreparationError::RecipeCategoryMismatch:
                return "recipe-category-mismatch";
            case FnvCraftingPreparationError::MissingSubCategory:
                return "missing-subcategory";
            case FnvCraftingPreparationError::SubCategoryNotInStore:
                return "subcategory-not-in-store";
            case FnvCraftingPreparationError::MissingActor:
                return "missing-actor";
            case FnvCraftingPreparationError::ActorIsNotPlayer:
                return "actor-is-not-player";
            case FnvCraftingPreparationError::MissingInventory:
                return "missing-inventory";
            case FnvCraftingPreparationError::InventoryMismatch:
                return "inventory-mismatch";
            case FnvCraftingPreparationError::PlayerStateUninitialized:
                return "player-state-uninitialized";
            case FnvCraftingPreparationError::PlayerStateMismatch:
                return "player-state-mismatch";
            case FnvCraftingPreparationError::InvalidNoSkillGate:
                return "invalid-no-skill-gate";
            case FnvCraftingPreparationError::UnsupportedSkill:
                return "unsupported-skill";
            case FnvCraftingPreparationError::UnresolvedSkillOffset:
                return "unresolved-skill-offset";
            case FnvCraftingPreparationError::InsufficientSkill:
                return "insufficient-skill";
            case FnvCraftingPreparationError::MissingItem:
                return "missing-item";
            case FnvCraftingPreparationError::UnsupportedCurrency:
                return "unsupported-currency";
            case FnvCraftingPreparationError::UnsupportedItemType:
                return "unsupported-item-type";
            case FnvCraftingPreparationError::ScriptedItem:
                return "scripted-item";
            case FnvCraftingPreparationError::InvalidQuantity:
                return "invalid-quantity";
            case FnvCraftingPreparationError::QuantityOverflow:
                return "quantity-overflow";
            case FnvCraftingPreparationError::InvalidInventory:
                return "invalid-inventory";
            case FnvCraftingPreparationError::InsufficientIngredients:
                return "insufficient-ingredients";
            case FnvCraftingPreparationError::OutputConstructionFailed:
                return "output-construction-failed";
        }
        return "unknown";
    }

    std::string_view getFnvCraftingCommitResultName(FnvCraftingCommitResult result)
    {
        switch (result)
        {
            case FnvCraftingCommitResult::Applied:
                return "applied";
            case FnvCraftingCommitResult::InvalidPlan:
                return "invalid-plan";
            case FnvCraftingCommitResult::ActorOrInventoryChanged:
                return "actor-or-inventory-changed";
            case FnvCraftingCommitResult::InventoryChanged:
                return "inventory-changed";
            case FnvCraftingCommitResult::MutationFailed:
                return "mutation-failed";
        }
        return "unknown";
    }
}
