#include "esm4base.hpp"

#include <MyGUI_TextIterator.h>
#include <MyGUI_UString.h>

#include <string>
#include <string_view>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwgui/tooltips.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwworld/actionteleport.hpp"
#include "../mwworld/nullaction.hpp"
#include "../mwworld/ptr.hpp"

namespace MWClass
{
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

    void ESM4Impl::insertStaticCollectionPhysics(
        const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation, MWPhysics::PhysicsSystem& physics)
    {
        insertObjectPhysics(ptr, model, rotation, physics);
    }

    MWGui::ToolTipInfo ESM4Impl::getToolTipInfo(std::string_view name, int count)
    {
        MWGui::ToolTipInfo info;
        info.caption = MyGUI::TextIterator::toTagsString(MyGUI::UString(std::string(name)))
            + MWGui::ToolTips::getCountString(count);
        return info;
    }

    bool ESM4Door::allowTelekinesis(const MWWorld::ConstPtr& ptr) const
    {
        return !ptr.getCellRef().getTeleport();
    }

    std::unique_ptr<MWWorld::Action> ESM4Door::activate(const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        if (!ptr.getCellRef().getTeleport())
        {
            Log(Debug::Warning) << "FNV/ESM4 door activate BLOCKED reason=non-teleport-door ref="
                                << ptr.getCellRef().getRefId();
            return std::make_unique<MWWorld::NullAction>();
        }

        if (actor != MWMechanics::getPlayer())
        {
            Log(Debug::Warning) << "FNV/ESM4 door activate BLOCKED reason=non-player-actor ref="
                                << ptr.getCellRef().getRefId();
            return std::make_unique<MWWorld::NullAction>();
        }

        const ESM::RefId destCell = ptr.getCellRef().getDestCell();
        if (destCell.empty())
        {
            Log(Debug::Warning) << "FNV/ESM4 door activate BLOCKED reason=missing-dest-cell ref="
                                << ptr.getCellRef().getRefId() << " destDoor=" << ptr.getCellRef().getEsm4DestDoor();
            return std::make_unique<MWWorld::NullAction>();
        }

        Log(Debug::Info) << "FNV/ESM4 door activate PASS ref=" << ptr.getCellRef().getRefId()
                         << " destCell=" << destCell << " destDoor=" << ptr.getCellRef().getEsm4DestDoor();
        return std::make_unique<MWWorld::ActionTeleport>(destCell, ptr.getCellRef().getDoorDest(), true);
    }
}
