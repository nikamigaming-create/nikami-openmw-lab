#ifndef GAME_MWCLASS_ESM4CREATURE_H
#define GAME_MWCLASS_ESM4CREATURE_H

#include <components/esm4/loadcrea.hpp>
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

namespace ESM
{
    struct CreatureState;
}

namespace MWWorld
{
    class ESMStore;
}

namespace MWClass
{
    class ESM4Creature final : public MWWorld::RegisteredClass<ESM4Creature, Actor>
    {
    public:
        ESM4Creature()
            : MWWorld::RegisteredClass<ESM4Creature, Actor>(ESM4::Creature::sRecordId)
        {
        }

        MWWorld::Ptr copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const override
        {
            const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>();
            return MWWorld::Ptr(cell.insert(ref), &cell);
        }

        void insertObjectRendering(const MWWorld::Ptr& ptr, const std::string& model,
            MWRender::RenderingInterface& renderingInterface) const override
        {
            if (ESM4Impl::worldViewerDisableEsm4Actors() && !ESM4Impl::worldViewerUseEsm4ActorProxies())
            {
                ESM4Impl::logWorldViewerSkippedActor(ptr, "creature");
                return;
            }
            renderingInterface.getObjects().insertCreature(ptr, model, false);
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

        std::string_view getModel(const MWWorld::ConstPtr& ptr) const override;
        std::string_view getName(const MWWorld::ConstPtr& ptr) const override;
        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override;
        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override;
        MWMechanics::CreatureStats& getCreatureStats(const MWWorld::Ptr& ptr) const override;
        MWWorld::ContainerStore& getContainerStore(const MWWorld::Ptr& ptr) const override;
        MWMechanics::Movement& getMovementSettings(const MWWorld::Ptr& ptr) const override;
        float getCapacity(const MWWorld::Ptr& ptr) const override;
        float getMaxSpeed(const MWWorld::Ptr& ptr) const override;
        float getWalkSpeed(const MWWorld::Ptr& ptr) const override;
        float getRunSpeed(const MWWorld::Ptr& ptr) const override;
        float getSwimSpeed(const MWWorld::Ptr& ptr) const override;
        float getSkill(const MWWorld::Ptr& ptr, ESM::RefId id) const override;
        bool isPersistent(const MWWorld::ConstPtr& ptr) const override;
        bool canFly(const MWWorld::ConstPtr& ptr) const override;
        bool canSwim(const MWWorld::ConstPtr& ptr) const override;
        bool canWalk(const MWWorld::ConstPtr& ptr) const override;
        void adjustScale(const MWWorld::ConstPtr& ptr, osg::Vec3f& scale, bool rendering) const override;

        void readAdditionalState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const override;
        void writeAdditionalState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const override;

        /// Validate all fallible FNV creature payload data before LiveCellRef applies the enclosing CellRef/RefData.
        static bool validateState(const ESM4::Creature& creature, const ESM::CreatureState& state,
            const MWWorld::ESMStore& store, std::string& error);

    private:
        static class ESM4CreatureCustomData& getCustomData(const MWWorld::ConstPtr& ptr);
    };
}

#endif
