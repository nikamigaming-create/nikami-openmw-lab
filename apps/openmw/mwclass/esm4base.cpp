#include "esm4base.hpp"

#include <MyGUI_TextIterator.h>
#include <MyGUI_UString.h>

#include <atomic>
#include <cstdlib>

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwgui/tooltips.hpp"

#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwworld/ptr.hpp"

namespace MWClass
{
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
}
