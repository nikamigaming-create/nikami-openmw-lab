#include "esm4container.hpp"

#include <limits>

#include <components/debug/debuglog.hpp>
#include <components/esm3/containerstate.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadweap.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/actionopen.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/failedaction.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWClass
{
    template <class Record>
    bool ESM4ContainerStore::addInitialRecord(
        const MWWorld::ESMStore& store, const ESM::RefId& id, int count)
    {
        const Record* record = store.get<Record>().search(id);
        if (record == nullptr)
            return false;

        ESM::CellRef cellRef = ESM::makeBlankCellRef();
        cellRef.mRefID = ESM::RefId::formIdRefId(record->mId);
        MWWorld::LiveCellRef<Record> liveRef(cellRef, record);
        const MWWorld::ConstPtr ptr(&liveRef);
        const int type = getType(ptr);
        for (MWWorld::ContainerStoreIterator item = begin(type); item != end(); ++item)
        {
            if (item->getCellRef().getRefId() != cellRef.mRefID)
                continue;
            item->getCellRef().setCount(addItems(item->getCellRef().getCount(false), count));
            flagAsModified();
            return true;
        }

        addNewStack(ptr, count);
        return true;
    }

    void ESM4ContainerStore::fill(const ESM4::Container& container, const MWWorld::ESMStore& store)
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
        auto data = std::make_unique<ESM4ContainerCustomData>(
            container, *MWBase::Environment::get().getESMStore());
        ptr.getRefData().setCustomData(std::move(data));

        MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
        ESM4ContainerStore& store = getCustomData(ptr).mStore;
        store.setPtr(ptr);
        for (const MWWorld::Ptr item : store)
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
