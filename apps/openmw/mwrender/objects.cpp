#include "objects.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include <osg/Group>
#include <osg/Geode>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/StateSet>
#include <osg/UserDataContainer>

#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/unrefqueue.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "vismask.hpp"

namespace MWRender
{
    namespace
    {
        bool envEnabled(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0' && value[0] != '0';
        }

        bool worldViewerDiagnosticFilterEnabled()
        {
            return envEnabled("OPENMW_WORLD_VIEWER_HIDE_DIAGNOSTIC_MODELS")
                || envEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
        }

        bool worldViewerEsm4ActorProxyEnabled()
        {
            return envEnabled("OPENMW_WORLD_VIEWER_ESM4_ACTOR_PROXIES");
        }

        bool contains(std::string_view value, std::string_view needle)
        {
            return value.find(needle) != std::string_view::npos;
        }

        std::string worldViewerFocusActorNeedle()
        {
            const char* value = std::getenv("OPENMW_WORLD_VIEWER_FOCUS_ACTOR");
            if (!value || *value == '\0')
                return {};

            return Misc::StringUtils::lowerCase(value);
        }

        void appendWorldViewerFocusField(std::string& haystack, std::string_view value)
        {
            if (value.empty())
                return;

            haystack += ' ';
            haystack += Misc::StringUtils::lowerCase(value);
        }

        bool worldViewerActorMatchesFocus(
            const MWWorld::Ptr& ptr, std::string_view model, const std::string& needle)
        {
            if (needle.empty())
                return true;

            std::string haystack;
            appendWorldViewerFocusField(haystack, ptr.getClass().getName(ptr));
            appendWorldViewerFocusField(haystack, ptr.getTypeDescription());
            appendWorldViewerFocusField(haystack, model);
            appendWorldViewerFocusField(haystack, ptr.getCellRef().getRefNum().toString("FormId:"));
            appendWorldViewerFocusField(haystack, ptr.getCellRef().getRefId().toDebugString());
            appendWorldViewerFocusField(haystack, ptr.toString());

            return contains(haystack, needle);
        }

        bool shouldHideWorldViewerNonFocusedActor(
            const MWWorld::Ptr& ptr, std::string_view actorKind, std::string_view model)
        {
            const std::string needle = worldViewerFocusActorNeedle();
            if (worldViewerActorMatchesFocus(ptr, model, needle))
                return false;

            static std::atomic<int> skipLogCount{ 0 };
            const int logIndex = skipLogCount.fetch_add(1);
            if (logIndex < 160)
            {
                const ESM::Position& pos = ptr.getRefData().getPosition();
                Log(Debug::Info) << "World viewer actor ledger: phase=focus-hide"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                 << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                 << " type=\"" << ptr.getTypeDescription() << "\""
                                 << " actorKind=" << actorKind
                                 << " focus=\"" << needle << "\""
                                 << " name=\"" << ptr.getClass().getName(ptr) << "\""
                                 << " model=\"" << model << "\""
                                 << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
            }
            else if (logIndex == 160)
                Log(Debug::Info) << "World viewer actor ledger: further focus-hide logs suppressed";

            return true;
        }

        bool isWorldViewerDiagnosticModel(std::string_view mesh)
        {
            if (mesh.empty())
                return false;

            std::string lower = Misc::StringUtils::lowerCase(mesh);
            std::replace(lower.begin(), lower.end(), '\\', '/');
            std::string_view value(lower);

            return contains(value, "meshes/markers/")
                || contains(value, "/editormarkers/")
                || contains(value, "staticcollectionpivotdummy")
                || contains(value, "fill_planes/")
                || contains(value, "fillplane_")
                || contains(value, "occlusion")
                || contains(value, "occluder")
                || contains(value, "portalmarker")
                || contains(value, "roommarker")
                || contains(value, "loadmarker")
                || contains(value, "xmarker")
                || contains(value, "headingmarker")
                || contains(value, "triggerbox")
                || contains(value, "collisionmarker")
                || contains(value, "water/water");
        }

        bool skipWorldViewerDiagnosticModel(const MWWorld::Ptr& ptr, std::string_view mesh)
        {
            if (!worldViewerDiagnosticFilterEnabled() || !isWorldViewerDiagnosticModel(mesh))
                return false;

            static std::atomic<int> skipLogCount{ 0 };
            const int logIndex = skipLogCount.fetch_add(1);
            if (logIndex < 160)
            {
                Log(Debug::Info) << "World viewer: skipped diagnostic model base="
                                 << ptr.getCellRef().getRefId().toDebugString()
                                 << " type=" << ptr.getTypeDescription()
                                 << " model=" << mesh
                                 << " ptr=" << ptr.toString();
            }
            else if (logIndex == 160)
                Log(Debug::Info) << "World viewer: further diagnostic model skip logs suppressed";

            return true;
        }

        class WorldViewerActorProxyAnimation final : public Animation
        {
        public:
            WorldViewerActorProxyAnimation(const MWWorld::Ptr& ptr, Resource::ResourceSystem* resourceSystem,
                std::string_view model, bool creature)
                : Animation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
                , mCreature(creature)
                , mAnimate(envEnabled("OPENMW_WORLD_VIEWER_ESM4_ACTOR_PROXY_ANIMATE"))
            {
                osg::Group* root = getOrCreateObjectRoot();
                root->setName(creature ? "World Viewer ESM4 Creature Proxy T-Pose"
                                       : "World Viewer ESM4 Actor Proxy T-Pose");

                mPoseRoot = new osg::MatrixTransform;
                mPoseRoot->setName("World Viewer ESM4 Actor Proxy Pose Root");
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                osg::StateSet* stateSet = geode->getOrCreateStateSet();
                osg::ref_ptr<osg::Material> material = new osg::Material;
                material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
                material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1.f, 1.f, 1.f, 1.f));
                material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 1.f));
                stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
                stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                const bool flatActorMaterials = envEnabled("OPENMW_WORLD_VIEWER_FORCE_FLAT_ACTOR_MATERIALS");
                const osg::Vec4f bodyColor = flatActorMaterials
                    ? osg::Vec4f(0.76f, 0.78f, 0.74f, 1.f)
                    : (creature ? osg::Vec4f(0.86f, 0.44f, 0.18f, 1.f)
                                : osg::Vec4f(0.12f, 0.42f, 0.95f, 1.f));
                const osg::Vec4f limbColor = flatActorMaterials
                    ? osg::Vec4f(0.52f, 0.54f, 0.52f, 1.f)
                    : (creature ? osg::Vec4f(0.42f, 0.22f, 0.10f, 1.f)
                                : osg::Vec4f(0.08f, 0.18f, 0.38f, 1.f));
                const osg::Vec4f headColor = flatActorMaterials
                    ? osg::Vec4f(0.86f, 0.80f, 0.70f, 1.f)
                    : (creature ? osg::Vec4f(0.96f, 0.68f, 0.30f, 1.f)
                                : osg::Vec4f(0.88f, 0.78f, 0.62f, 1.f));

                auto addCapsule = [&](osg::Vec3f center, float radius, float height, const osg::Vec4f& color,
                                      const osg::Quat& rotation = osg::Quat()) {
                    osg::ref_ptr<osg::Capsule> shape = new osg::Capsule(center, radius, height);
                    shape->setRotation(rotation);
                    auto* drawable = new osg::ShapeDrawable(shape);
                    drawable->setColor(color);
                    geode->addDrawable(drawable);
                };
                auto addSphere = [&](osg::Vec3f center, float radius, const osg::Vec4f& color) {
                    auto* drawable = new osg::ShapeDrawable(new osg::Sphere(center, radius));
                    drawable->setColor(color);
                    geode->addDrawable(drawable);
                };

                if (creature)
                {
                    const osg::Quat forward(1.57079632679f, osg::Vec3f(0.f, 1.f, 0.f));
                    addCapsule(osg::Vec3f(0.f, 0.f, 46.f), 24.f, 92.f, bodyColor, forward);
                    addSphere(osg::Vec3f(56.f, 0.f, 54.f), 20.f, headColor);
                    addCapsule(osg::Vec3f(-28.f, -20.f, 22.f), 7.f, 44.f, limbColor);
                    addCapsule(osg::Vec3f(-28.f, 20.f, 22.f), 7.f, 44.f, limbColor);
                    addCapsule(osg::Vec3f(28.f, -20.f, 22.f), 7.f, 44.f, limbColor);
                    addCapsule(osg::Vec3f(28.f, 20.f, 22.f), 7.f, 44.f, limbColor);
                }
                else
                {
                    const osg::Quat across(1.57079632679f, osg::Vec3f(0.f, 1.f, 0.f));
                    addCapsule(osg::Vec3f(0.f, 0.f, 86.f), 21.f, 104.f, bodyColor);
                    addSphere(osg::Vec3f(0.f, 0.f, 152.f), 21.f, headColor);
                    addCapsule(osg::Vec3f(-46.f, 0.f, 116.f), 7.f, 72.f, limbColor, across);
                    addCapsule(osg::Vec3f(46.f, 0.f, 116.f), 7.f, 72.f, limbColor, across);
                    addCapsule(osg::Vec3f(-12.f, 0.f, 30.f), 8.f, 58.f, limbColor);
                    addCapsule(osg::Vec3f(12.f, 0.f, 30.f), 8.f, 58.f, limbColor);
                }

                mPoseRoot->addChild(geode);
                root->addChild(mPoseRoot);

                static std::atomic<int> sProxyLogCount{ 0 };
                const int logIndex = sProxyLogCount.fetch_add(1);
                if (logIndex < 160)
                {
                    Log(Debug::Info) << "World viewer: inserted ESM4 actor proxy ref="
                                     << ptr.getCellRef().getRefNum().toString("FormId:")
                                     << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                     << " type=" << ptr.getTypeDescription()
                                     << " name=\"" << ptr.getClass().getName(ptr) << "\" model=\"" << model
                                     << "\" pose=\"tpose\" animated=" << (mAnimate ? 1 : 0)
                                     << " pos=(" << ptr.getRefData().getPosition().pos[0] << ","
                                     << ptr.getRefData().getPosition().pos[1] << ","
                                     << ptr.getRefData().getPosition().pos[2] << ")";
                }
                else if (logIndex == 160)
                    Log(Debug::Info) << "World viewer: further ESM4 actor proxy logs suppressed";
            }

            osg::Vec3f runAnimation(float duration) override
            {
                if (mAnimate && mPoseRoot)
                {
                    mTime += duration;
                    const float sway = std::sin(mTime * (mCreature ? 2.0f : 1.4f)) * (mCreature ? 0.08f : 0.05f);
                    mPoseRoot->setMatrix(osg::Matrix::rotate(sway, osg::Vec3f(0.f, 0.f, 1.f)));
                }

                return osg::Vec3f();
            }

        private:
            osg::ref_ptr<osg::MatrixTransform> mPoseRoot;
            bool mCreature = false;
            bool mAnimate = false;
            float mTime = 0.f;
        };
    }

    Objects::Objects(Resource::ResourceSystem* resourceSystem, const osg::ref_ptr<osg::Group>& rootNode,
        SceneUtil::UnrefQueue& unrefQueue)
        : mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mUnrefQueue(unrefQueue)
    {
    }

    Objects::~Objects()
    {
        clear();
    }

    void Objects::clear()
    {
        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (!ptr.isEmpty() && ptr.getClass().isActor() && ptr.getRefData().getCustomData())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            iter = mObjects.erase(iter);

            if (!ptr.isEmpty() && ptr.getRefData().getBaseNode())
            {
                osg::Node* baseNode = ptr.getRefData().getBaseNode();
                if (baseNode->getNumParents() > 0)
                    baseNode->getParent(0)->removeChild(baseNode);
                ptr.getRefData().setBaseNode(nullptr);
            }
        }

        for (CellMap::iterator iter = mCellSceneNodes.begin(); iter != mCellSceneNodes.end(); ++iter)
        {
            if (iter->second->getNumParents() > 0)
                iter->second->getParent(0)->removeChild(iter->second);
        }
        mCellSceneNodes.clear();
    }

    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        assert(mObjects.find(ptr.mRef) == mObjects.end());

        osg::ref_ptr<osg::Group> cellnode;

        CellMap::iterator found = mCellSceneNodes.find(ptr.getCell());
        if (found == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            cellnode->setName("Cell Root");
            mRootNode->addChild(cellnode);
            mCellSceneNodes[ptr.getCell()] = cellnode;
        }
        else
            cellnode = found->second;

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> insert(new SceneUtil::PositionAttitudeTransform);
        cellnode->addChild(insert);

        insert->getOrCreateUserDataContainer()->addUserObject(new PtrHolder(ptr));

        const float* f = ptr.getRefData().getPosition().pos;

        insert->setPosition(osg::Vec3(f[0], f[1], f[2]));

        const float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        insert->setScale(scaleVec);

        ptr.getRefData().setBaseNode(std::move(insert));
    }

    void Objects::insertModel(const MWWorld::Ptr& ptr, const std::string& mesh, bool allowLight)
    {
        insertBegin(ptr);
        if (skipWorldViewerDiagnosticModel(ptr, mesh))
        {
            ptr.getRefData().getBaseNode()->setNodeMask(0);
            return;
        }
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Object);
        bool animated = ptr.getClass().useAnim();
        std::string animationMesh = mesh;
        if (animated && !mesh.empty())
        {
            animationMesh = Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
            if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
                animated = false;
        }

        osg::ref_ptr<ObjectAnimation> anim(
            new ObjectAnimation(ptr, animationMesh, mResourceSystem, animated, allowLight));

        mObjects.emplace(ptr.mRef, std::move(anim));
    }

    void Objects::insertCreature(const MWWorld::Ptr& ptr, const std::string& mesh, bool weaponsShields)
    {
        insertBegin(ptr);
        const bool hideForFocus = shouldHideWorldViewerNonFocusedActor(ptr, "creature", mesh);
        ptr.getRefData().getBaseNode()->setNodeMask(hideForFocus ? 0 : Mask_Actor);

        if (worldViewerEsm4ActorProxyEnabled() && ptr.getType() == ESM::REC_CREA4)
        {
            osg::ref_ptr<Animation> anim(new WorldViewerActorProxyAnimation(ptr, mResourceSystem, mesh, true));
            mObjects.emplace(ptr.mRef, anim);
            return;
        }
        if (ptr.getType() == ESM::REC_CREA4 && envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
        {
            const ESM::Position& pos = ptr.getRefData().getPosition();
            Log(Debug::Info) << "World viewer actor ledger: phase=render-insert-begin"
                             << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                             << " base=" << ptr.getCellRef().getRefId().toDebugString()
                             << " type=\"" << ptr.getTypeDescription() << "\""
                             << " actorKind=creature"
                             << " name=\"" << ptr.getClass().getName(ptr) << "\""
                             << " model=\"" << mesh << "\""
                             << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
        }

        bool animated = true;
        std::string animationMesh
            = Misc::ResourceHelpers::correctActorModelPath(VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
        if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
            animated = false;

        // CreatureAnimation
        osg::ref_ptr<Animation> anim;

        if (weaponsShields)
            anim = new CreatureWeaponAnimation(ptr, animationMesh, mResourceSystem, animated);
        else
            anim = new CreatureAnimation(ptr, animationMesh, mResourceSystem, animated);

        if (mObjects.emplace(ptr.mRef, anim).second)
            ptr.getClass().getContainerStore(ptr).setContListener(static_cast<ActorAnimation*>(anim.get()));
        if (ptr.getType() == ESM::REC_CREA4 && envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
            Log(Debug::Info) << "World viewer actor ledger: phase=render-insert-end"
                             << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                             << " base=" << ptr.getCellRef().getRefId().toDebugString()
                             << " actorKind=creature"
                             << " object=1";
    }

    void Objects::insertNPC(const MWWorld::Ptr& ptr)
    {
        insertBegin(ptr);
        const std::string npcModel(ptr.getClass().getModel(ptr));
        const bool hideForFocus = shouldHideWorldViewerNonFocusedActor(ptr, "npc", npcModel);
        ptr.getRefData().getBaseNode()->setNodeMask(hideForFocus ? 0 : Mask_Actor);

        if (ptr.getType() == ESM::REC_NPC_4)
        {
            if (hideForFocus)
            {
                if (envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
                {
                    const ESM::Position& pos = ptr.getRefData().getPosition();
                    Log(Debug::Info) << "World viewer actor ledger: phase=focus-skip-native-parts"
                                     << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                     << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                     << " type=\"" << ptr.getTypeDescription() << "\""
                                     << " actorKind=npc"
                                     << " name=\"" << ptr.getClass().getName(ptr) << "\""
                                     << " model=\"" << npcModel << "\""
                                     << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
                }

                osg::ref_ptr<Animation> anim(
                    new WorldViewerActorProxyAnimation(ptr, mResourceSystem, npcModel, false));
                mObjects.emplace(ptr.mRef, anim);
                return;
            }

            if (worldViewerEsm4ActorProxyEnabled())
            {
                osg::ref_ptr<Animation> anim(
                    new WorldViewerActorProxyAnimation(ptr, mResourceSystem, npcModel, false));
                mObjects.emplace(ptr.mRef, anim);
                return;
            }

            if (envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
            {
                const ESM::Position& pos = ptr.getRefData().getPosition();
                Log(Debug::Info) << "World viewer actor ledger: phase=render-insert-begin"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                  << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                  << " type=\"" << ptr.getTypeDescription() << "\""
                                  << " actorKind=npc"
                                  << " name=\"" << ptr.getClass().getName(ptr) << "\""
                                  << " model=\"" << npcModel << "\""
                                  << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
            }
            osg::ref_ptr<ESM4NpcAnimation> anim(
                new ESM4NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));
            mObjects.emplace(ptr.mRef, anim);
            if (envEnabled("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY"))
            {
                const ESM::Position& pos = ptr.getRefData().getPosition();
                Log(Debug::Info) << "World viewer actor ledger: phase=render-insert-end"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                  << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                  << " type=\"" << ptr.getTypeDescription() << "\""
                                  << " actorKind=npc"
                                  << " name=\"" << ptr.getClass().getName(ptr) << "\""
                                  << " model=\"" << npcModel << "\""
                                  << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")"
                                  << " object=1";
            }
        }
        else
        {
            osg::ref_ptr<NpcAnimation> anim(
                new NpcAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));

            if (mObjects.emplace(ptr.mRef, anim).second)
            {
                ptr.getClass().getInventoryStore(ptr).setInvListener(anim.get());
                ptr.getClass().getInventoryStore(ptr).setContListener(anim.get());
            }
        }
    }

    bool Objects::removeObject(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getRefData().getBaseNode())
            return true;

        const auto iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
        {
            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            mObjects.erase(iter);

            if (ptr.getClass().isActor())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);

                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            ptr.getRefData().getBaseNode()->getParent(0)->removeChild(ptr.getRefData().getBaseNode());

            ptr.getRefData().setBaseNode(nullptr);
            return true;
        }
        return false;
    }

    void Objects::removeCell(const MWWorld::CellStore* store)
    {
        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (ptr.getCell() == store)
            {
                if (ptr.getClass().isActor() && ptr.getRefData().getCustomData())
                {
                    if (ptr.getClass().hasInventoryStore(ptr))
                        ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                    ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
                }

                iter->second->removeFromScene();
                mUnrefQueue.push(std::move(iter->second));
                iter = mObjects.erase(iter);
            }
            else
                ++iter;
        }

        CellMap::iterator cell = mCellSceneNodes.find(store);
        if (cell != mCellSceneNodes.end())
        {
            cell->second->getParent(0)->removeChild(cell->second);
            mCellSceneNodes.erase(cell);
        }
    }

    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {
        osg::ref_ptr<osg::Node> objectNode = cur.getRefData().getBaseNode();
        if (!objectNode)
            return;

        MWWorld::CellStore* newCell = cur.getCell();

        osg::Group* cellnode;
        if (mCellSceneNodes.find(newCell) == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            mRootNode->addChild(cellnode);
            mCellSceneNodes[newCell] = cellnode;
        }
        else
        {
            cellnode = mCellSceneNodes[newCell];
        }

        osg::UserDataContainer* userDataContainer = objectNode->getUserDataContainer();
        if (userDataContainer)
            for (unsigned int i = 0; i < userDataContainer->getNumUserObjects(); ++i)
            {
                if (dynamic_cast<PtrHolder*>(userDataContainer->getUserObject(i)))
                    userDataContainer->setUserObject(i, new PtrHolder(cur));
            }

        if (objectNode->getNumParents())
            objectNode->getParent(0)->removeChild(objectNode);
        cellnode->addChild(objectNode);

        PtrAnimationMap::iterator iter = mObjects.find(old.mRef);
        if (iter != mObjects.end())
            iter->second->updatePtr(cur);
    }

    Animation* Objects::getAnimation(const MWWorld::Ptr& ptr)
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

    const Animation* Objects::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second;

        return nullptr;
    }

}
