#include "esm4base.hpp"

#include "esm4npc.hpp"

#include <MyGUI_TextIterator.h>
#include <MyGUI_UString.h>

#include <atomic>
#include <cstdlib>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwgui/tooltips.hpp"

#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/ptr.hpp"

namespace MWClass
{
    namespace
    {
        // Fallout furniture is a placed ESM4 reference, while its occupancy
        // belongs to the actor.  This action deliberately owns only that
        // generic reference-to-actor boundary: it does not know about any
        // quest, scene, or named furniture record.
        class ActionEsm4Furniture final : public MWWorld::Action
        {
        public:
            explicit ActionEsm4Furniture(const MWWorld::Ptr& furniture)
                : MWWorld::Action(false, furniture)
                , mFurniture(furniture)
            {
            }

        private:
            void executeImp(const MWWorld::Ptr& actor) override
            {
                MWBase::World* const world = MWBase::Environment::tryGetWorld();
                const bool playerActor = world != nullptr && actor == world->getPlayerPtr();
                const bool esm4NpcActor = !actor.isEmpty() && actor.getType() == ESM4::Npc::sRecordId;
                if (actor.isEmpty() || mFurniture.isEmpty() || (!playerActor && !esm4NpcActor)
                    || mFurniture.getCell() == nullptr)
                {
                    Log(Debug::Warning) << "FNV/ESM4 furniture: activation rejected actorPresent="
                                        << !actor.isEmpty() << " furniturePresent=" << !mFurniture.isEmpty();
                    return;
                }

                if (world == nullptr)
                {
                    Log(Debug::Warning) << "FNV/ESM4 furniture: activation rejected because world is unavailable";
                    return;
                }

                const ESM::FormId furnitureRef = mFurniture.getCellRef().getRefNum();
                const bool alreadySeated = playerActor ? world->getPlayer().isOnFalloutFurniture(furnitureRef)
                                                       : [&]() {
                                                             const FalloutFurniturePlacement existing
                                                                 = ESM4Npc::getFurniturePlacement(actor);
                                                             return existing.mValid && existing.mFurnitureRef == furnitureRef;
                                                         }();
                if (alreadySeated)
                {
                    if (playerActor)
                        world->getPlayer().clearFalloutFurniture();
                    else
                    {
                        ESM4Npc::setFurnitureState(actor, FalloutFurnitureState::None);
                        ESM4Npc::setFurniturePlacement(actor, {});
                    }
                    MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(actor);
                    Log(Debug::Info) << "FNV/ESM4 furniture: state=none actor="
                                     << actor.getCellRef().getRefNum().toString("FormId:")
                                     << " furniture=" << furnitureRef.toString("FormId:");
                    return;
                }

                const ESM::Position& furniturePosition = mFurniture.getRefData().getPosition();
                MWWorld::Ptr seatedActor = world->moveObject(
                    actor, mFurniture.getCell(), furniturePosition.asVec3(), true, true);
                world->rotateObject(seatedActor, osg::Vec3f(0.f, 0.f, furniturePosition.rot[2]),
                    MWBase::RotationFlag_none);

                if (playerActor)
                    world->getPlayer().setFalloutFurnitureRef(furnitureRef);
                else
                {
                    FalloutFurniturePlacement placement;
                    placement.mEntryPosition = furniturePosition.asVec3();
                    placement.mSettledPosition = furniturePosition.asVec3();
                    placement.mEntryYaw = furniturePosition.rot[2];
                    placement.mSettledYaw = furniturePosition.rot[2];
                    placement.mFurnitureRef = furnitureRef;
                    placement.mValid = true;
                    ESM4Npc::setFurniturePlacement(seatedActor, placement);
                    ESM4Npc::setFurnitureState(seatedActor, FalloutFurnitureState::Seated);
                }
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(seatedActor);

                Log(Debug::Info) << "FNV/ESM4 furniture: state=seated actor="
                                 << seatedActor.getCellRef().getRefNum().toString("FormId:")
                                 << " furniture=" << furnitureRef.toString("FormId:") << " position=("
                                 << furniturePosition.pos[0] << ',' << furniturePosition.pos[1] << ','
                                 << furniturePosition.pos[2] << ") yaw=" << furniturePosition.rot[2];
            }

            MWWorld::Ptr mFurniture;
        };
    }

    bool ESM4Impl::worldViewerDisableEsm4Actors()
    {
        return std::getenv("OPENMW_WORLD_VIEWER_DISABLE_ESM4_ACTORS") != nullptr;
    }

    bool ESM4Impl::worldViewerUseEsm4ActorProxies()
    {
        return std::getenv("OPENMW_WORLD_VIEWER_ESM4_ACTOR_PROXIES") != nullptr;
    }

    void ESM4Impl::logWorldViewerSkippedActor(const MWWorld::ConstPtr& ptr, std::string_view actorType)
    {
        static std::atomic<int> sSkippedActors { 0 };
        const int count = sSkippedActors.fetch_add(1);
        if (count >= 120)
            return;

        Log(Debug::Info) << "World viewer: skipped " << actorType << " actor "
                         << ptr.getCellRef().getRefNum().toString("FormId:")
                         << " base=" << ptr.getCellRef().getRefId().toDebugString()
                         << " because OPENMW_WORLD_VIEWER_DISABLE_ESM4_ACTORS is set";
    }

    void ESM4Impl::insertObjectRendering(
        const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface)
    {
        if (!model.empty())
        {
            renderingInterface.getObjects().insertModel(ptr, model);
            ptr.getRefData().getBaseNode()->setNodeMask(MWRender::Mask_Static);
        }
    }

    void ESM4Impl::insertObjectPhysics(
        const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation, MWPhysics::PhysicsSystem& physics)
    {
        physics.addObject(ptr, VFS::Path::toNormalized(model), rotation, MWPhysics::CollisionType_World);
    }

    MWGui::ToolTipInfo ESM4Impl::getToolTipInfo(std::string_view name, int count)
    {
        MWGui::ToolTipInfo info;
        info.caption = MyGUI::TextIterator::toTagsString(MyGUI::UString(name)) + MWGui::ToolTips::getCountString(count);
        return info;
    }

    std::unique_ptr<MWWorld::Action> ESM4Impl::activateEsm4Furniture(
        const MWWorld::Ptr& furniture, const MWWorld::Ptr& actor)
    {
        (void)actor;
        return std::make_unique<ActionEsm4Furniture>(furniture);
    }
}
