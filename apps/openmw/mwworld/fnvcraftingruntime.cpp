#include "fnvcraftingruntime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <components/esm/defs.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/falloutformat.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>
#include <components/esm4/loadweap.hpp>

#include "esmstore.hpp"

namespace
{
    struct StationMapping
    {
        ESM::FormId mCategory;
    };

    std::optional<StationMapping> getStationMapping(
        const ESM4::Activator& station, std::span<const MWWorld::FnvCraftingStationRule> rules)
    {
        const auto found = std::ranges::find_if(rules, [&station](const auto& rule) {
            return station.mId == rule.mStationBase && station.mScriptId == rule.mStationScript;
        });
        if (found == rules.end())
            return std::nullopt;
        return StationMapping{ found->mCategory };
    }

    bool isDeleted(std::uint32_t flags)
    {
        return (flags & ESM4::Rec_Deleted) != 0;
    }

    std::optional<int> decodeQuantity(std::uint32_t raw)
    {
        const std::int32_t exact = std::bit_cast<std::int32_t>(raw);
        if (exact < static_cast<std::int32_t>(ESM4::Fallout::kRecipeMinimumQuantity))
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
    ItemValidation validateTypedItem(
        const MWWorld::ESMStore& store, const ESM::RefId& id, ScriptMember scriptMember)
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
        switch (store.find(id))
        {
            case ESM::REC_CMNY4:
                return ItemValidation::Currency;
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
            case ESM::REC_INTERNAL_PLAYER:
                return ItemValidation::Missing;
            default:
                return ItemValidation::Unsupported;
        }
    }

    std::string recordName(std::string_view fullName, std::string_view editorId, ESM::FormId id)
    {
        if (!fullName.empty())
            return std::string(fullName);
        if (!editorId.empty())
            return std::string(editorId);
        return ESM::RefId(id).toDebugString();
    }

    template <class Record>
    std::string typedItemName(const MWWorld::ESMStore& store, const ESM::RefId& id)
    {
        const Record* record = store.get<Record>().search(id);
        if (record == nullptr)
            return id.toDebugString();
        return recordName(record->mFullName, record->mEditorId, record->mId);
    }

    std::string itemName(const MWWorld::ESMStore& store, ESM::FormId form)
    {
        const ESM::RefId id(form);
        switch (store.find(id))
        {
            case ESM::REC_MISC4:
                return typedItemName<ESM4::MiscItem>(store, id);
            case ESM::REC_ALCH4:
                return typedItemName<ESM4::Potion>(store, id);
            case ESM::REC_AMMO4:
                return typedItemName<ESM4::Ammunition>(store, id);
            case ESM::REC_WEAP4:
                return typedItemName<ESM4::Weapon>(store, id);
            case ESM::REC_ARMO4:
                return typedItemName<ESM4::Armor>(store, id);
            default:
                return id.toDebugString();
        }
    }

    MWWorld::FnvCraftingPreparationError itemError(ItemValidation value);

    MWWorld::FnvCraftingPreparationError validateCatalogRecipe(const MWWorld::ESMStore& store,
        ESM::FormId expectedCategory, const ESM4::Recipe& recipe, const ESM4::RecipeCategory** subCategory)
    {
        if (subCategory != nullptr)
            *subCategory = nullptr;
        if (isDeleted(recipe.mFlags))
            return MWWorld::FnvCraftingPreparationError::DeletedRecord;
        if (!recipe.mConditions.empty())
            return MWWorld::FnvCraftingPreparationError::ConditionalRecipe;
        if (recipe.mData.mCategory.isZeroOrUnset() || recipe.mData.mCategory != expectedCategory)
            return MWWorld::FnvCraftingPreparationError::RecipeCategoryMismatch;
        if (recipe.mData.mSubCategory.isZeroOrUnset())
            return MWWorld::FnvCraftingPreparationError::MissingSubCategory;
        const ESM4::RecipeCategory* storedSubCategory
            = findExactRecord<ESM4::RecipeCategory>(store, recipe.mData.mSubCategory);
        if (storedSubCategory == nullptr)
            return MWWorld::FnvCraftingPreparationError::SubCategoryNotInStore;
        if (isDeleted(storedSubCategory->mFlags))
            return MWWorld::FnvCraftingPreparationError::DeletedRecord;
        if (subCategory != nullptr)
            *subCategory = storedSubCategory;

        if (recipe.mData.mRequiredSkill == ESM4::Fallout::kRecipeDefaultRequiredSkill)
        {
            if (recipe.mData.mRequiredSkillLevel != ESM4::Fallout::kRecipeDefaultRequiredSkillLevel)
                return MWWorld::FnvCraftingPreparationError::InvalidNoSkillGate;
        }
        else if (recipe.mData.mRequiredSkill < 0
            || static_cast<std::uint32_t>(recipe.mData.mRequiredSkill) < ESM4::Fallout::kRecipeSkillActorValueBegin
            || static_cast<std::uint32_t>(recipe.mData.mRequiredSkill) > ESM4::Fallout::kRecipeSkillActorValueEnd)
        {
            return MWWorld::FnvCraftingPreparationError::UnsupportedSkill;
        }

        if (recipe.mIngredients.empty() || recipe.mOutputs.empty())
            return MWWorld::FnvCraftingPreparationError::MissingItem;
        const auto validateItems = [&](const std::vector<ESM4::Recipe::Item>& items) {
            for (const ESM4::Recipe::Item& item : items)
            {
                if (const ItemValidation validation = validateItem(store, item.mItem);
                    validation != ItemValidation::Supported)
                    return itemError(validation);
                if (!decodeQuantity(item.mQuantity))
                    return MWWorld::FnvCraftingPreparationError::InvalidQuantity;
            }
            return MWWorld::FnvCraftingPreparationError::None;
        };
        const MWWorld::FnvCraftingPreparationError ingredientError = validateItems(recipe.mIngredients);
        if (ingredientError != MWWorld::FnvCraftingPreparationError::None)
            return ingredientError;
        return validateItems(recipe.mOutputs);
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
                return MWWorld::FnvCraftingPreparationError::None;
        }
        return MWWorld::FnvCraftingPreparationError::UnsupportedItemType;
    }

    bool checkedAdd(std::map<ESM::RefId, std::int64_t>& totals, const ESM::RefId& id, int value)
    {
        std::int64_t& total = totals[id];
        const std::int64_t maximum = std::numeric_limits<int>::max();
        if (value < static_cast<int>(ESM4::Fallout::kRecipeMinimumQuantity) || total > maximum - value)
            return false;
        total += value;
        return true;
    }

    std::optional<MWWorld::PreparedFnvCraftingPlan> fail(
        MWWorld::FnvCraftingPreparationError value, MWWorld::FnvCraftingPreparationError* output)
    {
        if (output != nullptr)
            *output = value;
        return std::nullopt;
    }

    std::optional<MWWorld::PreparedFnvCraftingCatalog> failCatalog(
        MWWorld::FnvCraftingPreparationError value, MWWorld::FnvCraftingPreparationError* output)
    {
        if (output != nullptr)
            *output = value;
        return std::nullopt;
    }
}

namespace MWWorld
{
    std::optional<ESM::FormId> getFnvCraftingStationCategory(
        const ESM4::Activator& station, std::span<const FnvCraftingStationRule> rules)
    {
        const std::optional<StationMapping> mapping = getStationMapping(station, rules);
        return mapping ? std::optional<ESM::FormId>(mapping->mCategory) : std::nullopt;
    }

    std::optional<PreparedFnvCraftingCatalog> prepareFnvCraftingCatalog(
        const FnvCraftingCatalogSource& source, FnvCraftingPreparationError* error)
    {
        if (error != nullptr)
            *error = FnvCraftingPreparationError::None;
        if (source.mGame != ESM4Game::FalloutNewVegas)
            return failCatalog(FnvCraftingPreparationError::NotFalloutNewVegas, error);
        if (source.mStore == nullptr)
            return failCatalog(FnvCraftingPreparationError::MissingStore, error);
        const ESMStore& store = *source.mStore;
        if (source.mStation == nullptr || source.mStation->mId.isZeroOrUnset())
            return failCatalog(FnvCraftingPreparationError::MissingStation, error);
        const ESM4::Activator* storedStation = findExactRecord<ESM4::Activator>(store, source.mStation->mId);
        if (storedStation == nullptr || storedStation != source.mStation)
            return failCatalog(FnvCraftingPreparationError::StationNotInStore, error);
        if (isDeleted(source.mStation->mFlags))
            return failCatalog(FnvCraftingPreparationError::DeletedRecord, error);
        const std::optional<ESM::FormId> categoryId
            = getFnvCraftingStationCategory(*source.mStation, source.mStationRules);
        if (!categoryId)
            return failCatalog(FnvCraftingPreparationError::UnsupportedStation, error);
        const ESM4::RecipeCategory* category = findExactRecord<ESM4::RecipeCategory>(store, *categoryId);
        if (category == nullptr)
            return failCatalog(FnvCraftingPreparationError::CategoryNotInStore, error);
        if (isDeleted(category->mFlags))
            return failCatalog(FnvCraftingPreparationError::DeletedRecord, error);

        PreparedFnvCraftingCatalog catalog;
        catalog.mStation = source.mStation->mId;
        catalog.mCategory = *categoryId;
        catalog.mCategoryName = recordName(category->mFullName, category->mEditorId, category->mId);
        for (const ESM4::Recipe& recipe : store.get<ESM4::Recipe>())
        {
            if (recipe.mData.mCategory != *categoryId || isDeleted(recipe.mFlags))
                continue;

            const ESM4::RecipeCategory* subCategory = nullptr;
            const FnvCraftingPreparationError blocker
                = validateCatalogRecipe(store, *categoryId, recipe, &subCategory);
            const auto describeItems = [&](const std::vector<ESM4::Recipe::Item>& authored) {
                std::vector<PreparedFnvCraftingCatalogItem> result;
                result.reserve(authored.size());
                for (const ESM4::Recipe::Item& item : authored)
                {
                    result.push_back({ { ESM::RefId(item.mItem), std::bit_cast<std::int32_t>(item.mQuantity) },
                        itemName(store, item.mItem) });
                }
                return result;
            };

            catalog.mEntries.push_back({ recipe.mId,
                recordName(recipe.mFullName, recipe.mEditorId, recipe.mId), recipe.mData.mSubCategory,
                subCategory != nullptr
                    ? recordName(subCategory->mFullName, subCategory->mEditorId, subCategory->mId)
                    : std::string{},
                recipe.mData.mRequiredSkill, recipe.mData.mRequiredSkillLevel, describeItems(recipe.mIngredients),
                describeItems(recipe.mOutputs), blocker });
        }
        if (catalog.mEntries.empty())
            return failCatalog(FnvCraftingPreparationError::MissingRecipe, error);
        return catalog;
    }

    struct PreparedFnvCraftingPlan::Impl
    {
        ESM::FormId mStation;
        ESM::FormId mCategory;
        ESM::FormId mRecipe;
        Ptr mActor;
        FnvCraftingInventory* mInventory = nullptr;
        std::vector<FnvCraftingItemDelta> mIngredients;
        std::vector<FnvCraftingItemDelta> mOutputs;
        std::map<ESM::RefId, std::int64_t> mSnapshot;
    };

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
        static const std::vector<FnvCraftingItemDelta> empty;
        return mImpl != nullptr ? mImpl->mIngredients : empty;
    }

    const std::vector<FnvCraftingItemDelta>& PreparedFnvCraftingPlan::getOutputs() const
    {
        static const std::vector<FnvCraftingItemDelta> empty;
        return mImpl != nullptr ? mImpl->mOutputs : empty;
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
        const std::optional<StationMapping> stationMapping
            = getStationMapping(*source.mStation, source.mStationRules);
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
        const ESM4::RecipeCategory* subCategory = findExactRecord<ESM4::RecipeCategory>(store, recipe.mData.mSubCategory);
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

        if (recipe.mData.mRequiredSkill == ESM4::Fallout::kRecipeDefaultRequiredSkill)
        {
            if (recipe.mData.mRequiredSkillLevel != ESM4::Fallout::kRecipeDefaultRequiredSkillLevel)
                return fail(FnvCraftingPreparationError::InvalidNoSkillGate, error);
        }
        else
        {
            if (recipe.mData.mRequiredSkill < 0)
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            const auto actorValue = static_cast<std::uint32_t>(recipe.mData.mRequiredSkill);
            if (actorValue < ESM4::Fallout::kRecipeSkillActorValueBegin
                || actorValue > ESM4::Fallout::kRecipeSkillActorValueEnd)
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            if (source.mSkills == nullptr)
                return fail(FnvCraftingPreparationError::MissingSkillProvider, error);
            const std::optional<double> skill = source.mSkills->getSkill(actorValue);
            if (!skill || !std::isfinite(*skill))
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            if (*skill < static_cast<double>(recipe.mData.mRequiredSkillLevel))
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
            const FnvCraftingPreparationError itemFailure
                = validateAuthoredItem(item, outputTotals, &authoredOutputs);
            if (itemFailure != FnvCraftingPreparationError::None)
                return fail(itemFailure, error);
        }

        auto impl = std::unique_ptr<PreparedFnvCraftingPlan::Impl>(new PreparedFnvCraftingPlan::Impl);
        impl->mStation = source.mStation->mId;
        impl->mCategory = source.mStationCategory->mId;
        impl->mRecipe = recipe.mId;
        impl->mActor = source.mActor;
        impl->mInventory = source.mInventory;
        impl->mOutputs = std::move(authoredOutputs);
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

        return PreparedFnvCraftingPlan(std::move(impl));
    }

    FnvCraftingCommitResult commitFnvCraftingTransaction(PreparedFnvCraftingPlan&& plan) noexcept
    {
        std::unique_ptr<PreparedFnvCraftingPlan::Impl> impl = std::move(plan.mImpl);
        if (impl == nullptr || impl->mInventory == nullptr)
            return FnvCraftingCommitResult::InvalidPlan;
        if (!impl->mInventory->stillBelongsTo(impl->mActor))
            return FnvCraftingCommitResult::ActorOrInventoryChanged;

        for (const auto& [item, expected] : impl->mSnapshot)
        {
            const std::optional<std::int64_t> actual = impl->mInventory->getCount(item);
            if (!actual || *actual != expected)
                return FnvCraftingCommitResult::InventoryChanged;
        }

        if (!impl->mInventory->apply(impl->mIngredients, impl->mOutputs))
            return FnvCraftingCommitResult::MutationFailed;
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
            case FnvCraftingPreparationError::MissingSkillProvider:
                return "missing-skill-provider";
            case FnvCraftingPreparationError::InvalidNoSkillGate:
                return "invalid-no-skill-gate";
            case FnvCraftingPreparationError::UnsupportedSkill:
                return "unsupported-skill";
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
