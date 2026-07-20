#include "esm4container.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm3/containerstate.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadweap.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/actionopen.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/failedaction.hpp"
#include "../mwworld/globalvariablename.hpp"
#include "../mwworld/worldmodel.hpp"

namespace
{
    constexpr int sMaxLevelledItemDepth = 32;

    struct ResolvedFnvContainerItem
    {
        ESM::RefId mId;
        int mCount = 0;
        std::optional<float> mCondition;
    };

    bool checkedMultiply(int left, int right, int& result)
    {
        if (left <= 0 || right <= 0 || left > std::numeric_limits<int>::max() / right)
            return false;
        result = left * right;
        return true;
    }

    bool appendResolved(std::vector<ResolvedFnvContainerItem>& result, const ESM::RefId& id, int count,
        std::optional<float> condition, std::string_view& failure)
    {
        if (id.empty() || count <= 0 || (condition && (!std::isfinite(*condition) || *condition < 0.f || *condition > 1.f)))
        {
            failure = "invalid-terminal-item";
            return false;
        }

        const auto found = std::find_if(result.begin(), result.end(), [&](const ResolvedFnvContainerItem& value) {
            return value.mId == id && value.mCondition == condition;
        });
        if (found == result.end())
        {
            result.push_back({ id, count, condition });
            return true;
        }
        if (found->mCount > std::numeric_limits<int>::max() - count)
        {
            failure = "aggregate-count-overflow";
            return false;
        }
        found->mCount += count;
        return true;
    }

    bool isSupportedContainerTerminal(int type)
    {
        switch (type)
        {
            case ESM::REC_ALCH4:
            case ESM::REC_AMMO4:
            case ESM::REC_ARMO4:
            case ESM::REC_BOOK4:
            case ESM::REC_CLOT4:
            case ESM::REC_IMOD4:
            case ESM::REC_INGR4:
            case ESM::REC_KEYM4:
            case ESM::REC_LIGH4:
            case ESM::REC_MISC4:
            case ESM::REC_WEAP4:
                return true;
            default:
                return false;
        }
    }

    std::optional<float> getChanceNone(const ESM4::LevelledItem& list, const MWWorld::ESMStore& store,
        MWBase::World* world)
    {
        if (list.mChanceGlobal.isZeroOrUnset())
            return static_cast<float>(static_cast<std::uint8_t>(list.chanceNone()));

        const ESM4::GlobalVariable* global
            = store.get<ESM4::GlobalVariable>().search(ESM::RefId(list.mChanceGlobal));
        if (global == nullptr || (global->mFlags & ESM4::Rec_Deleted) != 0 || global->mEditorId.empty())
            return std::nullopt;
        if (world != nullptr
            && world->getGlobalVariableType(MWWorld::GlobalVariableName(global->mEditorId)) != ' ')
            return world->getGlobalFloat(MWWorld::GlobalVariableName(global->mEditorId));
        return global->mValue;
    }

    bool resolveLevelledList(const MWWorld::ESMStore& store, const ESM::RefId& listId, int playerLevel,
        int requestedCount, Misc::Rng::Generator& prng, MWBase::World* world, int depth, std::set<ESM::RefId>& path,
        std::vector<ResolvedFnvContainerItem>& result, std::string_view& failure);

    bool resolveLevelledEntry(const MWWorld::ESMStore& store, const ESM4::LevelledItem& list,
        std::size_t entryIndex, int playerLevel, Misc::Rng::Generator& prng, MWBase::World* world, int depth,
        std::set<ESM::RefId>& path, std::vector<ResolvedFnvContainerItem>& result, std::string_view& failure)
    {
        const ESM4::LVLO& entry = list.mLvlObject[entryIndex];
        if (entry.item == 0 || entry.count <= 0)
        {
            failure = "invalid-levelled-entry";
            return false;
        }

        std::optional<float> condition;
        if (!list.mLvlObjectExtra.empty())
        {
            if (list.mLvlObjectExtra.size() != list.mLvlObject.size())
            {
                failure = "malformed-entry-extra-data";
                return false;
            }
            if (const std::optional<ESM4::LevelledItemExtraData>& extra = list.mLvlObjectExtra[entryIndex])
            {
                if (extra->mOwner != 0 || extra->mGlobalOrRequiredRank != 0)
                {
                    failure = "unsupported-entry-owner-or-rank";
                    return false;
                }
                condition = extra->mItemCondition;
            }
        }

        const ESM::RefId itemId(ESM::FormId::fromUint32(entry.item));
        const int type = store.find(itemId);
        if (type == ESM::REC_LVLI4)
        {
            if (condition)
            {
                failure = "condition-on-nested-list";
                return false;
            }
            return resolveLevelledList(
                store, itemId, playerLevel, entry.count, prng, world, depth + 1, path, result, failure);
        }
        if (!isSupportedContainerTerminal(type))
        {
            failure = type == 0 ? "missing-terminal-record" : "unsupported-terminal-record";
            return false;
        }
        return appendResolved(result, itemId, entry.count, condition, failure);
    }

    bool resolveLevelledListOnce(const MWWorld::ESMStore& store, const ESM4::LevelledItem& list, int playerLevel,
        Misc::Rng::Generator& prng, MWBase::World* world, int depth, std::set<ESM::RefId>& path,
        std::vector<ResolvedFnvContainerItem>& result, std::string_view& failure)
    {
        const std::optional<float> chanceNone = getChanceNone(list, store, world);
        if (!chanceNone || !std::isfinite(*chanceNone) || *chanceNone < 0.f || *chanceNone > 100.f)
        {
            failure = "invalid-chance-none";
            return false;
        }
        if (Misc::Rng::rollProbability(prng) * 100.f < *chanceNone)
            return true;

        std::vector<std::size_t> eligible;
        if (list.useAll() || list.calcAllLvlLessThanPlayer())
        {
            for (std::size_t index = 0; index < list.mLvlObject.size(); ++index)
                if (list.mLvlObject[index].level <= playerLevel)
                    eligible.push_back(index);
        }
        else
        {
            std::optional<std::int16_t> highestLevel;
            for (const ESM4::LVLO& entry : list.mLvlObject)
                if (entry.level <= playerLevel && (!highestLevel || entry.level > *highestLevel))
                    highestLevel = entry.level;
            if (highestLevel)
                for (std::size_t index = 0; index < list.mLvlObject.size(); ++index)
                    if (list.mLvlObject[index].level == *highestLevel)
                        eligible.push_back(index);
        }

        if (eligible.empty())
            return true;
        if (list.useAll())
        {
            for (const std::size_t index : eligible)
                if (!resolveLevelledEntry(store, list, index, playerLevel, prng, world, depth, path, result, failure))
                    return false;
            return true;
        }

        const int selection = Misc::Rng::rollDice(static_cast<int>(eligible.size()), prng);
        return resolveLevelledEntry(
            store, list, eligible[static_cast<std::size_t>(selection)], playerLevel, prng, world, depth, path,
            result, failure);
    }

    bool resolveLevelledList(const MWWorld::ESMStore& store, const ESM::RefId& listId, int playerLevel,
        int requestedCount, Misc::Rng::Generator& prng, MWBase::World* world, int depth, std::set<ESM::RefId>& path,
        std::vector<ResolvedFnvContainerItem>& result, std::string_view& failure)
    {
        if (requestedCount <= 0)
        {
            failure = "invalid-requested-count";
            return false;
        }
        if (depth > sMaxLevelledItemDepth)
        {
            failure = "maximum-depth-exceeded";
            return false;
        }
        if (!path.insert(listId).second)
        {
            failure = "cycle";
            return false;
        }

        const ESM4::LevelledItem* list = store.get<ESM4::LevelledItem>().search(listId);
        const bool valid = list != nullptr && (list->mFlags & ESM4::Rec_Deleted) == 0 && list->mHasChanceNone
            && list->mHasLvlItemFlags && (list->mLvlItemFlags & ~std::uint8_t{ 0x07 }) == 0;
        if (!valid)
        {
            failure = "invalid-levelled-list";
            path.erase(listId);
            return false;
        }

        bool resolved = true;
        if (list->calcEachItemInCount())
        {
            for (int index = 0; index < requestedCount && resolved; ++index)
                resolved = resolveLevelledListOnce(
                    store, *list, playerLevel, prng, world, depth, path, result, failure);
        }
        else
        {
            std::vector<ResolvedFnvContainerItem> once;
            resolved = resolveLevelledListOnce(store, *list, playerLevel, prng, world, depth, path, once, failure);
            if (resolved)
            {
                for (const ResolvedFnvContainerItem& item : once)
                {
                    int scaled = 0;
                    if (!checkedMultiply(item.mCount, requestedCount, scaled)
                        || !appendResolved(result, item.mId, scaled, item.mCondition, failure))
                    {
                        failure = "outer-count-overflow";
                        resolved = false;
                        break;
                    }
                }
            }
        }
        path.erase(listId);
        return resolved;
    }
}

namespace MWClass
{
    template <class Record>
    bool ESM4ContainerStore::addInitialRecord(
        const MWWorld::ESMStore& store, const ESM::RefId& id, int count, std::optional<float> condition)
    {
        const Record* record = store.get<Record>().search(id);
        if (record == nullptr)
            return false;

        ESM::CellRef cellRef = ESM::makeBlankCellRef();
        cellRef.mRefID = ESM::RefId::formIdRefId(record->mId);
        MWWorld::LiveCellRef<Record> liveRef(cellRef, record);
        const MWWorld::Ptr ptr(&liveRef);
        if (condition)
        {
            const int maxHealth = ptr.getClass().getItemMaxHealth(ptr);
            if (maxHealth > 0)
            {
                const int health = std::clamp(
                    static_cast<int>(std::lround(*condition * static_cast<float>(maxHealth))), 0, maxHealth);
                ptr.getCellRef().setCharge(health);
            }
        }
        const int type = getType(ptr);
        for (MWWorld::ContainerStoreIterator item = begin(type); item != end(); ++item)
        {
            if (item->getCellRef().getRefId() != cellRef.mRefID
                || item->getCellRef().getCharge() != ptr.getCellRef().getCharge())
                continue;
            item->getCellRef().setCount(addItems(item->getCellRef().getCount(false), count));
            flagAsModified();
            return true;
        }

        addNewStack(ptr, count);
        return true;
    }

    void ESM4ContainerStore::fillImpl(const ESM4::Container& container, const MWWorld::ESMStore& store,
        Misc::Rng::Generator* prng, int playerLevel, MWBase::World* world)
    {
        for (const ESM4::InventoryItem& item : container.mInventory)
        {
            if (item.count == 0 || item.count > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            {
                Log(Debug::Warning) << "Ignoring invalid ESM4 container item count " << item.count << " in "
                                    << ESM::RefId(container.mId);
                continue;
            }

            const ESM::RefId itemId = ESM::RefId::formIdRefId(ESM::FormId::fromUint32(item.item));
            const int count = static_cast<int>(item.count);
            if (store.find(itemId) == ESM::REC_LVLI4)
            {
                if (prng == nullptr)
                {
                    Log(Debug::Warning) << "Unable to resolve ESM4 levelled container item " << itemId << " in "
                                        << ESM::RefId(container.mId) << " without a runtime PRNG";
                    continue;
                }

                std::vector<ResolvedFnvContainerItem> plan;
                std::set<ESM::RefId> path;
                std::string_view failure;
                if (!resolveLevelledList(store, itemId, std::max(playerLevel, 1), count, *prng, world, 1, path, plan,
                        failure))
                {
                    Log(Debug::Warning) << "Unable to resolve ESM4 levelled container item " << itemId << " in "
                                        << ESM::RefId(container.mId) << " reason=" << failure;
                    continue;
                }

                bool committed = true;
                for (const ResolvedFnvContainerItem& resolved : plan)
                {
                    bool stored = false;
                    switch (store.find(resolved.mId))
                    {
                        case ESM::REC_AMMO4:
                            stored = addInitialRecord<ESM4::Ammunition>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_ARMO4:
                            stored = addInitialRecord<ESM4::Armor>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_MISC4:
                            stored = addInitialRecord<ESM4::MiscItem>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_WEAP4:
                            stored = addInitialRecord<ESM4::Weapon>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_ALCH4:
                            stored = addInitialRecord<ESM4::Potion>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_BOOK4:
                            stored = addInitialRecord<ESM4::Book>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_CLOT4:
                            stored = addInitialRecord<ESM4::Clothing>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_INGR4:
                            stored = addInitialRecord<ESM4::Ingredient>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_IMOD4:
                            stored = addInitialRecord<ESM4::ItemMod>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_KEYM4:
                            stored = addInitialRecord<ESM4::Key>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        case ESM::REC_LIGH4:
                            stored = addInitialRecord<ESM4::Light>(
                                store, resolved.mId, resolved.mCount, resolved.mCondition);
                            break;
                        default:
                            break;
                    }
                    committed = stored && committed;
                }
                if (!committed)
                    Log(Debug::Warning) << "Unable to commit complete ESM4 levelled container item " << itemId
                                        << " in " << ESM::RefId(container.mId);
                continue;
            }

            bool stored = false;
            switch (store.find(itemId))
            {
                case ESM::REC_AMMO4:
                    stored = addInitialRecord<ESM4::Ammunition>(store, itemId, count);
                    break;
                case ESM::REC_ARMO4:
                    stored = addInitialRecord<ESM4::Armor>(store, itemId, count);
                    break;
                case ESM::REC_MISC4:
                    stored = addInitialRecord<ESM4::MiscItem>(store, itemId, count);
                    break;
                case ESM::REC_WEAP4:
                    stored = addInitialRecord<ESM4::Weapon>(store, itemId, count);
                    break;
                case ESM::REC_ALCH4:
                    stored = addInitialRecord<ESM4::Potion>(store, itemId, count);
                    break;
                case ESM::REC_BOOK4:
                    stored = addInitialRecord<ESM4::Book>(store, itemId, count);
                    break;
                case ESM::REC_CLOT4:
                    stored = addInitialRecord<ESM4::Clothing>(store, itemId, count);
                    break;
                case ESM::REC_INGR4:
                    stored = addInitialRecord<ESM4::Ingredient>(store, itemId, count);
                    break;
                case ESM::REC_IMOD4:
                    stored = addInitialRecord<ESM4::ItemMod>(store, itemId, count);
                    break;
                case ESM::REC_KEYM4:
                    stored = addInitialRecord<ESM4::Key>(store, itemId, count);
                    break;
                case ESM::REC_LIGH4:
                    stored = addInitialRecord<ESM4::Light>(store, itemId, count);
                    break;
                default:
                    break;
            }

            if (!stored)
                Log(Debug::Warning) << "Unable to resolve fixed ESM4 container item " << itemId << " in "
                                    << ESM::RefId(container.mId);
        }
    }

    void ESM4ContainerStore::fill(const ESM4::Container& container, const MWWorld::ESMStore& store)
    {
        fillImpl(container, store, nullptr, ESM4Impl::sDefaultLevel, nullptr);
    }

    void ESM4ContainerStore::fill(const ESM4::Container& container, const MWWorld::ESMStore& store,
        Misc::Rng::Generator& prng, int playerLevel, MWBase::World* world)
    {
        fillImpl(container, store, &prng, playerLevel, world);
    }

    std::unique_ptr<MWWorld::ContainerStore> ESM4ContainerStore::clone()
    {
        auto result = std::make_unique<ESM4ContainerStore>(*this);
        result->updateRefNums();
        return result;
    }

    ESM4ContainerCustomData::ESM4ContainerCustomData(
        const ESM4::Container& container, const MWWorld::ESMStore& store)
    {
        mStore.fill(container, store);
    }

    ESM4ContainerCustomData::ESM4ContainerCustomData(const ESM4::Container& container,
        const MWWorld::ESMStore& store, Misc::Rng::Generator& prng, int playerLevel, MWBase::World* world)
    {
        mStore.fill(container, store, prng, playerLevel, world);
    }

    ESM4ContainerCustomData::ESM4ContainerCustomData(const ESM::InventoryState& inventory)
    {
        mStore.readState(inventory);
    }

    ESM4Container::ESM4Container()
        : MWWorld::RegisteredClass<ESM4Container, ESM4Base<ESM4::Container>>(ESM4::Container::sRecordId)
    {
    }

    ESM4ContainerCustomData& ESM4Container::getCustomData(const MWWorld::Ptr& ptr)
    {
        return dynamic_cast<ESM4ContainerCustomData&>(*ptr.getRefData().getCustomData());
    }

    const ESM4ContainerCustomData& ESM4Container::getCustomData(const MWWorld::ConstPtr& ptr)
    {
        return dynamic_cast<const ESM4ContainerCustomData&>(*ptr.getRefData().getCustomData());
    }

    void ESM4Container::ensureCustomData(const MWWorld::Ptr& ptr) const
    {
        if (ptr.getRefData().getCustomData() != nullptr)
            return;

        const ESM4::Container& container = *ptr.get<ESM4::Container>()->mBase;
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        const bool hasLevelledItems = std::any_of(container.mInventory.begin(), container.mInventory.end(),
            [&](const ESM4::InventoryItem& item) {
                return esmStore.find(ESM::RefId(ESM::FormId::fromUint32(item.item))) == ESM::REC_LVLI4;
            });

        std::unique_ptr<ESM4ContainerCustomData> data;
        if (hasLevelledItems)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld().get();
            int playerLevel = ESM4Impl::sDefaultLevel;
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (!player.isEmpty() && player.getClass().isActor())
                playerLevel = player.getClass().getCreatureStats(player).getLevel();
            data = std::make_unique<ESM4ContainerCustomData>(
                container, esmStore, world->getPrng(), playerLevel, world);
        }
        else
            data = std::make_unique<ESM4ContainerCustomData>(container, esmStore);
        ptr.getRefData().setCustomData(std::move(data));

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        ESM4ContainerStore& inventoryStore = getCustomData(ptr).mStore;
        inventoryStore.setPtr(ptr);
        for (const MWWorld::Ptr item : inventoryStore)
            MWBase::Environment::get().getWorldModel()->registerPtr(item);
    }

    MWWorld::Ptr ESM4Container::copyToCellImpl(
        const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const
    {
        const MWWorld::LiveCellRef<ESM4::Container>* ref = ptr.get<ESM4::Container>();
        MWWorld::Ptr newPtr(cell.insert(ref), &cell);
        if (newPtr.getRefData().getCustomData() != nullptr)
        {
            MWBase::Environment::get().getWorldModel()->registerPtr(newPtr);
            getCustomData(newPtr).mStore.setPtr(newPtr);
        }
        return newPtr;
    }

    std::string_view ESM4Container::getName(const MWWorld::ConstPtr& ptr) const
    {
        return ptr.get<ESM4::Container>()->mBase->mFullName;
    }

    bool ESM4Container::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return !getName(ptr).empty();
    }

    MWGui::ToolTipInfo ESM4Container::getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const
    {
        return ESM4Impl::getToolTipInfo(getName(ptr), count);
    }

    std::unique_ptr<MWWorld::Action> ESM4Container::activate(
        const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        if (ptr.getCellRef().isLocked())
        {
            const ESM::RefId keyId = ptr.getCellRef().getKey();
            const bool hasKey = !actor.isEmpty() && !keyId.empty()
                && !actor.getClass().getContainerStore(actor).search(keyId).isEmpty();
            if (hasKey)
                ptr.getCellRef().unlock();
            else
            {
                std::unique_ptr<MWWorld::Action> action
                    = std::make_unique<MWWorld::FailedAction>(std::string_view{}, ptr);
                action->setSound(ESM::RefId::stringRefId("LockedChest"));
                return action;
            }
        }

        std::unique_ptr<MWWorld::Action> action = std::make_unique<MWWorld::ActionOpen>(ptr);
        action->setSound(ESM::RefId(ptr.get<ESM4::Container>()->mBase->mOpenSound));
        return action;
    }

    MWWorld::ContainerStore& ESM4Container::getContainerStore(const MWWorld::Ptr& ptr) const
    {
        ensureCustomData(ptr);
        return getCustomData(ptr).mStore;
    }

    float ESM4Container::getCapacity(const MWWorld::Ptr& ptr) const
    {
        return ptr.get<ESM4::Container>()->mBase->mWeight;
    }

    float ESM4Container::getEncumbrance(const MWWorld::Ptr& ptr) const
    {
        return getContainerStore(ptr).getWeight();
    }

    bool ESM4Container::canLock(const MWWorld::ConstPtr& ptr) const
    {
        (void)ptr;
        return true;
    }

    ESM::RefId ESM4Container::getScript(const MWWorld::ConstPtr& ptr) const
    {
        return ESM::RefId(ptr.get<ESM4::Container>()->mBase->mScriptId);
    }

    void ESM4Container::readAdditionalState(
        const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const
    {
        if (!state.mHasCustomState)
            return;

        const ESM::ContainerState& containerState = state.asContainerState();
        ptr.getRefData().setCustomData(
            std::make_unique<ESM4ContainerCustomData>(containerState.mInventory));

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        getCustomData(ptr).mStore.setPtr(ptr);
    }

    void ESM4Container::writeAdditionalState(
        const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const
    {
        if (ptr.getRefData().getCustomData() == nullptr)
        {
            state.mHasCustomState = false;
            return;
        }

        ESM::ContainerState& containerState = state.asContainerState();
        getCustomData(ptr).mStore.writeState(containerState.mInventory);
        state.mHasCustomState = true;
    }

    bool ESM4Container::useAnim() const
    {
        return true;
    }
}
