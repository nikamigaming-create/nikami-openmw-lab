#include "fnvcraftingruntime.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
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

    std::optional<ESM::FormId> getStationCategory(const ESM4::Activator& station)
    {
        if (station.mId == ESM::FormId::fromUint32(sWorkbenchBase)
            && station.mScriptId == ESM::FormId::fromUint32(sWorkbenchScript))
            return ESM::FormId::fromUint32(sWorkbenchCategory);
        if (station.mId == ESM::FormId::fromUint32(sReloadingBenchBase)
            && station.mScriptId == ESM::FormId::fromUint32(sReloadingBenchScript))
            return ESM::FormId::fromUint32(sReloadingBenchCategory);
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

    MWWorld::FnvCraftingPreparationError validateRecipeGrouping(const MWWorld::ESMStore& store,
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
        return MWWorld::FnvCraftingPreparationError::None;
    }

    MWWorld::FnvCraftingPreparationError validateRecipeSkillShape(const ESM4::Recipe& recipe)
    {
        if (recipe.mData.mRequiredSkill == -1)
        {
            if (recipe.mData.mRequiredSkillLevel != 0)
                return MWWorld::FnvCraftingPreparationError::InvalidNoSkillGate;
            return MWWorld::FnvCraftingPreparationError::None;
        }
        if (recipe.mData.mRequiredSkill < 0)
            return MWWorld::FnvCraftingPreparationError::UnsupportedSkill;
        const std::uint32_t actorValue = static_cast<std::uint32_t>(recipe.mData.mRequiredSkill);
        if (actorValue < MWWorld::FalloutPlayerRuntimeState::SkillActorValueBegin
            || actorValue > MWWorld::FalloutPlayerRuntimeState::SkillActorValueEnd)
            return MWWorld::FnvCraftingPreparationError::UnsupportedSkill;
        return MWWorld::FnvCraftingPreparationError::None;
    }

    struct ValidatedRecipeItems
    {
        std::map<ESM::RefId, std::int64_t> mIngredientTotals;
        std::map<ESM::RefId, std::int64_t> mOutputTotals;
        std::vector<MWWorld::FnvCraftingItemDelta> mAuthoredOutputs;
    };

    MWWorld::FnvCraftingPreparationError validateRecipeItems(
        const MWWorld::ESMStore& store, const ESM4::Recipe& recipe, ValidatedRecipeItems* result)
    {
        if (recipe.mIngredients.empty() || recipe.mOutputs.empty())
            return MWWorld::FnvCraftingPreparationError::MissingItem;

        ValidatedRecipeItems validated;
        validated.mAuthoredOutputs.reserve(recipe.mOutputs.size());
        const auto validateAuthoredItem
            = [&](const ESM4::Recipe::Item& item, std::map<ESM::RefId, std::int64_t>& totals,
                  std::vector<MWWorld::FnvCraftingItemDelta>* authored) -> MWWorld::FnvCraftingPreparationError {
            const ItemValidation validation = validateItem(store, item.mItem);
            if (validation != ItemValidation::Supported)
                return itemError(validation);
            const std::optional<int> quantity = decodeQuantity(item.mQuantity);
            if (!quantity)
                return MWWorld::FnvCraftingPreparationError::InvalidQuantity;
            const ESM::RefId id(item.mItem);
            if (!checkedAdd(totals, id, *quantity))
                return MWWorld::FnvCraftingPreparationError::QuantityOverflow;
            if (authored != nullptr)
                authored->push_back({ id, *quantity });
            return MWWorld::FnvCraftingPreparationError::None;
        };

        for (const ESM4::Recipe::Item& item : recipe.mIngredients)
        {
            const MWWorld::FnvCraftingPreparationError error
                = validateAuthoredItem(item, validated.mIngredientTotals, nullptr);
            if (error != MWWorld::FnvCraftingPreparationError::None)
                return error;
        }
        for (const ESM4::Recipe::Item& item : recipe.mOutputs)
        {
            const MWWorld::FnvCraftingPreparationError error
                = validateAuthoredItem(item, validated.mOutputTotals, &validated.mAuthoredOutputs);
            if (error != MWWorld::FnvCraftingPreparationError::None)
                return error;
        }
        if (result != nullptr)
            *result = std::move(validated);
        return MWWorld::FnvCraftingPreparationError::None;
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
    class FnvCraftingCatalogBuilder
    {
    public:
        static PreparedFnvCraftingCatalogItem makeItem(FnvCraftingItemDelta delta, std::string name)
        {
            return PreparedFnvCraftingCatalogItem(std::move(delta), std::move(name));
        }

        static PreparedFnvCraftingCatalogEntry makeEntry(ESM::FormId recipe, std::string name, ESM::FormId subCategory,
            std::string subCategoryName, std::int32_t requiredSkill, std::uint32_t requiredSkillLevel,
            std::vector<PreparedFnvCraftingCatalogItem> ingredients,
            std::vector<PreparedFnvCraftingCatalogItem> outputs, FnvCraftingPreparationError staticBlocker)
        {
            return PreparedFnvCraftingCatalogEntry(recipe, std::move(name), subCategory, std::move(subCategoryName),
                requiredSkill, requiredSkillLevel, std::move(ingredients), std::move(outputs), staticBlocker);
        }

        static PreparedFnvCraftingCatalog makeCatalog(ESM::FormId station, ESM::FormId category,
            std::string categoryName, std::vector<PreparedFnvCraftingCatalogEntry> entries)
        {
            return PreparedFnvCraftingCatalog(station, category, std::move(categoryName), std::move(entries));
        }
    };

    PreparedFnvCraftingCatalogItem::PreparedFnvCraftingCatalogItem(FnvCraftingItemDelta delta, std::string name)
        : mDelta(std::move(delta))
        , mName(std::move(name))
    {
    }

    PreparedFnvCraftingCatalogEntry::PreparedFnvCraftingCatalogEntry(ESM::FormId recipe, std::string name,
        ESM::FormId subCategory, std::string subCategoryName, std::int32_t requiredSkill,
        std::uint32_t requiredSkillLevel, std::vector<PreparedFnvCraftingCatalogItem> ingredients,
        std::vector<PreparedFnvCraftingCatalogItem> outputs, FnvCraftingPreparationError staticBlocker)
        : mRecipe(recipe)
        , mName(std::move(name))
        , mSubCategory(subCategory)
        , mSubCategoryName(std::move(subCategoryName))
        , mRequiredSkill(requiredSkill)
        , mRequiredSkillLevel(requiredSkillLevel)
        , mIngredients(std::move(ingredients))
        , mOutputs(std::move(outputs))
        , mStaticBlocker(staticBlocker)
    {
    }

    PreparedFnvCraftingCatalog::PreparedFnvCraftingCatalog(ESM::FormId station, ESM::FormId category,
        std::string categoryName, std::vector<PreparedFnvCraftingCatalogEntry> entries)
        : mStation(station)
        , mCategory(category)
        , mCategoryName(std::move(categoryName))
        , mEntries(std::move(entries))
    {
    }

    std::optional<ESM::FormId> getFnvCraftingStationCategory(const ESM4::Activator& station)
    {
        return getStationCategory(station);
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
        const std::optional<ESM::FormId> stationCategory = getStationCategory(*source.mStation);
        if (!stationCategory)
            return failCatalog(FnvCraftingPreparationError::UnsupportedStation, error);

        const ESM4::RecipeCategory* category = findExactRecord<ESM4::RecipeCategory>(store, *stationCategory);
        if (category == nullptr)
            return failCatalog(FnvCraftingPreparationError::CategoryNotInStore, error);
        if (isDeleted(category->mFlags))
            return failCatalog(FnvCraftingPreparationError::DeletedRecord, error);

        std::vector<PreparedFnvCraftingCatalogEntry> entries;
        for (const ESM4::Recipe& recipe : store.get<ESM4::Recipe>())
        {
            if (recipe.mData.mCategory != *stationCategory || isDeleted(recipe.mFlags))
                continue;

            const ESM4::RecipeCategory* subCategory = nullptr;
            FnvCraftingPreparationError blocker = validateRecipeGrouping(store, *stationCategory, recipe, &subCategory);
            if (blocker == FnvCraftingPreparationError::None)
                blocker = validateRecipeSkillShape(recipe);
            if (blocker == FnvCraftingPreparationError::None)
                blocker = validateRecipeItems(store, recipe, nullptr);

            if (subCategory == nullptr && !recipe.mData.mSubCategory.isZeroOrUnset())
                subCategory = findExactRecord<ESM4::RecipeCategory>(store, recipe.mData.mSubCategory);

            const auto describeItems = [&](const std::vector<ESM4::Recipe::Item>& authored) {
                std::vector<PreparedFnvCraftingCatalogItem> result;
                result.reserve(authored.size());
                for (const ESM4::Recipe::Item& item : authored)
                {
                    result.push_back(FnvCraftingCatalogBuilder::makeItem(
                        { ESM::RefId(item.mItem), std::bit_cast<std::int32_t>(item.mQuantity) },
                        itemName(store, item.mItem)));
                }
                return result;
            };

            entries.push_back(FnvCraftingCatalogBuilder::makeEntry(recipe.mId,
                recordName(recipe.mFullName, recipe.mEditorId, recipe.mId), recipe.mData.mSubCategory,
                subCategory != nullptr ? recordName(subCategory->mFullName, subCategory->mEditorId, subCategory->mId)
                                       : std::string{},
                recipe.mData.mRequiredSkill, recipe.mData.mRequiredSkillLevel, describeItems(recipe.mIngredients),
                describeItems(recipe.mOutputs), blocker));
        }
        if (entries.empty())
            return failCatalog(FnvCraftingPreparationError::MissingRecipe, error);

        return FnvCraftingCatalogBuilder::makeCatalog(source.mStation->mId, *stationCategory,
            recordName(category->mFullName, category->mEditorId, category->mId), std::move(entries));
    }

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
        const std::optional<ESM::FormId> stationMapping = getStationCategory(*source.mStation);
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
        if (source.mStationCategory->mId != *stationMapping)
            return fail(FnvCraftingPreparationError::StationCategoryMismatch, error);

        if (source.mRecipe == nullptr || source.mRecipe->mId.isZeroOrUnset())
            return fail(FnvCraftingPreparationError::MissingRecipe, error);
        const ESM4::Recipe* storedRecipe = findExactRecord<ESM4::Recipe>(store, source.mRecipe->mId);
        if (storedRecipe == nullptr || storedRecipe != source.mRecipe)
            return fail(FnvCraftingPreparationError::RecipeNotInStore, error);
        const ESM4::Recipe& recipe = *source.mRecipe;
        const FnvCraftingPreparationError groupingError
            = validateRecipeGrouping(store, source.mStationCategory->mId, recipe, nullptr);
        if (groupingError != FnvCraftingPreparationError::None)
            return fail(groupingError, error);

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

        const FnvCraftingPreparationError skillShapeError = validateRecipeSkillShape(recipe);
        if (skillShapeError != FnvCraftingPreparationError::None)
            return fail(skillShapeError, error);
        if (recipe.mData.mRequiredSkill != -1)
        {
            const std::uint32_t actorValue = static_cast<std::uint32_t>(recipe.mData.mRequiredSkill);
            const std::optional<FalloutRuntimeActorValue> skill = source.mPlayerState->getCurrentActorValue(actorValue);
            if (!skill || !std::isfinite(skill->mValue))
                return fail(FnvCraftingPreparationError::UnsupportedSkill, error);
            if (skill->mRawSkillOffset && *skill->mRawSkillOffset != 0)
                return fail(FnvCraftingPreparationError::UnresolvedSkillOffset, error);
            if (static_cast<double>(skill->mValue) < static_cast<double>(recipe.mData.mRequiredSkillLevel))
                return fail(FnvCraftingPreparationError::InsufficientSkill, error);
        }

        ValidatedRecipeItems validatedItems;
        const FnvCraftingPreparationError itemsError = validateRecipeItems(store, recipe, &validatedItems);
        if (itemsError != FnvCraftingPreparationError::None)
            return fail(itemsError, error);
        const auto& ingredientTotals = validatedItems.mIngredientTotals;
        const auto& outputTotals = validatedItems.mOutputTotals;
        const auto& authoredOutputs = validatedItems.mAuthoredOutputs;

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
