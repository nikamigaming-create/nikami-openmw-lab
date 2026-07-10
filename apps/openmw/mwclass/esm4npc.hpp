#ifndef GAME_MWCLASS_ESM4ACTOR_H
#define GAME_MWCLASS_ESM4ACTOR_H

#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwgui/tooltips.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/registeredclass.hpp"

#include "actor.hpp"
#include "esm4base.hpp"

namespace MWClass
{
    class ESM4Npc final : public MWWorld::RegisteredClass<ESM4Npc, Actor>
    {
    public:
        ESM4Npc()
            : MWWorld::RegisteredClass<ESM4Npc, Actor>(ESM4::Npc::sRecordId)
        {
        }

        MWWorld::Ptr copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const override
        {
            const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>();
            return MWWorld::Ptr(cell.insert(ref), &cell);
        }

        void insertObjectRendering(const MWWorld::Ptr& ptr, const std::string& model,
            MWRender::RenderingInterface& renderingInterface) const override
        {
            if (ESM4Impl::worldViewerDisableEsm4Actors() && !ESM4Impl::worldViewerUseEsm4ActorProxies())
            {
                ESM4Impl::logWorldViewerSkippedActor(ptr, "NPC");
                return;
            }
            renderingInterface.getObjects().insertNPC(ptr);
        }

        void insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            insertObjectPhysics(ptr, model, rotation, physics);
        }

        void insertObjectPhysics(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            if (ESM4Impl::worldViewerDisableEsm4Actors())
                return;
            (void)rotation;
            physics.addActor(ptr, VFS::Path::toNormalized(model.empty() ? std::string(getModel(ptr)) : model));
        }

        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override { return true; }
        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override
        {
            return ESM4Impl::getToolTipInfo(getName(ptr), count);
        }

        std::string_view getModel(const MWWorld::ConstPtr& ptr) const override;
        std::string_view getName(const MWWorld::ConstPtr& ptr) const override;
        MWMechanics::CreatureStats& getCreatureStats(const MWWorld::Ptr& ptr) const override;
        MWMechanics::Movement& getMovementSettings(const MWWorld::Ptr& ptr) const override;
        MWWorld::ContainerStore& getContainerStore(const MWWorld::Ptr& ptr) const override;
        float getCapacity(const MWWorld::Ptr& ptr) const override;
        float getMaxSpeed(const MWWorld::Ptr& ptr) const override;
        float getWalkSpeed(const MWWorld::Ptr& ptr) const override;
        float getRunSpeed(const MWWorld::Ptr& ptr) const override;
        float getSwimSpeed(const MWWorld::Ptr& ptr) const override;
        float getSkill(const MWWorld::Ptr& ptr, ESM::RefId id) const override;
        int getServices(const MWWorld::ConstPtr& ptr) const override;
        int getBaseGold(const MWWorld::ConstPtr& ptr) const override;
        std::unique_ptr<MWWorld::Action> activate(const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const override;
        bool isPersistent(const MWWorld::ConstPtr& ptr) const override;
        bool isBipedal(const MWWorld::ConstPtr& ptr) const override;
        bool canSwim(const MWWorld::ConstPtr& ptr) const override;
        bool canWalk(const MWWorld::ConstPtr& ptr) const override;

        static const ESM4::Npc* getTraitsRecord(const MWWorld::Ptr& ptr);
        static const ESM4::Npc* getModelRecord(const MWWorld::Ptr& ptr);
        static const ESM4::Npc* getAIPackageRecord(const MWWorld::Ptr& ptr);
        static const ESM4::Npc* getStatsRecord(const MWWorld::Ptr& ptr);
        static const ESM4::Npc* getBaseDataRecord(const MWWorld::Ptr& ptr);
        static const ESM4::Race* getRace(const MWWorld::Ptr& ptr);
        static bool isFemale(const MWWorld::Ptr& ptr);
        static const std::vector<const ESM4::Armor*>& getEquippedArmor(const MWWorld::Ptr& ptr);
        static const std::vector<const ESM4::Clothing*>& getEquippedClothing(const MWWorld::Ptr& ptr);
        static const ESM4::Weapon* getEquippedWeapon(const MWWorld::Ptr& ptr);
        static bool addEquippedArmor(const MWWorld::Ptr& ptr, const ESM4::Armor* armor);
        static std::string_view chooseEquipmentModel(const ESM4::Armor* rec, bool isFemale);
        static std::string_view chooseEquipmentModel(const ESM4::Clothing* rec, bool isFemale);

    private:
        static ESM4NpcCustomData& getCustomData(const MWWorld::ConstPtr& ptr);
    };
}

#endif // GAME_MWCLASS_ESM4ACTOR_H
