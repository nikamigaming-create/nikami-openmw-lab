#ifndef GAME_MWCLASS_ESM4BASE_H
#define GAME_MWCLASS_ESM4BASE_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include <components/esm4/inventory.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadtree.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/tooltips.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/actiondoor.hpp"
#include "../mwworld/actionteleport.hpp"
#include "../mwworld/failedaction.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/registeredclass.hpp"

#include "classmodel.hpp"
#include "door.hpp"

namespace MWClass
{

    namespace ESM4Impl
    {
        void insertObjectRendering(
            const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface);
        void insertObjectPhysics(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics);
        bool worldViewerDisableEsm4Actors();
        bool worldViewerUseEsm4ActorProxies();
        void logWorldViewerSkippedActor(const MWWorld::ConstPtr& ptr, std::string_view actorType);
        MWGui::ToolTipInfo getToolTipInfo(std::string_view name, int count);

        // We don't handle ESM4 player stats yet, so for resolving levelled object we use an arbitrary number.
        constexpr int sDefaultLevel = 5;

        template <class Record>
        struct InventoryIcon
        {
            static const std::string& get(const Record&)
            {
                static const std::string sEmpty;
                return sEmpty;
            }
        };

        template <>
        struct InventoryIcon<ESM4::Ammunition>
        {
            static const std::string& get(const ESM4::Ammunition& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Armor>
        {
            static const std::string& get(const ESM4::Armor& record)
            {
                return !record.mIconMale.empty() ? record.mIconMale : record.mIconFemale;
            }
        };

        template <>
        struct InventoryIcon<ESM4::Book>
        {
            static const std::string& get(const ESM4::Book& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Clothing>
        {
            static const std::string& get(const ESM4::Clothing& record)
            {
                return !record.mIconMale.empty() ? record.mIconMale : record.mIconFemale;
            }
        };

        template <>
        struct InventoryIcon<ESM4::Ingredient>
        {
            static const std::string& get(const ESM4::Ingredient& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::ItemMod>
        {
            static const std::string& get(const ESM4::ItemMod& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Key>
        {
            static const std::string& get(const ESM4::Key& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Light>
        {
            static const std::string& get(const ESM4::Light& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::MiscItem>
        {
            static const std::string& get(const ESM4::MiscItem& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Potion>
        {
            static const std::string& get(const ESM4::Potion& record) { return record.mIcon; }
        };

        template <>
        struct InventoryIcon<ESM4::Weapon>
        {
            static const std::string& get(const ESM4::Weapon& record) { return record.mIcon; }
        };

        template <class Record>
        struct ItemValue
        {
            static int get(const Record&) { return 0; }
        };

        template <class Record>
        struct ItemWeight
        {
            static float get(const Record&) { return 0.f; }
        };

#define OPENMW_ESM4_VALUE_WEIGHT_TRAIT(Type, ValueExpr, WeightExpr)                                                   \
    template <>                                                                                                      \
    struct ItemValue<Type>                                                                                           \
    {                                                                                                                \
        static int get(const Type& record) { return static_cast<int>(ValueExpr); }                                    \
    };                                                                                                               \
    template <>                                                                                                      \
    struct ItemWeight<Type>                                                                                          \
    {                                                                                                                \
        static float get(const Type& record) { return static_cast<float>(WeightExpr); }                               \
    }

        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Ammunition, record.mData.mValue, record.mData.mWeight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Armor, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Book, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Clothing, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Ingredient, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::ItemMod, record.mData.mValue, record.mData.mWeight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Key, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Light, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::MiscItem, record.mData.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Potion, record.mItem.value, record.mData.weight);
        OPENMW_ESM4_VALUE_WEIGHT_TRAIT(ESM4::Weapon, record.mData.value, record.mData.weight);

#undef OPENMW_ESM4_VALUE_WEIGHT_TRAIT

        template <class LevelledRecord, class TargetRecord>
        const TargetRecord* resolveLevelled(const ESM::RefId& id, int level = sDefaultLevel)
        {
            if (id.empty())
                return nullptr;
            const MWWorld::ESMStore* esmStore = MWBase::Environment::get().getESMStore();
            const auto& targetStore = esmStore->get<TargetRecord>();
            const TargetRecord* res = targetStore.search(id);
            if (res)
                return res;
            const LevelledRecord* lvlRec = esmStore->get<LevelledRecord>().search(id);
            if (!lvlRec)
                return nullptr;
            for (const ESM4::LVLO& obj : lvlRec->mLvlObject)
            {
                ESM::RefId candidateId = ESM::FormId::fromUint32(obj.item);
                if (candidateId == id)
                    continue;
                const TargetRecord* candidate = resolveLevelled<LevelledRecord, TargetRecord>(candidateId, level);
                if (candidate && (!res || obj.level <= level))
                    res = candidate;
            }
            return res;
        }

        template <class LevelledRecord, class TargetRecord>
        void resolveLevelledAll(
            const ESM::RefId& id, std::vector<const TargetRecord*>& out, int level = sDefaultLevel, int depth = 0)
        {
            if (id.empty() || depth > 16)
                return;

            const MWWorld::ESMStore* esmStore = MWBase::Environment::get().getESMStore();
            const TargetRecord* record = esmStore->get<TargetRecord>().search(id);
            if (record != nullptr)
            {
                if (std::find(out.begin(), out.end(), record) == out.end())
                    out.push_back(record);
                return;
            }

            const LevelledRecord* lvlRec = esmStore->get<LevelledRecord>().search(id);
            if (lvlRec == nullptr)
                return;

            for (const ESM4::LVLO& obj : lvlRec->mLvlObject)
            {
                if (obj.level > level)
                    continue;

                const ESM::RefId candidateId = ESM::FormId::fromUint32(obj.item);
                if (candidateId == id)
                    continue;

                resolveLevelledAll<LevelledRecord, TargetRecord>(candidateId, out, level, depth + 1);
            }
        }

        // TODO: Figure out a better way to find markers and LOD meshes
        inline bool isMarkerModel(std::string_view model)
        {
            const std::size_t slash = model.find_last_of("/\\");
            if (slash != std::string_view::npos)
                model.remove_prefix(slash + 1);
            return Misc::StringUtils::ciStartsWith(model, "marker");
        }
        inline bool isLodModel(std::string_view model)
        {
            return Misc::StringUtils::ciEndsWith(model, "lod.nif");
        }
    }

    // Base for many ESM4 Classes
    template <typename Record>
    class ESM4Base : public MWWorld::Class
    {
        MWWorld::Ptr copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const override
        {
            const MWWorld::LiveCellRef<Record>* ref = ptr.get<Record>();
            return MWWorld::Ptr(cell.insert(ref), &cell);
        }

    protected:
        explicit ESM4Base(unsigned type)
            : MWWorld::Class(type)
        {
        }

    public:
        void insertObjectRendering(const MWWorld::Ptr& ptr, const std::string& model,
            MWRender::RenderingInterface& renderingInterface) const override
        {
            ESM4Impl::insertObjectRendering(ptr, model, renderingInterface);
        }

        void insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            insertObjectPhysics(ptr, model, rotation, physics);
        }

        void insertObjectPhysics(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            ESM4Impl::insertObjectPhysics(ptr, model, rotation, physics);
        }

        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override { return false; }

        std::string_view getName(const MWWorld::ConstPtr& ptr) const override { return {}; }

        int getValue(const MWWorld::ConstPtr& ptr) const override
        {
            return ESM4Impl::ItemValue<Record>::get(*ptr.get<Record>()->mBase);
        }

        float getWeight(const MWWorld::ConstPtr& ptr) const override
        {
            return ESM4Impl::ItemWeight<Record>::get(*ptr.get<Record>()->mBase);
        }

        const ESM::RefId& getUpSoundId(const MWWorld::ConstPtr& ptr) const override
        {
            static const ESM::RefId sEmpty;
            return sEmpty;
        }

        const ESM::RefId& getDownSoundId(const MWWorld::ConstPtr& ptr) const override
        {
            static const ESM::RefId sEmpty;
            return sEmpty;
        }

        std::string_view getModel(const MWWorld::ConstPtr& ptr) const override
        {
            std::string_view model = getClassModel<Record>(ptr);

            // TODO: There should be a better way to hide markers
            if (ESM4Impl::isMarkerModel(model) || ESM4Impl::isLodModel(model))
                return {};

            return model;
        }
    };

    class ESM4Static final : public MWWorld::RegisteredClass<ESM4Static, ESM4Base<ESM4::Static>>
    {
        friend MWWorld::RegisteredClass<ESM4Static, ESM4Base<ESM4::Static>>;
        ESM4Static()
            : MWWorld::RegisteredClass<ESM4Static, ESM4Base<ESM4::Static>>(ESM4::Static::sRecordId)
        {
        }
    };

    class ESM4Tree final : public MWWorld::RegisteredClass<ESM4Tree, ESM4Base<ESM4::Tree>>
    {
        friend MWWorld::RegisteredClass<ESM4Tree, ESM4Base<ESM4::Tree>>;
        ESM4Tree()
            : MWWorld::RegisteredClass<ESM4Tree, ESM4Base<ESM4::Tree>>(ESM4::Tree::sRecordId)
        {
        }
    };

    // For records with `mFullName` that should be shown as a tooltip.
    // All objects with a tooltip can be activated (activation can be handled in Lua).
    template <typename Record>
    class ESM4Named : public MWWorld::RegisteredClass<ESM4Named<Record>, ESM4Base<Record>>
    {
    public:
        ESM4Named()
            : MWWorld::RegisteredClass<ESM4Named, ESM4Base<Record>>(Record::sRecordId)
        {
        }

        std::string_view getName(const MWWorld::ConstPtr& ptr) const override
        {
            return ptr.get<Record>()->mBase->mFullName;
        }

        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override
        {
            return ESM4Impl::getToolTipInfo(getName(ptr), count);
        }

        const std::string& getInventoryIcon(const MWWorld::ConstPtr& ptr) const override
        {
            return ESM4Impl::InventoryIcon<Record>::get(*ptr.get<Record>()->mBase);
        }

        std::pair<std::vector<int>, bool> getEquipmentSlots(const MWWorld::ConstPtr& ptr) const override
        {
            if constexpr (std::is_same_v<Record, ESM4::Weapon>)
                return { { MWWorld::InventoryStore::Slot_CarriedRight }, false };
            else if constexpr (std::is_same_v<Record, ESM4::Ammunition>)
                return { { MWWorld::InventoryStore::Slot_Ammunition }, true };
            else if constexpr (std::is_same_v<Record, ESM4::Armor>)
            {
                const std::uint32_t flags = ptr.get<ESM4::Armor>()->mBase->mArmorFlags;
                constexpr std::uint32_t head = ESM4::Armor::FO3_Head | ESM4::Armor::FO3_Hair
                    | ESM4::Armor::FO3_Headband | ESM4::Armor::FO3_Hat | ESM4::Armor::FO3_EyeGlasses
                    | ESM4::Armor::FO3_NoseRing | ESM4::Armor::FO3_Earrings | ESM4::Armor::FO3_Mask
                    | ESM4::Armor::FO3_MouthObject;
                if ((flags & ESM4::Armor::FO3_UpperBody) != 0)
                    return { { MWWorld::InventoryStore::Slot_Robe }, false };
                if ((flags & head) != 0)
                    return { { MWWorld::InventoryStore::Slot_Helmet }, false };
                if ((flags & (ESM4::Armor::FO3_Necklace | ESM4::Armor::FO3_Choker)) != 0)
                    return { { MWWorld::InventoryStore::Slot_Amulet }, false };
                if ((flags & ESM4::Armor::FO3_LeftHand) != 0)
                    return { { MWWorld::InventoryStore::Slot_LeftGauntlet }, false };
                if ((flags & ESM4::Armor::FO3_RightHand) != 0)
                    return { { MWWorld::InventoryStore::Slot_RightGauntlet }, false };
                return { { MWWorld::InventoryStore::Slot_Cuirass }, false };
            }
            else
                return {};
        }

        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override { return !getName(ptr).empty(); }
    };

    class ESM4Door final : public MWWorld::RegisteredClass<ESM4Door, ESM4Base<ESM4::Door>>
    {
        friend MWWorld::RegisteredClass<ESM4Door, ESM4Base<ESM4::Door>>;

        ESM4Door()
            : MWWorld::RegisteredClass<ESM4Door, ESM4Base<ESM4::Door>>(ESM4::Door::sRecordId)
        {
        }

        void ensureCustomData(const MWWorld::Ptr& ptr) const
        {
            if (!ptr.getRefData().getCustomData())
                ptr.getRefData().setCustomData(std::make_unique<DoorCustomData>());
        }

    public:
        void insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            ESM4Base<ESM4::Door>::insertObject(ptr, model, rotation, physics);

            if (ptr.getRefData().getCustomData())
            {
                const DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();
                if (customData.mDoorState != MWWorld::DoorState::Idle)
                    MWBase::Environment::get().getWorld()->activateDoor(ptr, customData.mDoorState);
            }
        }

        bool isDoor() const override { return true; }

        bool useAnim() const override { return true; }

        std::string_view getName(const MWWorld::ConstPtr& ptr) const override { return ptr.get<ESM4::Door>()->mBase->mFullName; }

        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override
        {
            return ESM4Impl::getToolTipInfo(getName(ptr), count);
        }

        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override { return !getName(ptr).empty(); }

        bool canLock(const MWWorld::ConstPtr& ptr) const override { return true; }

        std::unique_ptr<MWWorld::Action> activate(const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const override
        {
            const MWWorld::LiveCellRef<ESM4::Door>* ref = ptr.get<ESM4::Door>();
            const ESM::RefId openSound(ref->mBase->mOpenSound);
            const ESM::RefId closeSound(ref->mBase->mCloseSound);

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
                    action->setSound(ESM::RefId::stringRefId("LockedDoor"));
                    return action;
                }
            }

            if (ptr.getCellRef().getTeleport())
            {
                std::unique_ptr<MWWorld::Action> action = std::make_unique<MWWorld::ActionTeleport>(
                    ptr.getCellRef().getDestCell(), ptr.getCellRef().getDoorDest(), true);
                action->setSound(openSound);
                return action;
            }

            std::unique_ptr<MWWorld::Action> action = std::make_unique<MWWorld::ActionDoor>(ptr);
            action->setSound(getDoorState(ptr) == MWWorld::DoorState::Opening ? closeSound : openSound);
            return action;
        }

        MWWorld::DoorState getDoorState(const MWWorld::ConstPtr& ptr) const override
        {
            if (!ptr.getRefData().getCustomData())
                return MWWorld::DoorState::Idle;
            return ptr.getRefData().getCustomData()->asDoorCustomData().mDoorState;
        }

        void setDoorState(const MWWorld::Ptr& ptr, MWWorld::DoorState state) const override
        {
            if (ptr.getCellRef().getTeleport())
                throw std::runtime_error("load doors can't be moved");

            ensureCustomData(ptr);
            ptr.getRefData().getCustomData()->asDoorCustomData().mDoorState = state;
        }
    };
}

#endif // GAME_MWCLASS_ESM4BASE_H
