#include "characterpreview.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Fog>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LightModel>
#include <osg/LightSource>
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/PositionAttitudeTransform>
#include <osg/PolygonMode>
#include <osg/Quat>
#include <osg/TexEnvCombine>
#include <osg/Texture2D>
#include <osg/ValueObject>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "../mwclass/esm4npc.hpp"

#include "animation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "playervisualpolicy.hpp"
#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        const ESM4::Npc* findFalloutInventoryPlayerVisualRecord()
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return nullptr;

            const char* env = std::getenv("OPENMW_FNV_PLAYER_NPC");
            const std::string_view wanted = env != nullptr && *env != '\0' ? std::string_view(env) : "Player";
            const ESM4::Npc* fallback = nullptr;

            for (const ESM4::Npc& npc : store->get<ESM4::Npc>())
            {
                if (!npc.mIsFONV)
                    continue;
                if (Misc::StringUtils::ciEqual(npc.mEditorId, wanted))
                    return &npc;
                if (fallback == nullptr && Misc::StringUtils::ciEqual(npc.mEditorId, "Player"))
                    fallback = &npc;
            }

            return fallback;
        }

        const ESM4::Armor* findFalloutInventoryArmorByEditorId(const std::string_view editorId)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || editorId.empty())
                return nullptr;

            for (const ESM4::Armor& armor : store->get<ESM4::Armor>())
            {
                if (Misc::StringUtils::ciEqual(armor.mEditorId, editorId))
                    return &armor;
            }

            return nullptr;
        }

        bool isFalloutContentLoaded()
        {
            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (const std::string& file : world->getContentFiles())
            {
                if (file.find("FalloutNV.esm") != std::string::npos || file.find("falloutnv.esm") != std::string::npos)
                    return true;
            }

            return false;
        }

        bool shouldUseFalloutInventoryPlayerVisual()
        {
            if (std::getenv("OPENMW_FNV_INVENTORY_PLAYER_PROXY") != nullptr)
                return true;

            return isFalloutContentLoaded();
        }

        void applyFalloutInventoryPlayerProxyConfiguredEquipment(const MWWorld::Ptr& visualPtr)
        {
            const ESM4PlayerVisualEquipmentPolicy policy = resolveESM4PlayerVisualEquipmentPolicy(
                std::getenv("OPENMW_ESM4_PLAYER_OUTFIT"), std::getenv("OPENMW_FNV_PLAYER_OUTFIT"),
                std::getenv("OPENMW_ESM4_PLAYER_HEADGEAR"), std::getenv("OPENMW_FNV_PLAYER_HEADGEAR"),
                std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr);
            if (policy.mOutfit.empty() && policy.mHeadgear.empty())
                return;

            visualPtr.getRefData().setCustomData(nullptr);
            const auto addArmor = [&](std::string_view editorId, std::string_view role) {
                if (editorId.empty())
                    return;
                const ESM4::Armor* armor = findFalloutInventoryArmorByEditorId(editorId);
                if (armor == nullptr)
                {
                    Log(Debug::Warning) << "ESM4 player visual: inventory proxy " << role << " "
                                        << editorId << " not found";
                    return;
                }

                const bool added = MWClass::ESM4Npc::addEquippedArmorReplacingSlots(visualPtr, armor);
                Log(Debug::Info) << "ESM4 player visual: inventory proxy " << role << " "
                                 << armor->mEditorId << " model="
                                 << MWClass::ESM4Npc::chooseEquipmentModel(
                                        armor, MWClass::ESM4Npc::isFemale(visualPtr))
                                 << " added=" << added;
            };

            addArmor(policy.mOutfit, "outfit");
            addArmor(policy.mHeadgear, "headgear");
        }

        std::string trimFalloutPreviewText(const char* value)
        {
            if (value == nullptr)
                return {};

            std::string text(value);
            while (!text.empty()
                && (std::isspace(static_cast<unsigned char>(text.front())) || text.front() == '"'
                    || text.front() == '\''))
                text.erase(text.begin());
            while (!text.empty()
                && (std::isspace(static_cast<unsigned char>(text.back())) || text.back() == '"'
                    || text.back() == '\''))
                text.pop_back();
            return text;
        }

        std::string getFalloutPreviewAnimationGroup()
        {
            std::string group = trimFalloutPreviewText(std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP"));
            if (group.empty())
                group = "idle";
            Misc::StringUtils::lowerCaseInPlace(group);
            return group;
        }

        float getFalloutPreviewAnimationStartPoint()
        {
            const char* value = std::getenv("OPENMW_FNV_ACTOR_KIT_ANIMATION_STARTPOINT");
            if (value == nullptr || value[0] == '\0')
                return 0.35f;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return 0.35f;
            return std::clamp(parsed, 0.f, 0.999f);
        }

        bool shouldUseFalloutNeutralActorBindPose()
        {
            const char* value = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_BIND_POSE");
            return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
        }

        std::string_view getFalloutNeutralActorPreviewProfile()
        {
            const char* value = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE");
            if (value == nullptr || value[0] == '\0')
                return "full-body";
            return value;
        }

        float getFalloutNeutralActorPreviewFloat(const char* name, float fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || value[0] == '\0')
                return fallback;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed))
                return fallback;
            return parsed;
        }

        bool getFalloutNeutralActorPreviewBool(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
        }

        void applyFalloutNeutralActorOrbitCamera(FalloutActorPreview::ViewMode viewMode, float distance, float cameraZ,
            float lookAtZ, osg::Vec3f& position, osg::Vec3f& lookAt, const char*& viewName)
        {
            const float diagonal = distance * 0.70710678118f;
            position = osg::Vec3f(0.f, distance, cameraZ);
            lookAt = osg::Vec3f(0.f, 0.f, lookAtZ);
            viewName = "front";
            switch (viewMode)
            {
                case FalloutActorPreview::ViewMode::Front:
                    break;
                case FalloutActorPreview::ViewMode::FrontLeft:
                    position = osg::Vec3f(-diagonal, diagonal, cameraZ);
                    viewName = "front-left";
                    break;
                case FalloutActorPreview::ViewMode::FrontRight:
                    position = osg::Vec3f(diagonal, diagonal, cameraZ);
                    viewName = "front-right";
                    break;
                case FalloutActorPreview::ViewMode::Left:
                    position = osg::Vec3f(-distance, 0.f, cameraZ);
                    viewName = "left";
                    break;
                case FalloutActorPreview::ViewMode::Right:
                    position = osg::Vec3f(distance, 0.f, cameraZ);
                    viewName = "right";
                    break;
                case FalloutActorPreview::ViewMode::Top:
                    position = osg::Vec3f(0.f, 24.f, cameraZ + 210.f);
                    lookAt = osg::Vec3f(0.f, 0.f, lookAtZ - 8.f);
                    viewName = "top";
                    break;
                case FalloutActorPreview::ViewMode::Back:
                    position = osg::Vec3f(0.f, -distance, cameraZ);
                    viewName = "back";
                    break;
                case FalloutActorPreview::ViewMode::IsoNW:
                    position = osg::Vec3f(-diagonal, diagonal, cameraZ + distance * 0.35f);
                    viewName = "iso-nw";
                    break;
                case FalloutActorPreview::ViewMode::IsoSW:
                    position = osg::Vec3f(-diagonal, -diagonal, cameraZ + distance * 0.35f);
                    viewName = "iso-sw";
                    break;
            }
        }

        void applyFalloutNeutralActorPreviewYawOffset(osg::Vec3f& position, const osg::Vec3f& lookAt, float degrees)
        {
            if (!std::isfinite(degrees) || std::abs(degrees) < 0.001f)
                return;

            const float radians = osg::DegreesToRadians(degrees);
            const float sinYaw = std::sin(radians);
            const float cosYaw = std::cos(radians);
            const osg::Vec3f delta = position - lookAt;
            position = lookAt + osg::Vec3f(
                                      delta.x() * cosYaw - delta.y() * sinYaw,
                                      delta.x() * sinYaw + delta.y() * cosYaw, delta.z());
        }

        osg::Vec3f getWorldPosition(const osg::Node* node)
        {
            if (node == nullptr)
                return osg::Vec3f();

            osg::NodePathList paths = node->getParentalNodePaths();
            if (paths.empty())
                return osg::Vec3f();

            return osg::computeLocalToWorld(paths.front()).getTrans();
        }

        bool isValidPoint(const osg::Vec3f& value)
        {
            return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z())
                && value.length2() > 0.001f;
        }

        float pointDistance(const osg::Vec3f& a, const osg::Vec3f& b)
        {
            if (!isValidPoint(a) || !isValidPoint(b))
                return 0.f;
            return (a - b).length();
        }

        std::string formatVec3(const osg::Vec3f& value)
        {
            std::ostringstream stream;
            stream << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
            return stream.str();
        }

        osg::ref_ptr<osg::Geode> makeFalloutPreviewAxisGeode(std::string_view name, float size, bool secondaryColors)
        {
            osg::ref_ptr<osg::Geode> geode = new osg::Geode;
            geode->setName(std::string(name));
            geode->setCullingActive(false);

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
            const osg::Vec4f xColor = secondaryColors ? osg::Vec4f(1.f, 0.f, 1.f, 1.f) : osg::Vec4f(1.f, 0.f, 0.f, 1.f);
            const osg::Vec4f yColor = secondaryColors ? osg::Vec4f(0.f, 1.f, 1.f, 1.f) : osg::Vec4f(0.f, 1.f, 0.f, 1.f);
            const osg::Vec4f zColor = secondaryColors ? osg::Vec4f(1.f, 1.f, 0.f, 1.f) : osg::Vec4f(0.1f, 0.35f, 1.f, 1.f);
            const auto addAxis = [&](const osg::Vec3f& axis, const osg::Vec4f& color) {
                vertices->push_back(osg::Vec3f());
                vertices->push_back(axis * size);
                colors->push_back(color);
                colors->push_back(color);
            };
            addAxis(osg::Vec3f(1.f, 0.f, 0.f), xColor);
            addAxis(osg::Vec3f(0.f, 1.f, 0.f), yColor);
            addAxis(osg::Vec3f(0.f, 0.f, 1.f), zColor);

            geometry->setVertexArray(vertices);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, vertices->size()));
            geode->addDrawable(geometry);
            osg::StateSet* stateSet = geode->getOrCreateStateSet();
            stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
            stateSet->setAttributeAndModes(new osg::LineWidth(6.f), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            return geode;
        }

        class FalloutActorPreviewDrawableAuditVisitor : public osg::NodeVisitor
        {
        public:
            FalloutActorPreviewDrawableAuditVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                if (node.getNodeMask() == 0)
                    return;
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override { auditDrawable(drawable); }

            void apply(osg::Geode& geode) override
            {
                if (geode.getNodeMask() == 0)
                    return;

                ++mGeodes;
                for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
                {
                    osg::Drawable* drawable = geode.getDrawable(index);
                    if (drawable == nullptr)
                        continue;

                    auditDrawable(*drawable);
                }

                traverse(geode);
            }

            unsigned int getGeodes() const { return mGeodes; }
            unsigned int getDrawables() const { return mDrawables; }
            unsigned int getGeometry() const { return mGeometry; }
            unsigned int getVertices() const { return mVertices; }
            const osg::BoundingBox& getBounds() const { return mBounds; }

        private:
            void auditGeometry(osg::Geometry& geometry)
            {
                ++mGeometry;
                if (const osg::Array* vertices = geometry.getVertexArray())
                    mVertices += vertices->getNumElements();
            }

            void auditDrawable(osg::Drawable& drawable)
            {
                if (drawable.getNodeMask() == 0)
                    return;

                ++mDrawables;
                const osg::BoundingBox bound = drawable.getBoundingBox();
                if (bound.valid())
                    mBounds.expandBy(bound);

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (osg::Geometry* renderGeometry = rig->getLastFrameGeometry())
                        auditGeometry(*renderGeometry);
                    else if (osg::ref_ptr<osg::Geometry> sourceGeometry = rig->getSourceGeometry())
                        auditGeometry(*sourceGeometry);
                    return;
                }

                if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                {
                    if (SceneUtil::OsgaRigGeometry* renderGeometry = holder->getGeometry(0))
                        auditGeometry(*renderGeometry);
                    return;
                }

                if (osg::Geometry* geometry = drawable.asGeometry())
                    auditGeometry(*geometry);
            }

            unsigned int mGeodes = 0;
            unsigned int mDrawables = 0;
            unsigned int mGeometry = 0;
            unsigned int mVertices = 0;
            osg::BoundingBox mBounds;
        };

        class FalloutActorPreviewRigWarmupVisitor : public osg::NodeVisitor
        {
        public:
            explicit FalloutActorPreviewRigWarmupVisitor(const osg::NodeVisitor* sourceVisitor)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                if (sourceVisitor != nullptr)
                {
                    setTraversalNumber(sourceVisitor->getTraversalNumber());
                    setFrameStamp(const_cast<osg::FrameStamp*>(sourceVisitor->getFrameStamp()));
                }
            }

            void apply(osg::Node& node) override
            {
                if (node.getNodeMask() == 0)
                    return;

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&node))
                    warmRig(*rig);
                else if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&node))
                    warmHolder(*holder);

                traverse(node);
            }

            void apply(osg::Drawable& drawable) override { warmDrawable(drawable); }

            void apply(osg::Geode& geode) override
            {
                if (geode.getNodeMask() == 0)
                    return;

                for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
                {
                    osg::Drawable* drawable = geode.getDrawable(index);
                    if (drawable == nullptr)
                        continue;

                    warmDrawable(*drawable);
                }

                traverse(geode);
            }

            unsigned int getRigGeometryCount() const { return mRigGeometry; }
            unsigned int getRigGeometryBoundCount() const { return mRigGeometryBound; }
            unsigned int getRigGeometryRefreshedCount() const { return mRigGeometryRefreshed; }
            unsigned int getHolderCount() const { return mHolders; }
            const osg::BoundingBox& getBounds() const { return mBounds; }

        private:
            void warmDrawable(osg::Drawable& drawable)
            {
                if (drawable.getNodeMask() == 0)
                    return;

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                    warmRig(*rig);
                else if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
                    warmHolder(*holder);
            }

            void warmRig(SceneUtil::RigGeometry& rig)
            {
                if (!rig.isFalloutCharacterRig())
                    return;

                ++mRigGeometry;

                osg::BoundingBox currentPoseBounds;
                if (rig.computeCurrentFalloutSkinningBounds(this, currentPoseBounds))
                {
                    ++mRigGeometryBound;
                    if (currentPoseBounds.valid())
                        mBounds.expandBy(currentPoseBounds);
                }

                rig.forceNextUpdate();
                if (rig.refreshFalloutSkinningForCurrentPose())
                    ++mRigGeometryRefreshed;
            }

            void warmHolder(SceneUtil::RigGeometryHolder& holder)
            {
                ++mHolders;
                holder.forceNextUpdate();
            }

            unsigned int mRigGeometry = 0;
            unsigned int mRigGeometryBound = 0;
            unsigned int mRigGeometryRefreshed = 0;
            unsigned int mHolders = 0;
            osg::BoundingBox mBounds;
        };
    }


    class DrawOnceCallback : public SceneUtil::NodeCallback<DrawOnceCallback>
    {
    public:
        DrawOnceCallback(osg::Node* subgraph)
            : mRendered(false)
            , mLastRenderedFrame(0)
            , mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mRendered)
            {
                mRendered = true;

                mLastRenderedFrame = nv->getTraversalNumber();

                osg::ref_ptr<osg::FrameStamp> previousFramestamp = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
                osg::FrameStamp* fs = new osg::FrameStamp(*previousFramestamp);
                fs->setSimulationTime(mSimulationTime);

                nv->setFrameStamp(fs);

                // Update keyframe controllers in the scene graph first...
                // RTTNode does not continue update traversal, so manually continue the update traversal since we need
                // it.
                mSubgraph->accept(*nv);
                FalloutActorPreviewRigWarmupVisitor rigWarmup(nv);
                mSubgraph->accept(rigWarmup);
                if (rigWarmup.getRigGeometryCount() > 0 || rigWarmup.getHolderCount() > 0)
                {
                    const osg::BoundingBox& bounds = rigWarmup.getBounds();
                    Log(Debug::Info)
                        << "FNV/ESM4 proof: neutral actor preview rig warmup"
                        << " frame=" << rigWarmup.getTraversalNumber()
                        << " rigGeometry=" << rigWarmup.getRigGeometryCount()
                        << " currentPoseBounds=" << rigWarmup.getRigGeometryBoundCount()
                        << " refreshedRigGeometry=" << rigWarmup.getRigGeometryRefreshedCount()
                        << " holders=" << rigWarmup.getHolderCount()
                        << " boundsValid=" << bounds.valid()
                        << " boundsCenter=" << formatVec3(bounds.valid() ? bounds.center() : osg::Vec3f())
                        << " runtime="
                        << (rigWarmup.getRigGeometryCount() == rigWarmup.getRigGeometryRefreshedCount()
                                ? "runtime-supported"
                                : "loaded-pending-runtime")
                        << " gate=runtime-neutral-actor-preview-rig-warmup";
                }
                traverse(node, nv);

                nv->setFrameStamp(previousFramestamp);
            }
            else
            {
                node->setNodeMask(0);
            }
        }

        void redrawNextFrame() { mRendered = false; }
        void setSimulationTime(double simulationTime) { mSimulationTime = simulationTime; }

        unsigned int getLastRenderedFrame() const { return mLastRenderedFrame; }

    private:
        bool mRendered;
        unsigned int mLastRenderedFrame;
        double mSimulationTime = 0.0;
        osg::ref_ptr<osg::Node> mSubgraph;
    };

    // Set up alpha blending mode to avoid issues caused by transparent objects writing onto the alpha value of the FBO
    // This makes the RTT have premultiplied alpha, though, so the source blend factor must be GL_ONE when it's applied
    class SetUpBlendVisitor : public osg::NodeVisitor
    {
    public:
        SetUpBlendVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (osg::ref_ptr<osg::StateSet> stateset = node.getStateSet())
            {
                osg::ref_ptr<osg::StateSet> newStateSet;
                if (stateset->getAttribute(osg::StateAttribute::BLENDFUNC)
                    || stateset->getBinNumber() == osg::StateSet::TRANSPARENT_BIN)
                {
                    osg::BlendFunc* blendFunc
                        = static_cast<osg::BlendFunc*>(stateset->getAttribute(osg::StateAttribute::BLENDFUNC));

                    if (blendFunc)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                        osg::ref_ptr<osg::BlendFunc> newBlendFunc = new osg::BlendFunc(*blendFunc);
                        newStateSet->setAttribute(newBlendFunc, osg::StateAttribute::ON);
                        // I *think* (based on some by-hand maths) that the RGB and dest alpha factors are unchanged,
                        // and only dest determines source alpha factor This has the benefit of being idempotent if we
                        // assume nothing used glBlendFuncSeparate before we touched it
                        if (blendFunc->getDestination() == osg::BlendFunc::ONE_MINUS_SRC_ALPHA)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ONE);
                        else if (blendFunc->getDestination() == osg::BlendFunc::ONE)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ZERO);
                        // Other setups barely exist in the wild and aren't worth supporting as they're not equippable
                        // gear
                        else
                            Log(Debug::Info) << "Unable to adjust blend mode for character preview. Source factor 0x"
                                             << std::hex << blendFunc->getSource() << ", destination factor 0x"
                                             << blendFunc->getDestination() << std::dec;
                    }
                }
                if (stateset->getMode(GL_BLEND) & osg::StateAttribute::ON)
                {
                    if (!newStateSet)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                    }
                    // Disable noBlendAlphaEnv
                    newStateSet->setTextureMode(7, GL_TEXTURE_2D, osg::StateAttribute::OFF);
                    newStateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON);
                }
            }
            traverse(node);
        }
    };

    class CharacterPreviewRTTNode : public SceneUtil::RTTNode
    {
        static constexpr float fovYDegrees = 12.3f;
        static constexpr float znear = 4.0f;
        static constexpr float zfar = 10000.f;

    public:
        CharacterPreviewRTTNode(uint32_t sizeX, uint32_t sizeY)
            : RTTNode(sizeX, sizeY, Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mAspectRatio(static_cast<float>(sizeX) / static_cast<float>(sizeY))
        {
            if (SceneUtil::AutoDepth::isReversed())
                mPerspectiveMatrix = static_cast<osg::Matrixf>(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar));
            else
                mPerspectiveMatrix = osg::Matrixf::perspective(fovYDegrees, mAspectRatio, znear, zfar);
            mGroup->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix", mPerspectiveMatrix));
            mViewMatrix = osg::Matrixf::identity();
            setColorBufferInternalFormat(GL_RGBA);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("CharacterPreview");
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(osg::Vec4(0.f, 0.f, 0.f, 0.f));
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar);
            camera->setViewport(0, 0, width(), height());
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setCullMask(~(Mask_UpdateVisitor));
            camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            SceneUtil::setCameraClearDepth(camera);

            camera->setNodeMask(Mask_RenderToTexture);
            camera->addChild(mGroup);
        }

        void apply(osg::Camera* camera) override
        {
            if (mCameraStateset)
                camera->setStateSet(mCameraStateset);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mPerspectiveMatrix, mPerspectiveMatrix });
        }

        void addChild(osg::Node* node) { mGroup->addChild(node); }

        void setCameraStateset(osg::StateSet* stateset) { mCameraStateset = stateset; }

        void setViewMatrix(const osg::Matrixf& viewMatrix) { mViewMatrix = viewMatrix; }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;
        osg::Matrixf mPerspectiveMatrix;
        osg::Matrixf mViewMatrix;
        osg::ref_ptr<osg::StateSet> mCameraStateset;
        float mAspectRatio;
    };

    CharacterPreview::CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mPosition(position)
        , mLookAt(lookAt)
        , mCharacter(character)
        , mAnimation(nullptr)
        , mSizeX(sizeX)
        , mSizeY(sizeY)
    {
        mTextureStateSet = new osg::StateSet;
        mTextureStateSet->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));

        mRTTNode = new CharacterPreviewRTTNode(sizeX, sizeY);
        mRTTNode->setNodeMask(Mask_RenderToTexture);

        osg::ref_ptr<SceneUtil::LightManager> lightManager = new SceneUtil::LightManager(SceneUtil::LightSettings{
            .mLightingMethod = mResourceSystem->getSceneManager()->getLightingMethod(),
            .mMaxLights = Settings::shaders().mMaxLights,
            .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
            .mLightFadeStart = Settings::shaders().mLightFadeStart,
            .mLightBoundsMultiplier = Settings::shaders().mLightBoundsMultiplier,
        });
        lightManager->setStartLight(1);
        osg::ref_ptr<osg::StateSet> stateset = lightManager->getOrCreateStateSet();
        stateset->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
        stateset->setMode(GL_LIGHTING, osg::StateAttribute::ON);
        stateset->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
        osg::ref_ptr<osg::Material> defaultMat(new osg::Material);
        defaultMat->setColorMode(isFalloutContentLoaded() ? osg::Material::AMBIENT_AND_DIFFUSE
                                                          : osg::Material::OFF);
        defaultMat->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
        defaultMat->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
        stateset->setAttribute(defaultMat);

        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

        // assign large value to effectively turn off fog
        // shaders don't respect glDisable(GL_FOG)
        osg::ref_ptr<osg::Fog> fog(new osg::Fog);
        fog->setStart(10000000);
        fog->setEnd(10000000);
        stateset->setAttributeAndModes(fog, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

        // TODO: Clean up this mess of loose uniforms that shaders depend on.
        // turn off sky blending
        int skyTextureSlot = mResourceSystem->getSceneManager()->getShaderManager().reserveGlobalTextureUnits(
            Shader::ShaderManager::Slot::SkyTexture);
        stateset->addUniform(new osg::Uniform("far", 10000000.0f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
        stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{ 1, 1 }));
        stateset->addUniform(new osg::Uniform("sky", skyTextureSlot));
        stateset->addUniform(new osg::Uniform("emissiveMult", 1.f));

        // Opaque stuff must have 1 as its fragment alpha as the FBO is translucent, so having blending off isn't enough
        osg::ref_ptr<osg::TexEnvCombine> noBlendAlphaEnv = new osg::TexEnvCombine();
        noBlendAlphaEnv->setCombine_Alpha(osg::TexEnvCombine::REPLACE);
        noBlendAlphaEnv->setSource0_Alpha(osg::TexEnvCombine::CONSTANT);
        noBlendAlphaEnv->setConstantColor(osg::Vec4(0.0, 0.0, 0.0, 1.0));
        noBlendAlphaEnv->setCombine_RGB(osg::TexEnvCombine::REPLACE);
        noBlendAlphaEnv->setSource0_RGB(osg::TexEnvCombine::PREVIOUS);
        osg::ref_ptr<osg::Texture2D> dummyTexture = new osg::Texture2D();
        dummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        dummyTexture->setInternalFormat(GL_DEPTH_COMPONENT);
        dummyTexture->setTextureSize(1, 1);
        // This might clash with a shadow map, so make sure it doesn't cast shadows
        dummyTexture->setShadowComparison(true);
        dummyTexture->setShadowCompareFunc(osg::Texture::ShadowCompareFunc::ALWAYS);
        stateset->setTextureAttributeAndModes(7, dummyTexture, osg::StateAttribute::ON);
        stateset->setTextureAttribute(7, noBlendAlphaEnv, osg::StateAttribute::ON);

        osg::ref_ptr<osg::LightModel> lightmodel = new osg::LightModel;
        lightmodel->setAmbientIntensity(osg::Vec4(0.0, 0.0, 0.0, 1.0));
        stateset->setAttributeAndModes(lightmodel, osg::StateAttribute::ON);

        osg::ref_ptr<osg::Light> light = new osg::Light;
        float diffuseR = Fallback::Map::getFloat("Inventory_DirectionalDiffuseR");
        float diffuseG = Fallback::Map::getFloat("Inventory_DirectionalDiffuseG");
        float diffuseB = Fallback::Map::getFloat("Inventory_DirectionalDiffuseB");
        float ambientR = Fallback::Map::getFloat("Inventory_DirectionalAmbientR");
        float ambientG = Fallback::Map::getFloat("Inventory_DirectionalAmbientG");
        float ambientB = Fallback::Map::getFloat("Inventory_DirectionalAmbientB");
        float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
        float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));
        float positionX = -std::cos(azimuth) * std::sin(altitude);
        float positionY = std::sin(azimuth) * std::sin(altitude);
        float positionZ = std::cos(altitude);
        light->setPosition(osg::Vec4(positionX, positionY, positionZ, 0.0));
        light->setDiffuse(osg::Vec4(diffuseR, diffuseG, diffuseB, 1));
        osg::Vec4 ambientRGBA = osg::Vec4(ambientR, ambientG, ambientB, 1);
        if (isFalloutContentLoaded())
        {
            light->setDiffuse(osg::Vec4(
                std::max(diffuseR, 0.95f), std::max(diffuseG, 0.95f), std::max(diffuseB, 0.95f), 1));
            ambientRGBA = osg::Vec4(
                std::max(ambientR, 0.45f), std::max(ambientG, 0.45f), std::max(ambientB, 0.45f), 1);
        }
        if (mResourceSystem->getSceneManager()->getForceShaders())
        {
            // When using shaders, we now skip the ambient sun calculation as this is the only place it's used.
            // Using the scene ambient will give identical results.
            lightmodel->setAmbientIntensity(ambientRGBA);
            light->setAmbient(osg::Vec4(0, 0, 0, 1));
        }
        else
            light->setAmbient(ambientRGBA);
        light->setSpecular(osg::Vec4(0, 0, 0, 0));
        light->setLightNum(0);
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        lightManager->setSunlight(light);

        osg::ref_ptr<osg::LightSource> lightSource = new osg::LightSource;
        lightSource->setLight(light);

        lightSource->setStateSetModes(*stateset, osg::StateAttribute::ON);

        lightManager->addChild(lightSource);

        mRTTNode->addChild(lightManager);

        mNode = new osg::PositionAttitudeTransform;
        lightManager->addChild(mNode);

        mDrawOnceCallback = new DrawOnceCallback(mRTTNode->mGroup);
        mRTTNode->addUpdateCallback(mDrawOnceCallback);

        mParent->addChild(mRTTNode);

        mCharacter.mCell = nullptr;
    }

    CharacterPreview::~CharacterPreview()
    {
        mParent->removeChild(mRTTNode);
    }

    int CharacterPreview::getTextureWidth() const
    {
        return mSizeX;
    }

    int CharacterPreview::getTextureHeight() const
    {
        return mSizeY;
    }

    void CharacterPreview::setBlendMode()
    {
        SetUpBlendVisitor visitor;
        mNode->accept(visitor);
    }

    void CharacterPreview::setRedrawSimulationTime(double simulationTime)
    {
        mDrawOnceCallback->setSimulationTime(simulationTime);
    }

    void CharacterPreview::onSetup()
    {
        setBlendMode();
    }

    osg::ref_ptr<Animation> CharacterPreview::createAnimation()
    {
        return new NpcAnimation(
            mCharacter, mNode, mResourceSystem, true, (renderHeadOnly() ? NpcAnimation::VM_HeadOnly : NpcAnimation::VM_Normal));
    }

    osg::ref_ptr<osg::Texture2D> CharacterPreview::getTexture()
    {
        return static_cast<osg::Texture2D*>(mRTTNode->getColorTexture(nullptr));
    }

    void CharacterPreview::rebuild()
    {
        mAnimation = nullptr;

        mAnimation = createAnimation();

        onSetup();

        redraw();
    }

    void CharacterPreview::redraw()
    {
        mRTTNode->setNodeMask(Mask_RenderToTexture);
        mDrawOnceCallback->redrawNextFrame();
    }

    void CharacterPreview::updateLive(double simulationTime)
    {
        if (!mAnimation)
            return;

        mAnimation->runAnimation(static_cast<float>(simulationTime));
        setRedrawSimulationTime(simulationTime);
        setBlendMode();
        redraw();
    }

    // --------------------------------------------------------------------------------------------------

    InventoryPreview::InventoryPreview(
        osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character, ViewMode viewMode)
        : CharacterPreview(parent, resourceSystem, character, 512, 1024, osg::Vec3f(0, 700, 71), osg::Vec3f(0, 0, 71))
        , mViewMode(viewMode)
    {
        mNode->setName("FNV Inventory Paper Doll Preview");
    }

    osg::ref_ptr<Animation> InventoryPreview::createAnimation()
    {
        if (shouldUseFalloutInventoryPlayerVisual())
        {
            if (const ESM4::Npc* falloutPlayerVisual = findFalloutInventoryPlayerVisualRecord())
            {
                ESM::CellRef proxyRef;
                proxyRef.blank();
                proxyRef.mRefID = ESM::RefId::stringRefId("Player");
                mFalloutPreviewRef = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, falloutPlayerVisual);
                MWWorld::Ptr visualPtr(mFalloutPreviewRef.get(), nullptr);
                visualPtr.getRefData().setCustomData(std::unique_ptr<MWWorld::CustomData>());
                applyFalloutInventoryPlayerProxyConfiguredEquipment(visualPtr);

                Log(Debug::Info) << "FNV/ESM4 proof: using Fallout inventory player visual proxy "
                                 << falloutPlayerVisual->mEditorId << " (" << ESM::RefId(falloutPlayerVisual->mId)
                                 << ")";
                return new ESM4NpcAnimation(visualPtr, mNode, mResourceSystem);
            }

            Log(Debug::Warning) << "FNV/ESM4 proof: requested Fallout inventory player visual proxy, but no FONV Player NPC was found";
        }

        mFalloutPreviewRef.reset();
        if (mCharacter.getType() == ESM4::Npc::sRecordId)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: using live Fallout inventory player visual "
                             << mCharacter.toString();
            return new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
        }

        return CharacterPreview::createAnimation();
    }

    void InventoryPreview::setViewport(int sizeX, int sizeY)
    {
        sizeX = std::max(sizeX, 0);
        sizeY = std::max(sizeY, 0);

        // NB Camera::setViewport has threading issues
        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        mViewport = new osg::Viewport(0, mSizeY - sizeY, std::min(mSizeX, sizeX), std::min(mSizeY, sizeY));
        stateset->setAttributeAndModes(mViewport);
        mRTTNode->setCameraStateset(stateset);

        redraw();
    }

    void InventoryPreview::update()
    {
        if (!mAnimation.get())
            return;

        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
        {
            const float previewStart = mFalloutPreviewRef != nullptr ? 0.35f : 0.0f;
            mDrawOnceCallback->setSimulationTime(previewStart);
            if (mAnimation->hasAnimation("idle"))
                mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart, 0);
            mAnimation->runAnimation(previewStart);
            setBlendMode();
            redraw();
            return;
        }

        npcAnimation->showWeapons(true);
        npcAnimation->updateParts();

        MWWorld::InventoryStore& inv = mCharacter.getClass().getInventoryStore(mCharacter);
        MWWorld::ContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        std::string groupname = "inventoryhandtohand";
        bool showCarriedLeft = true;
        if (iter != inv.end())
        {
            groupname = "inventoryweapononehand";
            if (iter->getType() == ESM::Weapon::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Weapon>* ref = iter->get<ESM::Weapon>();
                int type = ref->mBase->mData.mType;
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(type);
                showCarriedLeft = !(weaponInfo->mFlags & ESM::WeaponType::TwoHanded);

                std::string inventoryGroup = weaponInfo->mLongGroup;
                inventoryGroup = "inventory" + inventoryGroup;

                // We still should use one-handed animation as fallback
                if (mAnimation->hasAnimation(inventoryGroup))
                    groupname = std::move(inventoryGroup);
                else
                {
                    static const std::string oneHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeOneHand)->mLongGroup;
                    static const std::string twoHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::Weapon::LongBladeTwoHand)->mLongGroup;

                    // For real two-handed melee weapons use 2h swords animations as fallback, otherwise use the 1h ones
                    if (weaponInfo->mFlags & ESM::WeaponType::TwoHanded
                        && weaponInfo->mWeaponClass == ESM::WeaponType::Melee)
                        groupname = twoHandFallback;
                    else
                        groupname = oneHandFallback;
                }
            }
        }

        npcAnimation->showCarriedLeft(showCarriedLeft);

        const bool falloutPreview = mFalloutPreviewRef != nullptr || mCharacter.getType() == ESM4::Npc::sRecordId;
        if (falloutPreview && !npcAnimation->hasAnimation(groupname) && npcAnimation->hasAnimation("idle"))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll using Fallout idle pose instead of missing "
                             << groupname;
            groupname = "idle";
        }

        const float previewStart = 0.0f;
        mCurrentAnimGroup = std::move(groupname);
        npcAnimation->play(
            mCurrentAnimGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", previewStart, 0);

        MWWorld::ConstContainerStoreIterator torch = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (torch != inv.end() && torch->getType() == ESM::Light::sRecordId && showCarriedLeft)
        {
            if (!npcAnimation->getInfo("torch"))
                npcAnimation->play("torch", 2, BlendMask::BlendMask_LeftArm, false, 1.0f, "start", "stop", 0.0f,
                    std::numeric_limits<uint32_t>::max(), true);
        }
        else if (npcAnimation->getInfo("torch"))
            npcAnimation->disable("torch");

        npcAnimation->runAnimation(previewStart);
        mDrawOnceCallback->setSimulationTime(previewStart);

        setBlendMode();

        redraw();
    }

    int InventoryPreview::getSlotSelected(int posX, int posY)
    {
        if (!mViewport)
            return -1;
        NpcAnimation* npcAnimation = dynamic_cast<NpcAnimation*>(mAnimation.get());
        if (npcAnimation == nullptr)
            return -1;

        float projX = (posX / mViewport->width()) * 2 - 1.f;
        float projY = (posY / mViewport->height()) * 2 - 1.f;
        // With Intersector::WINDOW, the intersection ratios are slightly inaccurate. Seems to be a
        // precision issue - compiling with OSG_USE_FLOAT_MATRIX=0, Intersector::WINDOW works ok.
        // Using Intersector::PROJECTION results in better precision because the start/end points and the model matrices
        // don't go through as many transformations.
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::Intersector::PROJECTION, projX, projY));

        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
        // Set the traversal number from the last draw, so that the frame switch used for RigGeometry double buffering
        // works correctly
        visitor.setTraversalNumber(mDrawOnceCallback->getLastRenderedFrame());

        auto* camera = mRTTNode->getCamera(nullptr);
        osg::Node::NodeMask nodeMask = camera->getNodeMask();
        camera->setNodeMask(~0u);
        camera->accept(visitor);
        camera->setNodeMask(nodeMask);

        if (intersector->containsIntersections())
        {
            osgUtil::LineSegmentIntersector::Intersection intersection = intersector->getFirstIntersection();
            return npcAnimation->getSlot(intersection.nodePath);
        }
        return -1;
    }

    void InventoryPreview::updatePtr(const MWWorld::Ptr& ptr)
    {
        mCharacter = MWWorld::Ptr(ptr.getBase(), nullptr);
    }

    void InventoryPreview::onSetup()
    {
        CharacterPreview::onSetup();
        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);

        mNode->setScale(scale);

        osg::Vec3f position = mPosition;
        osg::Vec3f lookAt = mLookAt;
        if (mFalloutPreviewRef != nullptr)
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0, 700, 76);
                    lookAt = osg::Vec3f(0, 0, 76);
                    break;
                case ViewMode::Profile:
                    position = osg::Vec3f(320, 0, 104);
                    lookAt = osg::Vec3f(0, 0, 104);
                    break;
                case ViewMode::Top:
                    position = osg::Vec3f(0, -230, 310);
                    lookAt = osg::Vec3f(0, 0, 112);
                    break;
            }
            Log(Debug::Info) << "FNV/ESM4 proof: inventory paper doll camera "
                             << (mViewMode == ViewMode::Front ? "front"
                                     : (mViewMode == ViewMode::Profile ? "profile" : "top"))
                             << " position=(" << position.x() << "," << position.y() << "," << position.z()
                             << ") lookAt=(" << lookAt.x() << "," << lookAt.y() << "," << lookAt.z() << ")";
        }

        osg::Vec3f up = osg::Vec3f(0, 0, 1);
        auto viewMatrix = osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), up);
        mRTTNode->setViewMatrix(viewMatrix);
        if (mFalloutPreviewRef != nullptr)
            update();
    }

    // --------------------------------------------------------------------------------------------------

    RaceSelectionPreview::RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : CharacterPreview(
            parent, resourceSystem, MWMechanics::getPlayer(), 512, 512, osg::Vec3f(0, 125, 8), osg::Vec3f(0, 0, 8))
        , mBase(*mCharacter.get<ESM::NPC>()->mBase)
        , mRef(ESM::makeBlankCellRef(), &mBase)
        , mPitchRadians(osg::DegreesToRadians(6.f))
    {
        mCharacter = MWWorld::Ptr(&mRef, nullptr);
    }

    RaceSelectionPreview::~RaceSelectionPreview() {}

    void RaceSelectionPreview::setAngle(float angleRadians)
    {
        mNode->setAttitude(osg::Quat(mPitchRadians, osg::Vec3(1, 0, 0)) * osg::Quat(angleRadians, osg::Vec3(0, 0, 1)));
        redraw();
    }

    void RaceSelectionPreview::setPrototype(const ESM::NPC& proto)
    {
        mBase = proto;
        mBase.mId = ESM::RefId::stringRefId("Player");
        rebuild();
    }

    class UpdateCameraCallback : public SceneUtil::NodeCallback<UpdateCameraCallback, CharacterPreviewRTTNode*>
    {
    public:
        UpdateCameraCallback(
            osg::ref_ptr<const osg::Node> nodeToFollow, const osg::Vec3& posOffset, const osg::Vec3& lookAtOffset)
            : mNodeToFollow(std::move(nodeToFollow))
            , mPosOffset(posOffset)
            , mLookAtOffset(lookAtOffset)
        {
        }

        void operator()(CharacterPreviewRTTNode* node, osg::NodeVisitor* nv)
        {
            // Update keyframe controllers in the scene graph first...
            traverse(node, nv);

            // Now update camera utilizing the updated head position
            osg::NodePathList nodepaths = mNodeToFollow->getParentalNodePaths();
            if (nodepaths.empty())
                return;
            osg::Matrix worldMat = osg::computeLocalToWorld(nodepaths[0]);
            osg::Vec3 headOffset = worldMat.getTrans();

            auto viewMatrix
                = osg::Matrixf::lookAt(headOffset + mPosOffset, headOffset + mLookAtOffset, osg::Vec3(0, 0, 1));
            node->setViewMatrix(viewMatrix);
        }

    private:
        osg::ref_ptr<const osg::Node> mNodeToFollow;
        osg::Vec3 mPosOffset;
        osg::Vec3 mLookAtOffset;
    };

    void RaceSelectionPreview::onSetup()
    {
        CharacterPreview::onSetup();
        mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);
        mAnimation->runAnimation(0.f);

        // attach camera to follow the head node
        if (mUpdateCameraCallback)
            mRTTNode->removeUpdateCallback(mUpdateCameraCallback);

        const osg::Node* head = mAnimation->getNode("Bip01 Head");
        if (head)
        {
            mUpdateCameraCallback = new UpdateCameraCallback(head, mPosition, mLookAt);
            mRTTNode->addUpdateCallback(mUpdateCameraCallback);
        }
        else
            Log(Debug::Error) << "Error: Bip01 Head node not found";
    }

    // --------------------------------------------------------------------------------------------------

    FalloutActorPreview::FalloutActorPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, ViewMode viewMode, float cameraDistanceMultiplier, std::string profileOverride,
        osg::Vec3f editorRotation, float editorScale)
        : CharacterPreview(parent, resourceSystem, MWWorld::Ptr(character.getBase(), nullptr), 720, 720,
            osg::Vec3f(0, 420, 112), osg::Vec3f(0, 0, 112))
        , mViewMode(viewMode)
        , mCameraDistanceMultiplier(cameraDistanceMultiplier)
        , mProfileOverride(std::move(profileOverride))
        , mEditorRotation(editorRotation)
        , mEditorScale(editorScale)
    {
        mNode->setName("FNV Neutral Actor Preview");
    }

    osg::ref_ptr<Animation> FalloutActorPreview::createAnimation()
    {
        if (mCharacter.getType() == ESM4::Npc::sRecordId)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using ESM4NpcAnimation for "
                             << mCharacter.toString();
            osg::ref_ptr<ESM4NpcAnimation> animation = new ESM4NpcAnimation(mCharacter, mNode, mResourceSystem);
            animation->setProofPreviewAnimation(true);
            animation->setProofPreviewGameplayAudit(mViewMode == ViewMode::Front);
            return animation;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview using base CharacterPreview animation for "
                         << mCharacter.toString();
        osg::ref_ptr<Animation> animation = CharacterPreview::createAnimation();
        if (animation != nullptr)
        {
            animation->setProofPreviewAnimation(true);
            animation->setProofPreviewGameplayAudit(mViewMode == ViewMode::Front);
        }
        return animation;
    }

    void FalloutActorPreview::onSetup()
    {
        CharacterPreview::onSetup();

        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);
        const float editorScale = std::clamp(mEditorScale, 0.05f, 10.f);
        mNode->setScale(scale * editorScale);
        mNode->setAttitude(osg::Quat(osg::DegreesToRadians(mEditorRotation.x()), osg::Vec3f(1.f, 0.f, 0.f),
            osg::DegreesToRadians(mEditorRotation.y()), osg::Vec3f(0.f, 1.f, 0.f),
            osg::DegreesToRadians(mEditorRotation.z()), osg::Vec3f(0.f, 0.f, 1.f)));

        osg::Vec3f position(0.f, 420.f, 112.f);
        osg::Vec3f lookAt(0.f, 0.f, 112.f);
        const char* viewName = "front";
        const std::string envProfile(getFalloutNeutralActorPreviewProfile());
        const std::string profile = !mProfileOverride.empty() ? mProfileOverride : envProfile;

        if (Misc::StringUtils::ciEqual(profile, "full-body") || Misc::StringUtils::ciEqual(profile, "fullbody"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 760.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 78.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 78.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "face"))
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 190.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 116.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 116.f), position, lookAt,
                viewName);
        }
        else if (Misc::StringUtils::ciEqual(profile, "hands") || Misc::StringUtils::ciEqual(profile, "weapon-arms")
            || Misc::StringUtils::ciEqual(profile, "arms"))
        {
            switch (mViewMode)
            {
                case ViewMode::Front:
                    position = osg::Vec3f(0.f, 520.f, 82.f);
                    lookAt = osg::Vec3f(0.f, 0.f, 82.f);
                    viewName = "arm-elbow-hands-wide";
                    break;
                case ViewMode::FrontLeft:
                    position = osg::Vec3f(-42.f, 500.f, 82.f);
                    lookAt = osg::Vec3f(-28.f, 0.f, 82.f);
                    viewName = "left-elbow-hand";
                    break;
                case ViewMode::FrontRight:
                    position = osg::Vec3f(42.f, 500.f, 82.f);
                    lookAt = osg::Vec3f(28.f, 0.f, 82.f);
                    viewName = "right-elbow-hand";
                    break;
                case ViewMode::Left:
                case ViewMode::Right:
                case ViewMode::Top:
                case ViewMode::Back:
                case ViewMode::IsoNW:
                case ViewMode::IsoSW:
                    applyFalloutNeutralActorOrbitCamera(mViewMode,
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 420.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 112.f),
                        getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 112.f),
                        position, lookAt, viewName);
                    break;
            }
        }
        else
        {
            applyFalloutNeutralActorOrbitCamera(mViewMode,
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_DISTANCE", 420.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_CAMERA_Z", 112.f),
                getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_LOOK_Z", 112.f), position, lookAt,
                viewName);
        }

        const float yawOffsetDeg
            = getFalloutNeutralActorPreviewFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_YAW_OFFSET_DEG", 0.f);
        applyFalloutNeutralActorPreviewYawOffset(position, lookAt, yawOffsetDeg);
        position = lookAt + (position - lookAt) * std::clamp(mCameraDistanceMultiplier, 0.15f, 12.f);
        mRTTNode->setViewMatrix(osg::Matrixf::lookAt(position * scale.z(), lookAt * scale.z(), osg::Vec3f(0, 0, 1)));

        const std::string animationGroup = getFalloutPreviewAnimationGroup();
        const float previewStart = getFalloutPreviewAnimationStartPoint();
        const bool bindPose = shouldUseFalloutNeutralActorBindPose();
        if (mAnimation)
        {
            if (!bindPose && mAnimation->hasAnimation(animationGroup))
                mAnimation->play(animationGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop",
                    previewStart, std::numeric_limits<uint32_t>::max(), true);
            mAnimation->runAnimation(0.0f);
        }
        setRedrawSimulationTime(bindPose ? 0.0 : previewStart);

        if (getFalloutNeutralActorPreviewBool("OPENMW_FNV_DRAW_PART_AXES"))
        {
            mNode->addChild(makeFalloutPreviewAxisGeode("FNV Studio Axis Actor Root", 30.f, false));
            osg::Group* headNode = mAnimation
                ? dynamic_cast<osg::Group*>(const_cast<osg::Node*>(mAnimation->getNode("Bip01 Head")))
                : nullptr;
            if (headNode != nullptr)
                headNode->addChild(makeFalloutPreviewAxisGeode("FNV Studio Axis Head", 18.f, true));
        }

        osg::ComputeBoundsVisitor boundsVisitor;
        boundsVisitor.setTraversalMask(~(Mask_ParticleSystem | Mask_Effect));
        mNode->accept(boundsVisitor);
        const osg::BoundingBox& bounds = boundsVisitor.getBoundingBox();
        const bool boundsValid = bounds.valid();
        const osg::Vec3f boundsCenter = boundsValid ? bounds.center() : osg::Vec3f();
        const osg::Vec3f boundsSize = boundsValid
            ? osg::Vec3f(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(), bounds.zMax() - bounds.zMin())
            : osg::Vec3f();

        const osg::Vec3f head = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 Head") : nullptr);
        const osg::Vec3f pelvis = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 Pelvis") : nullptr);
        const osg::Vec3f leftShoulder = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L UpperArm") : nullptr);
        const osg::Vec3f rightShoulder = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R UpperArm") : nullptr);
        const osg::Vec3f leftElbow = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Forearm") : nullptr);
        const osg::Vec3f rightElbow = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Forearm") : nullptr);
        const osg::Vec3f leftHand = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Hand") : nullptr);
        const osg::Vec3f rightHand = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Hand") : nullptr);
        const osg::Vec3f leftHip = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Thigh") : nullptr);
        const osg::Vec3f rightHip = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Thigh") : nullptr);
        const osg::Vec3f leftKnee = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Calf") : nullptr);
        const osg::Vec3f rightKnee = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Calf") : nullptr);
        const osg::Vec3f leftFoot = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 L Foot") : nullptr);
        const osg::Vec3f rightFoot = getWorldPosition(mAnimation ? mAnimation->getNode("Bip01 R Foot") : nullptr);

        const float feetZ = (leftFoot.z() + rightFoot.z()) * 0.5f;
        const float headAboveFeet = head.z() - feetZ;
        const float headAbovePelvis = head.z() - pelvis.z();
        const float shoulderSpan = pointDistance(leftShoulder, rightShoulder);
        const float handSpan = pointDistance(leftHand, rightHand);
        const float footSpan = pointDistance(leftFoot, rightFoot);
        const float leftUpperArm = pointDistance(leftShoulder, leftElbow);
        const float leftForearm = pointDistance(leftElbow, leftHand);
        const float rightUpperArm = pointDistance(rightShoulder, rightElbow);
        const float rightForearm = pointDistance(rightElbow, rightHand);
        const float leftArmReach = pointDistance(leftShoulder, leftHand);
        const float rightArmReach = pointDistance(rightShoulder, rightHand);
        const float leftThigh = pointDistance(leftHip, leftKnee);
        const float leftShin = pointDistance(leftKnee, leftFoot);
        const float rightThigh = pointDistance(rightHip, rightKnee);
        const float rightShin = pointDistance(rightKnee, rightFoot);
        const bool upright = boundsValid && headAboveFeet > 70.f && headAbovePelvis > 25.f && boundsSize.z() > 70.f;
        const bool armsHuman = shoulderSpan >= 8.f && shoulderSpan <= 45.f && handSpan <= 80.f
            && leftUpperArm > 5.f && leftUpperArm < 42.f && leftForearm > 5.f && leftForearm < 42.f
            && rightUpperArm > 5.f && rightUpperArm < 42.f && rightForearm > 5.f && rightForearm < 42.f
            && leftArmReach < 70.f && rightArmReach < 70.f;
        const bool legsHuman = footSpan < 75.f && leftThigh > 8.f && leftThigh < 55.f && leftShin > 8.f
            && leftShin < 55.f && rightThigh > 8.f && rightThigh < 55.f && rightShin > 8.f && rightShin < 55.f;
        const bool human = upright && armsHuman && legsHuman;

        FalloutActorPreviewDrawableAuditVisitor drawableAudit;
        mNode->accept(drawableAudit);

        Log(human ? Debug::Info : Debug::Warning)
            << "FNV/ESM4 proof: neutral actor preview anatomy view=" << viewName
            << " actor=" << mCharacter.toString()
            << " profile=" << profile
            << " boundsValid=" << boundsValid
            << " boundsCenter=" << formatVec3(boundsCenter)
            << " boundsSize=" << formatVec3(boundsSize)
            << " head=" << formatVec3(head)
            << " pelvis=" << formatVec3(pelvis)
            << " leftShoulder=" << formatVec3(leftShoulder)
            << " rightShoulder=" << formatVec3(rightShoulder)
            << " leftElbow=" << formatVec3(leftElbow)
            << " rightElbow=" << formatVec3(rightElbow)
            << " leftHand=" << formatVec3(leftHand)
            << " rightHand=" << formatVec3(rightHand)
            << " leftHip=" << formatVec3(leftHip)
            << " rightHip=" << formatVec3(rightHip)
            << " leftKnee=" << formatVec3(leftKnee)
            << " rightKnee=" << formatVec3(rightKnee)
            << " leftFoot=" << formatVec3(leftFoot)
            << " rightFoot=" << formatVec3(rightFoot)
            << " headAboveFeet=" << headAboveFeet
            << " headAbovePelvis=" << headAbovePelvis
            << " shoulderSpan=" << shoulderSpan
            << " handSpan=" << handSpan
            << " footSpan=" << footSpan
            << " leftUpperArm=" << leftUpperArm
            << " leftForearm=" << leftForearm
            << " rightUpperArm=" << rightUpperArm
            << " rightForearm=" << rightForearm
            << " leftArmReach=" << leftArmReach
            << " rightArmReach=" << rightArmReach
            << " leftThigh=" << leftThigh
            << " leftShin=" << leftShin
            << " rightThigh=" << rightThigh
            << " rightShin=" << rightShin
            << " geodes=" << drawableAudit.getGeodes()
            << " drawables=" << drawableAudit.getDrawables()
            << " geometry=" << drawableAudit.getGeometry()
            << " vertices=" << drawableAudit.getVertices()
            << " animationGroup=" << animationGroup
            << " startPoint=" << previewStart
            << " bindPose=" << bindPose
            << " verdict=" << (human ? "OK" : "BAD")
            << " runtime=" << (human ? "runtime-supported" : "loaded-pending-runtime")
            << " gate=runtime-neutral-actor-preview-anatomy";

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview camera view=" << viewName
                         << " position=" << formatVec3(position)
                         << " lookAt=" << formatVec3(lookAt)
                         << " profile=" << profile
                         << " yawOffsetDeg=" << yawOffsetDeg
                         << " zoom=" << mCameraDistanceMultiplier
                         << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
    }

}
