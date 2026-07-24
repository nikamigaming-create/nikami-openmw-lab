#include "animation.hpp"
#include "falloutanimationtargets.hpp"
#include "fallouthitreaction.hpp"
#include "falloutweaponanimation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Switch>

#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>

#include <osgUtil/UpdateVisitor>

#include <osgAnimation/Bone>
#include <osgAnimation/UpdateBone>

#include <components/debug/debuglog.hpp>

#include <components/esm/defs.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <components/resource/animblendrulesmanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadnpc.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/nifosg/matrixtransform.hpp>
#include <components/nifosg/controller.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <components/sceneutil/keyframe.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"

#include "../mwmechanics/character.hpp" // FIXME: for MWMechanics::Priority
#include "../mwmechanics/weapontype.hpp"

#include "actorutil.hpp"
#include "camera.hpp"
#include "rotatecontroller.hpp"
#include "util.hpp"
#include "vismask.hpp"

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
#include "../mwmechanics/actorutil.hpp"
//## VR_PATCH END

namespace
{
    class MarkDrawablesVisitor : public osg::NodeVisitor
    {
    public:
        MarkDrawablesVisitor(osg::Node::NodeMask mask)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mMask(mask)
        {
        }

        void apply(osg::Drawable& drawable) override { drawable.setNodeMask(mMask); }

    private:
        osg::Node::NodeMask mMask = 0;
    };

    /// Removes all particle systems and related nodes in a subgraph.
    class RemoveParticlesVisitor : public osg::NodeVisitor
    {
    public:
        RemoveParticlesVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (dynamic_cast<osgParticle::ParticleProcessor*>(&node))
                mToRemove.emplace_back(&node);

            traverse(node);
        }

        void apply(osg::Drawable& drw) override
        {
            if (osgParticle::ParticleSystem* partsys = dynamic_cast<osgParticle::ParticleSystem*>(&drw))
                mToRemove.emplace_back(partsys);
        }

        void remove()
        {
            for (osg::Node* node : mToRemove)
            {
                // FIXME: a Drawable might have more than one parent
                if (node->getNumParents())
                    node->getParent(0)->removeChild(node);
            }
            mToRemove.clear();
        }

    private:
        std::vector<osg::ref_ptr<osg::Node>> mToRemove;
    };

    class DayNightCallback : public SceneUtil::NodeCallback<DayNightCallback, osg::Switch*>
    {
    public:
        DayNightCallback()
            : mCurrentState(0)
        {
        }

        void operator()(osg::Switch* node, osg::NodeVisitor* nv)
        {
            unsigned int state = MWBase::Environment::get().getWorld()->getNightDayMode();
            const unsigned int newState = node->getNumChildren() > state ? state : 0;

            if (newState != mCurrentState)
            {
                mCurrentState = newState;
                node->setSingleChildOn(mCurrentState);
            }

            traverse(node, nv);
        }

    private:
        unsigned int mCurrentState;
    };

    class AddSwitchCallbacksVisitor : public osg::NodeVisitor
    {
    public:
        AddSwitchCallbacksVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Switch& switchNode) override
        {
            if (switchNode.getName() == Constants::NightDayLabel)
                switchNode.addUpdateCallback(new DayNightCallback());

            traverse(switchNode);
        }
    };

    class HarvestVisitor : public osg::NodeVisitor
    {
    public:
        HarvestVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Switch& node) override
        {
            if (node.getName() == Constants::HerbalismLabel)
            {
                node.setSingleChildOn(1);
            }

            traverse(node);
        }
    };

    bool equalsParts(std::string_view value, std::string_view s1, std::string_view s2, std::string_view s3 = {})
    {
        if (value.starts_with(s1))
        {
            value = value.substr(s1.size());
            if (value.starts_with(s2))
                return value.substr(s2.size()) == s3;
        }
        return false;
    }

    bool isFalloutNpc(const MWWorld::Ptr& ptr)
    {
        return ptr.getType() == ESM::REC_NPC_4;
    }

    bool isFalloutCreature(const MWWorld::Ptr& ptr)
    {
        return ptr.getType() == ESM4::Creature::sRecordId;
    }

    bool isFalloutActor(const MWWorld::Ptr& ptr)
    {
        return isFalloutNpc(ptr) || isFalloutCreature(ptr);
    }

    bool isFalloutNpcAnimationContext(const MWWorld::Ptr& ptr)
    {
        if (ptr.getType() != ESM::REC_NPC_4)
            return false;

        const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>();
        return ref != nullptr && ref->mBase != nullptr && (ref->mBase->mIsFO3 || ref->mBase->mIsFONV);
    }

    bool isStrictFonvNpcAnimationContext(const MWWorld::Ptr& ptr)
    {
        if (ptr.getType() != ESM::REC_NPC_4)
            return false;

        const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>();
        return ref != nullptr && ref->mBase != nullptr && ref->mBase->mIsFONV && !ref->mBase->mIsFO3;
    }

    float matrixDifference(const osg::Matrixf& left, const osg::Matrixf& right)
    {
        float result = 0.f;
        const float* leftPtr = left.ptr();
        const float* rightPtr = right.ptr();
        for (int i = 0; i < 16; ++i)
            result = std::max(result, std::abs(leftPtr[i] - rightPtr[i]));
        return result;
    }

    bool isFiniteFalloutAuditVec3(const osg::Vec3f& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
    }

    bool isFiniteFalloutAuditMatrix(const osg::Matrix& matrix)
    {
        const auto* values = matrix.ptr();
        for (int i = 0; i < 16; ++i)
        {
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 1000000000.0)
                return false;
        }
        return true;
    }

    bool isFalloutSyntheticAttachmentHelperName(std::string_view lowerName)
    {
        return lowerName == "weapon" || lowerName == "torch" || lowerName == "sideweapon"
            || lowerName == "backweapon" || lowerName == "quiver";
    }

    bool isFalloutSyntheticAttachmentHelperNode(const osg::Node* node)
    {
        if (node == nullptr)
            return false;

        int syntheticHelper = 0;
        return node->getUserValue("esm4SyntheticAttachmentHelper", syntheticHelper) && syntheticHelper != 0;
    }

    bool shouldSkipFalloutSyntheticAttachmentHelperControllers(const MWWorld::Ptr& ptr)
    {
        if (const char* env = std::getenv("OPENMW_ESM4_SKIP_SYNTHETIC_ATTACHMENT_HELPER_CONTROLLERS"))
            return std::string_view(env) != "0";
        if (const char* env = std::getenv("OPENMW_FNV_SKIP_SYNTHETIC_ATTACHMENT_HELPER_CONTROLLERS"))
            return std::string_view(env) != "0";
        return false;
    }

    bool shouldEnableFalloutWeaponIdlePose(const MWWorld::Ptr& ptr)
    {
        if (const char* env = std::getenv("OPENMW_ESM4_ENABLE_WEAPON_IDLE_POSE"))
            return std::string_view(env) != "0";
        if (const char* env = std::getenv("OPENMW_FNV_ENABLE_WEAPON_IDLE_POSE"))
            return std::string_view(env) != "0";
        return isFalloutNpcAnimationContext(ptr);
    }

    bool isFalloutWeaponAimKf(std::string_view lowerKf)
    {
        static constexpr std::array<std::string_view, 13> names{ "h2haim.kf", "1hmaim.kf", "2hmaim.kf",
            "1hpaim.kf", "2hraim.kf", "2haaim.kf", "2hhaim.kf", "2hlaim.kf", "1gtaim.kf", "1mdaim.kf",
            "1lmaim.kf", "1hgaim.kf", "2hgaim.kf" };
        return std::any_of(names.begin(), names.end(), [&](std::string_view name) {
            return Misc::StringUtils::ciEndsWith(lowerKf, name);
        });
    }

    class FalloutTransformTargetVisitor : public osg::NodeVisitor
    {
    public:
        FalloutTransformTargetVisitor(std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mTargets(targets)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            const bool runtimePart = Misc::StringUtils::ciStartsWith(node.getName(), "FNV Part ");
            if (!runtimePart && !mInsideRuntimePart && !node.getName().empty())
                mTargets[Misc::StringUtils::lowerCase(node.getName())].push_back(&node);
            const bool wasInsideRuntimePart = mInsideRuntimePart;
            mInsideRuntimePart = mInsideRuntimePart || runtimePart;
            traverse(node);
            mInsideRuntimePart = wasInsideRuntimePart;
        }

        void apply(osg::Group& node) override
        {
            const bool runtimePart = Misc::StringUtils::ciStartsWith(node.getName(), "FNV Part ");
            const bool wasInsideRuntimePart = mInsideRuntimePart;
            mInsideRuntimePart = mInsideRuntimePart || runtimePart;
            traverse(node);
            mInsideRuntimePart = wasInsideRuntimePart;
        }

    private:
        std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& mTargets;
        bool mInsideRuntimePart = false;
    };

    class FalloutRiggedPartTransformVisitor : public osg::NodeVisitor
    {
    public:
        FalloutRiggedPartTransformVisitor(std::unordered_set<osg::MatrixTransform*>& transforms)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mTransforms(transforms)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            mPath.push_back(&node);
            traverse(node);
            mPath.pop_back();
        }

        void apply(osg::Geode& node) override
        {
            for (unsigned int i = 0; i < node.getNumDrawables(); ++i)
            {
                osg::Drawable* drawable = node.getDrawable(i);
                if (drawable != nullptr)
                    markIfRigged(*drawable);
            }
            traverse(node);
        }

        void apply(osg::Drawable& drawable) override
        {
            markIfRigged(drawable);
        }

    private:
        void markIfRigged(osg::Drawable& drawable)
        {
            if (dynamic_cast<SceneUtil::RigGeometry*>(&drawable) == nullptr)
                return;

            for (osg::MatrixTransform* transform : mPath)
                mTransforms.insert(transform);
        }

        std::unordered_set<osg::MatrixTransform*>& mTransforms;
        std::vector<osg::MatrixTransform*> mPath;
    };

    osg::Matrix getFalloutNodeWorldMatrix(osg::Node* node)
    {
        if (node == nullptr)
            return osg::Matrix();
        const osg::NodePathList paths = node->getParentalNodePaths();
        if (paths.empty())
            return osg::Matrix();
        return osg::computeLocalToWorld(paths.front());
    }

    osg::Matrix getFalloutParentWorldMatrix(osg::Node* node)
    {
        if (node == nullptr || node->getNumParents() == 0)
            return osg::Matrix();
        return getFalloutNodeWorldMatrix(node->getParent(0));
    }

    osg::Vec3f transformFalloutPoint(const osg::Vec3f& point, const osg::Matrix& matrix)
    {
        const osg::Vec3d transformed = osg::Vec3d(point) * matrix;
        return osg::Vec3f(transformed.x(), transformed.y(), transformed.z());
    }

    std::string formatFalloutAuditVec3(const osg::Vec3f& value)
    {
        std::ostringstream out;
        out << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
        return out.str();
    }

    std::string formatFalloutAuditQuat(const osg::Quat& value)
    {
        std::ostringstream out;
        out << "(" << value.x() << "," << value.y() << "," << value.z() << "," << value.w() << ")";
        return out.str();
    }

    float falloutMatrixBasisHandedness(const osg::Matrix& matrix)
    {
        const osg::Vec3f x(matrix(0, 0), matrix(0, 1), matrix(0, 2));
        const osg::Vec3f y(matrix(1, 0), matrix(1, 1), matrix(1, 2));
        const osg::Vec3f z(matrix(2, 0), matrix(2, 1), matrix(2, 2));
        return (x ^ y) * z;
    }

    float falloutQuatAngleDegrees(osg::Quat left, osg::Quat right)
    {
        const auto normalize = [](osg::Quat& value) {
            const double length2 = value.x() * value.x() + value.y() * value.y() + value.z() * value.z()
                + value.w() * value.w();
            if (!std::isfinite(length2) || length2 < 0.000001)
                return false;
            const double invLength = 1.0 / std::sqrt(length2);
            value.set(value.x() * invLength, value.y() * invLength, value.z() * invLength, value.w() * invLength);
            return true;
        };
        if (!normalize(left) || !normalize(right))
            return 0.f;
        const double dot = std::abs(left.x() * right.x() + left.y() * right.y() + left.z() * right.z()
            + left.w() * right.w());
        const double clamped = std::min(1.0, std::max(0.0, dot));
        return static_cast<float>(2.0 * std::acos(clamped) * 57.29577951308232);
    }

    void auditFalloutDuplicateBoneDeltas(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        static constexpr std::string_view bones[] = {
            "bip01 head",
            "bip01 neck1",
            "bip01 spine2",
            "bip01 l hand",
            "bip01 r hand",
            "bip01 l forearm",
            "bip01 r forearm",
        };

        unsigned int suspect = 0;
        float maxAngle = 0.f;
        float maxDistance = 0.f;
        std::string maxBone;
        for (std::string_view bone : bones)
        {
            const auto found = targets.find(std::string(bone));
            if (found == targets.end() || found->second.size() < 2)
                continue;

            osg::Matrix reference = getFalloutNodeWorldMatrix(found->second.front());
            const osg::Vec3f referenceOrigin = transformFalloutPoint(osg::Vec3f(), reference);
            const osg::Quat referenceRotation = reference.getRotate();
            for (std::size_t i = 1; i < found->second.size(); ++i)
            {
                osg::Matrix current = getFalloutNodeWorldMatrix(found->second[i]);
                const osg::Vec3f currentOrigin = transformFalloutPoint(osg::Vec3f(), current);
                const float distance = (currentOrigin - referenceOrigin).length();
                const float angle = falloutQuatAngleDegrees(referenceRotation, current.getRotate());
                if (angle > maxAngle || distance > maxDistance)
                {
                    maxAngle = std::max(maxAngle, angle);
                    maxDistance = std::max(maxDistance, distance);
                    maxBone = std::string(bone);
                }
                const bool bad = angle > 18.f || distance > 12.f;
                if (bad)
                    ++suspect;
                Log(bad ? Debug::Warning : Debug::Info)
                    << "FNV/ESM4 diag: duplicate bone audit " << ptr.getCellRef().getRefId()
                    << " bone='" << bone << "' duplicate=" << i
                    << " distance=" << distance << " angleDeg=" << angle
                    << " verdict=" << (bad ? "SUSPECT" : "OK");
            }
        }

        Log(suspect > 0 ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: duplicate bone audit summary " << ptr.getCellRef().getRefId()
            << " suspect=" << suspect << " maxDistance=" << maxDistance
            << " maxAngleDeg=" << maxAngle << " maxBone='" << maxBone << "'";
    }

    class FalloutRuntimePartVisitor : public osg::NodeVisitor
    {
    public:
        FalloutRuntimePartVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (Misc::StringUtils::ciStartsWith(node.getName(), "FNV Part "))
                mParts.push_back(&node);
            traverse(node);
        }

        std::vector<osg::Node*> mParts;
    };

    struct FalloutRigBoundsSample
    {
        std::string mName;
        std::string mKind;
        std::string mRootBone;
        std::size_t mBoneCount = 0;
        bool mRenderValid = false;
        bool mSourceValid = false;
        bool mLiveValid = false;
        osg::Vec3f mRenderCenterParentWorld;
        osg::Vec3f mRenderCenterPathWorld;
        osg::Vec3f mSourceCenterParentWorld;
        osg::Vec3f mSourceCenterPathWorld;
        osg::Vec3f mLiveCenterParentWorld;
        osg::Vec3f mLiveCenterPathWorld;
        osg::Vec3f mRenderExtent;
        osg::Vec3f mSourceExtent;
        osg::Vec3f mLiveExtent;
    };

    bool isFalloutHandGeometrySampleName(const std::string& name, const std::string& rootBone = std::string())
    {
        const std::string lowerName = Misc::StringUtils::lowerCase(name);
        const std::string lowerRootBone = Misc::StringUtils::lowerCase(rootBone);
        return lowerName.find("hand") != std::string::npos || lowerRootBone.find("hand") != std::string::npos
            || lowerRootBone.find("upperarm") != std::string::npos;
    }

    osg::Vec3f falloutBoundingBoxExtent(const osg::BoundingBox& box)
    {
        if (!box.valid())
            return osg::Vec3f();
        return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
    }

    class FalloutPartRigBoundsVisitor : public osg::NodeVisitor
    {
    public:
        explicit FalloutPartRigBoundsVisitor(const osg::Matrix& partParentWorld)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mPartParentWorld(partParentWorld)
        {
        }

        void apply(osg::Geode& geode) override
        {
            const osg::Matrix localToPart = osg::computeLocalToWorld(getNodePath());
            const osg::Matrix localToWorld = localToPart * mPartParentWorld;

            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::Drawable* drawable = geode.getDrawable(i);
                SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable);
                if (rig == nullptr)
                {
                    osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(drawable);
                    if (geometry == nullptr)
                        continue;

                    const osg::BoundingBox box = geometry->getBoundingBox();
                    FalloutRigBoundsSample sample;
                    sample.mName = geometry->getName();
                    sample.mKind = "osg::Geometry";
                    sample.mRenderValid = box.valid();
                    if (sample.mRenderValid)
                    {
                        sample.mRenderExtent = falloutBoundingBoxExtent(box);
                        sample.mRenderCenterParentWorld = transformFalloutPoint(box.center(), mPartParentWorld);
                        sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                    }
                    mSamples.push_back(sample);
                    continue;
                }

                FalloutRigBoundsSample sample;
                sample.mName = rig->getName();
                sample.mKind = "SceneUtil::RigGeometry";
                sample.mRootBone = std::string(rig->getRootBone());
                sample.mBoneCount = rig->getBoneCount();

                if (osg::Geometry* renderGeometry = rig->getLastFrameGeometry())
                {
                    const osg::BoundingBox box = renderGeometry->getBoundingBox();
                    sample.mRenderValid = box.valid();
                    if (sample.mRenderValid)
                    {
                        sample.mRenderExtent = falloutBoundingBoxExtent(box);
                        sample.mRenderCenterParentWorld = transformFalloutPoint(box.center(), mPartParentWorld);
                        sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                    }
                }
                osg::BoundingBox liveBox;
                if (rig->computeCurrentFalloutSkinningBounds(this, liveBox))
                {
                    sample.mLiveValid = true;
                    sample.mLiveExtent = falloutBoundingBoxExtent(liveBox);
                    sample.mLiveCenterParentWorld = transformFalloutPoint(liveBox.center(), mPartParentWorld);
                    sample.mLiveCenterPathWorld = transformFalloutPoint(liveBox.center(), localToWorld);
                }

                osg::ref_ptr<osg::Geometry> sourceGeometry = rig->getSourceGeometry();
                if (sourceGeometry != nullptr)
                {
                    const osg::BoundingBox box = sourceGeometry->getBoundingBox();
                    sample.mSourceValid = box.valid();
                    if (sample.mSourceValid)
                    {
                        sample.mSourceExtent = falloutBoundingBoxExtent(box);
                        sample.mSourceCenterParentWorld = transformFalloutPoint(box.center(), mPartParentWorld);
                        sample.mSourceCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                    }
                }

                mSamples.push_back(sample);
            }

            traverse(geode);
        }

        void apply(osg::Drawable& drawable) override
        {
            const osg::Matrix localToPart = osg::computeLocalToWorld(getNodePath());
            const osg::Matrix localToWorld = localToPart * mPartParentWorld;

            SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return;

            FalloutRigBoundsSample sample;
            sample.mName = rig->getName();
            sample.mKind = "SceneUtil::RigGeometry";
            sample.mRootBone = std::string(rig->getRootBone());
            sample.mBoneCount = rig->getBoneCount();

            if (osg::Geometry* renderGeometry = rig->getLastFrameGeometry())
            {
                const osg::BoundingBox box = renderGeometry->getBoundingBox();
                sample.mRenderValid = box.valid();
                if (sample.mRenderValid)
                {
                    sample.mRenderExtent = falloutBoundingBoxExtent(box);
                    sample.mRenderCenterParentWorld = transformFalloutPoint(box.center(), mPartParentWorld);
                    sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                }
            }
            osg::BoundingBox liveBox;
            if (rig->computeCurrentFalloutSkinningBounds(this, liveBox))
            {
                sample.mLiveValid = true;
                sample.mLiveExtent = falloutBoundingBoxExtent(liveBox);
                sample.mLiveCenterParentWorld = transformFalloutPoint(liveBox.center(), mPartParentWorld);
                sample.mLiveCenterPathWorld = transformFalloutPoint(liveBox.center(), localToWorld);
            }

            osg::ref_ptr<osg::Geometry> sourceGeometry = rig->getSourceGeometry();
            if (sourceGeometry != nullptr)
            {
                const osg::BoundingBox box = sourceGeometry->getBoundingBox();
                sample.mSourceValid = box.valid();
                if (sample.mSourceValid)
                {
                    sample.mSourceExtent = falloutBoundingBoxExtent(box);
                    sample.mSourceCenterParentWorld = transformFalloutPoint(box.center(), mPartParentWorld);
                    sample.mSourceCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                }
            }

            mSamples.push_back(sample);
        }

        std::vector<FalloutRigBoundsSample> mSamples;

    private:
        osg::Matrix mPartParentWorld;
    };

    class FalloutActorHandRigBoundsVisitor : public osg::NodeVisitor
    {
    public:
        explicit FalloutActorHandRigBoundsVisitor(const osg::Matrix& actorWorld)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mActorWorld(actorWorld)
        {
        }

        void apply(osg::Geode& geode) override
        {
            const osg::Matrix localToActor = osg::computeLocalToWorld(getNodePath());
            const osg::Matrix localToWorld = localToActor * mActorWorld;

            std::string fnvPartAncestor;
            std::string path;
            for (const osg::Node* node : getNodePath())
            {
                if (node == nullptr || node->getName().empty())
                    continue;
                if (!path.empty())
                    path += "/";
                path += node->getName();
                if (Misc::StringUtils::ciStartsWith(node->getName(), "FNV Part "))
                    fnvPartAncestor = node->getName();
            }

            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(geode.getDrawable(i));
                if (rig == nullptr)
                    continue;

                const std::string rigName = rig->getName();
                const std::string rootBone = std::string(rig->getRootBone());
                if (!isFalloutHandGeometrySampleName(rigName, rootBone))
                    continue;

                FalloutRigBoundsSample sample;
                sample.mName = rigName;
                sample.mKind = "SceneUtil::RigGeometry";
                sample.mRootBone = rootBone;
                sample.mBoneCount = rig->getBoneCount();

                if (osg::Geometry* renderGeometry = rig->getLastFrameGeometry())
                {
                    const osg::BoundingBox box = renderGeometry->getBoundingBox();
                    sample.mRenderValid = box.valid();
                    if (sample.mRenderValid)
                    {
                        sample.mRenderExtent = falloutBoundingBoxExtent(box);
                        sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                    }
                }
                osg::BoundingBox liveBox;
                if (rig->computeCurrentFalloutSkinningBounds(this, liveBox))
                {
                    sample.mLiveValid = true;
                    sample.mLiveExtent = falloutBoundingBoxExtent(liveBox);
                    sample.mLiveCenterPathWorld = transformFalloutPoint(liveBox.center(), localToWorld);
                }

                osg::ref_ptr<osg::Geometry> sourceGeometry = rig->getSourceGeometry();
                if (sourceGeometry != nullptr)
                {
                    const osg::BoundingBox box = sourceGeometry->getBoundingBox();
                    sample.mSourceValid = box.valid();
                    if (sample.mSourceValid)
                    {
                        sample.mSourceExtent = falloutBoundingBoxExtent(box);
                        sample.mSourceCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                    }
                }

                mSamples.push_back(sample);
                mPartAncestors.push_back(fnvPartAncestor);
                mPaths.push_back(path);
            }

            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::Drawable* drawable = geode.getDrawable(i);
                if (dynamic_cast<SceneUtil::RigGeometry*>(drawable) != nullptr)
                    continue;

                osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(drawable);
                if (geometry == nullptr)
                    continue;

                const bool handAncestor = fnvPartAncestor.find("lefthand") != std::string::npos
                    || fnvPartAncestor.find("righthand") != std::string::npos
                    || fnvPartAncestor.find("left hand") != std::string::npos
                    || fnvPartAncestor.find("right hand") != std::string::npos;
                if (!handAncestor && !isFalloutHandGeometrySampleName(geometry->getName()))
                    continue;

                const osg::BoundingBox box = geometry->getBoundingBox();
                FalloutRigBoundsSample sample;
                sample.mName = geometry->getName();
                sample.mKind = "osg::Geometry";
                sample.mRenderValid = box.valid();
                if (sample.mRenderValid)
                {
                    sample.mRenderExtent = falloutBoundingBoxExtent(box);
                    sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                }

                mSamples.push_back(sample);
                mPartAncestors.push_back(fnvPartAncestor);
                mPaths.push_back(path);
            }

            traverse(geode);
        }

        void apply(osg::Drawable& drawable) override
        {
            SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return;

            const std::string rigName = rig->getName();
            const std::string rootBone = std::string(rig->getRootBone());
            if (!isFalloutHandGeometrySampleName(rigName, rootBone))
                return;

            const osg::Matrix localToActor = osg::computeLocalToWorld(getNodePath());
            const osg::Matrix localToWorld = localToActor * mActorWorld;

            std::string fnvPartAncestor;
            std::string path;
            for (const osg::Node* node : getNodePath())
            {
                if (node == nullptr || node->getName().empty())
                    continue;
                if (!path.empty())
                    path += "/";
                path += node->getName();
                if (Misc::StringUtils::ciStartsWith(node->getName(), "FNV Part "))
                    fnvPartAncestor = node->getName();
            }

            FalloutRigBoundsSample sample;
            sample.mName = rigName;
            sample.mKind = "SceneUtil::RigGeometry";
            sample.mRootBone = rootBone;
            sample.mBoneCount = rig->getBoneCount();

            if (osg::Geometry* renderGeometry = rig->getLastFrameGeometry())
            {
                const osg::BoundingBox box = renderGeometry->getBoundingBox();
                sample.mRenderValid = box.valid();
                if (sample.mRenderValid)
                {
                    sample.mRenderExtent = falloutBoundingBoxExtent(box);
                    sample.mRenderCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                }
            }
            osg::BoundingBox liveBox;
            if (rig->computeCurrentFalloutSkinningBounds(this, liveBox))
            {
                sample.mLiveValid = true;
                sample.mLiveExtent = falloutBoundingBoxExtent(liveBox);
                sample.mLiveCenterPathWorld = transformFalloutPoint(liveBox.center(), localToWorld);
            }

            osg::ref_ptr<osg::Geometry> sourceGeometry = rig->getSourceGeometry();
            if (sourceGeometry != nullptr)
            {
                const osg::BoundingBox box = sourceGeometry->getBoundingBox();
                sample.mSourceValid = box.valid();
                if (sample.mSourceValid)
                {
                    sample.mSourceExtent = falloutBoundingBoxExtent(box);
                    sample.mSourceCenterPathWorld = transformFalloutPoint(box.center(), localToWorld);
                }
            }

            mSamples.push_back(sample);
            mPartAncestors.push_back(fnvPartAncestor);
            mPaths.push_back(path);
        }

        std::vector<FalloutRigBoundsSample> mSamples;
        std::vector<std::string> mPartAncestors;
        std::vector<std::string> mPaths;

    private:
        osg::Matrix mActorWorld;
    };

    osg::MatrixTransform* findFalloutTarget(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const std::string& bone)
    {
        const auto found = targets.find(bone);
        if (found == targets.end() || found->second.empty())
            return nullptr;
        return found->second.front();
    }

    osg::MatrixTransform* findFalloutTargetAny(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets,
        std::initializer_list<std::string_view> bones)
    {
        for (std::string_view bone : bones)
        {
            osg::MatrixTransform* target = findFalloutTarget(targets, std::string(bone));
            if (target != nullptr)
                return target;
        }
        return nullptr;
    }

    osg::Vec3f getFalloutTargetWorldOrigin(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const std::string& bone)
    {
        return transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(findFalloutTarget(targets, bone)));
    }

    void auditFalloutSkeletonBounds(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        osg::Vec3f minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
        osg::Vec3f maximum(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max());
        int pointCount = 0;

        for (const auto& [bone, nodes] : targets)
        {
            if (bone.empty() || nodes.empty())
                continue;

            osg::MatrixTransform* node = nodes.front();
            if (node == nullptr)
                continue;

            const osg::Vec3f point = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            minimum.x() = std::min(minimum.x(), point.x());
            minimum.y() = std::min(minimum.y(), point.y());
            minimum.z() = std::min(minimum.z(), point.z());
            maximum.x() = std::max(maximum.x(), point.x());
            maximum.y() = std::max(maximum.y(), point.y());
            maximum.z() = std::max(maximum.z(), point.z());
            ++pointCount;
        }

        if (pointCount == 0)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: skeleton bounds " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=no_points";
            return;
        }

        const osg::Vec3f size = maximum - minimum;
        const osg::Vec3f center = (minimum + maximum) * 0.5f;
        Log(Debug::Verbose) << "FNV/ESM4 diag: skeleton bounds " << ptr.getCellRef().getRefId()
                         << " points=" << pointCount
                         << " min=" << formatFalloutAuditVec3(minimum)
                         << " max=" << formatFalloutAuditVec3(maximum)
                         << " center=" << formatFalloutAuditVec3(center)
                         << " size=" << formatFalloutAuditVec3(size);

        const auto pointOrNaN = [&](std::initializer_list<std::string_view> bones) {
            osg::MatrixTransform* node = findFalloutTargetAny(targets, bones);
            return node != nullptr ? transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node))
                                   : osg::Vec3f(std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::quiet_NaN());
        };
        const osg::Vec3f head = pointOrNaN({ "bip01 head" });
        const osg::Vec3f neck = pointOrNaN({ "bip01 neck1", "bip01 neck" });
        const osg::Vec3f spine = pointOrNaN({ "bip01 spine2", "bip01 spine1", "bip01 spine" });
        const osg::Vec3f pelvis = pointOrNaN({ "bip01 pelvis" });
        const osg::Vec3f leftShoulder = pointOrNaN({ "bip01 l upperarm" });
        const osg::Vec3f rightShoulder = pointOrNaN({ "bip01 r upperarm" });
        const osg::Vec3f leftElbow = pointOrNaN({ "bip01 l forearm" });
        const osg::Vec3f rightElbow = pointOrNaN({ "bip01 r forearm" });
        const osg::Vec3f leftHand = pointOrNaN({ "bip01 l hand" });
        const osg::Vec3f rightHand = pointOrNaN({ "bip01 r hand" });
        const osg::Vec3f leftThigh = pointOrNaN({ "bip01 l thigh" });
        const osg::Vec3f rightThigh = pointOrNaN({ "bip01 r thigh" });
        const osg::Vec3f leftKnee = pointOrNaN({ "bip01 l calf" });
        const osg::Vec3f rightKnee = pointOrNaN({ "bip01 r calf" });
        const osg::Vec3f leftFoot = pointOrNaN({ "bip01 l foot" });
        const osg::Vec3f rightFoot = pointOrNaN({ "bip01 r foot" });

        Log(Debug::Verbose) << "FNV/ESM4 diag: skeleton anchors " << ptr.getCellRef().getRefId()
                         << " head=" << formatFalloutAuditVec3(head)
                         << " neck=" << formatFalloutAuditVec3(neck)
                         << " spine=" << formatFalloutAuditVec3(spine)
                         << " pelvis=" << formatFalloutAuditVec3(pelvis)
                         << " leftShoulder=" << formatFalloutAuditVec3(leftShoulder)
                         << " rightShoulder=" << formatFalloutAuditVec3(rightShoulder)
                         << " leftElbow=" << formatFalloutAuditVec3(leftElbow)
                         << " rightElbow=" << formatFalloutAuditVec3(rightElbow)
                         << " leftHand=" << formatFalloutAuditVec3(leftHand)
                         << " rightHand=" << formatFalloutAuditVec3(rightHand)
                         << " leftThigh=" << formatFalloutAuditVec3(leftThigh)
                         << " rightThigh=" << formatFalloutAuditVec3(rightThigh)
                         << " leftKnee=" << formatFalloutAuditVec3(leftKnee)
                         << " rightKnee=" << formatFalloutAuditVec3(rightKnee)
                         << " leftFoot=" << formatFalloutAuditVec3(leftFoot)
                         << " rightFoot=" << formatFalloutAuditVec3(rightFoot);

        Log(Debug::Verbose) << "FNV/ESM4 diag: skeleton segments " << ptr.getCellRef().getRefId()
                         << " neckToHead=" << (head - neck).length()
                         << " spineToNeck=" << (neck - spine).length()
                         << " pelvisToSpine=" << (spine - pelvis).length()
                         << " shoulderSpan=" << (rightShoulder - leftShoulder).length()
                         << " leftUpperArm=" << (leftElbow - leftShoulder).length()
                         << " rightUpperArm=" << (rightElbow - rightShoulder).length()
                         << " leftForearm=" << (leftHand - leftElbow).length()
                         << " rightForearm=" << (rightHand - rightElbow).length()
                         << " hipSpan=" << (rightThigh - leftThigh).length()
                         << " leftThigh=" << (leftKnee - leftThigh).length()
                         << " rightThigh=" << (rightKnee - rightThigh).length()
                         << " leftCalf=" << (leftFoot - leftKnee).length()
                         << " rightCalf=" << (rightFoot - rightKnee).length()
                         << " footSpread=" << (rightFoot - leftFoot).length();
    }

    std::string describeFalloutClosestTarget(const osg::Vec3f& point,
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets,
        std::initializer_list<std::string_view> bones)
    {
        float bestDistance = std::numeric_limits<float>::max();
        std::string bestBone;
        std::ostringstream distances;
        bool first = true;
        for (std::string_view bone : bones)
        {
            osg::MatrixTransform* target = findFalloutTarget(targets, std::string(bone));
            if (target == nullptr)
                continue;

            const osg::Vec3f origin = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(target));
            const float distance = (point - origin).length();
            if (!first)
                distances << ",";
            distances << bone << "=" << distance;
            first = false;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestBone = std::string(bone);
            }
        }

        if (bestBone.empty())
            return "closest=(none) distances=[]";

        std::ostringstream result;
        result << "closest=" << bestBone << " closestDistance=" << bestDistance << " distances=["
               << distances.str() << "]";
        return result.str();
    }

    float falloutVectorAngleDegrees(const osg::Vec3f& left, const osg::Vec3f& right)
    {
        const float leftLength = left.length();
        const float rightLength = right.length();
        if (leftLength <= 0.001f || rightLength <= 0.001f)
            return 180.f;

        const double dot = std::clamp(static_cast<double>(left * right) / (leftLength * rightLength), -1.0, 1.0);
        return static_cast<float>(std::acos(dot) * 180.0 / osg::PI);
    }

    void auditFalloutMirrorSymmetry(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        if (std::getenv("OPENMW_FNV_MIRROR_SYMMETRY_AUDIT") == nullptr)
            return;

        osg::MatrixTransform* root = findFalloutTarget(targets, "bip01");
        if (root == nullptr)
            root = findFalloutTarget(targets, "bip01 pelvis");
        if (root == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: mirror symmetry " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_root";
            return;
        }

        osg::Matrix worldToRoot;
        worldToRoot.invert(getFalloutNodeWorldMatrix(root));

        const auto requireLocal = [&](const std::string& bone, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTarget(targets, bone);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(
                transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node)), worldToRoot);
            return true;
        };

        const auto mirrorError = [](const osg::Vec3f& left, const osg::Vec3f& right, char axis) {
            osg::Vec3f mirrored = right;
            if (axis == 'x')
                mirrored.x() = -mirrored.x();
            else if (axis == 'y')
                mirrored.y() = -mirrored.y();
            else if (axis == 'z')
                mirrored.z() = -mirrored.z();
            return (left - mirrored).length();
        };

        const auto logPair = [&](std::string_view label, const std::string& leftBone, const std::string& rightBone) {
            osg::Vec3f left;
            osg::Vec3f right;
            if (!requireLocal(leftBone, left) || !requireLocal(rightBone, right))
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: mirror symmetry " << ptr.getCellRef().getRefId()
                                    << " pair=" << label << " verdict=BAD reason=missing_bone";
                return;
            }

            const float mirrorX = mirrorError(left, right, 'x');
            const float mirrorY = mirrorError(left, right, 'y');
            const float mirrorZ = mirrorError(left, right, 'z');
            const float same = (left - right).length();
            char bestAxis = 'x';
            float best = mirrorX;
            if (mirrorY < best)
            {
                bestAxis = 'y';
                best = mirrorY;
            }
            if (mirrorZ < best)
            {
                bestAxis = 'z';
                best = mirrorZ;
            }

            Log(best > 12.f ? Debug::Warning : Debug::Info)
                << "FNV/ESM4 diag: mirror symmetry " << ptr.getCellRef().getRefId()
                << " pair=" << label
                << " left=" << formatFalloutAuditVec3(left)
                << " right=" << formatFalloutAuditVec3(right)
                << " sameError=" << same
                << " mirrorXError=" << mirrorX
                << " mirrorYError=" << mirrorY
                << " mirrorZError=" << mirrorZ
                << " bestMirrorAxis=" << bestAxis
                << " bestMirrorError=" << best
                << " verdict=" << (best > 12.f ? "BAD" : "OK");
        };

        logPair("upperarm", "bip01 l upperarm", "bip01 r upperarm");
        logPair("forearm", "bip01 l forearm", "bip01 r forearm");
        logPair("hand", "bip01 l hand", "bip01 r hand");
        logPair("thigh", "bip01 l thigh", "bip01 r thigh");
        logPair("knee", "bip01 l calf", "bip01 r calf");
        logPair("foot", "bip01 l foot", "bip01 r foot");
    }

    struct FalloutSeatedLegChainAudit
    {
        bool mValid = false;
        bool mBad = true;
        std::string mReason = "missing_bone";
    };

    FalloutSeatedLegChainAudit auditFalloutSeatedLegChain(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        const auto requireBone = [&](const std::string& bone, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTarget(targets, bone);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            return true;
        };

        osg::Vec3f pelvis;
        osg::Vec3f leftKnee;
        osg::Vec3f rightKnee;
        osg::Vec3f leftFoot;
        osg::Vec3f rightFoot;
        FalloutSeatedLegChainAudit audit;
        if (!(requireBone("bip01 pelvis", pelvis) && requireBone("bip01 l calf", leftKnee)
                && requireBone("bip01 r calf", rightKnee) && requireBone("bip01 l foot", leftFoot)
                && requireBone("bip01 r foot", rightFoot)))
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: seated leg chain " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_bone";
            return audit;
        }

        const osg::Vec3f leftThigh = leftKnee - pelvis;
        const osg::Vec3f rightThigh = rightKnee - pelvis;
        const osg::Vec3f leftCalf = leftFoot - leftKnee;
        const osg::Vec3f rightCalf = rightFoot - rightKnee;
        const osg::Vec3f kneeMid = (leftKnee + rightKnee) * 0.5f;
        const osg::Vec3f footMid = (leftFoot + rightFoot) * 0.5f;
        const auto horizontalRatio = [](const osg::Vec3f& value) {
            const float length = value.length();
            return length <= 0.001f ? 0.f : osg::Vec2f(value.x(), value.y()).length() / length;
        };
        const auto verticalDownRatio = [](const osg::Vec3f& value) {
            const float length = value.length();
            return length <= 0.001f ? 0.f : std::max(0.f, -value.z()) / length;
        };

        const float leftThighHorizontal = horizontalRatio(leftThigh);
        const float rightThighHorizontal = horizontalRatio(rightThigh);
        const float leftCalfVerticalDown = verticalDownRatio(leftCalf);
        const float rightCalfVerticalDown = verticalDownRatio(rightCalf);
        const float leftKneeAngle = falloutVectorAngleDegrees(-leftThigh, leftCalf);
        const float rightKneeAngle = falloutVectorAngleDegrees(-rightThigh, rightCalf);
        const float footZDelta = std::abs(leftFoot.z() - rightFoot.z());
        const float feetBelowPelvis = pelvis.z() - footMid.z();
        const float kneesBelowPelvis = pelvis.z() - kneeMid.z();
        const float leftKneeForward = osg::Vec2f(leftKnee.x() - pelvis.x(), leftKnee.y() - pelvis.y()).length();
        const float rightKneeForward = osg::Vec2f(rightKnee.x() - pelvis.x(), rightKnee.y() - pelvis.y()).length();
        const float kneeMirrorDelta = (leftKnee - kneeMid).length() - (rightKnee - kneeMid).length();
        const float footMirrorDelta = (leftFoot - footMid).length() - (rightFoot - footMid).length();
        const bool badThigh = leftThighHorizontal < 0.55f || rightThighHorizontal < 0.55f;
        const bool badCalf = leftCalfVerticalDown < 0.45f || rightCalfVerticalDown < 0.45f;
        const bool badKneeAngle
            = leftKneeAngle < 45.f || leftKneeAngle > 135.f || rightKneeAngle < 45.f || rightKneeAngle > 135.f;
        const bool badFootLevel = footZDelta > 8.f;
        const bool badFeetHeight = feetBelowPelvis < 5.f || feetBelowPelvis > 95.f;
        const bool badKneeTravel = leftKneeForward < 8.f || rightKneeForward < 8.f;
        const bool badKneeHeight = kneesBelowPelvis < -25.f || kneesBelowPelvis > 70.f;
        const bool bad
            = badThigh || badCalf || badKneeAngle || badFootLevel || badFeetHeight || badKneeTravel || badKneeHeight;
        const char* reason = badThigh ? "thigh_not_horizontal"
            : badCalf                  ? "calf_not_down"
            : badKneeAngle             ? "knee_angle"
            : badFootLevel             ? "foot_z_mismatch"
            : badFeetHeight            ? "feet_height"
            : badKneeTravel            ? "knee_not_forward"
            : badKneeHeight            ? "knee_height"
                                      : "ok";

        Log(bad ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: seated leg chain " << ptr.getCellRef().getRefId()
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " leftKnee=" << formatFalloutAuditVec3(leftKnee)
            << " rightKnee=" << formatFalloutAuditVec3(rightKnee)
            << " leftFoot=" << formatFalloutAuditVec3(leftFoot)
            << " rightFoot=" << formatFalloutAuditVec3(rightFoot)
            << " leftThighVec=" << formatFalloutAuditVec3(leftThigh)
            << " rightThighVec=" << formatFalloutAuditVec3(rightThigh)
            << " leftCalfVec=" << formatFalloutAuditVec3(leftCalf)
            << " rightCalfVec=" << formatFalloutAuditVec3(rightCalf)
            << " leftThighHorizontal=" << leftThighHorizontal
            << " rightThighHorizontal=" << rightThighHorizontal
            << " leftCalfVerticalDown=" << leftCalfVerticalDown
            << " rightCalfVerticalDown=" << rightCalfVerticalDown
            << " leftKneeAngle=" << leftKneeAngle
            << " rightKneeAngle=" << rightKneeAngle
            << " feetBelowPelvis=" << feetBelowPelvis
            << " kneesBelowPelvis=" << kneesBelowPelvis
            << " leftKneeForward=" << leftKneeForward
            << " rightKneeForward=" << rightKneeForward
            << " footZDelta=" << footZDelta
            << " kneeMirrorDelta=" << kneeMirrorDelta
            << " footMirrorDelta=" << footMirrorDelta
            << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;

        audit.mValid = true;
        audit.mBad = bad;
        audit.mReason = reason;
        return audit;
    }

    bool auditFalloutSeatedUpperBody(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        const auto requireBone = [&](const std::string& bone, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTarget(targets, bone);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            return true;
        };

        osg::Vec3f head;
        osg::Vec3f spine2;
        osg::Vec3f pelvis;
        osg::Vec3f leftUpperArm;
        osg::Vec3f rightUpperArm;
        osg::Vec3f leftForearm;
        osg::Vec3f rightForearm;
        osg::Vec3f leftHand;
        osg::Vec3f rightHand;
        osg::Vec3f leftKnee;
        osg::Vec3f rightKnee;
        if (!(requireBone("bip01 head", head) && requireBone("bip01 spine2", spine2)
                && requireBone("bip01 pelvis", pelvis) && requireBone("bip01 l upperarm", leftUpperArm)
                && requireBone("bip01 r upperarm", rightUpperArm) && requireBone("bip01 l forearm", leftForearm)
                && requireBone("bip01 r forearm", rightForearm) && requireBone("bip01 l hand", leftHand)
                && requireBone("bip01 r hand", rightHand) && requireBone("bip01 l calf", leftKnee)
                && requireBone("bip01 r calf", rightKnee)))
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: seated upper body audit " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_bone";
            return false;
        }

        const osg::Vec3f kneeMid = (leftKnee + rightKnee) * 0.5f;
        const osg::Vec3f shoulderMid = (leftUpperArm + rightUpperArm) * 0.5f;
        const osg::Vec3f thighForward = kneeMid - pelvis;
        const osg::Vec3f torsoForward = head - spine2;
        const osg::Vec3f shoulderToHead = head - shoulderMid;
        const osg::Vec3f leftUpper = leftForearm - leftUpperArm;
        const osg::Vec3f rightUpper = rightForearm - rightUpperArm;
        const osg::Vec3f leftLower = leftHand - leftForearm;
        const osg::Vec3f rightLower = rightHand - rightForearm;
        const osg::Vec3f leftArmWhole = leftHand - leftUpperArm;
        const osg::Vec3f rightArmWhole = rightHand - rightUpperArm;
        const osg::Vec3f shoulderSpan = rightUpperArm - leftUpperArm;

        osg::Matrix worldToRoot;
        osg::MatrixTransform* root = findFalloutTarget(targets, "bip01");
        const bool hasRoot = root != nullptr && worldToRoot.invert(getFalloutNodeWorldMatrix(root));
        const auto toRootLocal = [&](const osg::Vec3f& point) {
            return hasRoot ? transformFalloutPoint(point, worldToRoot) : point;
        };
        const auto bestMirroredError = [](const osg::Vec3f& left, const osg::Vec3f& right) {
            const osg::Vec3f mirroredX(-right.x(), right.y(), right.z());
            const osg::Vec3f mirroredY(right.x(), -right.y(), right.z());
            const osg::Vec3f mirroredZ(right.x(), right.y(), -right.z());
            return std::min({ (left - mirroredX).length(), (left - mirroredY).length(), (left - mirroredZ).length() });
        };

        const auto horizontal = [](const osg::Vec3f& value) {
            return osg::Vec3f(value.x(), value.y(), 0.f);
        };
        const float torsoThighYaw = falloutVectorAngleDegrees(horizontal(torsoForward), horizontal(thighForward));
        const float shoulderThighYaw = falloutVectorAngleDegrees(horizontal(shoulderToHead), horizontal(thighForward));
        const float shoulderSpanLength = shoulderSpan.length();
        const float leftUpperLength = leftUpper.length();
        const float rightUpperLength = rightUpper.length();
        const float leftLowerLength = leftLower.length();
        const float rightLowerLength = rightLower.length();
        const float leftArmWholeLength = leftArmWhole.length();
        const float rightArmWholeLength = rightArmWhole.length();
        const float armLengthDelta = std::abs(leftArmWholeLength - rightArmWholeLength);
        const float shoulderTilt = std::abs(leftUpperArm.z() - rightUpperArm.z());
        const float headAboveSpine = head.z() - spine2.z();
        const float headAboveShoulders = head.z() - shoulderMid.z();
        const float shouldersAboveSpine = shoulderMid.z() - spine2.z();
        const float shouldersAbovePelvis = shoulderMid.z() - pelvis.z();
        const float forearmMirrorError
            = hasRoot ? bestMirroredError(toRootLocal(leftForearm), toRootLocal(rightForearm)) : 0.f;
        const float handMirrorError = hasRoot ? bestMirroredError(toRootLocal(leftHand), toRootLocal(rightHand)) : 0.f;

        const bool badTorsoYaw = torsoThighYaw > 115.f && shoulderThighYaw > 95.f;
        const bool badShoulders = shoulderSpanLength < 20.f || shoulderSpanLength > 55.f || shoulderTilt > 34.f;
        const bool badArmCollapse = leftUpperLength < 4.f || rightUpperLength < 4.f || leftLowerLength < 4.f
            || rightLowerLength < 4.f || armLengthDelta > 3.f;
        const bool badHeadStack = headAboveSpine < 8.f || headAboveSpine > 45.f;
        const bool badHeadShoulders = headAboveShoulders < 6.f || headAboveShoulders > 28.f;
        const bool badShoulderStack = shouldersAboveSpine < 4.f || shouldersAbovePelvis < 18.f;
        const bool badHandSymmetry = hasRoot && (forearmMirrorError > 12.f || handMirrorError > 12.f);
        const bool bad = badTorsoYaw || badShoulders || badArmCollapse || badHeadStack || badHeadShoulders
            || badShoulderStack || badHandSymmetry;
        const char* reason = badTorsoYaw ? "torso_thigh_yaw"
            : badShoulders               ? "shoulder_span"
            : badArmCollapse             ? "arm_chain"
            : badHeadStack               ? "head_stack"
            : badHeadShoulders           ? "head_shoulders"
            : badShoulderStack           ? "shoulder_stack"
            : badHandSymmetry            ? "hand_symmetry"
                                         : "ok";

        Log(bad ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: seated upper body audit " << ptr.getCellRef().getRefId()
            << " head=" << formatFalloutAuditVec3(head)
            << " spine2=" << formatFalloutAuditVec3(spine2)
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " kneeMid=" << formatFalloutAuditVec3(kneeMid)
            << " shoulderMid=" << formatFalloutAuditVec3(shoulderMid)
            << " thighForward=" << formatFalloutAuditVec3(thighForward)
            << " torsoForward=" << formatFalloutAuditVec3(torsoForward)
            << " shoulderToHead=" << formatFalloutAuditVec3(shoulderToHead)
            << " torsoThighYaw=" << torsoThighYaw
            << " shoulderThighYaw=" << shoulderThighYaw
            << " shoulderSpan=" << shoulderSpanLength
            << " shoulderTilt=" << shoulderTilt
            << " leftUpperLength=" << leftUpperLength
            << " rightUpperLength=" << rightUpperLength
            << " leftLowerLength=" << leftLowerLength
            << " rightLowerLength=" << rightLowerLength
            << " leftArmWholeLength=" << leftArmWholeLength
            << " rightArmWholeLength=" << rightArmWholeLength
            << " armLengthDelta=" << armLengthDelta
            << " headAboveSpine=" << headAboveSpine
            << " headAboveShoulders=" << headAboveShoulders
            << " shouldersAboveSpine=" << shouldersAboveSpine
            << " shouldersAbovePelvis=" << shouldersAbovePelvis
            << " forearmMirrorError=" << forearmMirrorError
            << " handMirrorError=" << handMirrorError
            << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        return !bad;
    }

    bool auditFalloutSeatedPlacement(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        osg::MatrixTransform* rootNode = findFalloutTarget(targets, "bip01");
        osg::MatrixTransform* pelvisNode = findFalloutTarget(targets, "bip01 pelvis");
        if (rootNode == nullptr || pelvisNode == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: seated placement audit " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_bone";
            return false;
        }

        const osg::Vec3f scene = ptr.getRefData().getPosition().asVec3();
        const osg::Vec3f root = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(rootNode));
        const osg::Vec3f pelvis = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(pelvisNode));
        const float pelvisSceneHorizontal = osg::Vec2f(pelvis.x() - scene.x(), pelvis.y() - scene.y()).length();
        const float pelvisSceneDz = pelvis.z() - scene.z();
        const float pelvisRootHorizontal = osg::Vec2f(pelvis.x() - root.x(), pelvis.y() - root.y()).length();
        const float pelvisRootDz = pelvis.z() - root.z();
        const bool bad = pelvisSceneHorizontal > 48.f || pelvisSceneDz < -16.f || pelvisSceneDz > 48.f
            || pelvisRootHorizontal > 96.f || pelvisRootDz < -35.f || pelvisRootDz > 70.f;
        const char* reason = pelvisSceneHorizontal > 48.f ? "pelvis_scene_horizontal"
            : pelvisSceneDz < -16.f || pelvisSceneDz > 48.f ? "pelvis_scene_z"
            : pelvisRootHorizontal > 96.f                  ? "pelvis_root_horizontal"
            : pelvisRootDz < -35.f || pelvisRootDz > 70.f  ? "pelvis_root_z"
                                                           : "ok";

        Log(bad ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: seated placement audit " << ptr.getCellRef().getRefId()
            << " scene=" << formatFalloutAuditVec3(scene)
            << " root=" << formatFalloutAuditVec3(root)
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " pelvisSceneHorizontal=" << pelvisSceneHorizontal
            << " pelvisSceneDz=" << pelvisSceneDz
            << " pelvisRootHorizontal=" << pelvisRootHorizontal
            << " pelvisRootDz=" << pelvisRootDz
            << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        return !bad;
    }

    bool setFalloutNodeWorldOrigin(osg::MatrixTransform* node, const osg::Vec3f& worldOrigin)
    {
        if (node == nullptr)
            return false;

        osg::Matrix parentWorld = getFalloutParentWorldMatrix(node);
        osg::Matrix worldToParent;
        if (!worldToParent.invert(parentWorld))
            return false;

        const osg::Vec3f localOrigin = transformFalloutPoint(worldOrigin, worldToParent);
        if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
            nifTransform->setTranslation(localOrigin);
        else
        {
            osg::Matrixf matrix = node->getMatrix();
            matrix.setTrans(localOrigin);
            node->setMatrix(matrix);
            node->dirtyBound();
        }
        return true;
    }

    bool setFalloutTargetsWorldOrigin(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const std::string& bone,
        const osg::Vec3f& worldOrigin)
    {
        const auto found = targets.find(bone);
        if (found == targets.end())
            return false;

        bool applied = false;
        for (osg::MatrixTransform* node : found->second)
            applied = setFalloutNodeWorldOrigin(node, worldOrigin) || applied;
        return applied;
    }

    osg::Vec3f normalizedOr(const osg::Vec3f& value, const osg::Vec3f& fallback)
    {
        osg::Vec3f result = value;
        if (result.normalize() <= 0.001f)
            return fallback;
        return result;
    }

    osg::Vec3f solveFalloutTwoBoneKnee(
        const osg::Vec3f& hip, const osg::Vec3f& foot, float thighLength, float calfLength, const osg::Vec3f& bendHint)
    {
        osg::Vec3f hipToFoot = foot - hip;
        float distance = hipToFoot.normalize();
        if (distance <= 0.001f)
            return hip + bendHint * thighLength;

        const float maxReach = std::max(0.001f, thighLength + calfLength - 0.001f);
        distance = std::min(distance, maxReach);
        const float along = std::clamp(
            (thighLength * thighLength - calfLength * calfLength + distance * distance) / (2.f * distance),
            0.f, thighLength);
        const float height = std::sqrt(std::max(0.f, thighLength * thighLength - along * along));
        osg::Vec3f perpendicular = bendHint - hipToFoot * (bendHint * hipToFoot);
        if (perpendicular.normalize() <= 0.001f)
            perpendicular = osg::Vec3f(0.f, 0.f, 1.f);
        return hip + hipToFoot * along + perpendicular * height;
    }

    bool shouldApplyFalloutSeatedHumanIk()
    {
        if (const char* env = std::getenv("OPENMW_FNV_SEATED_HUMAN_IK"))
            return std::string_view(env) != "0";
        return false;
    }

    bool shouldApplyFalloutStandingLegIk(const MWWorld::Ptr& ptr)
    {
        if (const char* env = std::getenv("OPENMW_ESM4_STANDING_LEG_IK"))
            return std::string_view(env) != "0";
        if (const char* env = std::getenv("OPENMW_FNV_STANDING_LEG_IK"))
            return std::string_view(env) != "0";
        return false;
    }

    bool shouldApplyFalloutStandingArmIk(const MWWorld::Ptr& /*ptr*/)
    {
        if (const char* env = std::getenv("OPENMW_ESM4_STANDING_ARM_IK"))
            return std::string_view(env) != "0";
        if (const char* env = std::getenv("OPENMW_FNV_STANDING_ARM_IK"))
            return std::string_view(env) != "0";
        return false;
    }

    bool applyFalloutSeatedHumanIk(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        const bool seatedIk = shouldApplyFalloutSeatedHumanIk();
        const bool standingLegIk = shouldApplyFalloutStandingLegIk(ptr);
        const bool standingArmIk = shouldApplyFalloutStandingArmIk(ptr);
        const bool legIk = seatedIk || standingLegIk;
        if (!seatedIk && !standingLegIk && !standingArmIk)
            return false;
        const char* ikMode = standingLegIk ? "standing-leg" : standingArmIk ? "standing-arm" : "seated-human";

        const auto requireBone = [&](const std::string& bone, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTarget(targets, bone);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            return true;
        };

        osg::Vec3f pelvis;
        osg::Vec3f spine2;
        osg::Vec3f head;
        osg::Vec3f leftKnee;
        osg::Vec3f rightKnee;
        osg::Vec3f leftFoot;
        osg::Vec3f rightFoot;
        osg::Vec3f leftHip;
        osg::Vec3f rightHip;
        osg::Vec3f leftShoulder;
        osg::Vec3f rightShoulder;
        osg::Vec3f leftElbow;
        osg::Vec3f rightElbow;
        osg::Vec3f leftHand;
        osg::Vec3f rightHand;
        if (!(requireBone("bip01 pelvis", pelvis) && requireBone("bip01 spine2", spine2)
                && requireBone("bip01 head", head) && requireBone("bip01 l calf", leftKnee)
                && requireBone("bip01 r calf", rightKnee) && requireBone("bip01 l foot", leftFoot)
                && requireBone("bip01 r foot", rightFoot)))
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: human IK mode=" << ikMode << " "
                                << ptr.getCellRef().getRefId() << " verdict=BAD reason=missing_bone";
            return false;
        }

        osg::Vec3f forward(head.x() - spine2.x(), head.y() - spine2.y(), 0.f);
        forward = normalizedOr(forward, osg::Vec3f(1.f, 0.f, 0.f));
        osg::Vec3f lateral = normalizedOr(rightKnee - leftKnee, osg::Vec3f(-forward.y(), forward.x(), 0.f));
        lateral.z() = 0.f;
        lateral = normalizedOr(lateral, osg::Vec3f(-forward.y(), forward.x(), 0.f));
        const osg::Vec3f up(0.f, 0.f, 1.f);
        const bool hasHipRoots = requireBone("bip01 l thigh", leftHip) && requireBone("bip01 r thigh", rightHip);

        const float leftThighLength = std::clamp((leftKnee - pelvis).length(), 18.f, 42.f);
        const float rightThighLength = std::clamp((rightKnee - pelvis).length(), 18.f, 42.f);
        const float leftCalfLength = std::clamp((leftFoot - leftKnee).length(), 18.f, 42.f);
        const float rightCalfLength = std::clamp((rightFoot - rightKnee).length(), 18.f, 42.f);
        const float sourceFootDrop = (pelvis.z() - leftFoot.z() + pelvis.z() - rightFoot.z()) * 0.5f;
        const float standingLegLength
            = (leftThighLength + rightThighLength + leftCalfLength + rightCalfLength) * 0.5f;
        const float kneeForward = standingLegIk
            ? std::clamp(
                (osg::Vec2f(leftKnee.x() - pelvis.x(), leftKnee.y() - pelvis.y()).length()
                    + osg::Vec2f(rightKnee.x() - pelvis.x(), rightKnee.y() - pelvis.y()).length())
                    * 0.18f,
                3.f, 8.f)
            : std::clamp(
                (osg::Vec2f(leftKnee.x() - pelvis.x(), leftKnee.y() - pelvis.y()).length()
                    + osg::Vec2f(rightKnee.x() - pelvis.x(), rightKnee.y() - pelvis.y()).length())
                    * 0.5f,
                22.f, 34.f);
        const float side = standingLegIk && hasHipRoots ? std::clamp((rightHip - leftHip).length() * 0.5f, 4.f, 11.f)
                                                        : std::clamp((rightKnee - leftKnee).length() * 0.25f, 5.f, 10.f);
        const float footForward = standingLegIk ? std::clamp(kneeForward * 0.35f, 1.5f, 4.f) : kneeForward + 4.f;
        const float footDrop = standingLegIk ? std::clamp(std::max(sourceFootDrop, standingLegLength * 0.92f), 54.f, 68.f)
                                             : std::clamp(sourceFootDrop, 24.f, 42.f);
        const float footZ = pelvis.z() - footDrop;

        const osg::Vec3f leftHipBase = hasHipRoots ? leftHip : pelvis - lateral * side;
        const osg::Vec3f rightHipBase = hasHipRoots ? rightHip : pelvis + lateral * side;
        const osg::Vec3f leftFootTarget = standingLegIk
            ? osg::Vec3f(leftHipBase.x() + forward.x() * footForward,
                leftHipBase.y() + forward.y() * footForward, footZ)
            : pelvis + forward * footForward - lateral * side - up * footDrop;
        const osg::Vec3f rightFootTarget = standingLegIk
            ? osg::Vec3f(rightHipBase.x() + forward.x() * footForward,
                rightHipBase.y() + forward.y() * footForward, footZ)
            : osg::Vec3f(pelvis.x() + forward.x() * footForward + lateral.x() * side,
                pelvis.y() + forward.y() * footForward + lateral.y() * side, footZ);
        const osg::Vec3f leftKneeTarget
            = solveFalloutTwoBoneKnee(leftHipBase, leftFootTarget, leftThighLength, leftCalfLength,
                standingLegIk ? forward + up * 0.04f : forward + up * 0.15f);
        const osg::Vec3f rightKneeTarget
            = solveFalloutTwoBoneKnee(rightHipBase, rightFootTarget, rightThighLength, rightCalfLength,
                standingLegIk ? forward + up * 0.04f : forward + up * 0.15f);

        bool legTargetsApplied = false;
        if (legIk)
        {
            legTargetsApplied = setFalloutTargetsWorldOrigin(targets, "bip01 l calf", leftKneeTarget)
                && setFalloutTargetsWorldOrigin(targets, "bip01 r calf", rightKneeTarget)
                && setFalloutTargetsWorldOrigin(targets, "bip01 l foot", leftFootTarget)
                && setFalloutTargetsWorldOrigin(targets, "bip01 r foot", rightFootTarget);
        }

        bool appliedUpper = false;
        if (requireBone("bip01 l upperarm", leftShoulder) && requireBone("bip01 r upperarm", rightShoulder)
            && requireBone("bip01 l forearm", leftElbow) && requireBone("bip01 r forearm", rightElbow)
            && requireBone("bip01 l hand", leftHand) && requireBone("bip01 r hand", rightHand))
        {
            const osg::Vec3f shoulderMid = (leftShoulder + rightShoulder) * 0.5f;
            const float targetShoulderZ = pelvis.z() + 22.f;
            const float shoulderDz = targetShoulderZ - shoulderMid.z();
            const osg::Vec3f shoulderDelta(0.f, 0.f, shoulderDz);
            osg::Vec3f leftShoulderTarget = leftShoulder + shoulderDelta;
            osg::Vec3f rightShoulderTarget = rightShoulder + shoulderDelta;
            osg::Vec3f leftElbowTarget = leftShoulderTarget + forward * 6.f - lateral * 5.f - up * 15.f;
            osg::Vec3f rightElbowTarget = rightShoulderTarget + forward * 6.f + lateral * 5.f - up * 15.f;
            osg::Vec3f leftHandTarget = leftElbowTarget + forward * 5.f + lateral * 5.f - up * 15.f;
            osg::Vec3f rightHandTarget = rightElbowTarget + forward * 5.f - lateral * 5.f - up * 15.f;
            const osg::Vec3f headTarget(head.x(), head.y(), targetShoulderZ + 10.f);
            const float targetUpperLength = std::clamp(
                ((leftElbow - leftShoulder).length() + (rightElbow - rightShoulder).length()) * 0.5f, 12.f, 22.f);
            const float targetLowerLength
                = std::clamp(((leftHand - leftElbow).length() + (rightHand - rightElbow).length()) * 0.5f, 12.f, 22.f);

            if (osg::MatrixTransform* root = findFalloutTarget(targets, "bip01"))
            {
                osg::Matrix rootToWorld = getFalloutNodeWorldMatrix(root);
                osg::Matrix worldToRoot;
                if (worldToRoot.invert(rootToWorld))
                {
                    const auto mirroredWorld = [&](const osg::Vec3f& worldPoint, char axis) {
                        osg::Vec3f local = transformFalloutPoint(worldPoint, worldToRoot);
                        if (axis == 'x')
                            local.x() = -local.x();
                        else if (axis == 'y')
                            local.y() = -local.y();
                        else
                            local.z() = -local.z();
                        return transformFalloutPoint(local, rootToWorld);
                    };

                    float bestScore = std::numeric_limits<float>::max();
                    osg::Vec3f bestLeftElbow = leftElbowTarget;
                    osg::Vec3f bestLeftHand = leftHandTarget;
                    osg::Vec3f bestRightElbow = rightElbowTarget;
                    osg::Vec3f bestRightHand = rightHandTarget;
                    for (char axis : { 'x', 'y', 'z' })
                    {
                        for (float handForward = 2.f; handForward <= 18.f; handForward += 2.f)
                        {
                            for (float handSide = -12.f; handSide <= 12.f; handSide += 3.f)
                            {
                                for (float handDrop = 8.f; handDrop <= 24.f; handDrop += 2.f)
                                {
                                    const osg::Vec3f candidateLeftHand
                                        = leftShoulderTarget + forward * handForward + lateral * handSide
                                        - up * handDrop;
                                    const osg::Vec3f candidateLeftElbow = solveFalloutTwoBoneKnee(leftShoulderTarget,
                                        candidateLeftHand, targetUpperLength, targetLowerLength,
                                        forward - lateral * 0.4f - up * 0.2f);
                                    const osg::Vec3f candidateRightElbow = mirroredWorld(candidateLeftElbow, axis);
                                    const osg::Vec3f candidateRightHand = mirroredWorld(candidateLeftHand, axis);
                                    const float leftUpperLength = (candidateLeftElbow - leftShoulderTarget).length();
                                    const float rightUpperLength
                                        = (candidateRightElbow - rightShoulderTarget).length();
                                    const float leftLowerLength = (candidateLeftHand - candidateLeftElbow).length();
                                    const float rightLowerLength = (candidateRightHand - candidateRightElbow).length();
                                    const float leftWholeLength = (candidateLeftHand - leftShoulderTarget).length();
                                    const float rightWholeLength = (candidateRightHand - rightShoulderTarget).length();
                                    const float collapsePenalty = leftUpperLength < 4.f || rightUpperLength < 4.f
                                            || leftLowerLength < 4.f || rightLowerLength < 4.f
                                        ? 1000.f
                                        : 0.f;
                                    const float reachPenalty = leftWholeLength < 10.f || rightWholeLength < 10.f
                                            || leftWholeLength > targetUpperLength + targetLowerLength
                                            || rightWholeLength > targetUpperLength + targetLowerLength
                                        ? 250.f
                                        : 0.f;
                                    const float score = collapsePenalty + reachPenalty
                                        + std::abs(rightWholeLength - leftWholeLength) * 25.f
                                        + std::abs(leftWholeLength - 20.f) * 0.25f
                                        + std::abs(rightUpperLength - targetUpperLength) * 0.25f
                                        + std::abs(rightLowerLength - targetLowerLength) * 0.25f;
                                    if (score < bestScore)
                                    {
                                        bestScore = score;
                                        bestLeftElbow = candidateLeftElbow;
                                        bestLeftHand = candidateLeftHand;
                                        bestRightElbow = candidateRightElbow;
                                        bestRightHand = candidateRightHand;
                                    }
                                }
                            }
                        }
                    }

                    leftElbowTarget = bestLeftElbow;
                    leftHandTarget = bestLeftHand;
                    rightElbowTarget = bestRightElbow;
                    rightHandTarget = bestRightHand;
                }
            }

            if (standingArmIk)
            {
                leftShoulderTarget = leftShoulder;
                rightShoulderTarget = rightShoulder;
                const float shoulderWidth = (rightShoulderTarget - leftShoulderTarget).length();
                const float armDrop = std::clamp(shoulderMid.z() - pelvis.z() - 1.5f, 26.f, 36.f);
                const float handSide = std::clamp(shoulderWidth * 0.12f, 2.5f, 5.0f);
                const float handForward = 2.5f;
                leftHandTarget = leftShoulderTarget + forward * handForward - lateral * handSide - up * armDrop;
                rightHandTarget = rightShoulderTarget + forward * handForward + lateral * handSide - up * armDrop;
                leftElbowTarget = solveFalloutTwoBoneKnee(leftShoulderTarget, leftHandTarget, targetUpperLength,
                    targetLowerLength, forward - lateral * 0.35f - up * 0.05f);
                rightElbowTarget = solveFalloutTwoBoneKnee(rightShoulderTarget, rightHandTarget, targetUpperLength,
                    targetLowerLength, forward + lateral * 0.35f - up * 0.05f);

                // Upper-body actor repair must be applied as bone rotations. Writing world-space origins here
                // can make telemetry look better while detaching the skinned limb chain.
                appliedUpper = false;
            }

            Log(appliedUpper ? Debug::Info : Debug::Warning)
                << "FNV/ESM4 diag: human IK upper mode=" << ikMode << " " << ptr.getCellRef().getRefId()
                << " shoulderMidBefore=" << formatFalloutAuditVec3(shoulderMid)
                << " shoulderDz=" << shoulderDz
                << " leftShoulderTarget=" << formatFalloutAuditVec3(leftShoulderTarget)
                << " rightShoulderTarget=" << formatFalloutAuditVec3(rightShoulderTarget)
                << " leftElbowTarget=" << formatFalloutAuditVec3(leftElbowTarget)
                << " rightElbowTarget=" << formatFalloutAuditVec3(rightElbowTarget)
                << " leftHandTarget=" << formatFalloutAuditVec3(leftHandTarget)
                << " rightHandTarget=" << formatFalloutAuditVec3(rightHandTarget)
                << " headTarget=" << formatFalloutAuditVec3(headTarget)
                << " applied=" << (appliedUpper ? 1 : 0)
                << " reason=" << (appliedUpper ? "standing_arm_ik" : "rotation_space_required")
                << " verdict=" << (appliedUpper ? "OK" : "BAD");
        }

        const bool overallApplied = legIk ? legTargetsApplied : appliedUpper;
        Log(overallApplied ? Debug::Info : Debug::Warning)
            << "FNV/ESM4 diag: human IK mode=" << ikMode << " " << ptr.getCellRef().getRefId()
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " forward=" << formatFalloutAuditVec3(forward)
            << " lateral=" << formatFalloutAuditVec3(lateral)
            << " leftKneeBefore=" << formatFalloutAuditVec3(leftKnee)
            << " rightKneeBefore=" << formatFalloutAuditVec3(rightKnee)
            << " leftFootBefore=" << formatFalloutAuditVec3(leftFoot)
            << " rightFootBefore=" << formatFalloutAuditVec3(rightFoot)
            << " leftHipBase=" << formatFalloutAuditVec3(leftHipBase)
            << " rightHipBase=" << formatFalloutAuditVec3(rightHipBase)
            << " sourceFootDrop=" << sourceFootDrop
            << " footDrop=" << footDrop
            << " kneeForward=" << kneeForward
            << " footForward=" << footForward
            << " leftKneeTarget=" << formatFalloutAuditVec3(leftKneeTarget)
            << " rightKneeTarget=" << formatFalloutAuditVec3(rightKneeTarget)
            << " leftFootTarget=" << formatFalloutAuditVec3(leftFootTarget)
            << " rightFootTarget=" << formatFalloutAuditVec3(rightFootTarget)
            << " upperApplied=" << appliedUpper
            << " legApplied=" << legTargetsApplied
            << " verdict=" << (overallApplied ? "OK" : "BAD");
        return overallApplied;
    }

    bool auditFalloutWorldPosture(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        const auto requireBone = [&](std::initializer_list<std::string_view> bones, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTargetAny(targets, bones);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            return true;
        };

        osg::Vec3f head;
        osg::Vec3f neck;
        osg::Vec3f spine2;
        osg::Vec3f pelvis;
        osg::Vec3f leftFoot;
        osg::Vec3f rightFoot;
        const bool hasRequired = requireBone({ "bip01 head" }, head)
            && requireBone({ "bip01 neck1", "bip01 neck" }, neck)
            && requireBone({ "bip01 spine2", "bip01 spine1", "bip01 spine" }, spine2)
            && requireBone({ "bip01 pelvis" }, pelvis)
            && requireBone({ "bip01 l foot" }, leftFoot) && requireBone({ "bip01 r foot" }, rightFoot);
        if (!hasRequired)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: world posture " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_bone";
            return false;
        }

        const osg::Vec3f feetMid = (leftFoot + rightFoot) * 0.5f;
        const float headAbovePelvis = head.z() - pelvis.z();
        const float headAboveFeet = head.z() - feetMid.z();
        const float pelvisAboveFeet = pelvis.z() - feetMid.z();
        const float torsoHorizontal = osg::Vec2f(head.x() - pelvis.x(), head.y() - pelvis.y()).length();
        const float footSpread = (leftFoot - rightFoot).length();
        const bool seatedAudit = std::getenv("OPENMW_FNV_SEATED_POSTURE_AUDIT") != nullptr;
        const bool badHeadPelvis = seatedAudit ? headAbovePelvis < 12.f : headAbovePelvis < 35.f;
        const bool badHeadFeet = seatedAudit ? headAboveFeet < 18.f : headAboveFeet < 25.f;
        const bool badPelvisFeet = seatedAudit ? pelvisAboveFeet < -35.f : pelvisAboveFeet < -25.f;
        const bool badTorsoLayover = seatedAudit ? torsoHorizontal > 70.f
                                                 : torsoHorizontal > std::max(55.f, headAbovePelvis * 1.5f);
        const bool badFootSpread = footSpread > (seatedAudit ? 120.f : 95.f);
        const bool bad = badHeadPelvis || badHeadFeet || badPelvisFeet || badTorsoLayover || badFootSpread;
        const char* reason = badHeadPelvis ? "head_pelvis_z"
            : badHeadFeet                  ? "head_feet_z"
            : badPelvisFeet                ? "pelvis_feet_z"
            : badTorsoLayover              ? "torso_horizontal"
            : badFootSpread                ? "foot_spread"
                                           : "ok";

        Log(bad ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: world posture " << ptr.getCellRef().getRefId()
            << " head=" << formatFalloutAuditVec3(head)
            << " neck=" << formatFalloutAuditVec3(neck)
            << " spine2=" << formatFalloutAuditVec3(spine2)
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " leftFoot=" << formatFalloutAuditVec3(leftFoot)
            << " rightFoot=" << formatFalloutAuditVec3(rightFoot)
            << " headAbovePelvis=" << headAbovePelvis
            << " headAboveFeet=" << headAboveFeet
            << " pelvisAboveFeet=" << pelvisAboveFeet
            << " torsoHorizontal=" << torsoHorizontal
            << " footSpread=" << footSpread
            << " auditMode=" << (seatedAudit ? "seated" : "standing")
            << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        if (seatedAudit)
        {
            const FalloutSeatedLegChainAudit legAudit = auditFalloutSeatedLegChain(targets, ptr);
            return !bad && legAudit.mValid && !legAudit.mBad;
        }

        return !bad;
    }

    bool shouldAuditFalloutStandingUpperBody()
    {
        return std::getenv("OPENMW_ESM4_STANDING_UPPER_AUDIT") != nullptr
            || std::getenv("OPENMW_FNV_STANDING_UPPER_AUDIT") != nullptr;
    }

    bool auditFalloutStandingUpperBody(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        const auto requireBone = [&](std::initializer_list<std::string_view> bones, osg::Vec3f& value) {
            osg::MatrixTransform* node = findFalloutTargetAny(targets, bones);
            if (node == nullptr)
                return false;
            value = transformFalloutPoint(osg::Vec3f(), getFalloutNodeWorldMatrix(node));
            return true;
        };

        osg::Vec3f pelvis;
        osg::Vec3f leftShoulder;
        osg::Vec3f rightShoulder;
        osg::Vec3f leftElbow;
        osg::Vec3f rightElbow;
        osg::Vec3f leftHand;
        osg::Vec3f rightHand;
        const bool hasRequired = requireBone({ "bip01 pelvis" }, pelvis)
            && requireBone({ "bip01 l upperarm" }, leftShoulder)
            && requireBone({ "bip01 r upperarm" }, rightShoulder)
            && requireBone({ "bip01 l forearm" }, leftElbow)
            && requireBone({ "bip01 r forearm" }, rightElbow)
            && requireBone({ "bip01 l hand" }, leftHand)
            && requireBone({ "bip01 r hand" }, rightHand);
        if (!hasRequired)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: standing upper body audit " << ptr.getCellRef().getRefId()
                                << " verdict=BAD reason=missing_bone";
            return false;
        }

        const osg::Vec3f shoulderMid = (leftShoulder + rightShoulder) * 0.5f;
        const osg::Vec3f handMid = (leftHand + rightHand) * 0.5f;
        const float shoulderSpan = (leftShoulder - rightShoulder).length();
        const float handSpan = (leftHand - rightHand).length();
        const float elbowSpan = (leftElbow - rightElbow).length();
        const float handSpreadRatio = handSpan / std::max(1.f, shoulderSpan);
        const float leftShoulderToHand = (leftHand - leftShoulder).length();
        const float rightShoulderToHand = (rightHand - rightShoulder).length();
        const float leftUpperLength = (leftElbow - leftShoulder).length();
        const float rightUpperLength = (rightElbow - rightShoulder).length();
        const float leftForearmLength = (leftHand - leftElbow).length();
        const float rightForearmLength = (rightHand - rightElbow).length();
        const float handMidDrop = shoulderMid.z() - handMid.z();
        const float leftHandDrop = leftShoulder.z() - leftHand.z();
        const float rightHandDrop = rightShoulder.z() - rightHand.z();
        const float handMidPelvisZ = handMid.z() - pelvis.z();
        const float minWeaponHandSpan = std::max(18.f, shoulderSpan * 0.65f);
        const bool chestLevelHands = handMidDrop >= 8.f && handMidDrop <= 28.f && handMidPelvisZ > 6.f;
        const bool badTightHands = chestLevelHands && handSpan < minWeaponHandSpan;
        const bool badWideHands = handSpan > std::max(62.f, shoulderSpan * 2.25f);
        const bool badWideElbows = elbowSpan > std::max(58.f, shoulderSpan * 2.1f);
        const bool aimLikeHands = handSpan <= std::max(42.f, shoulderSpan * 1.45f)
            && elbowSpan <= std::max(38.f, shoulderSpan * 1.65f);
        const bool badHighHands = handMidDrop < -40.f || (!aimLikeHands && handMidDrop < 8.f);
        const bool badCollapsed
            = leftUpperLength < 6.f || rightUpperLength < 6.f || leftForearmLength < 6.f || rightForearmLength < 6.f;
        const bool badArmStretch
            = leftUpperLength > 34.f || rightUpperLength > 34.f || leftForearmLength > 34.f || rightForearmLength > 34.f
            || leftShoulderToHand > 55.f || rightShoulderToHand > 55.f;
        const bool badAsymmetry = std::abs(leftShoulderToHand - rightShoulderToHand) > 18.f;
        const bool bad = badWideHands || badTightHands || badWideElbows || badHighHands || badCollapsed
            || badArmStretch || badAsymmetry;
        const char* reason = badWideHands ? "hand_span"
            : badTightHands              ? "hand_collapse"
            : badWideElbows               ? "elbow_span"
            : badHighHands                ? "hand_height"
            : badCollapsed                ? "arm_collapse"
            : badArmStretch               ? "arm_stretch"
            : badAsymmetry                ? "arm_asymmetry"
                                          : "ok";

        Log(bad ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: standing upper body audit " << ptr.getCellRef().getRefId()
            << " pelvis=" << formatFalloutAuditVec3(pelvis)
            << " leftShoulder=" << formatFalloutAuditVec3(leftShoulder)
            << " rightShoulder=" << formatFalloutAuditVec3(rightShoulder)
            << " leftElbow=" << formatFalloutAuditVec3(leftElbow)
            << " rightElbow=" << formatFalloutAuditVec3(rightElbow)
            << " leftHand=" << formatFalloutAuditVec3(leftHand)
            << " rightHand=" << formatFalloutAuditVec3(rightHand)
            << " shoulderSpan=" << shoulderSpan
            << " elbowSpan=" << elbowSpan
            << " handSpan=" << handSpan
            << " handSpreadRatio=" << handSpreadRatio
            << " leftShoulderToHand=" << leftShoulderToHand
            << " rightShoulderToHand=" << rightShoulderToHand
            << " leftUpperLength=" << leftUpperLength
            << " rightUpperLength=" << rightUpperLength
            << " leftForearmLength=" << leftForearmLength
            << " rightForearmLength=" << rightForearmLength
            << " handMidDrop=" << handMidDrop
            << " leftHandDrop=" << leftHandDrop
            << " rightHandDrop=" << rightHandDrop
            << " handMidPelvisZ=" << handMidPelvisZ
            << " minWeaponHandSpan=" << minWeaponHandSpan
            << " chestLevelHands=" << chestLevelHands
            << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        return !bad;
    }

    bool shouldAuditGenericProofPosture(const MWWorld::Ptr& ptr)
    {
        const char* target = std::getenv("OPENMW_PROOF_POSTURE_TARGET");
        if (target == nullptr || *target == '\0')
            return false;

        const std::string targetLower = Misc::StringUtils::lowerCase(target);
        const std::string idLower = Misc::StringUtils::lowerCase(ptr.getCellRef().getRefId().toDebugString());
        if (targetLower == idLower)
            return true;

        const std::string ptrLower = Misc::StringUtils::lowerCase(ptr.toString());
        if (ptrLower.find(targetLower) != std::string::npos)
            return true;

        try
        {
            const std::string nameLower = Misc::StringUtils::lowerCase(std::string(ptr.getClass().getName(ptr)));
            if (!nameLower.empty()
                && (nameLower == targetLower || nameLower.find(targetLower) != std::string::npos
                    || targetLower.find(nameLower) != std::string::npos))
                return true;
        }
        catch (const std::exception&) {}

        return false;
    }

    void auditGenericProofPosture(osg::Node* root, const MWWorld::Ptr& ptr)
    {
        if (root == nullptr || !shouldAuditGenericProofPosture(ptr))
            return;

        static std::unordered_map<std::string, int> sSamples;
        const std::string key = ptr.toString();
        int& samples = sSamples[key];
        const int maxSamples = [] {
            if (const char* value = std::getenv("OPENMW_PROOF_POSTURE_SAMPLES"))
            {
                try
                {
                    return std::max(1, std::stoi(value));
                }
                catch (...) {}
            }
            return 8;
        }();
        if (samples >= maxSamples)
            return;
        ++samples;

        std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> targets;
        FalloutTransformTargetVisitor visitor(targets);
        root->accept(visitor);

        Log(Debug::Info) << "OPENMW PROOF posture sample target=" << ptr.getCellRef().getRefId()
                         << " sample=" << samples
                         << " targetCount=" << targets.size()
                         << " label="
                         << (std::getenv("OPENMW_PROOF_POSTURE_LABEL") != nullptr
                                    ? std::getenv("OPENMW_PROOF_POSTURE_LABEL")
                                    : "");
        auditFalloutSkeletonBounds(targets, ptr);
        auditFalloutWorldPosture(targets, ptr);
        if (shouldAuditFalloutStandingUpperBody())
            auditFalloutStandingUpperBody(targets, ptr);
        auditFalloutMirrorSymmetry(targets, ptr);
        if (std::getenv("OPENMW_PROOF_POSTURE_SEATED") != nullptr)
        {
            auditFalloutSeatedLegChain(targets, ptr);
            auditFalloutSeatedUpperBody(targets, ptr);
        }
    }

    std::string getFalloutRuntimePartClass(std::string_view name)
    {
        std::string lowered(name);
        Misc::StringUtils::lowerCaseInPlace(lowered);
        if (lowered.find("headold") != std::string::npos)
            return "head";
        if (lowered.find("weapon") != std::string::npos || lowered.find("revolver") != std::string::npos
            || lowered.find("pistol") != std::string::npos || lowered.find("gun") != std::string::npos)
            return "weapon";
        if (lowered.find("headgear") != std::string::npos || lowered.find("hat") != std::string::npos)
            return "headgear";
        if (lowered.find("teeth") != std::string::npos)
            return "faceTeeth";
        if (lowered.find("tongue") != std::string::npos)
            return "faceTongue";
        if (lowered.find("lefthand") != std::string::npos)
            return "leftHand";
        if (lowered.find("righthand") != std::string::npos)
            return "rightHand";
        if (lowered.find("mouth") != std::string::npos)
            return "faceMouth";
        if (lowered.find("eye") != std::string::npos)
            return "faceEye";
        if (lowered.find("beard") != std::string::npos)
            return "faceBeard";
        if (lowered.find("hair") != std::string::npos)
            return "faceHair";
        if (lowered.find("brow") != std::string::npos)
            return "faceBrow";
        if (lowered.find("armor") != std::string::npos || lowered.find("clothes") != std::string::npos
            || lowered.find("republican") != std::string::npos)
            return "body";
        return "other";
    }

    bool isFalloutFaceRuntimePartClass(std::string_view partClass)
    {
        return partClass == "face" || partClass == "faceEye" || partClass == "faceMouth"
            || partClass == "faceTeeth" || partClass == "faceTongue" || partClass == "faceBeard"
            || partClass == "faceHair" || partClass == "faceBrow";
    }

    osg::Matrix getFalloutPartWorldMatrix(osg::Node* part)
    {
        osg::Matrix world = getFalloutNodeWorldMatrix(part);
        if (matrixDifference(osg::Matrixf(world), osg::Matrixf()) < 0.00001f)
            world = getFalloutParentWorldMatrix(part);
        return world;
    }

    void logFalloutPartMatrixAudit(osg::Node* part, const std::string& partClass, const osg::Matrix& anchorWorld,
        const osg::Vec3f& center, const osg::Vec3f& anchor,
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        if (part == nullptr)
            return;

        const osg::Matrix partWorld = getFalloutPartWorldMatrix(part);
        const osg::Matrix parentWorld = getFalloutParentWorldMatrix(part);
        const osg::Matrix partInAnchor = partWorld * osg::Matrix::inverse(anchorWorld);
        const osg::Matrix partInParent = partWorld * osg::Matrix::inverse(parentWorld);
        const osg::Quat partWorldQuat = partWorld.getRotate();
        const osg::Quat anchorWorldQuat = anchorWorld.getRotate();
        const osg::Quat partInAnchorQuat = partInAnchor.getRotate();
        const float anchorAngle = falloutQuatAngleDegrees(partWorldQuat, anchorWorldQuat);

        std::string candidateDistances;
        if (partClass == "head" || isFalloutFaceRuntimePartClass(partClass) || partClass == "headgear")
        {
            candidateDistances = describeFalloutClosestTarget(center, targets,
                { "bip01 head", "bip01 head_nub", "bip01 neck1", "bip01 neck", "bip01 spine2" });
        }
        else if (partClass == "leftHand")
        {
            candidateDistances = describeFalloutClosestTarget(center, targets,
                { "bip01 l forearm", "bip01 l foretwist", "bip01 l hand", "bip01 l finger0",
                    "bip01 l finger1", "bip01 l finger2", "bip01 l finger3", "bip01 l finger4" });
        }
        else if (partClass == "rightHand")
        {
            candidateDistances = describeFalloutClosestTarget(center, targets,
                { "bip01 r forearm", "bip01 r foretwist", "bip01 r hand", "bip01 r finger0",
                    "bip01 r finger1", "bip01 r finger2", "bip01 r finger3", "bip01 r finger4" });
        }
        else if (partClass == "weapon")
        {
            candidateDistances = describeFalloutClosestTarget(center, targets,
                { "weapon", "bip01 r hand", "bip01 r forearm", "bip01 r finger0", "bip01 r finger1",
                    "bip01 r finger2", "bip01 r finger3", "bip01 r finger4" });
        }

        Log(Debug::Info) << "FNV/ESM4 PART MATRIX AUDIT " << ptr.getCellRef().getRefId()
                         << " part='" << part->getName() << "' class=" << partClass
                         << " center=" << formatFalloutAuditVec3(center)
                         << " anchor=" << formatFalloutAuditVec3(anchor)
                         << " partWorldTrans=" << formatFalloutAuditVec3(partWorld.getTrans())
                         << " parentWorldTrans=" << formatFalloutAuditVec3(parentWorld.getTrans())
                         << " partInParentTrans=" << formatFalloutAuditVec3(partInParent.getTrans())
                         << " partInAnchorTrans=" << formatFalloutAuditVec3(partInAnchor.getTrans())
                         << " partWorldQuat=" << formatFalloutAuditQuat(partWorldQuat)
                         << " anchorWorldQuat=" << formatFalloutAuditQuat(anchorWorldQuat)
                         << " partInAnchorQuat=" << formatFalloutAuditQuat(partInAnchorQuat)
                         << " anchorAngleDeg=" << anchorAngle
                         << " finiteCenter=" << isFiniteFalloutAuditVec3(center)
                         << " finiteAnchor=" << isFiniteFalloutAuditVec3(anchor)
                         << " finitePartWorld=" << isFiniteFalloutAuditMatrix(partWorld)
                         << " finiteParentWorld=" << isFiniteFalloutAuditMatrix(parentWorld)
                         << " finiteAnchorWorld=" << isFiniteFalloutAuditMatrix(anchorWorld)
                         << " finitePartInParent=" << isFiniteFalloutAuditMatrix(partInParent)
                         << " finitePartInAnchor=" << isFiniteFalloutAuditMatrix(partInAnchor)
                         << " partHandedness=" << falloutMatrixBasisHandedness(partWorld)
                         << " anchorHandedness=" << falloutMatrixBasisHandedness(anchorWorld)
                         << (candidateDistances.empty() ? "" : " candidateTargets=") << candidateDistances;
    }

    bool shouldAuditFalloutRootAttachment()
    {
        return std::getenv("OPENMW_FNV_ROOT_ATTACHMENT_AUDIT") != nullptr
            || std::getenv("OPENMW_ESM4_ROOT_ATTACHMENT_AUDIT") != nullptr;
    }

    bool shouldAuditFalloutActorRenderLiveGeometry()
    {
        return std::getenv("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY") != nullptr
            || std::getenv("OPENMW_ESM4_ROOT_ATTACHMENT_AUDIT") != nullptr
            || std::getenv("OPENMW_ESM4_STANDING_UPPER_AUDIT") != nullptr
            || std::getenv("OPENMW_FNV_PART_MATRIX_AUDIT") != nullptr;
    }

    osg::Vec3f falloutMatrixBasisRow(const osg::Matrix& matrix, int row)
    {
        return osg::Vec3f(matrix(row, 0), matrix(row, 1), matrix(row, 2));
    }

    void auditFalloutRootAttachmentFrame(osg::Group* objectRoot,
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr)
    {
        if (!shouldAuditFalloutRootAttachment() || objectRoot == nullptr)
            return;

        static std::unordered_map<std::string, unsigned int> sRootAttachmentSamples;
        const std::string refId = ptr.getCellRef().getRefId().serializeText();
        unsigned int& samples = sRootAttachmentSamples[refId];
        if (samples >= 6)
            return;
        ++samples;

        osg::Node* bip01Node = findFalloutTarget(targets, "bip01");
        osg::Node* pelvisNode = findFalloutTarget(targets, "bip01 pelvis");
        osg::Node* spine2Node = findFalloutTarget(targets, "bip01 spine2");
        osg::Node* headNode = findFalloutTarget(targets, "bip01 head");
        osg::Node* leftFootNode = findFalloutTarget(targets, "bip01 l foot");
        osg::Node* rightFootNode = findFalloutTarget(targets, "bip01 r foot");
        osg::Node* leftHandNode = findFalloutTarget(targets, "bip01 l hand");
        osg::Node* rightHandNode = findFalloutTarget(targets, "bip01 r hand");
        osg::Node* weaponNode = findFalloutTarget(targets, "weapon");

        const auto localMatrix = [](osg::Node* node) {
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(node))
                return transform->getMatrix();
            return osg::Matrix{};
        };

        const osg::Matrix rootWorld = getFalloutNodeWorldMatrix(objectRoot);
        const osg::Matrix bip01World = getFalloutNodeWorldMatrix(bip01Node);
        const osg::Vec3f root = transformFalloutPoint(osg::Vec3f(), rootWorld);
        const osg::Vec3f bip01 = transformFalloutPoint(osg::Vec3f(), bip01World);
        const osg::Vec3f pelvis = getFalloutTargetWorldOrigin(targets, "bip01 pelvis");
        const osg::Vec3f spine2 = getFalloutTargetWorldOrigin(targets, "bip01 spine2");
        const osg::Vec3f head = getFalloutTargetWorldOrigin(targets, "bip01 head");
        const osg::Vec3f leftFoot = getFalloutTargetWorldOrigin(targets, "bip01 l foot");
        const osg::Vec3f rightFoot = getFalloutTargetWorldOrigin(targets, "bip01 r foot");
        const osg::Vec3f leftHand = getFalloutTargetWorldOrigin(targets, "bip01 l hand");
        const osg::Vec3f rightHand = getFalloutTargetWorldOrigin(targets, "bip01 r hand");
        const osg::Vec3f weapon = getFalloutTargetWorldOrigin(targets, "weapon");
        const osg::Vec3f footMid = (leftFoot + rightFoot) * 0.5f;
        const osg::Matrix leftHandLocal = localMatrix(leftHandNode);
        const osg::Matrix rightHandLocal = localMatrix(rightHandNode);
        const osg::Matrix weaponLocal = localMatrix(weaponNode);

        Log(Debug::Info) << "FNV/ESM4 ACTOR ROOT ATTACHMENT AUDIT " << ptr.getCellRef().getRefId()
                         << " sample=" << samples
                         << " objectRoot='" << objectRoot->getName() << "'"
                         << " targetCount=" << targets.size()
                         << " hasBip01=" << (bip01Node != nullptr)
                         << " hasPelvis=" << (pelvisNode != nullptr)
                         << " hasSpine2=" << (spine2Node != nullptr)
                         << " hasHead=" << (headNode != nullptr)
                         << " hasLeftFoot=" << (leftFootNode != nullptr)
                         << " hasRightFoot=" << (rightFootNode != nullptr)
                         << " hasWeapon=" << (weaponNode != nullptr)
                         << " root=" << formatFalloutAuditVec3(root)
                         << " rootBasisX=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rootWorld, 0))
                         << " rootBasisY=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rootWorld, 1))
                         << " rootBasisZ=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rootWorld, 2))
                         << " rootHandedness=" << falloutMatrixBasisHandedness(rootWorld)
                         << " bip01=" << formatFalloutAuditVec3(bip01)
                         << " bip01BasisZ=" << formatFalloutAuditVec3(falloutMatrixBasisRow(bip01World, 2))
                         << " pelvis=" << formatFalloutAuditVec3(pelvis)
                         << " spine2=" << formatFalloutAuditVec3(spine2)
                         << " head=" << formatFalloutAuditVec3(head)
                         << " leftFoot=" << formatFalloutAuditVec3(leftFoot)
                         << " rightFoot=" << formatFalloutAuditVec3(rightFoot)
                          << " leftHand=" << formatFalloutAuditVec3(leftHand)
                          << " rightHand=" << formatFalloutAuditVec3(rightHand)
                          << " weapon=" << formatFalloutAuditVec3(weapon)
                          << " leftHandLocalT=" << formatFalloutAuditVec3(leftHandLocal.getTrans())
                          << " leftHandLocalX=" << formatFalloutAuditVec3(falloutMatrixBasisRow(leftHandLocal, 0))
                          << " leftHandLocalY=" << formatFalloutAuditVec3(falloutMatrixBasisRow(leftHandLocal, 1))
                          << " leftHandLocalZ=" << formatFalloutAuditVec3(falloutMatrixBasisRow(leftHandLocal, 2))
                          << " rightHandLocalT=" << formatFalloutAuditVec3(rightHandLocal.getTrans())
                          << " rightHandLocalX=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rightHandLocal, 0))
                          << " rightHandLocalY=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rightHandLocal, 1))
                          << " rightHandLocalZ=" << formatFalloutAuditVec3(falloutMatrixBasisRow(rightHandLocal, 2))
                          << " weaponLocalT=" << formatFalloutAuditVec3(weaponLocal.getTrans())
                          << " weaponLocalX=" << formatFalloutAuditVec3(falloutMatrixBasisRow(weaponLocal, 0))
                          << " weaponLocalY=" << formatFalloutAuditVec3(falloutMatrixBasisRow(weaponLocal, 1))
                          << " weaponLocalZ=" << formatFalloutAuditVec3(falloutMatrixBasisRow(weaponLocal, 2))
                          << " headMinusPelvis=" << formatFalloutAuditVec3(head - pelvis)
                         << " footMidMinusPelvis=" << formatFalloutAuditVec3(footMid - pelvis)
                         << " rootMinusPelvis=" << formatFalloutAuditVec3(root - pelvis);
    }

    struct FalloutTransformOracleConfig
    {
        std::string mOutputPath;
        std::uint32_t mTargetForm = 0;
        unsigned int mStartUpdate = 1;
        unsigned int mSampleEvery = 1;
        unsigned int mMaxSamples = 120;
        bool mEnabled = false;
    };

    unsigned int falloutTransformOracleEnvUnsigned(const char* name, unsigned int fallback)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return fallback;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end != value ? static_cast<unsigned int>(parsed) : fallback;
    }

    std::uint32_t falloutTransformOracleParseForm(std::string_view value)
    {
        const std::size_t hex = value.find("0x");
        const std::size_t offset = hex == std::string_view::npos ? 0 : hex + 2;
        const int base = hex == std::string_view::npos ? 0 : 16;
        const std::string token(value.substr(offset));
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(token.c_str(), &end, base);
        return end != token.c_str() ? static_cast<std::uint32_t>(parsed) : 0;
    }

    const FalloutTransformOracleConfig& getFalloutTransformOracleConfig()
    {
        static const FalloutTransformOracleConfig config = [] {
            FalloutTransformOracleConfig value;
            const char* output = std::getenv("OPENMW_ESM4_TRANSFORM_ORACLE_OUTPUT");
            const char* target = std::getenv("OPENMW_ESM4_TRANSFORM_ORACLE_REF");
            if (output == nullptr || *output == '\0' || target == nullptr || *target == '\0')
                return value;
            value.mOutputPath = output;
            value.mTargetForm = falloutTransformOracleParseForm(target);
            value.mStartUpdate
                = std::max(1u, falloutTransformOracleEnvUnsigned("OPENMW_ESM4_TRANSFORM_ORACLE_START", 1));
            value.mSampleEvery
                = std::max(1u, falloutTransformOracleEnvUnsigned("OPENMW_ESM4_TRANSFORM_ORACLE_EVERY", 1));
            value.mMaxSamples
                = std::max(1u, falloutTransformOracleEnvUnsigned("OPENMW_ESM4_TRANSFORM_ORACLE_MAX", 120));
            value.mEnabled = value.mTargetForm != 0;
            return value;
        }();
        return config;
    }

    void writeFalloutTransformOracleJsonString(std::ostream& stream, std::string_view value)
    {
        stream << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '\\':
                    stream << "\\\\";
                    break;
                case '"':
                    stream << "\\\"";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    stream << (ch < 0x20 ? '?' : static_cast<char>(ch));
                    break;
            }
        }
        stream << '"';
    }

    void writeFalloutTransformOracleVec3(std::ostream& stream, const osg::Vec3f& value)
    {
        stream << '[' << value.x() << ',' << value.y() << ',' << value.z() << ']';
    }

    void writeFalloutTransformOracleRotation(std::ostream& stream, const osg::Matrix& value)
    {
        stream << '[';
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                if (row != 0 || column != 0)
                    stream << ',';
                stream << value(row, column);
            }
        }
        stream << ']';
    }

    void writeFalloutTransformOracleTransform(
        std::ostream& stream, const osg::Matrix& local, const osg::Matrix& actor)
    {
        stream << "{\"localRotation\":";
        writeFalloutTransformOracleRotation(stream, local);
        stream << ",\"localTranslation\":";
        writeFalloutTransformOracleVec3(stream, local.getTrans());
        stream << ",\"localScale\":";
        writeFalloutTransformOracleVec3(stream, local.getScale());
        stream << ",\"actorRotation\":";
        writeFalloutTransformOracleRotation(stream, actor);
        stream << ",\"actorTranslation\":";
        writeFalloutTransformOracleVec3(stream, actor.getTrans());
        stream << ",\"actorScale\":";
        writeFalloutTransformOracleVec3(stream, actor.getScale());
        stream << '}';
    }

    void writeFalloutTransformOracleFrame(osg::Group* objectRoot,
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr,
        std::string_view activeGroups, const std::optional<osg::Vec3f>& accumulationTranslation)
    {
        const FalloutTransformOracleConfig& config = getFalloutTransformOracleConfig();
        if (!config.mEnabled || objectRoot == nullptr)
            return;

        const std::string refId = ptr.getCellRef().getRefId().serializeText();
        const std::uint32_t refForm = falloutTransformOracleParseForm(refId);
        if ((refForm & 0x00ffffffu) != (config.mTargetForm & 0x00ffffffu))
            return;

        struct State
        {
            unsigned int mUpdates = 0;
            unsigned int mSamples = 0;
            bool mOpenFailed = false;
            std::unique_ptr<std::ofstream> mOutput;
        };
        static State state;
        ++state.mUpdates;
        if (state.mSamples >= config.mMaxSamples || state.mUpdates < config.mStartUpdate
            || (state.mUpdates - config.mStartUpdate) % config.mSampleEvery != 0)
            return;

        if (state.mOutput == nullptr && !state.mOpenFailed)
        {
            state.mOutput = std::make_unique<std::ofstream>(config.mOutputPath, std::ios::out | std::ios::trunc);
            if (!*state.mOutput)
            {
                state.mOpenFailed = true;
                state.mOutput.reset();
                Log(Debug::Error) << "FNV/ESM4 transform oracle failed to open " << config.mOutputPath;
                return;
            }
            *state.mOutput << "{\"schema\":\"nikami-openmw-transform-oracle/v1\",\"event\":\"start\","
                              "\"coordinateSpace\":\"OSG-row-vector/local-and-actor\",\"ref\":";
            writeFalloutTransformOracleJsonString(*state.mOutput, refId);
            *state.mOutput << "}\n";
        }
        if (state.mOutput == nullptr)
            return;

        ++state.mSamples;
        const osg::Matrix actorWorld = getFalloutNodeWorldMatrix(objectRoot);
        const osg::Matrix worldToActor = osg::Matrix::inverse(actorWorld);
        static constexpr std::array<std::string_view, 67> nodeNames{ "Bip01", "Bip01 NonAccum",
            "Bip01 Looking", "Bip01 Translate", "Bip01 Rotate", "Bip", "Bip01 Pelvis", "Bip01 Spine",
            "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck", "Bip01 Neck1", "Bip01 Head", "Bip01 L Clavicle",
            "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 L Thumb1", "Bip01 L Thumb11",
            "Bip01 L Thumb12", "Bip01 L Finger1", "Bip01 L Finger11", "Bip01 L Finger12", "Bip01 L Finger2",
            "Bip01 L Finger21", "Bip01 L Finger22", "Bip01 L Finger3", "Bip01 L Finger31", "Bip01 L Finger32",
            "Bip01 L Finger4", "Bip01 L Finger41", "Bip01 L Finger42", "Bip01 L ForeTwist",
            "Bip01 LUpArmTwistBone", "Bip01 LPauldron", "Bip01 R Clavicle", "Bip01 R UpperArm",
            "Bip01 R Forearm", "Bip01 R Hand", "Bip01 R Thumb1", "Bip01 R Thumb11", "Bip01 R Thumb12",
            "Bip01 R Finger1", "Bip01 R Finger11", "Bip01 R Finger12", "Bip01 R Finger2", "Bip01 R Finger21",
            "Bip01 R Finger22", "Bip01 R Finger3", "Bip01 R Finger31", "Bip01 R Finger32", "Bip01 R Finger4",
            "Bip01 R Finger41", "Bip01 R Finger42", "Bip01 R ForeTwist", "Bip01 RUpArmTwistBone",
            "Bip01 RPauldron", "Weapon", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0",
            "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 R Toe0", "FNV Face Surface Frame" };

        std::ostream& output = *state.mOutput;
        output << std::setprecision(9)
               << "{\"schema\":\"nikami-openmw-transform-oracle/v1\",\"event\":\"actor-frame\","
                  "\"sample\":"
               << state.mSamples << ",\"update\":" << state.mUpdates << ",\"ref\":";
        writeFalloutTransformOracleJsonString(output, refId);
        output << ",\"activeGroups\":";
        writeFalloutTransformOracleJsonString(output, activeGroups);
        output << ",\"accumulationTranslation\":";
        if (accumulationTranslation)
            writeFalloutTransformOracleVec3(output, *accumulationTranslation);
        else
            output << "null";
        output << ",\"nodes\":[";
        for (std::size_t i = 0; i < nodeNames.size(); ++i)
        {
            if (i != 0)
                output << ',';
            const std::string lowerName = Misc::StringUtils::lowerCase(nodeNames[i]);
            osg::MatrixTransform* node = findFalloutTarget(targets, lowerName);
            output << "{\"name\":";
            writeFalloutTransformOracleJsonString(output, nodeNames[i]);
            output << ",\"present\":" << (node != nullptr ? "true" : "false");
            if (node != nullptr)
            {
                const osg::Matrix local = node->getMatrix();
                const osg::Matrix actor = getFalloutNodeWorldMatrix(node) * worldToActor;
                output << ",\"transform\":";
                writeFalloutTransformOracleTransform(output, local, actor);
            }
            output << '}';
        }
        output << "]}\n";
        state.mOutput->flush();
    }

    float auditFalloutRuntimeParts(osg::Group* objectRoot,
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const MWWorld::Ptr& ptr,
        bool renderLiveGeometryAudit = false)
    {
        if (objectRoot == nullptr)
            return 0.f;

        FalloutRuntimePartVisitor visitor;
        objectRoot->accept(visitor);
        if (visitor.mParts.empty())
            return 0.f;

        const osg::Vec3f head = getFalloutTargetWorldOrigin(targets, "bip01 head");
        const osg::Vec3f neck = getFalloutTargetWorldOrigin(targets, "bip01 neck1");
        const osg::Vec3f spine2 = getFalloutTargetWorldOrigin(targets, "bip01 spine2");
        const osg::Vec3f pelvis = getFalloutTargetWorldOrigin(targets, "bip01 pelvis");
        const osg::Vec3f leftHand = getFalloutTargetWorldOrigin(targets, "bip01 l hand");
        const osg::Vec3f rightHand = getFalloutTargetWorldOrigin(targets, "bip01 r hand");
        const osg::Vec3f weapon = getFalloutTargetWorldOrigin(targets, "weapon");
        const osg::Matrix worldToActor = osg::Matrix::inverse(getFalloutNodeWorldMatrix(objectRoot));

        float maxDistance = 0.f;
        std::string maxPart;
        std::string maxClass;
        unsigned int logged = 0;
        unsigned int suspect = 0;
        const bool partMatrixAudit = std::getenv("OPENMW_FNV_PART_MATRIX_AUDIT") != nullptr;
        const bool handGeometryAudit = partMatrixAudit || renderLiveGeometryAudit;

        for (osg::Node* part : visitor.mParts)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            part->accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            if (!box.valid())
                continue;

            const osg::Vec3f center = transformFalloutPoint(box.center(), getFalloutParentWorldMatrix(part));
            const std::string partClass = getFalloutRuntimePartClass(part->getName());
            osg::Vec3f anchor;
            float limit = 180.f;
            if (partClass == "head")
            {
                anchor = head;
                limit = 28.f;
            }
            else if (partClass == "headgear")
            {
                anchor = head;
                limit = 62.f;
            }
            else if (isFalloutFaceRuntimePartClass(partClass))
            {
                anchor = head;
                if (partClass == "faceEye")
                    limit = 22.f;
                else if (partClass == "faceMouth" || partClass == "faceTeeth" || partClass == "faceTongue")
                    limit = 28.f;
                else if (partClass == "faceBrow")
                    limit = 26.f;
                else if (partClass == "faceBeard")
                    limit = 42.f;
                else if (partClass == "faceHair")
                    limit = 56.f;
                else
                    limit = 40.f;
            }
            else if (partClass == "leftHand")
            {
                anchor = leftHand;
                limit = 30.f;
            }
            else if (partClass == "rightHand")
            {
                anchor = rightHand;
                limit = 30.f;
            }
            else if (partClass == "weapon")
            {
                anchor = weapon;
                limit = 28.f;
            }
            else if (partClass == "body")
            {
                anchor = (spine2 + pelvis) * 0.5f;
                limit = 80.f;
            }
            else
                anchor = neck;

            const float distance = (center - anchor).length();
            const osg::Vec3f centerLocal = transformFalloutPoint(center, worldToActor);
            const osg::Vec3f anchorLocal = transformFalloutPoint(anchor, worldToActor);
            const osg::Vec3f relLocal = centerLocal - anchorLocal;
            const bool finiteCenter = isFiniteFalloutAuditVec3(center);
            const bool finiteAnchor = isFiniteFalloutAuditVec3(anchor);
            const bool finiteDistance = std::isfinite(distance);
            if (finiteDistance && distance > maxDistance)
            {
                maxDistance = distance;
                maxPart = part->getName();
                maxClass = partClass;
            }
            const bool bad = !finiteCenter || !finiteAnchor || !finiteDistance || distance > limit;
            if (bad)
                ++suspect;
            osg::Matrix anchorWorld;
            if (partClass == "leftHand")
                anchorWorld = getFalloutNodeWorldMatrix(findFalloutTarget(targets, "bip01 l hand"));
            else if (partClass == "rightHand")
                anchorWorld = getFalloutNodeWorldMatrix(findFalloutTarget(targets, "bip01 r hand"));
            else if (partClass == "weapon")
                anchorWorld = getFalloutNodeWorldMatrix(findFalloutTarget(targets, "weapon"));
            else if (partClass == "body")
                anchorWorld = getFalloutNodeWorldMatrix(findFalloutTarget(targets, "bip01 spine2"));
            else
                anchorWorld = getFalloutNodeWorldMatrix(findFalloutTarget(targets, "bip01 head"));
            if (partMatrixAudit && partClass != "other")
                logFalloutPartMatrixAudit(part, partClass, anchorWorld, center, anchor, targets, ptr);
            if (handGeometryAudit && (partClass == "leftHand" || partClass == "rightHand"))
            {
                static unsigned int sHandPartRigAuditLines = 0;
                FalloutPartRigBoundsVisitor rigBoundsVisitor(getFalloutParentWorldMatrix(part));
                part->accept(rigBoundsVisitor);
                unsigned int rigIndex = 0;
                for (const FalloutRigBoundsSample& sample : rigBoundsVisitor.mSamples)
                {
                    if (sHandPartRigAuditLines >= 48)
                        break;
                    const float renderParentDistance = sample.mRenderValid
                        ? (sample.mRenderCenterParentWorld - anchor).length()
                        : -1.f;
                    const float renderPathDistance = sample.mRenderValid
                        ? (sample.mRenderCenterPathWorld - anchor).length()
                        : -1.f;
                    const float sourceParentDistance = sample.mSourceValid
                        ? (sample.mSourceCenterParentWorld - anchor).length()
                        : -1.f;
                    const float sourcePathDistance = sample.mSourceValid
                        ? (sample.mSourceCenterPathWorld - anchor).length()
                        : -1.f;
                    const float liveParentDistance = sample.mLiveValid
                        ? (sample.mLiveCenterParentWorld - anchor).length()
                        : -1.f;
                    const float livePathDistance = sample.mLiveValid
                        ? (sample.mLiveCenterPathWorld - anchor).length()
                        : -1.f;
                    Log(Debug::Info)
                        << "FNV/ESM4 HAND GEOMETRY BOUNDS AUDIT " << ptr.getCellRef().getRefId()
                        << " part='" << part->getName() << "' class=" << partClass
                        << " sampleIndex=" << rigIndex++
                        << " kind=" << sample.mKind
                        << " drawable='" << sample.mName << "' rootBone=" << sample.mRootBone
                        << " boneCount=" << sample.mBoneCount
                        << " anchor=" << formatFalloutAuditVec3(anchor)
                        << " renderValid=" << sample.mRenderValid
                        << " renderCenterParentWorld=" << formatFalloutAuditVec3(sample.mRenderCenterParentWorld)
                        << " renderCenterPathWorld=" << formatFalloutAuditVec3(sample.mRenderCenterPathWorld)
                        << " renderExtent=" << formatFalloutAuditVec3(sample.mRenderExtent)
                        << " renderParentDistance=" << renderParentDistance
                        << " renderPathDistance=" << renderPathDistance
                        << " sourceValid=" << sample.mSourceValid
                        << " sourceCenterParentWorld=" << formatFalloutAuditVec3(sample.mSourceCenterParentWorld)
                        << " sourceCenterPathWorld=" << formatFalloutAuditVec3(sample.mSourceCenterPathWorld)
                        << " sourceExtent=" << formatFalloutAuditVec3(sample.mSourceExtent)
                        << " sourceParentDistance=" << sourceParentDistance
                        << " sourcePathDistance=" << sourcePathDistance
                        << " liveValid=" << sample.mLiveValid
                        << " liveCenterParentWorld=" << formatFalloutAuditVec3(sample.mLiveCenterParentWorld)
                        << " liveCenterPathWorld=" << formatFalloutAuditVec3(sample.mLiveCenterPathWorld)
                        << " liveExtent=" << formatFalloutAuditVec3(sample.mLiveExtent)
                        << " liveParentDistance=" << liveParentDistance
                        << " livePathDistance=" << livePathDistance;
                    ++sHandPartRigAuditLines;
                }
                if (rigBoundsVisitor.mSamples.empty() && sHandPartRigAuditLines < 48)
                {
                    Log(Debug::Warning) << "FNV/ESM4 HAND GEOMETRY BOUNDS AUDIT " << ptr.getCellRef().getRefId()
                                        << " part='" << part->getName() << "' class=" << partClass
                                        << " geometryCount=0";
                    ++sHandPartRigAuditLines;
                }
            }
            if (logged < 14 && (bad || partClass != "other"))
            {
                ++logged;
                std::string candidateDistances;
                if (isFalloutFaceRuntimePartClass(partClass) || partClass == "head" || partClass == "headgear")
                {
                    candidateDistances = describeFalloutClosestTarget(center, targets,
                        { "bip01 head", "bip01 head_nub", "bip01 neck1", "bip01 neck", "bip01 spine2" });
                }
                else if (partClass == "leftHand")
                {
                    candidateDistances = describeFalloutClosestTarget(center, targets,
                        { "bip01 l forearm", "bip01 l foretwist", "bip01 l hand", "bip01 l finger0",
                            "bip01 l finger1", "bip01 l finger2", "bip01 l finger3", "bip01 l finger4" });
                }
                else if (partClass == "rightHand")
                {
                    candidateDistances = describeFalloutClosestTarget(center, targets,
                        { "bip01 r forearm", "bip01 r foretwist", "bip01 r hand", "bip01 r finger0",
                            "bip01 r finger1", "bip01 r finger2", "bip01 r finger3", "bip01 r finger4" });
                }
                else if (partClass == "weapon")
                {
                    candidateDistances = describeFalloutClosestTarget(center, targets,
                        { "weapon", "bip01 r hand", "bip01 r forearm", "bip01 r finger0", "bip01 r finger1",
                            "bip01 r finger2", "bip01 r finger3", "bip01 r finger4" });
                }
                Log(bad ? Debug::Warning : Debug::Info)
                    << "FNV/ESM4 diag: runtime part audit " << ptr.getCellRef().getRefId()
                    << " part='" << part->getName() << "' class=" << partClass
                    << " center=" << formatFalloutAuditVec3(center)
                    << " anchor=" << formatFalloutAuditVec3(anchor)
                    << " centerLocal=" << formatFalloutAuditVec3(centerLocal)
                    << " anchorLocal=" << formatFalloutAuditVec3(anchorLocal)
                    << " relLocal=" << formatFalloutAuditVec3(relLocal)
                    << " distance=" << distance << " limit=" << limit
                    << " finiteCenter=" << finiteCenter
                    << " finiteAnchor=" << finiteAnchor
                    << " finiteDistance=" << finiteDistance
                    << (candidateDistances.empty() ? "" : " candidateTargets=") << candidateDistances
                    << " verdict=" << (bad ? "SUSPECT" : "OK");
            }
        }

        Log(suspect > 0 ? Debug::Warning : Debug::Info)
            << "FNV/ESM4 diag: runtime part audit summary " << ptr.getCellRef().getRefId()
            << " parts=" << visitor.mParts.size() << " suspect=" << suspect
            << " maxDistance=" << maxDistance << " maxClass=" << maxClass
            << " maxPart='" << maxPart << "'";
        if (handGeometryAudit)
        {
            static unsigned int sActorHandRigAuditLines = 0;
            FalloutActorHandRigBoundsVisitor handRigVisitor(getFalloutNodeWorldMatrix(objectRoot));
            objectRoot->accept(handRigVisitor);
            unsigned int loggedHandRigs = 0;
            for (std::size_t i = 0; i < handRigVisitor.mSamples.size() && loggedHandRigs < 18
                 && sActorHandRigAuditLines < 96; ++i)
            {
                const FalloutRigBoundsSample& sample = handRigVisitor.mSamples[i];
                const std::string lowerName = Misc::StringUtils::lowerCase(sample.mName);
                const std::string lowerRoot = Misc::StringUtils::lowerCase(sample.mRootBone);
                const bool left = lowerName.find("left") != std::string::npos
                    || lowerName.find(" l ") != std::string::npos || lowerRoot.find(" l ") != std::string::npos;
                const bool right = lowerName.find("right") != std::string::npos
                    || lowerName.find(" r ") != std::string::npos || lowerRoot.find(" r ") != std::string::npos;
                const osg::Vec3f anchor = left && !right ? leftHand : rightHand;
                const float renderDistance = sample.mRenderValid ? (sample.mRenderCenterPathWorld - anchor).length() : -1.f;
                const float sourceDistance = sample.mSourceValid ? (sample.mSourceCenterPathWorld - anchor).length() : -1.f;
                const float liveDistance = sample.mLiveValid ? (sample.mLiveCenterPathWorld - anchor).length() : -1.f;
                Log(Debug::Info)
                    << "FNV/ESM4 ACTOR HAND GEOMETRY AUDIT " << ptr.getCellRef().getRefId()
                    << " sampleIndex=" << i
                    << " kind=" << sample.mKind
                    << " drawable='" << sample.mName << "' rootBone=" << sample.mRootBone
                    << " side=" << (left && !right ? "left" : right ? "right" : "unknown")
                    << " fnvPartAncestor='" << handRigVisitor.mPartAncestors[i] << "'"
                    << " boneCount=" << sample.mBoneCount
                    << " anchor=" << formatFalloutAuditVec3(anchor)
                    << " renderValid=" << sample.mRenderValid
                    << " renderCenterWorld=" << formatFalloutAuditVec3(sample.mRenderCenterPathWorld)
                    << " renderExtent=" << formatFalloutAuditVec3(sample.mRenderExtent)
                    << " renderDistance=" << renderDistance
                    << " sourceValid=" << sample.mSourceValid
                    << " sourceCenterWorld=" << formatFalloutAuditVec3(sample.mSourceCenterPathWorld)
                    << " sourceExtent=" << formatFalloutAuditVec3(sample.mSourceExtent)
                    << " sourceDistance=" << sourceDistance
                    << " liveValid=" << sample.mLiveValid
                    << " liveCenterWorld=" << formatFalloutAuditVec3(sample.mLiveCenterPathWorld)
                    << " liveExtent=" << formatFalloutAuditVec3(sample.mLiveExtent)
                    << " liveDistance=" << liveDistance
                    << " path='" << handRigVisitor.mPaths[i] << "'";
                ++loggedHandRigs;
                ++sActorHandRigAuditLines;
            }
            if (handRigVisitor.mSamples.empty() && sActorHandRigAuditLines < 96)
            {
                Log(Debug::Warning) << "FNV/ESM4 ACTOR HAND GEOMETRY AUDIT " << ptr.getCellRef().getRefId()
                                    << " geometryCount=0";
                ++sActorHandRigAuditLines;
            }
        }
        return maxDistance;
    }

    float calcAnimVelocity(const SceneUtil::TextKeyMap& keys, SceneUtil::KeyframeController* nonaccumctrl,
        const osg::Vec3f& accum, std::string_view groupname)
    {
        float starttime = std::numeric_limits<float>::max();
        float stoptime = 0.0f;

        // Pick the last Loop Stop key and the last Loop Start key.
        // This is required because of broken text keys in AshVampire.nif.
        // It has *two* WalkForward: Loop Stop keys at different times, the first one is used for stopping playback
        // but the animation velocity calculation uses the second one.
        // As result the animation velocity calculation is not correct, and this incorrect velocity must be replicated,
        // because otherwise the Creature's Speed (dagoth uthol) would not be sufficient to move fast enough.
        auto keyiter = keys.rbegin();
        while (keyiter != keys.rend())
        {
            if (equalsParts(keyiter->second, groupname, ": start")
                || equalsParts(keyiter->second, groupname, ": loop start"))
            {
                starttime = keyiter->first;
                break;
            }
            ++keyiter;
        }
        keyiter = keys.rbegin();
        while (keyiter != keys.rend())
        {
            if (equalsParts(keyiter->second, groupname, ": stop"))
                stoptime = keyiter->first;
            else if (equalsParts(keyiter->second, groupname, ": loop stop"))
            {
                stoptime = keyiter->first;
                break;
            }
            ++keyiter;
        }

        if (stoptime > starttime)
        {
            osg::Vec3f startpos = osg::componentMultiply(nonaccumctrl->getTranslation(starttime), accum);
            osg::Vec3f endpos = osg::componentMultiply(nonaccumctrl->getTranslation(stoptime), accum);

            return (startpos - endpos).length() / (stoptime - starttime);
        }

        return 0.0f;
    }

    class GetExtendedBonesVisitor : public osg::NodeVisitor
    {
    public:
        GetExtendedBonesVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (SceneUtil::hasUserDescription(&node, "CustomBone"))
            {
                mFoundBones.emplace_back(&node, node.getParent(0));
                return;
            }

            traverse(node);
        }

        std::vector<std::pair<osg::Node*, osg::Group*>> mFoundBones;
    };

    class RemoveFinishedCallbackVisitor : public SceneUtil::RemoveVisitor
    {
    public:
        bool mHasMagicEffects;

        RemoveFinishedCallbackVisitor()
            : RemoveVisitor()
            , mHasMagicEffects(false)
        {
        }

        void apply(osg::Node& node) override { traverse(node); }

        void apply(osg::Group& group) override
        {
            traverse(group);

            osg::Callback* callback = group.getUpdateCallback();
            if (callback)
            {
                // We should remove empty transformation nodes and finished callbacks here
                MWRender::UpdateVfxCallback* vfxCallback = dynamic_cast<MWRender::UpdateVfxCallback*>(callback);
                if (vfxCallback)
                {
                    if (vfxCallback->mFinished)
                        mToRemove.emplace_back(group.asNode(), group.getParent(0));
                    else
                        mHasMagicEffects = true;
                }
            }
        }

        void apply(osg::MatrixTransform& node) override { traverse(node); }

        void apply(osg::Geometry&) override {}
    };

    class RemoveCallbackVisitor : public SceneUtil::RemoveVisitor
    {
    public:
        bool mHasMagicEffects;

        RemoveCallbackVisitor()
            : RemoveVisitor()
            , mHasMagicEffects(false)
        {
        }

        RemoveCallbackVisitor(std::string_view effectId)
            : RemoveVisitor()
            , mHasMagicEffects(false)
            , mEffectId(effectId)
        {
        }

        void apply(osg::Node& node) override { traverse(node); }

        void apply(osg::Group& group) override
        {
            traverse(group);

            osg::Callback* callback = group.getUpdateCallback();
            if (callback)
            {
                MWRender::UpdateVfxCallback* vfxCallback = dynamic_cast<MWRender::UpdateVfxCallback*>(callback);
                if (vfxCallback)
                {
                    bool toRemove = mEffectId == "" || vfxCallback->mParams.mEffectId == mEffectId;
                    if (toRemove)
                        mToRemove.emplace_back(group.asNode(), group.getParent(0));
                    else
                        mHasMagicEffects = true;
                }
            }
        }

        void apply(osg::MatrixTransform& node) override { traverse(node); }

        void apply(osg::Geometry&) override {}

    private:
        std::string_view mEffectId;
    };

    class FindVfxCallbacksVisitor : public osg::NodeVisitor
    {
    public:
        std::vector<MWRender::UpdateVfxCallback*> mCallbacks;

        FindVfxCallbacksVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        FindVfxCallbacksVisitor(std::string_view effectId)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mEffectId(effectId)
        {
        }

        void apply(osg::Node& node) override { traverse(node); }

        void apply(osg::Group& group) override
        {
            osg::Callback* callback = group.getUpdateCallback();
            if (callback)
            {
                MWRender::UpdateVfxCallback* vfxCallback = dynamic_cast<MWRender::UpdateVfxCallback*>(callback);
                if (vfxCallback)
                {
                    if (mEffectId == "" || vfxCallback->mParams.mEffectId == mEffectId)
                    {
                        mCallbacks.push_back(vfxCallback);
                    }
                }
            }
            traverse(group);
        }

        void apply(osg::MatrixTransform& node) override { traverse(node); }

        void apply(osg::Geometry&) override {}

    private:
        std::string_view mEffectId;
    };

    void assignBoneBlendCallbackRecursive(MWRender::BoneAnimBlendController* controller, osg::Node* parent, bool isRoot)
    {
        // Attempt to cast node to an osgAnimation::Bone
        if (!isRoot && dynamic_cast<osgAnimation::Bone*>(parent))
        {
            // Wrapping in a custom callback object allows for nested callback chaining, otherwise it has link to self
            // issues we need to share the base BoneAnimBlendController as that contains blending information and is
            // guaranteed to update before
            osgAnimation::Bone* bone = static_cast<osgAnimation::Bone*>(parent);
            osg::ref_ptr<osg::Callback> cb = new MWRender::BoneAnimBlendControllerWrapper(controller, bone);

            // Ensure there is no other AnimBlendController - this can happen when using
            // multiple animations with different roots, such as NPC animation
            osg::Callback* updateCb = bone->getUpdateCallback();
            while (updateCb)
            {
                if (dynamic_cast<MWRender::BoneAnimBlendController*>(updateCb))
                {
                    osg::ref_ptr<osg::Callback> nextCb = updateCb->getNestedCallback();
                    bone->removeUpdateCallback(updateCb);
                    updateCb = nextCb;
                }
                else
                {
                    updateCb = updateCb->getNestedCallback();
                }
            }

            // Find UpdateBone callback and bind to just after that (order is important)
            // NOTE: if it doesn't have an UpdateBone callback, we shouldn't be doing blending!
            updateCb = bone->getUpdateCallback();
            while (updateCb)
            {
                if (dynamic_cast<osgAnimation::UpdateBone*>(updateCb))
                {
                    // Override the immediate callback after the UpdateBone
                    osg::ref_ptr<osg::Callback> lastCb = updateCb->getNestedCallback();
                    updateCb->setNestedCallback(cb);
                    if (lastCb)
                        cb->setNestedCallback(lastCb);
                    break;
                }

                updateCb = updateCb->getNestedCallback();
            }
        }

        // Traverse child bones if this is a group
        osg::Group* group = parent->asGroup();
        if (group)
            for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                assignBoneBlendCallbackRecursive(controller, group->getChild(i), false);
    }
}

namespace MWRender
{
    class TransparencyUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        TransparencyUpdater(const float alpha)
            : mAlpha(alpha)
        {
        }

        void setAlpha(const float alpha) { mAlpha = alpha; }

    protected:
        void setDefaults(osg::StateSet* stateset) override
        {
            osg::BlendFunc* blendfunc(new osg::BlendFunc);
            stateset->setAttributeAndModes(blendfunc, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

            stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
            stateset->setRenderBinMode(osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);

            // FIXME: overriding diffuse/ambient/emissive colors
            osg::Material* material = new osg::Material;
            material->setColorMode(osg::Material::OFF);
            material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, mAlpha));
            material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
            stateset->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateset->addUniform(
                new osg::Uniform("colorMode", 0), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor* /*nv*/) override
        {
            osg::Material* material
                = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            material->setAlpha(osg::Material::FRONT_AND_BACK, mAlpha);
        }

    private:
        float mAlpha;
    };

    void UpdateVfxCallback::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        traverse(node, nv);

        if (mFinished)
            return;

        double newTime = nv->getFrameStamp()->getSimulationTime();
        if (mStartingTime == 0)
        {
            mStartingTime = newTime;
            return;
        }

        double duration = newTime - mStartingTime;
        mStartingTime = newTime;

        mParams.mAnimTime->addTime(duration);
        if (mParams.mAnimTime->getTime() >= mParams.mMaxControllerLength)
        {
            if (mParams.mLoop)
            {
                // Start from the beginning again; carry over the remainder
                // Not sure if this is actually needed, the controller function might already handle loops
                float remainder = mParams.mAnimTime->getTime() - mParams.mMaxControllerLength;
                mParams.mAnimTime->resetTime(remainder);
            }
            else
            {
                // Hide effect immediately
                node->setNodeMask(0);
                mFinished = true;
            }
        }
    }

    class ResetAccumRootCallback : public SceneUtil::NodeCallback<ResetAccumRootCallback, osg::MatrixTransform*>
    {
    public:
        void operator()(osg::MatrixTransform* transform, osg::NodeVisitor* nv)
        {
            osg::Matrix mat = transform->getMatrix();
            osg::Vec3f position = mat.getTrans();
            position = mResetAllTranslation ? osg::Vec3f() : osg::componentMultiply(mResetAxes, position);
            mat.setTrans(position);
            transform->setMatrix(mat);

            traverse(transform, nv);
        }

        void setAccumulate(const osg::Vec3f& accumulate)
        {
            // anything that accumulates (1.f) should be reset in the callback to (0.f)
            mResetAxes.x() = accumulate.x() != 0.f ? 0.f : 1.f;
            mResetAxes.y() = accumulate.y() != 0.f ? 0.f : 1.f;
            mResetAxes.z() = accumulate.z() != 0.f ? 0.f : 1.f;
        }

        void setResetAllTranslation(bool resetAll)
        {
            mResetAllTranslation = resetAll;
        }

    private:
        osg::Vec3f mResetAxes;
        bool mResetAllTranslation = false;
    };

    Animation::Animation(
        const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem)
        : mInsert(std::move(parentNode))
        , mSkeleton(nullptr)
        , mNodeMapCreated(false)
        , mPtr(ptr)
        , mResourceSystem(resourceSystem)
        , mAccumulate(1.f, 1.f, 0.f)
        , mTextKeyListener(nullptr)
        , mHeadYawRadians(0.f)
        , mHeadPitchRadians(0.f)
        , mUpperBodyYawRadians(0.f)
        , mLegsYawRadians(0.f)
        , mBodyPitchRadians(0.f)
        , mHasMagicEffects(false)
        , mAlpha(1.f)
        , mPlayScriptedOnly(false)
        , mRequiresBoneMap(false)
        , mProofPreviewAnimation(false)
        , mProofPreviewGameplayAudit(false)
        , mBethesdaBoneLodLevel(-1)
    {
        for (size_t i = 0; i < sNumBlendMasks; i++)
            mAnimationTimePtr[i] = std::make_shared<AnimationTime>();

        mLightListCallback = new SceneUtil::LightListCallback;
    }

    Animation::~Animation()
    {
        removeFromSceneImpl();
    }

    void Animation::setProofPreviewAnimation(bool enabled)
    {
        mProofPreviewAnimation = enabled;
    }

    void Animation::setProofPreviewGameplayAudit(bool enabled)
    {
        mProofPreviewGameplayAudit = enabled;
    }

    void Animation::setActive(int active)
    {
        if (mSkeleton)
            mSkeleton->setActive(static_cast<SceneUtil::Skeleton::ActiveType>(active));
    }

    void Animation::updatePtr(const MWWorld::Ptr& ptr)
    {
        mPtr = ptr;
    }

    void Animation::setAccumulation(const osg::Vec3f& accum)
    {
        mAccumulate = accum;

        if (mResetAccumRootCallback)
            mResetAccumRootCallback->setAccumulate(mAccumulate);
    }

    // controllerName is used for Collada animated deforming models
    size_t Animation::detectBlendMask(const osg::Node* node, const std::string& controllerName) const
    {
        static const std::string_view sBlendMaskRoots[sNumBlendMasks] = {
            "", /* Lower body / character root */
            "Bip01 Spine", /* Torso */
            "Bip01 L Clavicle", /* Left arm */
            "Bip01 R Clavicle", /* Right arm */
            "Bip01 Neck", /* Head */
        };

        while (node != mObjectRoot)
        {
            const std::string& name = node->getName();
            for (size_t i = 1; i < sNumBlendMasks; i++)
            {
                if (name == sBlendMaskRoots[i] || controllerName == sBlendMaskRoots[i])
                    return i;
            }

            assert(node->getNumParents() > 0);

            node = node->getParent(0);
        }

        return 0;
    }

    const SceneUtil::TextKeyMap& Animation::AnimSource::getTextKeys() const
    {
        return mKeyframes->mTextKeys;
    }

    void Animation::loadAdditionalAnimations(VFS::Path::NormalizedView model, const std::string& baseModel)
    {
        constexpr VFS::Path::NormalizedView meshes("meshes/");
        if (!model.value().starts_with(meshes.value()))
            return;

        std::string path(model.value());

        constexpr VFS::Path::NormalizedView animations("animations/");
        path.replace(0, meshes.value().size(), animations.value());

        const std::string::size_type extensionStart = path.find_last_of(VFS::Path::extensionSeparator);
        if (extensionStart == std::string::npos)
            return;

        path.replace(extensionStart, path.size() - extensionStart, "/");

        for (const VFS::Path::Normalized& name : mResourceSystem->getVFS()->getRecursiveDirectoryIterator(path))
        {
            if (Misc::getFileExtension(name) == "kf")
            {
                addSingleAnimSource(name, baseModel);
            }
        }
    }

    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)
    {
        VFS::Path::Normalized kfname(model);

        if (Misc::getFileExtension(kfname) == "nif")
            kfname.changeExtension("kf");

        addSingleAnimSource(kfname, baseModel);

        if (Settings::game().mUseAdditionalAnimSources)
            loadAdditionalAnimations(kfname, baseModel);
    }

    std::vector<std::string> getFonvBoneAliases(const std::string& name)
    {
        if (name == "bip01 l finger0")
            return { "bip01 l thumb1" };
        if (name == "bip01 l finger01")
            return { "bip01 l thumb11" };
        if (name == "bip01 l finger02")
            return { "bip01 l thumb12" };
        if (name == "bip01 r finger0")
            return { "bip01 r thumb1" };
        if (name == "bip01 r finger01")
            return { "bip01 r thumb11" };
        if (name == "bip01 r finger02")
            return { "bip01 r thumb12" };
        if (name == "bip01 l forearmtwist")
            return { "bip01 l foretwist" };
        if (name == "bip01 r forearmtwist")
            return { "bip01 r foretwist" };
        if (name == "bip01 l upperarmtwist")
            return { "bip01 luparmtwist", "bip01 l uparmtwist" };
        if (name == "bip01 r upperarmtwist")
            return { "bip01 ruparmtwist", "bip01 r uparmtwist" };
        if (name == "screen01" || name == "screen01root")
            return { "bip01 screen01", "screen01root" };
        if (name == "screenstatic" || name == "screenstaticroot")
            return { "bip01 screenstatic", "screenstaticroot" };
        if (name == "voicebox_talk" || name == "##voicebox_talk")
            return { "bip01 voicebox1", "voicebox_root" };

        return {};
    }

    Animation::NodeMap::const_iterator findFonvAnimationBone(
        const Animation::NodeMap& nodeMap, const std::string& name, std::string& resolvedName)
    {
        for (const std::string& alias : getFonvBoneAliases(name))
        {
            // The alias table describes an explicit skeleton-dialect conversion. Keep this lookup exact so a
            // broader numeric/semantic match cannot redirect an authored thumb segment onto an unrelated finger.
            Animation::NodeMap::const_iterator found = nodeMap.find(alias);
            if (found != nodeMap.end())
            {
                resolvedName = found->first;
                return found;
            }
        }

        return nodeMap.end();
    }

    bool isFiniteVec3(const osg::Vec3f& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
    }

    bool isFiniteQuat(const osg::Quat& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z())
            && std::isfinite(value.w());
    }

    bool normalizeFiniteQuat(osg::Quat& value)
    {
        if (!isFiniteQuat(value))
            return false;

        const double length2 = value.x() * value.x() + value.y() * value.y() + value.z() * value.z()
            + value.w() * value.w();
        if (!std::isfinite(length2) || length2 < 0.000001 || length2 > 1000000.0)
            return false;

        const double invLength = 1.0 / std::sqrt(length2);
        value.set(value.x() * invLength, value.y() * invLength, value.z() * invLength, value.w() * invLength);
        return true;
    }

    bool isSaneFalloutHelperMatrix(const osg::Matrixf& matrix)
    {
        const float* values = matrix.ptr();
        for (int i = 0; i < 16; ++i)
        {
            if (!std::isfinite(values[i]) || std::abs(values[i]) > 100000.f)
                return false;
        }
        return true;
    }

    osg::Quat getFalloutBindRotation(osg::MatrixTransform* transform)
    {
        static std::unordered_map<const osg::MatrixTransform*, osg::Quat> sBindRotations;
        if (transform == nullptr)
            return osg::Quat();

        auto [it, inserted] = sBindRotations.emplace(transform, transform->getMatrix().getRotate());
        return it->second;
    }

    const std::string& getFalloutRotationMode()
    {
        static const std::string sMode = [] {
            if (const char* env = std::getenv("OPENMW_FNV_ROTATION_MODE"))
                return std::string(env);
            return std::string("bindCoreBindLowerBindUpper");
        }();
        return sMode;
    }

    osg::Quat getFalloutRootUpCorrectionRotation()
    {
        const char* env = std::getenv("OPENMW_FNV_ROOT_UP_CORRECTION");
        if (env == nullptr || env[0] == '\0')
            return osg::Quat();

        const std::string_view mode(env);
        if (mode == "y-90")
            return osg::Quat(-osg::PI_2, osg::Vec3f(0.f, 1.f, 0.f));
        if (mode == "y90")
            return osg::Quat(osg::PI_2, osg::Vec3f(0.f, 1.f, 0.f));
        if (mode == "x-90")
            return osg::Quat(-osg::PI_2, osg::Vec3f(1.f, 0.f, 0.f));
        if (mode == "x90")
            return osg::Quat(osg::PI_2, osg::Vec3f(1.f, 0.f, 0.f));
        if (mode == "z-90")
            return osg::Quat(-osg::PI_2, osg::Vec3f(0.f, 0.f, 1.f));
        if (mode == "z90")
            return osg::Quat(osg::PI_2, osg::Vec3f(0.f, 0.f, 1.f));
        return osg::Quat();
    }

    osg::MatrixTransform* findFalloutRootBip01(osg::Node* root)
    {
        if (root == nullptr)
            return nullptr;

        class Bip01Visitor : public osg::NodeVisitor
        {
        public:
            Bip01Visitor()
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                if (mBip01 == nullptr)
                {
                    std::string name = Misc::StringUtils::lowerCase(node.getName());
                    if (name == "bip01")
                        mBip01 = dynamic_cast<osg::MatrixTransform*>(&node);
                }

                if (mBip01 == nullptr)
                    traverse(node);
            }

            osg::MatrixTransform* mBip01 = nullptr;
        };

        Bip01Visitor visitor;
        root->accept(visitor);
        return visitor.mBip01;
    }

    bool hasFalloutRootUpCorrection()
    {
        const char* env = std::getenv("OPENMW_FNV_BONE_ROOT_UP_CORRECTION");
        return env != nullptr && env[0] != '\0';
    }

    void applyFalloutRootUpCorrection(osg::MatrixTransform* transform, const osg::Quat& correction)
    {
        if (transform == nullptr)
            return;

        osg::Matrixf before = transform->getMatrix();
        osg::Quat rotation = correction * before.getRotate();
        normalizeFiniteQuat(rotation);
        if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(transform))
            nifTransform->setRotation(rotation);
        else
            transform->setMatrix(osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(before.getTrans()));
        transform->dirtyBound();
    }

    osg::ref_ptr<osg::Group> wrapFalloutActorRootIfRequested(
        osg::ref_ptr<osg::Group> objectRoot, const MWWorld::Ptr& ptr)
    {
        if (!isFalloutActor(ptr))
            return objectRoot;

        const char* env = std::getenv("OPENMW_FNV_ROOT_UP_CORRECTION");
        if (env == nullptr || env[0] == '\0')
            return objectRoot;

        osg::Quat correction = getFalloutRootUpCorrectionRotation();
        if (!normalizeFiniteQuat(correction))
            return objectRoot;

        osg::ref_ptr<osg::MatrixTransform> wrapper = new osg::MatrixTransform;
        wrapper->setName("FNV Actor Root Up Correction");
        osg::Matrixf wrapperMatrix = osg::Matrixf::rotate(correction);
        osg::Vec3f bip01Translation;
        bool haveBip01Translation = false;
        if (osg::MatrixTransform* bip01 = findFalloutRootBip01(objectRoot.get()))
        {
            bip01Translation = bip01->getMatrix().getTrans();
            haveBip01Translation = true;
        }

        const char* pivotEnv = std::getenv("OPENMW_FNV_ROOT_UP_CORRECTION_PIVOT");
        const bool pivotBip01 = pivotEnv != nullptr && std::string_view(pivotEnv) == "bip01" && haveBip01Translation;
        if (pivotBip01)
        {
            wrapperMatrix = osg::Matrixf::translate(-bip01Translation) * osg::Matrixf::rotate(correction)
                * osg::Matrixf::translate(bip01Translation);
        }

        const char* translateEnv = std::getenv("OPENMW_FNV_ROOT_UP_CORRECTION_TRANSLATE");
        const bool translateBip01XY
            = translateEnv != nullptr && std::string_view(translateEnv) == "bip01xy" && haveBip01Translation;
        if (translateBip01XY)
        {
            const osg::Vec3d transformedBip01 = osg::Vec3d(bip01Translation) * wrapperMatrix;
            wrapperMatrix = wrapperMatrix
                * osg::Matrixf::translate(osg::Vec3f(-transformedBip01.x(), -transformedBip01.y(), 0.f));
        }

        wrapper->setMatrix(wrapperMatrix);
        wrapper->addChild(objectRoot);

        Log(Debug::Verbose) << "FNV/ESM4 diag: wrapped actor root for " << ptr.getCellRef().getRefId()
                         << " mode=" << env
                         << " child='" << objectRoot->getName() << "'"
                         << " pivotBip01=" << pivotBip01
                         << " translateBip01XY=" << translateBip01XY
                         << " bip01Local=(" << bip01Translation.x() << "," << bip01Translation.y() << ","
                         << bip01Translation.z() << ")";
        return wrapper;
    }

    osg::ref_ptr<osg::Group> correctFalloutCreatureForwardAxis(
        osg::ref_ptr<osg::Group> objectRoot, const MWWorld::Ptr& ptr)
    {
        if (objectRoot == nullptr || ptr.getType() != ESM4::Creature::sRecordId)
            return objectRoot;

        const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>();
        if (ref == nullptr || ref->mBase == nullptr || !ref->mBase->mIsFONV)
            return objectRoot;

        std::string model = Misc::StringUtils::lowerCase(std::string(ptr.getClass().getModel(ptr)));
        std::replace(model.begin(), model.end(), '\\', '/');
        if (model.find("/nvsecuritron/") == std::string::npos)
            return objectRoot;

        // The FNV securitron skeleton's authored screen/front points along -X while OpenMW actor movement and
        // facing use +Y. The gameplay controller therefore turned correctly while Victor's rendered body looked
        // a quarter-turn away from its target. Keep actor/world yaw authoritative and correct only this known
        // authored visual assembly. Other creature families retain their native axes until measured independently.
        osg::ref_ptr<osg::MatrixTransform> wrapper = new osg::MatrixTransform;
        wrapper->setName("FNV Securitron Forward Axis");
        wrapper->setMatrix(osg::Matrixf::rotate(-osg::PI_2, osg::Vec3f(0.f, 0.f, 1.f)));
        wrapper->addChild(objectRoot);
        return wrapper;
    }


    float getFalloutIdleSeedSeconds(std::string_view groupname)
    {
        if (std::getenv("OPENMW_FNV_DISABLE_IDLE_SEED") != nullptr)
            return -1.f;

        const char* layerSpecificEnv = nullptr;
        if (groupname == "idle")
            layerSpecificEnv = std::getenv("OPENMW_FNV_OVERLAY_IDLE_SEED_SECONDS");
        else if (const char* forcedGroup = std::getenv("OPENMW_FNV_FORCE_IDLE_GROUP"))
        {
            if (groupname == forcedGroup)
                layerSpecificEnv = std::getenv("OPENMW_FNV_BASE_IDLE_SEED_SECONDS");
        }
        if (layerSpecificEnv != nullptr)
        {
            char* end = nullptr;
            const float value = std::strtof(layerSpecificEnv, &end);
            if (end != layerSpecificEnv && std::isfinite(value) && value >= 0.f)
                return value;
        }

        if (const char* env = std::getenv("OPENMW_FNV_IDLE_SEED_SECONDS"))
        {
            char* end = nullptr;
            const float value = std::strtof(env, &end);
            if (end != env && std::isfinite(value) && value >= 0.f)
                return value;
        }

        return 1.f;
    }

    bool shouldFreezeFalloutIdleAnimation()
    {
        return std::getenv("OPENMW_FNV_FREEZE_IDLE_ANIM") != nullptr;
    }

    bool isFalloutSeededIdleGroup(std::string_view groupname)
    {
        if (groupname == "idle")
            return true;

        if (const char* forcedGroup = std::getenv("OPENMW_FNV_FORCE_IDLE_GROUP"))
            return !forcedGroup[0] || groupname == forcedGroup ? groupname == forcedGroup : false;

        return false;
    }

    bool isFalloutSeededLocomotionGroup(std::string_view groupname)
    {
        return groupname.starts_with("walk") || groupname.starts_with("run") || groupname.starts_with("sneak")
            || groupname.starts_with("swim") || groupname.starts_with("turn");
    }

    float getFalloutLocomotionSeedSeconds(float duration)
    {
        if (!std::isfinite(duration) || duration <= 0.f)
            return 0.f;

        return std::clamp(duration * 0.06f, 0.033f, 0.08f);
    }

    osg::Quat falloutHalfTurn(char axis)
    {
        switch (axis)
        {
            case 'x':
                return osg::Quat(osg::PI, osg::Vec3f(1.f, 0.f, 0.f));
            case 'y':
                return osg::Quat(osg::PI, osg::Vec3f(0.f, 1.f, 0.f));
            case 'z':
                return osg::Quat(osg::PI, osg::Vec3f(0.f, 0.f, 1.f));
            default:
                return osg::Quat();
        }
    }

    osg::Quat falloutQuarterTurn(char axis, int sign)
    {
        const double angle = (sign < 0 ? -0.5 : 0.5) * osg::PI;
        switch (axis)
        {
            case 'x':
                return osg::Quat(angle, osg::Vec3f(1.f, 0.f, 0.f));
            case 'y':
                return osg::Quat(angle, osg::Vec3f(0.f, 1.f, 0.f));
            case 'z':
                return osg::Quat(angle, osg::Vec3f(0.f, 0.f, 1.f));
            default:
                return osg::Quat();
        }
    }

    osg::Quat swizzleFalloutKeyRotation(osg::Quat rotation)
    {
        const char* mode = std::getenv("OPENMW_FNV_QUAT_SWIZZLE");
        if (mode == nullptr || mode[0] == '\0')
            return rotation;

        const std::string_view value(mode);
        if (value.size() == 3)
        {
            bool used[3] = { false, false, false };
            double components[3] = {};
            const double source[3] = { rotation.x(), rotation.y(), rotation.z() };
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                const char axis = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
                const int index = axis == 'x' ? 0 : axis == 'y' ? 1 : axis == 'z' ? 2 : -1;
                if (index < 0 || used[index])
                    return rotation;
                used[index] = true;
                components[i] = source[index] * (std::isupper(static_cast<unsigned char>(value[i])) ? -1.0 : 1.0);
            }
            return osg::Quat(components[0], components[1], components[2], rotation.w());
        }

        if (value == "xzy")
            return osg::Quat(rotation.x(), rotation.z(), rotation.y(), rotation.w());
        if (value == "yxz")
            return osg::Quat(rotation.y(), rotation.x(), rotation.z(), rotation.w());
        if (value == "yzx")
            return osg::Quat(rotation.y(), rotation.z(), rotation.x(), rotation.w());
        if (value == "zxy")
            return osg::Quat(rotation.z(), rotation.x(), rotation.y(), rotation.w());
        if (value == "zyx")
            return osg::Quat(rotation.z(), rotation.y(), rotation.x(), rotation.w());
        if (value == "negx")
            return osg::Quat(-rotation.x(), rotation.y(), rotation.z(), rotation.w());
        if (value == "negy")
            return osg::Quat(rotation.x(), -rotation.y(), rotation.z(), rotation.w());
        if (value == "negz")
            return osg::Quat(rotation.x(), rotation.y(), -rotation.z(), rotation.w());
        if (value == "negxy")
            return osg::Quat(-rotation.x(), -rotation.y(), rotation.z(), rotation.w());
        if (value == "negxz")
            return osg::Quat(-rotation.x(), rotation.y(), -rotation.z(), rotation.w());
        if (value == "negyz")
            return osg::Quat(rotation.x(), -rotation.y(), -rotation.z(), rotation.w());
        return rotation;
    }

    bool isFalloutLowerBodyBone(const std::string& lowerBone)
    {
        return lowerBone.find("thigh") != std::string::npos || lowerBone.find("calf") != std::string::npos
            || lowerBone.find("foot") != std::string::npos || lowerBone.find("toe") != std::string::npos;
    }

    bool isFalloutCalfFootBone(const std::string& lowerBone)
    {
        return lowerBone.find("calf") != std::string::npos || lowerBone.find("foot") != std::string::npos
            || lowerBone.find("toe") != std::string::npos;
    }

    bool isFalloutRightCalfFootBone(const std::string& lowerBone)
    {
        return (lowerBone.find("r calf") != std::string::npos || lowerBone.find("r foot") != std::string::npos
                   || lowerBone.find("r toe") != std::string::npos)
            && isFalloutCalfFootBone(lowerBone);
    }

    bool isFalloutDynamicChairSitSource(const std::string& sourceName)
    {
        std::string lowered = Misc::StringUtils::lowerCase(sourceName);
        return lowered.find("idleanims/dynamicidle_chairsit.kf") != std::string::npos
            || lowered.find("idleanims\\dynamicidle_chairsit.kf") != std::string::npos;
    }

    bool isFalloutDynamicSitSource(const std::string& sourceName)
    {
        std::string lowered = Misc::StringUtils::lowerCase(sourceName);
        return lowered.find("idleanims/dynamicidle_sit.kf") != std::string::npos
            || lowered.find("idleanims\\dynamicidle_sit.kf") != std::string::npos;
    }

    bool isFalloutThighBone(const std::string& lowerBone)
    {
        return lowerBone.find("thigh") != std::string::npos;
    }

    bool isFalloutSpineBone(const std::string& lowerBone)
    {
        return lowerBone.find("spine") != std::string::npos || lowerBone.find("pelvis") != std::string::npos;
    }

    bool isFalloutCorePoseBone(const std::string& lowerBone)
    {
        return lowerBone == "bip01" || lowerBone == "bip01 nonaccum" || isFalloutSpineBone(lowerBone)
            || lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos;
    }

    bool isFalloutArmPoseBone(const std::string& lowerBone)
    {
        return lowerBone.find("upperarm") != std::string::npos || lowerBone.find("forearm") != std::string::npos
            || lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
            || lowerBone.find("thumb") != std::string::npos || lowerBone.find("foretwist") != std::string::npos
            || lowerBone.find("uparmtwist") != std::string::npos;
    }

    osg::Quat composeFalloutSplitKeyRotation(osg::Quat rotation, osg::Quat bindRotation, const std::string& lowerBone)
    {
        return rotation * falloutHalfTurn(isFalloutLowerBodyBone(lowerBone) ? 'z' : 'x') * bindRotation;
    }

    std::string_view getFalloutProcedureRotationMode()
    {
        if (const char* env = std::getenv("OPENMW_FNV_PROCEDURE_ROTATION_MODE"))
            return env;
        return "fnvProcedure";
    }

    osg::Quat composeFalloutRotationMode(
        std::string_view mode, osg::Quat rotation, osg::Quat bindRotation, const std::string& lowerBone)
    {
        if (mode == "bindCoreBindLowerRawUpper")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone))
                return bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerSplitUpper")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone))
                return bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation * falloutHalfTurn('x') * bindRotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerSplitUpperRawRightForearmHand")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone))
                return bindRotation;
            if (lowerBone.find("r forearm") != std::string::npos
                || lowerBone.find("r foretwist") != std::string::npos
                || lowerBone.find("r hand") != std::string::npos || lowerBone.find("r finger") != std::string::npos
                || lowerBone.find("r thumb") != std::string::npos)
                return rotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation * falloutHalfTurn('x') * bindRotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerBindUpper")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone) || isFalloutArmPoseBone(lowerBone))
                return bindRotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerBindArmsRawHands")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone)
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("forearm") != std::string::npos
                || lowerBone.find("foretwist") != std::string::npos
                || lowerBone.find("uparmtwist") != std::string::npos)
                return bindRotation;
            if (lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
                || lowerBone.find("thumb") != std::string::npos)
                return rotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerBindUpperRawForearmsHands")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone)
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("uparmtwist") != std::string::npos)
                return bindRotation;
            if (lowerBone.find("forearm") != std::string::npos || lowerBone.find("foretwist") != std::string::npos
                || lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
                || lowerBone.find("thumb") != std::string::npos)
                return rotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerRawUpperBindHands")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone)
                || lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
                || lowerBone.find("thumb") != std::string::npos)
                return bindRotation;
            if (lowerBone.find("upperarm") != std::string::npos || lowerBone.find("forearm") != std::string::npos
                || lowerBone.find("foretwist") != std::string::npos
                || lowerBone.find("uparmtwist") != std::string::npos)
                return rotation;
            return bindRotation;
        }
        if (mode == "bindCoreBindLowerRawClavicleBindArms")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone)
                || lowerBone.find("forearm") != std::string::npos || lowerBone.find("foretwist") != std::string::npos
                || lowerBone.find("hand") != std::string::npos || lowerBone.find("finger") != std::string::npos
                || lowerBone.find("thumb") != std::string::npos)
                return bindRotation;
            if (lowerBone.find("clavicle") != std::string::npos || lowerBone.find("upperarm") != std::string::npos
                || lowerBone.find("uparmtwist") != std::string::npos)
                return rotation;
            return bindRotation;
        }
        if (mode == "standingUpperBody")
        {
            if (lowerBone == "bip01" || lowerBone == "bip01 nonaccum" || isFalloutSpineBone(lowerBone)
                || isFalloutLowerBodyBone(lowerBone))
                return bindRotation;
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone))
                return composeFalloutSplitKeyRotation(rotation, bindRotation, lowerBone);
            return bindRotation;
        }
        if (mode == "bindThenKey")
            return bindRotation * rotation;
        if (mode == "inverseKeyThenBind")
            return rotation.inverse() * bindRotation;
        if (mode == "xKeyThenBind")
            return falloutHalfTurn('x') * rotation * bindRotation;
        if (mode == "yKeyThenBind")
            return falloutHalfTurn('y') * rotation * bindRotation;
        if (mode == "zKeyThenBind")
            return falloutHalfTurn('z') * rotation * bindRotation;
        if (mode == "keyXThenBind")
            return rotation * falloutHalfTurn('x') * bindRotation;
        if (mode == "keyYThenBind")
            return rotation * falloutHalfTurn('y') * bindRotation;
        if (mode == "keyZThenBind")
            return rotation * falloutHalfTurn('z') * bindRotation;
        if (mode == "splitXZThenBind")
            return falloutHalfTurn(isFalloutLowerBodyBone(lowerBone) ? 'z' : 'x') * rotation * bindRotation;
        if (mode == "splitKeyXZThenBind")
            return composeFalloutSplitKeyRotation(rotation, bindRotation, lowerBone);
        if (mode == "rawKey" || mode == "nativeKey")
            return rotation;
        if (mode == "rawCoreSplitLimbs")
        {
            if (isFalloutCorePoseBone(lowerBone))
                return rotation;
            return composeFalloutSplitKeyRotation(rotation, bindRotation, lowerBone);
        }
        if (mode == "splitCoreRawLimbs")
        {
            if (isFalloutArmPoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone))
                return rotation;
            return composeFalloutSplitKeyRotation(rotation, bindRotation, lowerBone);
        }
        if (mode == "rawMajorSplitHands")
        {
            if (isFalloutCorePoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone) || isFalloutArmPoseBone(lowerBone))
                return rotation;
            return composeFalloutSplitKeyRotation(rotation, bindRotation, lowerBone);
        }
        if (mode == "rawBodyXHead")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos)
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArms")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArmsXLegs")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone) || isFalloutLowerBodyBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArmsXThighsRawCalves")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone) || isFalloutThighBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArmsXThighsYCalves")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone) || isFalloutThighBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutCalfFootBone(lowerBone))
                return falloutHalfTurn('y') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArmsXThighsZCalves")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone) || isFalloutThighBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutCalfFootBone(lowerBone))
                return falloutHalfTurn('z') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadArmsRawThighsXCalves")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone) || isFalloutCalfFootBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawBodyXHeadUpperArms")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("uparmtwist") != std::string::npos)
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation * bindRotation;
            return rotation;
        }
        if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootX"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootY"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZ"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyX"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyY"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyZ"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZ90"
            || mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZNeg90"
            || mode == "rawLowerSpineKeyBindHeadArmsCalfFootZ90"
            || mode == "rawLowerSpineKeyBindHeadArmsCalfFootZNeg90")
        {
            if (mode == "rawLowerSpineKeyBindHeadArmsCalfFootZ90" && isFalloutCalfFootBone(lowerBone))
                return falloutQuarterTurn('z', 1) * rotation;
            if (mode == "rawLowerSpineKeyBindHeadArmsCalfFootZNeg90" && isFalloutCalfFootBone(lowerBone))
                return falloutQuarterTurn('z', -1) * rotation;
            if (isFalloutRightCalfFootBone(lowerBone))
            {
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootX")
                    return falloutHalfTurn('x') * rotation * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootY")
                    return falloutHalfTurn('y') * rotation * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZ")
                    return falloutHalfTurn('z') * rotation * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyX")
                    return rotation * falloutHalfTurn('x') * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyY")
                    return rotation * falloutHalfTurn('y') * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootKeyZ")
                    return rotation * falloutHalfTurn('z') * bindRotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZ90")
                    return falloutQuarterTurn('z', 1) * rotation;
                if (mode == "rawLowerSpineKeyBindHeadArmsRightCalfFootZNeg90")
                    return falloutQuarterTurn('z', -1) * rotation;
            }
            mode = "rawLowerSpineKeyBindHeadArms";
        }
        if (mode == "rawLowerSpineKeyBindHeadUpperArms" || mode == "rawLowerSpineKeyBindHeadArms"
            || mode == "rawLowerSpineKeyBindHeadAsymForearms")
        {
            if (isFalloutLowerBodyBone(lowerBone))
                return rotation;
            if (isFalloutSpineBone(lowerBone) || lowerBone == "bip01" || lowerBone == "bip01 nonaccum")
                return rotation * bindRotation;
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("uparmtwist") != std::string::npos
                || mode == "rawLowerSpineKeyBindHeadArms"
                || (mode == "rawLowerSpineKeyBindHeadAsymForearms"
                    && (lowerBone.find("l forearm") != std::string::npos
                        || lowerBone.find("l foretwist") != std::string::npos)))
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation * bindRotation;
            return rotation * bindRotation;
        }
        if (mode == "fnvProcedure" || mode == "rawBodyXHeadAsymForearms")
        {
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("uparmtwist") != std::string::npos
                || lowerBone.find("forearm") != std::string::npos || lowerBone.find("foretwist") != std::string::npos)
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return rotation * bindRotation;
            return rotation;
        }
        if (mode == "seatedMixed")
        {
            if (isFalloutLowerBodyBone(lowerBone))
                return falloutHalfTurn('y') * rotation * bindRotation;
            if (isFalloutSpineBone(lowerBone) || lowerBone == "bip01" || lowerBone == "bip01 nonaccum")
                return rotation * bindRotation;
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || isFalloutArmPoseBone(lowerBone))
                return falloutHalfTurn('x') * rotation * bindRotation;
            return rotation * bindRotation;
        }
        if (mode == "seatedProofSpineBind" || mode == "seatedProofSpineKeyBind" || mode == "seatedProofSpineX"
            || mode == "seatedProofSpineY" || mode == "seatedProofSpineZ" || mode == "seatedProofSpineZ90"
            || mode == "seatedProofSpineZNeg90" || mode == "seatedProofSpineKeyZ90"
            || mode == "seatedProofSpineKeyZNeg90")
        {
            if (isFalloutLowerBodyBone(lowerBone))
                return bindRotation * rotation;
            if (isFalloutSpineBone(lowerBone) || lowerBone == "bip01" || lowerBone == "bip01 nonaccum")
            {
                if (mode == "seatedProofSpineBind")
                    return bindRotation * rotation;
                if (mode == "seatedProofSpineKeyBind")
                    return rotation * bindRotation;
                if (mode == "seatedProofSpineX")
                    return falloutHalfTurn('x') * rotation * bindRotation;
                if (mode == "seatedProofSpineY")
                    return falloutHalfTurn('y') * rotation * bindRotation;
                if (mode == "seatedProofSpineZ90")
                    return falloutQuarterTurn('z', 1) * bindRotation * rotation;
                if (mode == "seatedProofSpineZNeg90")
                    return falloutQuarterTurn('z', -1) * bindRotation * rotation;
                if (mode == "seatedProofSpineKeyZ90")
                    return falloutQuarterTurn('z', 1) * rotation * bindRotation;
                if (mode == "seatedProofSpineKeyZNeg90")
                    return falloutQuarterTurn('z', -1) * rotation * bindRotation;
                return falloutHalfTurn('z') * rotation * bindRotation;
            }
            if (lowerBone.find("neck") != std::string::npos || lowerBone.find("head") != std::string::npos
                || lowerBone.find("upperarm") != std::string::npos || lowerBone.find("uparmtwist") != std::string::npos)
                return falloutHalfTurn('x') * rotation * bindRotation;
            if (isFalloutArmPoseBone(lowerBone))
                return bindRotation * rotation;
            return rotation * bindRotation;
        }
        return rotation * bindRotation;
    }

    osg::Quat composeFalloutBindRelativeRotation(
        osg::MatrixTransform* transform, osg::Quat rotation, const std::string& lowerBone, bool procedureIdle)
    {
        normalizeFiniteQuat(rotation);
        rotation = swizzleFalloutKeyRotation(rotation);
        normalizeFiniteQuat(rotation);
        osg::Quat bindRotation = getFalloutBindRotation(transform);
        normalizeFiniteQuat(bindRotation);
        if (procedureIdle)
        {
            const std::string_view procedureMode = getFalloutProcedureRotationMode();
            if (procedureMode != "legacy")
                return composeFalloutRotationMode(procedureMode, rotation, bindRotation, lowerBone);
            if (isFalloutLowerBodyBone(lowerBone))
                return rotation;
            if (isFalloutSpineBone(lowerBone))
                return bindRotation * rotation;
            return rotation * falloutHalfTurn('x') * bindRotation;
        }

        return composeFalloutRotationMode(getFalloutRotationMode(), rotation, bindRotation, lowerBone);
    }

    float quatAngleDeltaDegrees(osg::Quat left, osg::Quat right)
    {
        if (!normalizeFiniteQuat(left) || !normalizeFiniteQuat(right))
            return 0.f;

        const double dot = std::clamp(std::abs(left.x() * right.x() + left.y() * right.y() + left.z() * right.z()
                                         + left.w() * right.w()),
            0.0, 1.0);
        return static_cast<float>(2.0 * std::acos(dot) * 180.0 / osg::PI);
    }

    float falloutRotationFromBindDegrees(osg::MatrixTransform* transform)
    {
        if (transform == nullptr)
            return 0.f;

        osg::Quat current = transform->getMatrix().getRotate();
        return quatAngleDeltaDegrees(current, getFalloutBindRotation(transform));
    }

    bool isFalloutMatrixAuditBone(const std::string& lowerBone)
    {
        if (std::getenv("OPENMW_FNV_FULL_MATRIX_AUDIT") != nullptr)
            return true;

        return lowerBone == "bip01" || lowerBone == "bip01 pelvis" || lowerBone == "bip01 spine"
            || lowerBone == "bip01 spine1" || lowerBone == "bip01 spine2" || lowerBone == "bip01 neck"
            || lowerBone == "bip01 head" || lowerBone == "bip01 l upperarm" || lowerBone == "bip01 r upperarm"
            || lowerBone == "bip01 l forearm" || lowerBone == "bip01 r forearm" || lowerBone == "bip01 l hand"
            || lowerBone == "bip01 r hand" || lowerBone == "bip01 l thigh" || lowerBone == "bip01 r thigh"
            || lowerBone == "bip01 l calf" || lowerBone == "bip01 r calf" || lowerBone == "bip01 l foot"
            || lowerBone == "bip01 r foot" || lowerBone == "bip01 l toe0" || lowerBone == "bip01 r toe0";
    }

    float basisHandedness(const osg::Matrixf& matrix)
    {
        const osg::Vec3f x(matrix(0, 0), matrix(0, 1), matrix(0, 2));
        const osg::Vec3f y(matrix(1, 0), matrix(1, 1), matrix(1, 2));
        const osg::Vec3f z(matrix(2, 0), matrix(2, 1), matrix(2, 2));
        return (x ^ y) * z;
    }

    std::string formatQuat(const osg::Quat& quat)
    {
        std::ostringstream out;
        out << "(" << quat.x() << "," << quat.y() << "," << quat.z() << "," << quat.w() << ")";
        return out.str();
    }

    std::string formatVec3(const osg::Vec3f& value)
    {
        std::ostringstream out;
        out << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
        return out.str();
    }

    osg::Matrixf makeFalloutAuditLocalMatrix(
        osg::MatrixTransform* transform, const osg::Matrixf& before, const osg::Quat& rotation,
        const osg::Vec3f& translation)
    {
        if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(transform))
        {
            NifOsg::MatrixTransform probe(*nifTransform, osg::CopyOp::SHALLOW_COPY);
            probe.setRotation(rotation);
            probe.setTranslation(translation);
            return probe.getMatrix();
        }

        return osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(translation);
    }

    void logFalloutProcedureMatrixAudit(const MWWorld::Ptr& ptr, osg::MatrixTransform* transform,
        const std::string& bone, std::string_view group, const std::string& source, osg::Quat rawKey,
        osg::Quat bind, const osg::Matrixf& before, const osg::Matrixf& selected, float& maxNativeLocalDelta,
        std::string& maxNativeLocalDeltaBone, float& maxNativeWorldDelta, std::string& maxNativeWorldDeltaBone)
    {
        if (transform == nullptr)
            return;

        const std::string lowerBone = Misc::StringUtils::lowerCase(bone);
        osg::Quat swizzledKey = swizzleFalloutKeyRotation(rawKey);
        normalizeFiniteQuat(swizzledKey);
        normalizeFiniteQuat(rawKey);
        normalizeFiniteQuat(bind);

        const osg::Vec3f translation = selected.getTrans();
        const osg::Matrixf nativeLocal = makeFalloutAuditLocalMatrix(transform, before, rawKey, translation);
        const osg::Matrixf swizzledLocal = makeFalloutAuditLocalMatrix(transform, before, swizzledKey, translation);
        const osg::Matrixf bindThenKeyLocal = makeFalloutAuditLocalMatrix(transform, before, bind * swizzledKey, translation);
        const osg::Matrixf keyThenBindLocal = makeFalloutAuditLocalMatrix(transform, before, swizzledKey * bind, translation);
        const osg::Matrixf splitLocal
            = makeFalloutAuditLocalMatrix(transform, before, composeFalloutSplitKeyRotation(swizzledKey, bind, lowerBone), translation);
        const osg::Matrixf selectedLocal = selected;

        const osg::Matrixf parentWorld = osg::Matrixf(getFalloutParentWorldMatrix(transform));
        const osg::Matrixf nativeWorld = nativeLocal * parentWorld;
        const osg::Matrixf selectedWorld = selectedLocal * parentWorld;
        const float nativeLocalDelta = matrixDifference(selectedLocal, nativeLocal);
        const float nativeWorldDelta = matrixDifference(selectedWorld, nativeWorld);
        if (nativeLocalDelta > maxNativeLocalDelta)
        {
            maxNativeLocalDelta = nativeLocalDelta;
            maxNativeLocalDeltaBone = bone;
        }
        if (nativeWorldDelta > maxNativeWorldDelta)
        {
            maxNativeWorldDelta = nativeWorldDelta;
            maxNativeWorldDeltaBone = bone;
        }

        const std::pair<std::string_view, float> localDeltas[] = {
            { "native", nativeLocalDelta },
            { "swizzled", matrixDifference(selectedLocal, swizzledLocal) },
            { "bindThenKey", matrixDifference(selectedLocal, bindThenKeyLocal) },
            { "keyThenBind", matrixDifference(selectedLocal, keyThenBindLocal) },
            { "splitKey", matrixDifference(selectedLocal, splitLocal) },
        };
        std::string_view closestLocal = localDeltas[0].first;
        float closestLocalDelta = localDeltas[0].second;
        for (const auto& [name, delta] : localDeltas)
        {
            if (delta < closestLocalDelta)
            {
                closestLocal = name;
                closestLocalDelta = delta;
            }
        }

        const osg::Vec3f nativeOrigin = transformFalloutPoint(osg::Vec3f(), nativeWorld);
        const osg::Vec3f selectedOrigin = transformFalloutPoint(osg::Vec3f(), selectedWorld);
        Log(Debug::Info) << "FNV/ESM4 PROCEDURE MATRIX AUDIT " << ptr.getCellRef().getRefId()
                         << " group=" << group
                         << " source=" << source
                         << " bone=" << bone
                         << " rawKeyQuat=" << formatQuat(rawKey)
                         << " swizzledKeyQuat=" << formatQuat(swizzledKey)
                         << " bindQuat=" << formatQuat(bind)
                         << " selectedQuat=" << formatQuat(selectedLocal.getRotate())
                         << " nativeQuat=" << formatQuat(nativeLocal.getRotate())
                         << " swizzledQuat=" << formatQuat(swizzledLocal.getRotate())
                         << " bindThenKeyQuat=" << formatQuat(bindThenKeyLocal.getRotate())
                         << " keyThenBindQuat=" << formatQuat(keyThenBindLocal.getRotate())
                         << " splitKeyQuat=" << formatQuat(splitLocal.getRotate())
                         << " nativeLocalDelta=" << nativeLocalDelta
                         << " nativeWorldDelta=" << nativeWorldDelta
                         << " closestLocal=" << closestLocal
                         << " closestLocalDelta=" << closestLocalDelta
                         << " selectedOrigin=" << formatVec3(selectedOrigin)
                         << " nativeOrigin=" << formatVec3(nativeOrigin)
                         << " selectedHandedness=" << basisHandedness(selectedLocal)
                         << " nativeHandedness=" << basisHandedness(nativeLocal);
    }

    float maxFalloutTargetRotationFromBindDegrees(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets, const std::string& bone)
    {
        float result = 0.f;
        const auto it = targets.find(bone);
        if (it == targets.end())
            return result;

        for (osg::MatrixTransform* transform : it->second)
            result = std::max(result, falloutRotationFromBindDegrees(transform));
        return result;
    }

    struct FalloutPoseSemanticSample
    {
        float mHead = 0.f;
        float mSpine2 = 0.f;
        float mLeftUpperArm = 0.f;
        float mRightUpperArm = 0.f;
        float mLeftForearm = 0.f;
        float mRightForearm = 0.f;
        float mLeftThigh = 0.f;
        float mRightThigh = 0.f;
        float mMaxMajor = 0.f;
        std::string mMaxMajorBone;
        bool mBad = false;
        std::string mReason;
    };

    FalloutPoseSemanticSample sampleFalloutPoseSemantics(
        const std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& targets)
    {
        FalloutPoseSemanticSample sample;
        sample.mHead = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 head");
        sample.mSpine2 = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 spine2");
        sample.mLeftUpperArm = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 l upperarm");
        sample.mRightUpperArm = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 r upperarm");
        sample.mLeftForearm = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 l forearm");
        sample.mRightForearm = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 r forearm");
        sample.mLeftThigh = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 l thigh");
        sample.mRightThigh = maxFalloutTargetRotationFromBindDegrees(targets, "bip01 r thigh");

        const std::pair<std::string, float> majorBones[] = {
            { "head", sample.mHead },
            { "spine2", sample.mSpine2 },
            { "lUpperArm", sample.mLeftUpperArm },
            { "rUpperArm", sample.mRightUpperArm },
            { "lForearm", sample.mLeftForearm },
            { "rForearm", sample.mRightForearm },
            { "lThigh", sample.mLeftThigh },
            { "rThigh", sample.mRightThigh },
        };
        for (const auto& [name, angle] : majorBones)
        {
            if (angle > sample.mMaxMajor)
            {
                sample.mMaxMajor = angle;
                sample.mMaxMajorBone = name;
            }
        }

        const bool seatedAudit = std::getenv("OPENMW_FNV_SEATED_POSTURE_AUDIT") != nullptr;
        if (seatedAudit && (sample.mLeftUpperArm > 75.f || sample.mRightUpperArm > 75.f))
            sample.mReason = "seated_raised_upperarm";
        else if (seatedAudit && sample.mSpine2 > 75.f)
            sample.mReason = "seated_spine_sideways";
        else if (seatedAudit && (sample.mLeftForearm > 75.f || sample.mRightForearm > 75.f))
            sample.mReason = "seated_raised_forearm";
        else if (sample.mSpine2 > 105.f)
            sample.mReason = "spine2";
        else if (sample.mHead > 105.f)
            sample.mReason = "head";
        else if (!seatedAudit && (sample.mLeftThigh > 125.f || sample.mRightThigh > 125.f))
            sample.mReason = "thigh";
        else if (sample.mLeftUpperArm > 145.f || sample.mRightUpperArm > 145.f || sample.mLeftForearm > 155.f
            || sample.mRightForearm > 155.f)
            sample.mReason = "arm";
        else if (sample.mMaxMajor > 170.f)
            sample.mReason = "major";
        else
            sample.mReason = "ok";

        sample.mBad = sample.mReason != "ok";
        return sample;
    }

    bool isFalloutAccumulationBone(const std::string& lowerBone)
    {
        return lowerBone == "bip01" || lowerBone == "bip01 pelvis" || lowerBone.find("nonaccum") != std::string::npos;
    }

    bool isSafeFalloutBoneTranslation(const osg::Vec3f& translation, const osg::Vec3f& currentTranslation)
    {
        if (!isFiniteVec3(translation))
            return false;

        // FNV keyframes carry local bone offsets as well as rotations. Keep the authored offsets, but reject values
        // that are far outside a human skeleton's local space so bad helper/controller data cannot tear actors apart.
        const osg::Vec3f delta = translation - currentTranslation;
        return translation.length2() < 250000.f && delta.length2() < 65536.f;
    }

    bool shouldApplyFalloutBoneTranslations()
    {
        return std::getenv("OPENMW_FNV_APPLY_BONE_TRANSLATIONS") != nullptr;
    }

    bool shouldApplyFalloutProcedureBoneTranslations()
    {
        return shouldApplyFalloutBoneTranslations()
            || std::getenv("OPENMW_FNV_APPLY_PROCEDURE_BONE_TRANSLATIONS") != nullptr;
    }

    bool shouldApplyFalloutAccumulationRotation()
    {
        return std::getenv("OPENMW_FNV_APPLY_ACCUM_ROTATION") != nullptr;
    }

    bool shouldUseNativeFalloutAnimationCallbacks()
    {
        // Fallout KFs are data: sampled local translation/rotation/scale belongs on the
        // matched target node through the same callback path OpenMW uses for native actors.
        // The old hand-composed Fallout matrix path is intentionally unreachable.
        return true;
    }

    bool shouldMirrorFalloutDuplicatePoses()
    {
        return std::getenv("OPENMW_FNV_DISABLE_DUPLICATE_POSE_MIRROR") == nullptr;
    }

    bool shouldMirrorFalloutSkinnedDuplicateBone(const std::string& lowerBone)
    {
        if (!Misc::StringUtils::ciStartsWith(lowerBone, "bip01"))
            return false;

        // Keep actor/root accumulation on the primary skeleton. Duplicating these onto every skinned part is what
        // produced the severe "pulled apart" look; limbs and torso child bones still need the pose for skinning.
        if (lowerBone == "bip01" || lowerBone.find("nonaccum") != std::string::npos)
            return false;

        return true;
    }

    std::string getFalloutSyntheticGroupFromKf(std::string_view kfname)
    {
        std::string stem(kfname);
        const std::size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos)
            stem = stem.substr(slash + 1);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos)
            stem.resize(dot);
        Misc::StringUtils::lowerCaseInPlace(stem);

        if (stem.find("unequip") != std::string::npos)
            return "unequip";
        if (stem.find("equip") != std::string::npos)
            return "equip";
        if (stem.find("reload") != std::string::npos)
            return "reload";
        if (stem.find("attack") != std::string::npos)
        {
            if (stem.find("attackright") != std::string::npos)
                return "attack2";
            if (stem.find("attack3") != std::string::npos || stem.find("attack4") != std::string::npos
                || stem.find("attack5") != std::string::npos || stem.find("attack6") != std::string::npos
                || stem.find("attack7") != std::string::npos || stem.find("attack8") != std::string::npos
                || stem.find("attack9") != std::string::npos)
                return "attack3";
            return "attack1";
        }
        if (stem.find("aim") != std::string::npos)
            return "weaponpose";
        if (stem.find("crouch") != std::string::npos || stem.find("kneel") != std::string::npos
            || stem == "specialidle_sitcycle" || stem == "mtspecialidle_deactivateloop")
            return "kneel";
        if (stem.find("floorsleep") != std::string::npos || stem.find("prone") != std::string::npos
            || stem == "specialidle_sleepcycle" || stem == "mtspecialidle_knockdownfacedown")
            return "prone";
        if (stem == "talking" || stem.starts_with("talk") || stem.find("_talk") != std::string::npos)
            return "talk";
        if (stem.find("wave") != std::string::npos || stem.find("gesture") != std::string::npos
            || stem == "specialidle_mtponder" || stem == "specialidle_salutes")
            return "wave";
        if (stem == "swimidle")
            return "swimidle";
        if (stem.find("flyaway") != std::string::npos)
            return "flyforward";
        if (stem.find("specialidle") != std::string::npos)
            return "idle2";
        if (stem == "mtidle" || stem == "idle" || Misc::StringUtils::ciEndsWith(stem, "idle"))
            return "idle";
        if (Misc::StringUtils::ciEndsWith(stem, "turnleft"))
            return "turnleft";
        if (Misc::StringUtils::ciEndsWith(stem, "turnright"))
            return "turnright";
        if (stem == "mtforward")
            return "walkforward";
        if (stem == "mtbackward")
            return "walkback";
        if (stem == "mtleft")
            return "walkleft";
        if (stem == "mtright")
            return "walkright";
        if (Misc::StringUtils::ciEndsWith(stem, "fastforward") || Misc::StringUtils::ciEndsWith(stem, "runforward"))
            return "runforward";
        if (Misc::StringUtils::ciEndsWith(stem, "fastbackward") || Misc::StringUtils::ciEndsWith(stem, "runbackward"))
            return "runback";
        if (Misc::StringUtils::ciEndsWith(stem, "fastleft") || Misc::StringUtils::ciEndsWith(stem, "runleft"))
            return "runleft";
        if (Misc::StringUtils::ciEndsWith(stem, "fastright") || Misc::StringUtils::ciEndsWith(stem, "runright"))
            return "runright";
        if (Misc::StringUtils::ciEndsWith(stem, "forward") || Misc::StringUtils::ciEndsWith(stem, "walkforward"))
            return "walkforward";
        if (Misc::StringUtils::ciEndsWith(stem, "backward") || Misc::StringUtils::ciEndsWith(stem, "walkbackward"))
            return "walkback";
        if (Misc::StringUtils::ciEndsWith(stem, "left") || Misc::StringUtils::ciEndsWith(stem, "walkleft"))
            return "walkleft";
        if (Misc::StringUtils::ciEndsWith(stem, "right") || Misc::StringUtils::ciEndsWith(stem, "walkright"))
            return "walkright";
        return {};
    }

    std::string getFalloutProcedureGroupFromKf(std::string_view kfname)
    {
        if (const char* overrideGroup = std::getenv("OPENMW_FNV_PROCEDURE_KF_OVERRIDE_GROUP"))
        {
            std::string group = Misc::StringUtils::lowerCase(overrideGroup);
            if (group == "idle" || group == "sitchairlisten" || group == "sitchairtalk" || group == "sitchaireat")
                return group;
            Log(Debug::Warning) << "FNV/ESM4 diag: ignoring invalid OPENMW_FNV_PROCEDURE_KF_OVERRIDE_GROUP='"
                                << overrideGroup << "'";
        }

        std::string stem(kfname);
        const std::size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos)
            stem = stem.substr(slash + 1);
        const std::size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos)
            stem.resize(dot);
        Misc::StringUtils::lowerCaseInPlace(stem);

        if (stem == "dynamicidle_chairsit")
            return "chairsit";
        for (std::string_view direction : { "forward", "back", "left", "right" })
        {
            if (stem == "chair_" + std::string(direction) + "enter")
                return "chair" + std::string(direction) + "enter";
            if (stem == "chair_" + std::string(direction) + "exit")
                return "chair" + std::string(direction) + "exit";
        }
        if (stem == "dynamicidle_sit" || stem == "dynamicidle_sleep")
            return "idle";
        if (stem.find("sitchairlisten") != std::string::npos)
            return "sitchairlisten";
        if (stem.find("sitchairtalk") != std::string::npos || stem.find("chair") != std::string::npos
            && stem.find("talk") != std::string::npos)
            return "sitchairtalk";
        if (stem.find("sitchaireat") != std::string::npos || stem.find("sittablechaireat") != std::string::npos)
            return "sitchaireat";
        return "idle";
    }

    std::string summarizeFalloutTextKeys(const SceneUtil::TextKeyMap& textkeys, unsigned int limit = 32)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3);
        unsigned int count = 0;
        for (auto it = textkeys.begin(); it != textkeys.end(); ++it)
        {
            if (count != 0)
                stream << " | ";
            if (count >= limit)
            {
                stream << "...";
                break;
            }
            stream << it->first << ":" << it->second;
            ++count;
        }
        if (count == 0)
            stream << "<none>";
        return stream.str();
    }

    float inferFalloutTextKeyStop(const SceneUtil::TextKeyMap& textkeys, float fallback)
    {
        for (auto it = textkeys.rbegin(); it != textkeys.rend(); ++it)
        {
            if (std::isfinite(it->first) && it->first > 0.f)
                return it->first;
        }
        return fallback;
    }

    void addSyntheticLoopingTextKeys(SceneUtil::TextKeyMap& textkeys, const std::string& group)
    {
        constexpr float start = 0.f;
        const float stop = inferFalloutTextKeyStop(textkeys, 4.f);
        textkeys.emplace(start, group + ": start");
        textkeys.emplace(start, group + ": loop start");
        textkeys.emplace(stop, group + ": loop stop");
        textkeys.emplace(stop, group + ": stop");
    }

    void addSyntheticOneShotTextKeys(SceneUtil::TextKeyMap& textkeys, const std::string& group)
    {
        constexpr float start = 0.f;
        const float stop = inferFalloutTextKeyStop(textkeys, 1.f);
        textkeys.emplace(start, group + ": start");
        textkeys.emplace(stop, group + ": stop");
    }

    bool isSyntheticFalloutLoopingGroup(std::string_view group)
    {
        return group == "idle" || group == "idle2" || group == "stand" || group == "weaponpose"
            || group == "swimidle" || group == "kneel" || group == "prone" || group == "walk"
            || group == "talk" || group == "flyforward"
            || group.starts_with("walk") || group.starts_with("run")
            || group.starts_with("turn") || group.starts_with("sneak");
    }

    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(const std::string& kfname,
        const std::string& baseModel, bool falloutProcedureIdle, std::string_view controllerOverlayKf,
        std::string_view falloutSemanticGroup)
    {
        if (!mResourceSystem->getVFS()->exists(kfname))
            return nullptr;

        auto animsrc = std::make_shared<AnimSource>();
        animsrc->mKeyframes = mResourceSystem->getKeyframeManager()->get(VFS::Path::toNormalized(kfname));
        animsrc->mSourceName = kfname;

        if (!controllerOverlayKf.empty())
        {
            const std::string overlayPath(controllerOverlayKf);
            if (mResourceSystem->getVFS()->exists(overlayPath))
            {
                const osg::ref_ptr<const SceneUtil::KeyframeHolder> overlay
                    = mResourceSystem->getKeyframeManager()->get(VFS::Path::toNormalized(overlayPath));
                if (animsrc->mKeyframes != nullptr && overlay != nullptr
                    && !overlay->mKeyframeControllers.empty())
                {
                    animsrc->mKeyframes = mergeFonvWeaponControllerOverlay(*animsrc->mKeyframes, *overlay);
                    Log(Debug::Verbose) << "FNV/ESM4 diag: merged " << overlay->mKeyframeControllers.size()
                                        << " hand-grip controller(s) from " << overlayPath << " over " << kfname;
                }
            }
            else
                Log(Debug::Warning) << "FNV/ESM4: weapon hand-grip overlay is absent: " << overlayPath;
        }

        std::string lowerKf = Misc::StringUtils::lowerCase(kfname);
        std::string lowerBaseModel = Misc::StringUtils::lowerCase(baseModel);
        // A legacy ESM3 player shell can still request Morrowind's xbase_anim.kf while the
        // world-viewer has replaced its visual root with Starfield's native human skeleton.
        // None of xbase_anim's Bip01 controllers can bind to that C_/L_/R_ skeleton, so adding
        // the source only repeats warnings and leaves a useless animation source behind.
        const bool starfieldHumanSkeleton
            = lowerBaseModel.find("actors/human/characterassets/") != std::string::npos;
        const bool morrowindBaseAnimation = Misc::StringUtils::ciEndsWith(lowerKf, "xbase_anim.kf");
        if (starfieldHumanSkeleton && morrowindBaseAnimation)
        {
            static unsigned int sSkippedStarfieldMorrowindBaseAnimationLogs = 0;
            if (sSkippedStarfieldMorrowindBaseAnimationLogs++ == 0)
                Log(Debug::Verbose) << "World viewer: skipped incompatible Morrowind animation source " << kfname
                                    << " for Starfield human skeleton " << baseModel;
            return nullptr;
        }
        const bool isFonvActorAnim
            = shouldSynthesizeFonvSemanticAlias(isFalloutNpcAnimationContext(mPtr), falloutSemanticGroup)
            && (lowerKf.find("meshes/characters/_male/") != std::string::npos
                || lowerKf.find("meshes\\characters\\_male\\") != std::string::npos
                || lowerKf.find("characters/_male/") != std::string::npos
                || lowerKf.find("characters\\_male\\") != std::string::npos
                || lowerBaseModel.find("characters\\_male\\") != std::string::npos
                || lowerBaseModel.find("characters/_male/") != std::string::npos);
        const bool isFonvCreatureAnim = lowerKf.find("meshes/creatures/") != std::string::npos
            || lowerBaseModel.find("meshes\\creatures\\") != std::string::npos
            || lowerBaseModel.find("meshes/creatures/") != std::string::npos;
        const bool isFonvAnim = isFonvActorAnim || isFonvCreatureAnim;

        if (animsrc->mKeyframes && !animsrc->mKeyframes->mKeyframeControllers.empty() && isFonvAnim)
        {
            // Callers that selected an exact retail action manifest provide the semantic group explicitly. Filename
            // inference remains for legacy locomotion/creature sources, but never overrides an authored manifest.
            const std::string group = falloutSemanticGroup.empty()
                ? getFalloutSyntheticGroupFromKf(kfname)
                : std::string(falloutSemanticGroup);
            if (!group.empty() && !animsrc->mKeyframes->mTextKeys.hasGroupStart(group))
            {
                osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                    = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                if (isSyntheticFalloutLoopingGroup(group))
                    addSyntheticLoopingTextKeys(keyframes->mTextKeys, group);
                else
                    addSyntheticOneShotTextKeys(keyframes->mTextKeys, group);
                animsrc->mKeyframes = keyframes;
                Log(Debug::Verbose) << "FNV/ESM4 diag: synthesized "
                                    << (isSyntheticFalloutLoopingGroup(group) ? "looping" : "one-shot")
                                    << " KF text key group '" << group << "' for " << kfname;
            }
        }
        if (animsrc->mKeyframes && !animsrc->mKeyframes->mKeyframeControllers.empty()
            && isFonvCreatureAnim && !falloutSemanticGroup.empty()
            && !animsrc->mKeyframes->mTextKeys.hasGroupStart(falloutSemanticGroup))
        {
            // Creature directories contain many transitions and special idles which can expose the same retail
            // groups. A caller that selected one exact same-rig KF gives it a proof-facing semantic alias, making
            // source choice independent of recursive VFS iteration and of unrelated transition filenames.
            osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
            const std::string group(falloutSemanticGroup);
            if (isSyntheticFalloutLoopingGroup(group))
                addSyntheticLoopingTextKeys(keyframes->mTextKeys, group);
            else
                addSyntheticOneShotTextKeys(keyframes->mTextKeys, group);
            animsrc->mKeyframes = keyframes;
            Log(Debug::Verbose) << "FNV/ESM4 diag: aliased selected creature KF " << kfname
                                << " to semantic group '" << group << "'";
        }
        if (animsrc->mKeyframes && !animsrc->mKeyframes->mKeyframeControllers.empty() && isFonvActorAnim)
        {
            const char* forcedGroup = std::getenv("OPENMW_FNV_FORCED_KF_GROUP");
            const char* forcedOverlayGroup = std::getenv("OPENMW_FNV_FORCED_OVERLAY_GROUP");
            const char* forcedSource = std::getenv("OPENMW_FNV_FORCED_KF_SOURCE");
            std::vector<std::string> forcedSources;
            if (forcedSource != nullptr && forcedSource[0] != '\0')
            {
                std::stringstream stream(forcedSource);
                std::string source;
                while (std::getline(stream, source, ';'))
                {
                    source = Misc::StringUtils::lowerCase(VFS::Path::toNormalized(source));
                    if (!source.empty())
                        forcedSources.push_back(source);
                }
            }
            const bool sourceMatches = forcedSources.empty()
                || std::find(forcedSources.begin(), forcedSources.end(), lowerKf) != forcedSources.end();
            const bool primarySourceMatches = forcedSources.empty() ? sourceMatches : lowerKf == forcedSources.front();
            const bool overlaySourceMatches = forcedSources.empty() ? sourceMatches : lowerKf == forcedSources.back();
            const auto synthesizeForcedGroup = [&](const char* groupEnv, std::string_view reason, bool sourceEligible) {
                const std::string group = groupEnv != nullptr ? Misc::StringUtils::lowerCase(groupEnv) : "";
                if (group.empty() || !sourceEligible || animsrc->mKeyframes->mTextKeys.hasGroupStart(group))
                    return;

                osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                    = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                addSyntheticLoopingTextKeys(keyframes->mTextKeys, group);
                animsrc->mKeyframes = keyframes;
                Log(Debug::Verbose) << "FNV/ESM4 diag: synthesized missing forced actor KF " << reason
                                 << " text key group '" << group << "' for " << kfname;
            };
            if (forcedGroup != nullptr)
                synthesizeForcedGroup(forcedGroup, "primary", primarySourceMatches);
            if (forcedOverlayGroup != nullptr)
                synthesizeForcedGroup(forcedOverlayGroup, "overlay", overlaySourceMatches);
            const bool weaponIdlePose = shouldEnableFalloutWeaponIdlePose(mPtr);
            if (weaponIdlePose && isFalloutWeaponAimKf(lowerKf)
                && !animsrc->mKeyframes->mTextKeys.hasGroupStart("weaponpose"))
            {
                osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                    = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                addSyntheticLoopingTextKeys(keyframes->mTextKeys, "weaponpose");
                animsrc->mKeyframes = keyframes;
                Log(Debug::Verbose) << "FNV/ESM4 diag: synthesized retail weapon overlay group 'weaponpose' for "
                                 << kfname;
            }
        }

        if (!animsrc->mKeyframes || animsrc->mKeyframes->mTextKeys.empty()
            || animsrc->mKeyframes->mKeyframeControllers.empty())
            return nullptr;

        const NodeMap& nodeMap = getNodeMap();
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        unsigned int matchedControllers = 0;
        unsigned int missingRequiredControllers = 0;
        unsigned int controllerCollisions = 0;
        unsigned int exactControllers = 0;
        unsigned int aliasedControllers = 0;
        unsigned int deferredVisualControllers = 0;
        unsigned int skippedExactDuplicateControllers = 0;
        unsigned int skippedSyntheticAttachmentHelperControllers = 0;
        unsigned int falloutActorBasisApplied = 0;
        unsigned int falloutActorBasisMissed = 0;
        unsigned int falloutActorBasisAudited = 0;
        std::map<std::pair<std::size_t, std::string>, std::string> resolvedControllerAuthors;
        const bool auditFalloutControllerTargets
            = isFonvAnim && std::getenv("OPENMW_FNV_CONTROLLER_TARGET_AUDIT") != nullptr;
        const bool auditFalloutActorControllers = isFonvActorAnim
            && (lowerKf.find("locomotion/mtidle.kf") != std::string::npos
                || lowerKf.find("locomotion\\mtidle.kf") != std::string::npos);
        if (auditFalloutActorControllers)
        {
            static unsigned int sFalloutActorControllerSourceLogs = 0;
            if (sFalloutActorControllerSourceLogs < 12)
            {
                ++sFalloutActorControllerSourceLogs;
                Log(Debug::Verbose) << "FNV/ESM4 diag: actor controller audit source=" << kfname
                                 << " baseModel=" << baseModel
                                 << " isActor=" << isFonvActorAnim
                                 << " isCreature=" << isFonvCreatureAnim
                                 << " controllerCount=" << controllerMap.size();
            }
        }
        for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
             it != controllerMap.end(); ++it)
        {
            std::string bonename = Misc::StringUtils::lowerCase(it->first);
            const std::string authoredBonename = bonename;
            const std::string_view duplicateOf = isFonvAnim
                ? getFonvExactDuplicateControllerTarget(authoredBonename)
                : std::string_view{};
            if (!duplicateOf.empty())
            {
                const bool canonicalTrackIsAuthored = std::any_of(controllerMap.begin(), controllerMap.end(),
                    [duplicateOf](const auto& controller) {
                        return Misc::StringUtils::lowerCase(controller.first) == duplicateOf;
                    });
                if (canonicalTrackIsAuthored)
                {
                    ++skippedExactDuplicateControllers;
                    if (auditFalloutControllerTargets)
                        Log(Debug::Info) << "FNV/ESM4 CONTROLLER TARGET AUDIT source=" << kfname << " authored='"
                                         << authoredBonename << "' resolution=exact-authored-duplicate canonical='"
                                         << duplicateOf << "'";
                    Log(Debug::Verbose) << "FNV/ESM4: skipped exact authored duplicate controller target '"
                                        << authoredBonename << "' because canonical target '" << duplicateOf
                                        << "' is present in " << kfname;
                    continue;
                }
            }
            const std::vector<std::string> explicitAliases
                = isFonvAnim ? getFonvBoneAliases(authoredBonename) : std::vector<std::string>{};
            std::string resolutionKind = "missing";
            NodeMap::const_iterator found = nodeMap.find(bonename);
            if (found != nodeMap.end())
            {
                bonename = found->first;
                resolutionKind = "exact";
                ++exactControllers;
            }
            if (found == nodeMap.end() && isFonvAnim)
            {
                found = findFonvAnimationBone(nodeMap, authoredBonename, bonename);
                if (found != nodeMap.end())
                {
                    resolutionKind = "explicit-alias";
                    ++aliasedControllers;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: aliased KF bone '" << authoredBonename << "' to '"
                                     << bonename << "' for " << kfname;
                }
            }
            const bool requiredSkeletonTarget
                = !isFonvAnim || isFonvRequiredSkeletonControllerTarget(authoredBonename);
            if (found == nodeMap.end() && !requiredSkeletonTarget)
                resolutionKind = "deferred-visual";
            if (auditFalloutControllerTargets)
            {
                std::ostringstream aliases;
                for (std::size_t aliasIndex = 0; aliasIndex < explicitAliases.size(); ++aliasIndex)
                {
                    if (aliasIndex != 0)
                        aliases << ',';
                    aliases << explicitAliases[aliasIndex];
                }
                Log(found != nodeMap.end() ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 CONTROLLER TARGET AUDIT source=" << kfname << " authored='" << authoredBonename
                    << "' resolution=" << resolutionKind << " resolved='"
                    << (found != nodeMap.end() ? bonename : std::string("<none>")) << "' explicitAliases=["
                    << aliases.str() << ']';
            }
            if (found == nodeMap.end())
            {
                if (isFonvAnim && !requiredSkeletonTarget)
                {
                    ++deferredVisualControllers;
                    Log(Debug::Verbose) << "FNV/ESM4: deferred optional visual controller target '"
                                        << authoredBonename << "' absent from assembled actor graph " << baseModel
                                        << " (referenced by " << kfname << ")";
                }
                else
                {
                    ++missingRequiredControllers;
                    if (isFonvAnim)
                        Log(Debug::Verbose) << "FNV/ESM4: animation controller bone '" << bonename
                                            << "' is absent from " << baseModel << " (referenced by " << kfname << ")";
                    else
                        Log(Debug::Warning) << "Warning: addAnimSource: can't find bone '" + bonename << "' in "
                                            << baseModel << " (referenced by " << kfname << ")";
                }
                continue;
            }
            const std::string lowerResolvedBone = Misc::StringUtils::lowerCase(bonename);
            if (isFonvActorAnim && shouldSkipFalloutSyntheticAttachmentHelperControllers(mPtr)
                && isFalloutSyntheticAttachmentHelperName(lowerResolvedBone)
                && isFalloutSyntheticAttachmentHelperNode(found->second))
            {
                ++skippedSyntheticAttachmentHelperControllers;
                static unsigned int sSkippedSyntheticAttachmentHelperLogs = 0;
                if (sSkippedSyntheticAttachmentHelperLogs < 24)
                {
                    ++sSkippedSyntheticAttachmentHelperLogs;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: skipped synthetic attachment helper controller bone="
                                     << bonename << " source=" << kfname
                                     << " so carried parts inherit the live actor hand/body transform";
                }
                continue;
            }
            osg::Node* node = found->second;

            // FO3/FNV author the held-weapon transform as a root-level "Weapon" KF target. The scene-node match
            // can resolve through a synthetic attachment helper, so the authored controller-map key is the stable
            // retail contract here. Keep that track in the right-arm aim overlay; otherwise arms-only weaponpose
            // playback silently drops the weapon transform while locomotion continues to drive the lower body.
            const size_t blendMask = isFonvActorAnim && authoredBonename == "weapon"
                ? BoneGroup_RightArm
                : detectBlendMask(node, it->second->getName());

            // clone the controller, because each Animation needs its own ControllerSource
            osg::ref_ptr<SceneUtil::KeyframeController> cloned
                = osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY);
            if (isFonvActorAnim)
            {
                const bool auditBone = auditFalloutActorControllers
                    && (bonename.find("hand") != std::string::npos
                        || bonename.find("finger") != std::string::npos
                        || bonename.find("forearm") != std::string::npos
                        || bonename.find("head") != std::string::npos);
                if (auto* nifController = dynamic_cast<NifOsg::KeyframeController*>(cloned.get()))
                {
                    float bindScale = 1.f;
                    if (const auto* nifTarget = dynamic_cast<const NifOsg::MatrixTransform*>(found->second.get()))
                        bindScale = nifTarget->mScale;
                    nifController->setFalloutActorTransformBasis(
                        bonename, found->second->getMatrix().getTrans(), getFalloutBindRotation(found->second),
                        bindScale);
                    if (std::getenv("OPENMW_FNV_CONTROLLER_KEY_AUDIT") != nullptr)
                    {
                        Log(Debug::Info) << "FNV/ESM4 CONTROLLER KEY AUDIT source=" << kfname
                                         << " bone=" << bonename
                                         << " rotationInterpolation="
                                         << nifController->getRotationInterpolationType()
                                         << " rotationKeys=" << nifController->getRotationKeyCount()
                                         << " translationInterpolation="
                                         << nifController->getTranslationInterpolationType()
                                         << " translationKeys=" << nifController->getTranslationKeyCount()
                                         << " bspline=" << nifController->usesBSplineTransform();
                    }
                    ++falloutActorBasisApplied;
                    static unsigned int sFalloutBasisLogs = 0;
                    if ((sFalloutBasisLogs < 8
                            && (bonename.find("finger42") != std::string::npos
                                || bonename.find("finger12") != std::string::npos
                                || bonename.find("head") != std::string::npos))
                        || (auditBone && falloutActorBasisAudited < 20))
                    {
                        ++sFalloutBasisLogs;
                        ++falloutActorBasisAudited;
                        Log(Debug::Verbose) << "FNV/ESM4 diag: enabled actor rotation basis for " << bonename
                                         << " source=" << kfname
                                         << " sourceType=" << typeid(*it->second).name()
                                         << " clonedType=" << typeid(*cloned).name();
                    }
                }
                else
                {
                    ++falloutActorBasisMissed;
                    static unsigned int sFalloutBasisMissLogs = 0;
                    if (sFalloutBasisMissLogs < 8 || (auditBone && falloutActorBasisAudited < 20))
                    {
                        ++sFalloutBasisMissLogs;
                        ++falloutActorBasisAudited;
                        Log(Debug::Verbose) << "FNV/ESM4 diag: actor rotation basis skipped non-NifOsg controller bone="
                                         << bonename << " sourceType=" << typeid(*it->second).name()
                                         << " clonedType=" << typeid(*cloned).name() << " source=" << kfname;
                    }
                }
            }
            cloned->setSource(mAnimationTimePtr[blendMask]);

            const bool didInsert
                = animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned)).second;
            if (!didInsert)
            {
                ++controllerCollisions;
                const auto author = resolvedControllerAuthors.find(std::make_pair(blendMask, bonename));
                Log(isFonvAnim ? Debug::Error : Debug::Warning)
                    << "Animation controller target collision in " << kfname << ": authored target '"
                    << authoredBonename << "' and earlier target '"
                    << (author != resolvedControllerAuthors.end() ? author->second : std::string("<unknown>"))
                    << "' both resolved to '" << bonename << "' in blend mask " << blendMask
                    << "; ignoring the later controller";
                continue;
            }
            resolvedControllerAuthors.emplace(std::make_pair(blendMask, bonename), authoredBonename);
            ++matchedControllers;
        }
        if (isFonvAnim && (missingRequiredControllers != 0 || controllerCollisions != 0))
        {
            Log(Debug::Error) << "FNV/ESM4 rejected partial animation source " << kfname << " for " << baseModel
                              << ": exact=" << exactControllers << " explicitAliases=" << aliasedControllers
                              << " missingRequired=" << missingRequiredControllers << " deferredVisual="
                              << deferredVisualControllers << " collisions=" << controllerCollisions
                              << ". Fallout controller targets must bind by exact authored name or exact explicit alias.";
            return nullptr;
        }
        if (isFonvAnim)
        {
            const bool isChairTransition = lowerKf.find("idleanims/chair_") != std::string::npos
                && (lowerKf.find("enter.kf") != std::string::npos || lowerKf.find("exit.kf") != std::string::npos);
            const bool isProcedureIdle = falloutProcedureIdle && lowerKf.find("idleanims/") != std::string::npos
                && (lowerKf.find("dynamicidle_sit") != std::string::npos
                    || lowerKf.find("dynamicidle_chairsit") != std::string::npos
                    || lowerKf.find("sitchair") != std::string::npos
                    || lowerKf.find("sittablechair") != std::string::npos
                    || lowerKf.find("dynamicidle_sleep") != std::string::npos || isChairTransition);
            if (isProcedureIdle)
            {
                const std::string group = getFalloutProcedureGroupFromKf(kfname);
                if (!animsrc->getTextKeys().hasGroupStart(group))
                {
                    osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                        = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                    const float stopTime = inferFalloutTextKeyStop(keyframes->mTextKeys, 4.f);
                    keyframes->mTextKeys.emplace(0.f, group + ": start");
                    if (!isChairTransition)
                    {
                        keyframes->mTextKeys.emplace(0.f, group + ": loop start");
                        keyframes->mTextKeys.emplace(stopTime, group + ": loop stop");
                    }
                    keyframes->mTextKeys.emplace(stopTime, group + ": stop");
                    animsrc->mKeyframes = keyframes;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: synthesized " << group
                                     << " text keys for procedure source " << kfname << " stopTime=" << stopTime;
                }
                Log(Debug::Verbose) << "FNV/ESM4 diag: procedure text keys source=" << kfname << " group=" << group
                                 << " keys=[" << summarizeFalloutTextKeys(animsrc->getTextKeys()) << "]";
            }
            animsrc->mFalloutProcedureIdle = isProcedureIdle;

            Log(Debug::Verbose) << "FNV/ESM4 diag: animation source " << kfname << " bound " << matchedControllers << "/"
                             << controllerMap.size() << " controller(s) to " << baseModel << ", missing "
                             << missingRequiredControllers << ", skippedSyntheticAttachmentHelpers "
                             << skippedSyntheticAttachmentHelperControllers << ", collisions "
                             << controllerCollisions << ", exact " << exactControllers << ", explicitAliases "
                             << aliasedControllers << ", deferredVisual " << deferredVisualControllers
                             << ", skippedExactDuplicates " << skippedExactDuplicateControllers;
            if (auditFalloutActorControllers)
                Log(Debug::Verbose) << "FNV/ESM4 diag: actor controller audit result source=" << kfname
                                 << " basisApplied=" << falloutActorBasisApplied
                                 << " basisMissed=" << falloutActorBasisMissed
                                 << " matched=" << matchedControllers
                                 << " missing=" << missingRequiredControllers;
            std::ostringstream groups;
            unsigned int groupCount = 0;
            for (const std::string& group : animsrc->getTextKeys().getGroups())
            {
                if (groupCount != 0)
                    groups << ",";
                groups << group;
                ++groupCount;
            }
            Log(Debug::Verbose) << "FNV/ESM4 diag: animation source " << kfname << " groups=[" << groups.str() << "]";
            if (!animsrc->mKeyframes->mFalloutHeadAnimTracks.empty())
            {
                std::ostringstream headAnimSample;
                unsigned int sampleCount = 0;
                for (const auto& [name, track] : animsrc->mKeyframes->mFalloutHeadAnimTracks)
                {
                    if (sampleCount >= 8)
                        break;
                    if (sampleCount != 0)
                        headAnimSample << " | ";
                    float minValue = track.mDefaultValue;
                    float maxValue = track.mDefaultValue;
                    for (const auto& [time, value] : track.mKeys)
                    {
                        minValue = std::min(minValue, value);
                        maxValue = std::max(maxValue, value);
                    }
                    headAnimSample << name << "{"
                                   << (track.mType
                                               == SceneUtil::KeyframeHolder::FalloutHeadAnimTrack::Type::Float
                                           ? "float"
                                           : "bool")
                                   << " keys=" << track.mKeys.size() << " default=" << track.mDefaultValue
                                   << " range=(" << minValue << "," << maxValue << ")}";
                    ++sampleCount;
                }
                Log(Debug::Verbose) << "FNV/ESM4 diag: animation source " << kfname << " headAnimTracks="
                                 << animsrc->mKeyframes->mFalloutHeadAnimTracks.size() << " sample=["
                                 << headAnimSample.str() << "]";
            }
        }

        mAnimSources.push_back(animsrc);

        mSupportedDirections.clear();
        for (const std::string& group : mAnimSources.back()->getTextKeys().getGroups())
            mSupportedAnimations.insert(group);

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
        mObjectRoot->accept(assignVisitor);

        // Determine the movement accumulation bone if necessary
        if (!mAccumRoot)
        {
            // Priority matters! bip01 is preferred.
            static const std::initializer_list<std::string_view> accumRootNames = { "bip01", "root bone" };
            NodeMap::const_iterator found = nodeMap.end();
            for (const std::string_view& name : accumRootNames)
            {
                found = nodeMap.find(name);
                if (found == nodeMap.end())
                    continue;
                for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
                     it != controllerMap.end(); ++it)
                {
                    if (Misc::StringUtils::ciEqual(it->first, name))
                    {
                        mAccumRoot = found->second;
                        break;
                    }
                }
                if (mAccumRoot)
                    break;
            }
        }

        // Get the blending rules
        if (useSmoothAnimationTransitions())
        {
            // Note, even if the actual config is .json - we should send a .yaml path to AnimBlendRulesManager, the
            // manager will check for .json if it will not find a specified .yaml file.
            VFS::Path::Normalized blendConfigPath(kfname);
            blendConfigPath.changeExtension("yaml");

            // globalBlendConfigPath is only used with actors! Objects have no default blending.
            constexpr VFS::Path::NormalizedView globalBlendConfigPath("animations/animation-config.yaml");

            osg::ref_ptr<const SceneUtil::AnimBlendRules> blendRules;
            if (mPtr.getClass().isActor())
            {
                blendRules
                    = mResourceSystem->getAnimBlendRulesManager()->getRules(globalBlendConfigPath, blendConfigPath);
                if (blendRules == nullptr)
                    Log(Debug::Warning) << "Unable to find animation blending rules: '" << blendConfigPath << "' or '"
                                        << globalBlendConfigPath << "'";
            }
            else
            {
                blendRules = mResourceSystem->getAnimBlendRulesManager()->getRules(blendConfigPath, blendConfigPath);
            }

            // At this point blendRules will either be nullptr or an AnimBlendRules instance with > 0 rules inside.
            animsrc->mAnimBlendRules = blendRules;
        }

        return animsrc;
    }

    void Animation::clearAnimSources()
    {
        // Property-bearing KF callbacks retain the pre-animation StateSet so it can be
        // restored. Detach them before releasing their time/source ownership.
        detachActiveControllers();
        mStates.clear();

        for (size_t i = 0; i < sNumBlendMasks; i++)
            mAnimationTimePtr[i]->setTimePtr(std::shared_ptr<float>());

        mAccumCtrl = nullptr;

        mSupportedAnimations.clear();
        mSupportedDirections.clear();
        mAnimSources.clear();

        mAnimVelocities.clear();
    }

    bool Animation::hasAnimation(std::string_view anim) const
    {
        return mSupportedAnimations.find(anim) != mSupportedAnimations.end();
    }

    std::string Animation::getAnimationSourceName(std::string_view anim) const
    {
        // Match play(): the last inserted source containing the requested group wins.
        for (AnimSourceList::const_reverse_iterator source = mAnimSources.rbegin(); source != mAnimSources.rend();
             ++source)
        {
            if ((*source)->getTextKeys().hasGroupStart(anim))
                return (*source)->mSourceName;
        }
        return {};
    }

    std::string Animation::getAnimationGroupFromSource(
        std::string_view sourceName, std::string_view groupPrefix) const
    {
        for (AnimSourceList::const_reverse_iterator source = mAnimSources.rbegin(); source != mAnimSources.rend();
             ++source)
        {
            if (!Misc::StringUtils::ciEqual((*source)->mSourceName, sourceName))
                continue;
            for (const std::string& group : (*source)->getTextKeys().getGroups())
            {
                if (groupPrefix.empty() || Misc::StringUtils::ciStartsWith(group, groupPrefix))
                    return group;
            }
        }
        return {};
    }

    bool Animation::prepareFalloutHitReaction()
    {
        const bool isStrictFonvNpc = isStrictFonvNpcAnimationContext(mPtr);
        std::string baseModel;
        if (isStrictFonvNpc)
            baseModel = mPtr.getClass().getCorrectedModel(mPtr);
        const std::string selectedSource = getAnimationSourceName(FonvNpcHitReactionSemanticGroup);
        const std::string exactSource(FonvNpcHitReactionSource);
        const bool exactSourceExists
            = isStrictFonvNpc && mResourceSystem != nullptr && mResourceSystem->getVFS()->exists(exactSource);
        const FonvNpcHitReactionResolution resolution
            = resolveFonvNpcHitReaction(isStrictFonvNpc, baseModel, selectedSource, exactSourceExists);

        if (resolution == FonvNpcHitReactionResolution::NotApplicable)
            return true;

        if (resolution == FonvNpcHitReactionResolution::IncompatibleSkeleton)
        {
            Log(Debug::Error) << "FNV NPC hit reaction rejected incompatible skeleton: " << baseModel;
            return false;
        }

        if (resolution == FonvNpcHitReactionResolution::MissingSource)
        {
            Log(Debug::Error) << "FNV NPC hit reaction exact source is unavailable: " << exactSource;
            return false;
        }

        if (resolution == FonvNpcHitReactionResolution::BindExact)
        {
            const std::shared_ptr<AnimSource> source
                = addSingleAnimSource(exactSource, baseModel, false, {}, FonvNpcHitReactionSemanticGroup);
            if (source == nullptr)
            {
                Log(Debug::Error) << "FNV NPC hit reaction exact source failed controller binding: " << exactSource
                                  << " skeleton=" << baseModel;
                return false;
            }
        }

        const std::string finalSource = getAnimationSourceName(FonvNpcHitReactionSemanticGroup);
        const unsigned int controllerMask = getAnimationGroupControllerMask(FonvNpcHitReactionSemanticGroup);
        if (!isPreparedFonvNpcHitReaction(finalSource, controllerMask))
        {
            Log(Debug::Error) << "FNV NPC hit reaction failed final source validation: expected=" << exactSource
                              << " selected=" << finalSource << " controllerMask=" << controllerMask;
            return false;
        }
        return true;
    }

    bool Animation::prepareFalloutWeaponAnimation(
        std::uint8_t animationType, std::uint8_t reloadAnimation, FonvWeaponAction)
    {
        const std::vector<FonvWeaponActionSource> manifest
            = getFonvWeaponActionManifest(animationType, reloadAnimation);
        if (manifest.empty())
        {
            Log(Debug::Error) << "FNV animation has no exact action manifest: animationType="
                              << static_cast<unsigned int>(animationType)
                              << " reloadAnimation=" << static_cast<unsigned int>(reloadAnimation);
            return false;
        }

        const std::string baseModel = mPtr.getClass().getCorrectedModel(mPtr);
        bool requiredSourcesAvailable = true;
        for (const FonvWeaponActionSource& action : manifest)
        {
            if (getAnimationSourceName(action.mSemanticGroup) == action.mPath)
                continue;

            const std::shared_ptr<AnimSource> source
                = addSingleAnimSource(action.mPath, baseModel, false, {}, action.mSemanticGroup);
            if (source == nullptr && action.mRequired)
            {
                requiredSourcesAvailable = false;
                Log(Debug::Error) << "FNV animation required exact action source is unavailable: animationType="
                                  << static_cast<unsigned int>(animationType)
                                  << " group=" << action.mSemanticGroup << " path=" << action.mPath;
            }
        }
        return requiredSourcesAvailable;
    }

    std::vector<std::string> Animation::getAnimationGroups() const
    {
        std::vector<std::string> result;
        result.reserve(mSupportedAnimations.size());
        for (std::string_view group : mSupportedAnimations)
            result.emplace_back(group);
        std::sort(result.begin(), result.end());
        return result;
    }

    unsigned int Animation::getAnimationGroupControllerMask(std::string_view group) const
    {
        // Match play(): the last inserted source containing the requested group wins.
        for (AnimSourceList::const_reverse_iterator source = mAnimSources.rbegin(); source != mAnimSources.rend(); ++source)
        {
            if (!(*source)->getTextKeys().hasGroupStart(group))
                continue;

            unsigned int mask = 0;
            for (std::size_t boneGroup = 0; boneGroup < sNumBlendMasks; ++boneGroup)
            {
                if (!(*source)->mControllerMap[boneGroup].empty())
                    mask |= 1u << boneGroup;
            }
            return mask;
        }
        return 0;
    }

    bool Animation::isLoopingAnimation(std::string_view group) const
    {
        // In Morrowind, a some animation groups are always considered looping, regardless
        // of loop start/stop keys.
        // To be match vanilla behavior we probably only need to check this list, but we don't
        // want to prevent modded animations with custom group names from looping either.
        static const std::unordered_set<std::string_view> loopingAnimations = { "walkforward", "walkback", "walkleft",
            "walkright", "swimwalkforward", "swimwalkback", "swimwalkleft", "swimwalkright", "runforward", "runback",
            "runleft", "runright", "swimrunforward", "swimrunback", "swimrunleft", "swimrunright", "sneakforward",
            "sneakback", "sneakleft", "sneakright", "turnleft", "turnright", "swimturnleft", "swimturnright",
            "spellturnleft", "spellturnright", "torch", "idle", "idle2", "idle3", "idle4", "idle5", "idle6", "idle7",
            "idle8", "idle9", "idlesneak", "idlestorm", "idleswim", "jump", "inventoryhandtohand",
            "inventoryweapononehand", "inventoryweapontwohand", "inventoryweapontwowide" };
        static const std::vector<std::string_view> shortGroups = MWMechanics::getAllWeaponTypeShortGroups();

        if (getTextKeyTime(std::string(group) + ": loop start") >= 0)
            return true;

        // Most looping animations have variants for each weapon type shortgroup.
        // Just remove the shortgroup instead of enumerating all of the possible animation groupnames.
        // Make sure we pick the longest shortgroup so e.g. "bow" doesn't get picked over "crossbow"
        // when the shortgroup is crossbow.
        std::size_t suffixLength = 0;
        for (std::string_view suffix : shortGroups)
        {
            if (suffix.length() > suffixLength && group.ends_with(suffix))
            {
                suffixLength = suffix.length();
            }
        }
        group.remove_suffix(suffixLength);

        return loopingAnimations.count(group) > 0;
    }

    float Animation::getStartTime(const std::string& groupname) const
    {
        for (AnimSourceList::const_reverse_iterator iter(mAnimSources.rbegin()); iter != mAnimSources.rend(); ++iter)
        {
            const SceneUtil::TextKeyMap& keys = (*iter)->getTextKeys();

            const auto found = keys.findGroupStart(groupname);
            if (found != keys.end())
                return found->first;
        }
        return -1.f;
    }

    float Animation::getTextKeyTime(std::string_view textKey) const
    {
        for (AnimSourceList::const_reverse_iterator iter(mAnimSources.rbegin()); iter != mAnimSources.rend(); ++iter)
        {
            const SceneUtil::TextKeyMap& keys = (*iter)->getTextKeys();

            for (auto iterKey = keys.begin(); iterKey != keys.end(); ++iterKey)
            {
                if (iterKey->second.starts_with(textKey))
                    return iterKey->first;
            }
        }

        return -1.f;
    }

    void Animation::handleTextKey(AnimState& state, std::string_view groupname,
        SceneUtil::TextKeyMap::ConstIterator key, const SceneUtil::TextKeyMap& map)
    {
        std::string_view evt = key->second;

        if (evt.starts_with(groupname) && evt.substr(groupname.size()).starts_with(": "))
        {
            size_t off = groupname.size() + 2;
            if (evt.substr(off) == "loop start")
                state.mLoopStartTime = key->first;
            else if (evt.substr(off) == "loop stop")
                state.mLoopStopTime = key->first;
        }

        try
        {
            if (mTextKeyListener != nullptr)
                mTextKeyListener->handleTextKey(groupname, key, map);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Error handling text key " << evt << ": " << e.what();
        }
    }

    void Animation::play(std::string_view groupname, const AnimPriority& priority, int blendMask, bool autodisable,
        float speedmult, std::string_view start, std::string_view stop, float startpoint, uint32_t loops,
        bool loopfallback)
    {
        const bool falloutNpc = isFalloutNpcAnimationContext(mPtr);
        if (!mObjectRoot || mAnimSources.empty())
        {
            if (falloutNpc)
                Log(Debug::Warning) << "FNV/ESM4 diag: play request for " << mPtr.getCellRef().getRefId()
                                    << " group '" << groupname << "' ignored objectRoot=" << static_cast<bool>(mObjectRoot)
                                    << " animSources=" << mAnimSources.size();
            return;
        }

        if (falloutNpc)
            Log(Debug::Verbose) << "FNV/ESM4 diag: play request for " << mPtr.getCellRef().getRefId() << " group '"
                             << groupname << "' sources=" << mAnimSources.size() << " blendMask=" << blendMask
                             << " start='" << start << "' stop='" << stop << "' loops=" << loops
                             << " startpoint=" << startpoint;

        if (groupname.empty())
        {
            resetActiveGroups();
            return;
        }

        AnimStateMap::iterator foundstateiter = mStates.find(groupname);
        if (foundstateiter != mStates.end())
        {
            foundstateiter->second.mPriority = priority;
        }

        AnimStateMap::iterator stateiter = mStates.begin();
        while (stateiter != mStates.end())
        {
            if (stateiter->second.mPriority == priority && stateiter->first != groupname)
                mStates.erase(stateiter++);
            else
                ++stateiter;
        }

        if (foundstateiter != mStates.end())
        {
            if (falloutNpc)
                Log(Debug::Verbose) << "FNV/ESM4 diag: play reused active group '" << groupname << "' for "
                                 << mPtr.getCellRef().getRefId();
            resetActiveGroups();
            return;
        }

        /* Look in reverse; last-inserted source has priority. */
        AnimState state;
        AnimSourceList::reverse_iterator iter(mAnimSources.rbegin());
        size_t checkedSources = 0;
        for (; iter != mAnimSources.rend(); ++iter)
        {
            ++checkedSources;
            const SceneUtil::TextKeyMap& textkeys = (*iter)->getTextKeys();
            if (reset(state, textkeys, groupname, start, stop, startpoint, loopfallback))
            {
                state.mSource = *iter;
                state.mSpeedMult = speedmult;
                state.mLoopCount = loops;
                state.mPlaying = (state.getTime() < state.mStopTime);
                state.mPriority = priority;
                state.mBlendMask = blendMask;
                state.mAutoDisable = autodisable;
                state.mGroupname = groupname;
                state.mStartKey = start;
                if (falloutNpc && startpoint == 0.f && state.mStopTime > state.mStartTime)
                {
                    if (isFalloutSeededIdleGroup(groupname))
                    {
                        const float idleSeedSeconds = getFalloutIdleSeedSeconds(groupname);
                        if (idleSeedSeconds >= 0.f)
                        {
                            const float seededIdleTime
                                = std::min(state.mStartTime + idleSeedSeconds, state.mStopTime - 0.01f);
                            if (seededIdleTime > state.getTime())
                            {
                                Log(Debug::Verbose) << "FNV/ESM4 diag: seeding Fallout idle pose for "
                                                 << mPtr.getCellRef().getRefId() << " from=" << state.getTime()
                                                 << " to=" << seededIdleTime << " seconds=" << idleSeedSeconds;
                                state.setTime(seededIdleTime);
                                state.mPlaying = (state.getTime() < state.mStopTime);
                            }
                        }
                        else
                            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout idle seed disabled for "
                                             << mPtr.getCellRef().getRefId();
                    }
                    else if (isFalloutSeededLocomotionGroup(groupname))
                    {
                        const float locomotionSeedSeconds
                            = getFalloutLocomotionSeedSeconds(state.mStopTime - state.mStartTime);
                        const float seededLocomotionTime
                            = std::min(state.mStartTime + locomotionSeedSeconds, state.mStopTime - 0.01f);
                        if (seededLocomotionTime > state.getTime())
                        {
                            Log(Debug::Verbose) << "FNV/ESM4 diag: seeding Fallout locomotion pose for "
                                             << mPtr.getCellRef().getRefId() << " group '" << groupname
                                             << "' from=" << state.getTime() << " to=" << seededLocomotionTime
                                             << " seconds=" << locomotionSeedSeconds;
                            state.setTime(seededLocomotionTime);
                            state.mPlaying = (state.getTime() < state.mStopTime);
                        }
                    }
                }
                mStates[std::string{ groupname }] = state;

                if (falloutNpc)
                {
                    size_t controllerCount = 0;
                    for (size_t mask = 0; mask < sNumBlendMasks; ++mask)
                        if (state.blendMaskContains(mask))
                            controllerCount += state.mSource->mControllerMap[mask].size();

                    Log(Debug::Verbose) << "FNV/ESM4 diag: play matched " << mPtr.getCellRef().getRefId() << " group '"
                                     << groupname << "' source=" << state.mSource->mSourceName
                                     << " checkedSources=" << checkedSources << " controllers=" << controllerCount
                                     << " startTime=" << state.mStartTime
                                     << " loopStart=" << state.mLoopStartTime << " loopStop=" << state.mLoopStopTime
                                     << " stopTime=" << state.mStopTime << " playing=" << state.mPlaying;
                }

                if (state.mPlaying)
                {
                    auto textkey = textkeys.lowerBound(state.getTime());
                    while (textkey != textkeys.end() && textkey->first <= state.getTime())
                    {
                        handleTextKey(state, groupname, textkey, textkeys);
                        ++textkey;
                    }
                }

                if (state.getTime() >= state.mLoopStopTime && state.mLoopCount > 0)
                {
                    state.mLoopCount--;
                    state.setTime(state.mLoopStartTime);
                    state.mPlaying = true;
                    if (state.getTime() >= state.mLoopStopTime)
                        break;

                    auto textkey = textkeys.lowerBound(state.getTime());
                    while (textkey != textkeys.end() && textkey->first <= state.getTime())
                    {
                        handleTextKey(state, groupname, textkey, textkeys);
                        ++textkey;
                    }
                }

                break;
            }
        }

        if (falloutNpc && iter == mAnimSources.rend())
            Log(Debug::Warning) << "FNV/ESM4 diag: play failed to match " << mPtr.getCellRef().getRefId()
                                << " group '" << groupname << "' checkedSources=" << checkedSources
                                << " supported=" << mSupportedAnimations.count(groupname);

        resetActiveGroups();
    }

    bool Animation::reset(AnimState& state, const SceneUtil::TextKeyMap& keys, std::string_view groupname,
        std::string_view start, std::string_view stop, float startpoint, bool loopfallback)
    {
        // Look for text keys in reverse. This normally wouldn't matter, but for some reason undeadwolf_2.nif has two
        // separate walkforward keys, and the last one is supposed to be used.
        auto groupend = keys.rbegin();
        for (; groupend != keys.rend(); ++groupend)
        {
            if (groupend->second.starts_with(groupname) && groupend->second.compare(groupname.size(), 2, ": ") == 0)
                break;
        }

        auto startkey = groupend;
        while (startkey != keys.rend() && !equalsParts(startkey->second, groupname, ": ", start))
            ++startkey;
        if (startkey == keys.rend() && start == "loop start")
        {
            startkey = groupend;
            while (startkey != keys.rend() && !equalsParts(startkey->second, groupname, ": start"))
                ++startkey;
        }
        if (startkey == keys.rend())
            return false;

        auto stopkey = groupend;
        std::size_t checkLength = groupname.size() + 2 + stop.size();
        while (stopkey != keys.rend()
            // We have to ignore extra garbage at the end.
            // The Scrib's idle3 animation has "Idle3: Stop." instead of "Idle3: Stop".
            // Why, just why? :(
            && !equalsParts(std::string_view{ stopkey->second }.substr(0, checkLength), groupname, ": ", stop))
            ++stopkey;
        if (stopkey == keys.rend())
            return false;

        if (startkey->first > stopkey->first)
            return false;

        state.mStartTime = startkey->first;
        if (loopfallback)
        {
            state.mLoopStartTime = startkey->first;
            state.mLoopStopTime = stopkey->first;
        }
        else
        {
            state.mLoopStartTime = startkey->first;
            state.mLoopStopTime = std::numeric_limits<float>::max();
        }
        state.mStopTime = stopkey->first;

        state.setTime(state.mStartTime + ((state.mStopTime - state.mStartTime) * startpoint));

        // mLoopStartTime and mLoopStopTime normally get assigned when encountering these keys while playing the
        // animation (see handleTextKey). But if startpoint is already past these keys, or start time is == stop time,
        // we need to assign them now.

        auto key = groupend;
        for (; key != startkey && key != keys.rend(); ++key)
        {
            if (key->first > state.getTime())
                continue;

            if (equalsParts(key->second, groupname, ": loop start"))
                state.mLoopStartTime = key->first;
            else if (equalsParts(key->second, groupname, ": loop stop"))
                state.mLoopStopTime = key->first;
        }

        return true;
    }

    void Animation::setTextKeyListener(TextKeyListener* listener)
    {
        mTextKeyListener = listener;
    }

    const Animation::NodeMap& Animation::getNodeMap() const
    {
        if (!mNodeMapCreated && mObjectRoot)
        {
            // If the base of this animation is an osgAnimation, we should map the bones not matrix transforms
            if (mRequiresBoneMap)
            {
                SceneUtil::NodeMapVisitorBoneOnly visitor(mNodeMap);
                mObjectRoot->accept(visitor);
                if (isFalloutActor(mPtr))
                {
                    SceneUtil::NodeMapVisitor helperVisitor(mNodeMap);
                    mObjectRoot->accept(helperVisitor);
                }
            }
            else
            {
                SceneUtil::NodeMapVisitor visitor(mNodeMap);
                mObjectRoot->accept(visitor);
            }
            mNodeMapCreated = true;
        }
        return mNodeMap;
    }

//## VR_PATCH BEGIN
// VR needs some bones to just do nothing.
    static bool vrOverride(const std::string& groupname, const std::string& bone)
    {
        if (VR::getKBMouseModeActive() || !VR::getVR())
            return false;

        // TODO: It's difficult to design a good override system when
        // I don't have a good understanding of the animation code. So for
        // now i just hardcode blocking of updaters for nodes that should not be animated in VR.
        // Add any bone+groupname pair that is messing with Vr comfort here.
        using Overrides = std::set<std::string>;
        using GroupOverrides = std::map<std::string, Overrides>;
        static GroupOverrides sVrOverrides = {
            { "crossbow", { "weapon bone" } },
            { "throwweapon", { "weapon bone" } },
            { "bowandarrow", { "weapon bone" } },
        };

        bool override = false;
        auto find = sVrOverrides.find(groupname);
        if (find != sVrOverrides.end())
        {
            override = !!find->second.count(bone);
        }

        return override;
    }

//## VR_PATCH END
    template <typename ControllerType>
    inline osg::Callback* Animation::handleBlendTransform(const osg::ref_ptr<osg::Node>& node,
        osg::ref_ptr<SceneUtil::KeyframeController> keyframeController,
        std::map<osg::ref_ptr<osg::Node>, osg::ref_ptr<ControllerType>>& blendControllers,
        const AnimBlendStateData& stateData, const osg::ref_ptr<const SceneUtil::AnimBlendRules>& blendRules,
        const AnimState& active)
    {
        osg::ref_ptr<ControllerType> animController;
        if (blendControllers.contains(node))
        {
            animController = blendControllers.at(node);
            animController->setKeyframeTrack(keyframeController, stateData, blendRules);
        }
        else
        {
            animController = new ControllerType(keyframeController, stateData, blendRules);
            blendControllers.emplace(node, animController);

            if constexpr (std::is_same_v<ControllerType, BoneAnimBlendController>)
                assignBoneBlendCallbackRecursive(animController, node, true);
        }

        keyframeController->mTime = active.mTime;

        osg::Callback* asCallback = animController->getAsCallback();
        if constexpr (std::is_same_v<ControllerType, BoneAnimBlendController>)
        {
            // IMPORTANT: we must gather all transforms at point of change before next update
            // instead of at the root update callback because the root bone may require blending.
            if (animController->getBlendTrigger())
                animController->gatherRecursiveBoneTransforms(static_cast<osgAnimation::Bone*>(node.get()));

            // Register blend callback after the initial animation callback
            node->addUpdateCallback(asCallback);
            mActiveControllers.emplace_back(node, asCallback);

            return keyframeController->getAsCallback();
        }
        else
            return asCallback;
    }

    std::string Animation::describeActiveFalloutAnimationStates() const
    {
        std::ostringstream activeGroups;
        for (size_t blendMask = 0; blendMask < sNumBlendMasks; ++blendMask)
        {
            AnimStateMap::const_iterator active = mStates.end();
            for (AnimStateMap::const_iterator state = mStates.begin(); state != mStates.end(); ++state)
            {
                if (!state->second.blendMaskContains(blendMask))
                    continue;

                if (active == mStates.end()
                    || active->second.mPriority[(BoneGroup)blendMask] < state->second.mPriority[(BoneGroup)blendMask])
                    active = state;
            }

            if (active == mStates.end())
                continue;

            if (activeGroups.tellp() > 0)
                activeGroups << " | ";

            const size_t controllerCount = active->second.mSource
                ? active->second.mSource->mControllerMap[blendMask].size()
                : 0;
            activeGroups << blendMask << ":" << active->second.mGroupname << "@t=" << active->second.getTime()
                         << " controllers=" << controllerCount;
            if (active->second.mSource)
                activeGroups << " src=" << active->second.mSource->mSourceName;
        }

        if (activeGroups.tellp() == 0)
            return "none";
        return activeGroups.str();
    }

    size_t Animation::forceFalloutNativeUpdateTraversalOnce(std::string_view reason)
    {
        if (!mObjectRoot)
            return 0;

        float sampleTime = 0.f;
        for (size_t blendMask = 0; blendMask < sNumBlendMasks; ++blendMask)
        {
            AnimStateMap::const_iterator active = mStates.end();
            for (AnimStateMap::const_iterator state = mStates.begin(); state != mStates.end(); ++state)
            {
                if (!state->second.blendMaskContains(blendMask))
                    continue;

                if (active == mStates.end()
                    || active->second.mPriority[(BoneGroup)blendMask] < state->second.mPriority[(BoneGroup)blendMask])
                    active = state;
            }

            if (active != mStates.end())
                sampleTime = std::max(sampleTime, active->second.getTime());
        }

        if (mSkeleton)
            mSkeleton->markBoneMatriceDirty();

        static unsigned int sFalloutForcedTraversalNumber = 1;
        const unsigned int traversalNumber = ++sFalloutForcedTraversalNumber;

        osg::ref_ptr<osg::FrameStamp> frameStamp = new osg::FrameStamp;
        frameStamp->setFrameNumber(traversalNumber);
        frameStamp->setReferenceTime(sampleTime);
        frameStamp->setSimulationTime(sampleTime);

        osgUtil::UpdateVisitor updateVisitor;
        updateVisitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        updateVisitor.setTraversalNumber(traversalNumber);
        updateVisitor.setFrameStamp(frameStamp.get());
        mObjectRoot->accept(updateVisitor);

        static std::unordered_map<std::string, unsigned int> sFalloutForcedTraversalLogs;
        const std::string refId = mPtr.getCellRef().getRefId().serializeText();
        std::string logKey = refId;
        logKey += ":";
        logKey += std::string(reason);
        unsigned int& logs = sFalloutForcedTraversalLogs[logKey];
        if (logs < 8)
        {
            ++logs;
            Log(Debug::Verbose) << "FNV/ESM4 diag: forced native update traversal for "
                             << mPtr.getCellRef().getRefId()
                             << " reason=" << reason
                             << " traversalNumber=" << traversalNumber
                             << " sampleTime=" << sampleTime
                             << " activeGroups=[" << describeActiveFalloutAnimationStates() << "]"
                             << " activeControllers=" << mActiveControllers.size();
        }

        return mActiveControllers.size();
    }

    int Animation::getBethesdaBoneLodLevel() const
    {
        if (!isFalloutNpcAnimationContext(mPtr))
            return 0;

        if (const char* forced = std::getenv("OPENMW_ESM4_BONE_LOD_FORCE"))
        {
            char* end = nullptr;
            const long value = std::strtol(forced, &end, 10);
            if (end != forced && *end == '\0' && value >= 0 && value <= 8)
                return static_cast<int>(value);
        }

        const MWWorld::Ptr player = MWMechanics::getPlayer();
        if (mPtr == player
            || Misc::StringUtils::ciEqual(mPtr.getCellRef().getRefId().serializeText(), "player"))
            return 0;

        // FalloutNV 1.4.0.525 HighProcess computes:
        // floor((cameraDistance / actorScale) * 12 * cameraLodAdjust
        //       / (iBoneLODDistMult * actorFadeMultiplier)).
        // The retail defaults 1000 and 15 produce the observed 1250-unit step
        // at actorScale=1 and cameraLodAdjust=1.
        float distanceMultiplier = 1000.f;
        float actorFadeMultiplier = 15.f;
        float distanceConstant = 12.f;
        float forcedLodDistance = 0.f;
        const char* configured = std::getenv("OPENMW_ESM4_BONE_LOD_DISTANCE");
        if (configured == nullptr)
            configured = std::getenv("OPENMW_ESM4_BONE_LOD_NEAR_DISTANCE");
        if (configured != nullptr)
        {
            char* end = nullptr;
            const float value = std::strtof(configured, &end);
            if (end != configured && *end == '\0' && std::isfinite(value) && value > 0.f)
                forcedLodDistance = value;
        }

        osg::Vec3d lodOrigin(player.getRefData().getPosition().asVec3());
        float cameraLodAdjust = 1.f;
        if (MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getCamera())
        {
            lodOrigin = camera->getPosition();
            cameraLodAdjust = camera->getLodScale();
        }
        const osg::Vec3d delta = osg::Vec3d(mPtr.getRefData().getPosition().asVec3()) - lodOrigin;
        const float distance = static_cast<float>(std::sqrt(delta.length2()));
        if (forcedLodDistance > 0.f)
            return std::clamp(static_cast<int>(distance / forcedLodDistance), 0, 8);

        const float actorScale = std::max(std::abs(mPtr.getCellRef().getScale()), 0.0001f);
        if (!std::isfinite(cameraLodAdjust) || cameraLodAdjust <= 0.f)
            cameraLodAdjust = 1.f;
        const float quotient = (distance / actorScale) * distanceConstant * cameraLodAdjust
            / (distanceMultiplier * actorFadeMultiplier);
        return std::clamp(static_cast<int>(std::floor(quotient)), 0, 8);
    }

    bool Animation::isBethesdaBoneLodSuppressed(const osg::Node* node) const
    {
        if (node == nullptr || mBethesdaBoneLodLevel <= 0)
            return false;
        unsigned int group = 0;
        return node->getUserValue("bethesdaBoneLodGroup", group)
            && group < static_cast<unsigned int>(mBethesdaBoneLodLevel);
    }

    bool Animation::shouldDeferBethesdaBoneLodChange() const
    {
        if (!mPlayScriptedOnly)
            return false;
        return std::any_of(mStates.begin(), mStates.end(), [](const auto& state) {
            return state.second.mPlaying
                && state.second.mPriority.contains(MWMechanics::Priority_Scripted);
        });
    }

    void Animation::detachActiveControllers()
    {
        // State overrides are stacked in callback attachment order; unwind them LIFO
        // so an aliased target cannot restore another external controller's snapshot.
        for (auto active = mActiveControllers.rbegin(); active != mActiveControllers.rend(); ++active)
        {
            const auto& [node, callback] = *active;
            if (node == nullptr || callback == nullptr)
                continue;
            if (auto* controller = dynamic_cast<NifOsg::KeyframeController*>(callback.get());
                controller != nullptr && controller->hasPropertyChannels())
            {
                controller->restorePropertyState();
            }
            node->removeUpdateCallback(callback);
            callback->setNestedCallback(nullptr);
        }
        mActiveControllers.clear();
    }

    void Animation::resetActiveGroups()
    {
//## VR_PATCH BEGIN
        const bool isPlayer = (mPtr == MWMechanics::getPlayer());

//## VR_PATCH END
        // REC_NPC_4 is shared by Oblivion and the later Bethesda formats.  The
        // accumulation reset, bone-LOD handling, native callback traversal, and
        // direct (non-smoothed) controller path below reproduce FO3/FNV runtime
        // semantics; applying them to TES4 corrupts Oblivion skeleton poses.
        const bool falloutNpc = isFalloutNpcAnimationContext(mPtr);
        size_t falloutAddedControllers = 0;
        size_t falloutBoneLodSuppressedControllers = 0;
        bool accumResetAttached = false;
        const int requestedBoneLodLevel = falloutNpc ? getBethesdaBoneLodLevel() : 0;
        if (mBethesdaBoneLodLevel < 0 || !shouldDeferBethesdaBoneLodChange())
            mBethesdaBoneLodLevel = requestedBoneLodLevel;
        // Remove all previous external controllers from the scene graph and restore
        // any StateSet that an external KF property track temporarily replaced.
        detachActiveControllers();

        mAccumCtrl = nullptr;
        if (mObjectRoot)
            mObjectRoot->setDataVariance(osg::Object::DYNAMIC);

        for (size_t blendMask = 0; blendMask < sNumBlendMasks; blendMask++)
        {
            AnimStateMap::const_iterator active = mStates.end();

            AnimStateMap::const_iterator state = mStates.begin();
            for (; state != mStates.end(); ++state)
            {
                if (!state->second.blendMaskContains(blendMask))
                    continue;

                if (active == mStates.end()
                    || active->second.mPriority[(BoneGroup)blendMask] < state->second.mPriority[(BoneGroup)blendMask])
                    active = state;
            }

            mAnimationTimePtr[blendMask]->setTimePtr(
                active == mStates.end() ? std::shared_ptr<float>() : active->second.mTime);

            // add external controllers for the AnimSource active in this blend mask
            if (active != mStates.end())
            {
                std::shared_ptr<AnimSource> animsrc = active->second.mSource;
                const AnimBlendStateData stateData
                    = { .mGroupname = active->second.mGroupname, .mStartKey = active->second.mStartKey };

                for (AnimSource::ControllerMap::iterator it = animsrc->mControllerMap[blendMask].begin();
                     it != animsrc->mControllerMap[blendMask].end(); ++it)
                {
                    osg::ref_ptr<osg::Node> node = getNodeMap().at(
                        it->first); // this should not throw, we already checked for the node existing in addAnimSource

                    if (falloutNpc && isBethesdaBoneLodSuppressed(node))
                    {
                        ++falloutBoneLodSuppressedControllers;
                        continue;
                    }

                    const bool useSmoothAnims = !falloutNpc && useSmoothAnimationTransitions();

                    osg::Callback* callback = it->second->getAsCallback();
                    auto* nifKeyframeController = dynamic_cast<NifOsg::KeyframeController*>(it->second.get());
                    const bool propertyController
                        = nifKeyframeController != nullptr && nifKeyframeController->hasPropertyChannels();
                    const bool transformController
                        = nifKeyframeController == nullptr || nifKeyframeController->hasTransformChannels();
                    // A compound Fallout KF controller owns its transform and render-property channels atomically.
                    // Feeding it into a transform-only blender would discard material/UV animation.
                    if (useSmoothAnims && !propertyController)
                    {
                        if (dynamic_cast<NifOsg::MatrixTransform*>(node.get()))
                        {
                            callback = handleBlendTransform<NifAnimBlendController>(node, it->second,
                                mAnimBlendControllers, stateData, animsrc->mAnimBlendRules, active->second);
                        }
                        else if (dynamic_cast<osgAnimation::Bone*>(node.get()))
                        {
                            callback = handleBlendTransform<BoneAnimBlendController>(node, it->second,
                                mBoneAnimBlendControllers, stateData, animsrc->mAnimBlendRules, active->second);
                        }
                    }
//## VR_PATCH BEGIN
                    // Some bones need to be still and do nothing in VR
                    // I'm SURE we'll TOTALLY make a cleaner solution for this before the end of 2090
                    node->setDataVariance(osg::Object::DYNAMIC);
                    const bool addSceneGraphCallback
                        = (propertyController || !falloutNpc || shouldUseNativeFalloutAnimationCallbacks())
                        && (!isPlayer || !vrOverride(active->first, it->first));
                    if (addSceneGraphCallback)
//## VR_PATCH END
                    {
                        node->addUpdateCallback(callback);
                        mActiveControllers.emplace_back(node, callback);
                        if (falloutNpc)
                            ++falloutAddedControllers;
                    }

                    if (transformController && blendMask == 0 && node == mAccumRoot)
                    {
                        mAccumCtrl = it->second;

                        // Bethesda locomotion stores actor displacement on the accumulation root. Apply that
                        // displacement to gameplay movement, then clear its accumulated axes from the rendered
                        // skeleton just like the native engine. Leaving the callback off for Fallout actors makes
                        // the complete body surge away from and back to its physics reference every animation cycle.
                        if (!mResetAccumRootCallback)
                        {
                            mResetAccumRootCallback = new ResetAccumRootCallback;
                            mResetAccumRootCallback->setAccumulate(mAccumulate);
                        }
                        mResetAccumRootCallback->setResetAllTranslation(falloutNpc);
                        // Keep the reset last in the callback chain so it sees the sampled controller value.
                        mAccumRoot->addUpdateCallback(mResetAccumRootCallback);
                        mActiveControllers.emplace_back(mAccumRoot, mResetAccumRootCallback);
                        accumResetAttached = true;
                    }
                }
            }
        }

        if (falloutNpc && mAccumRoot != nullptr && !accumResetAttached)
        {
            // Retail FO3/FNV keeps Bip01 neutral even when the active sequence does not contain a Bip01 controller
            // (notably mtidle.kf).  The skeleton NIF's authored Bip01 bind translation is not the runtime pose; if it
            // leaks through, idle is one root-height above locomotion and the whole actor drops when walking starts.
            if (!mResetAccumRootCallback)
            {
                mResetAccumRootCallback = new ResetAccumRootCallback;
                mResetAccumRootCallback->setAccumulate(mAccumulate);
            }
            mResetAccumRootCallback->setResetAllTranslation(true);
            mAccumRoot->addUpdateCallback(mResetAccumRootCallback);
            mActiveControllers.emplace_back(mAccumRoot, mResetAccumRootCallback);
        }

        if (falloutNpc)
            Log(Debug::Verbose) << "FNV/ESM4 diag: active animation group reset for " << mPtr.getCellRef().getRefId()
                             << " states=" << mStates.size() << " callbacks=" << falloutAddedControllers
                             << " boneLod=" << mBethesdaBoneLodLevel
                             << " boneLodSuppressed=" << falloutBoneLodSuppressedControllers
                             << " activeControllers=" << mActiveControllers.size()
                             << " activeGroups=[" << describeActiveFalloutAnimationStates() << "]";

        addControllers();

        if (falloutNpc && shouldUseNativeFalloutAnimationCallbacks())
            forceFalloutNativeUpdateTraversalOnce("resetActiveGroups");
    }

    void Animation::adjustSpeedMult(const std::string& groupname, float speedmult)
    {
        AnimStateMap::iterator state(mStates.find(groupname));
        if (state != mStates.end())
            state->second.mSpeedMult = speedmult;
    }

    bool Animation::isPlaying(std::string_view groupname) const
    {
        AnimStateMap::const_iterator state(mStates.find(groupname));
        if (state != mStates.end())
            return state->second.mPlaying;
        return false;
    }

    bool Animation::getInfo(std::string_view groupname, float* complete, float* speedmult, size_t* loopcount) const
    {
        AnimStateMap::const_iterator iter = mStates.find(groupname);
        if (iter == mStates.end())
        {
            if (complete)
                *complete = 0.0f;
            if (speedmult)
                *speedmult = 0.0f;
            if (loopcount)
                *loopcount = 0;
            return false;
        }

        if (complete)
        {
            if (iter->second.mStopTime > iter->second.mStartTime)
                *complete = (iter->second.getTime() - iter->second.mStartTime)
                    / (iter->second.mStopTime - iter->second.mStartTime);
            else
                *complete = (iter->second.mPlaying ? 0.0f : 1.0f);
        }
        if (speedmult)
            *speedmult = iter->second.mSpeedMult;

        if (loopcount)
            *loopcount = iter->second.mLoopCount;
        return true;
    }

    std::string_view Animation::getActiveGroup(BoneGroup boneGroup) const
    {
        if (auto timePtr = mAnimationTimePtr[boneGroup]->getTimePtr())
            for (auto& state : mStates)
                if (state.second.mTime == timePtr)
                    return state.first;
        return "";
    }

    float Animation::getCurrentTime(std::string_view groupname) const
    {
        AnimStateMap::const_iterator iter = mStates.find(groupname);
        if (iter == mStates.end())
            return -1.f;

        return iter->second.getTime();
    }

    void Animation::disable(std::string_view groupname)
    {
        AnimStateMap::iterator iter = mStates.find(groupname);
        if (iter != mStates.end())
            mStates.erase(iter);
        resetActiveGroups();
    }

    float Animation::getFalloutHeadAnimTrackValue(std::string_view trackName) const
    {
        for (const auto& [groupName, state] : mStates)
        {
            if (!state.mPlaying)
                continue;

            AnimSourceList::const_reverse_iterator animsrc(mAnimSources.rbegin());
            for (; animsrc != mAnimSources.rend(); ++animsrc)
            {
                if ((*animsrc)->getTextKeys().getGroups().find(groupName) != (*animsrc)->getTextKeys().getGroups().end())
                {
                    const auto found = (*animsrc)->mKeyframes->mFalloutHeadAnimTracks.find(std::string(trackName));
                    if (found != (*animsrc)->mKeyframes->mFalloutHeadAnimTracks.end())
                    {
                        const auto& track = found->second;
                        if (track.mType == SceneUtil::KeyframeHolder::FalloutHeadAnimTrack::Type::Float)
                        {
                            if (track.mKeys.empty())
                                return track.mDefaultValue;

                            float time = *state.mTime;
                            if (time <= track.mKeys.front().first)
                                return track.mKeys.front().second;
                            if (time >= track.mKeys.back().first)
                                return track.mKeys.back().second;

                            auto it = std::lower_bound(track.mKeys.begin(), track.mKeys.end(), time,
                                [](const std::pair<float, float>& a, float b) {
                                    return a.first < b;
                                });

                            if (it == track.mKeys.begin())
                                return it->second;

                            auto prev = it - 1;
                            float denom = it->first - prev->first;
                            if (denom <= 0.000001f)
                                return it->second;
                            float t = (time - prev->first) / denom;
                            return prev->second + t * (it->second - prev->second);
                        }
                    }
                    break;
                }
            }
        }
        return 0.0f;
    }

    float Animation::getVelocity(std::string_view groupname) const
    {
        if (!mAccumRoot)
            return 0.0f;

        std::map<std::string, float>::const_iterator found = mAnimVelocities.find(groupname);
        if (found != mAnimVelocities.end())
            return found->second;

        // Look in reverse; last-inserted source has priority.
        AnimSourceList::const_reverse_iterator animsrc(mAnimSources.rbegin());
        for (; animsrc != mAnimSources.rend(); ++animsrc)
        {
            const SceneUtil::TextKeyMap& keys = (*animsrc)->getTextKeys();
            if (keys.hasGroupStart(groupname))
                break;
        }
        if (animsrc == mAnimSources.rend())
            return 0.0f;

        float velocity = 0.0f;
        const SceneUtil::TextKeyMap& keys = (*animsrc)->getTextKeys();

        const AnimSource::ControllerMap& ctrls = (*animsrc)->mControllerMap[0];
        for (AnimSource::ControllerMap::const_iterator it = ctrls.begin(); it != ctrls.end(); ++it)
        {
            const auto* controller = dynamic_cast<const NifOsg::KeyframeController*>(it->second.get());
            if (controller != nullptr && !controller->hasTransformChannels())
                continue;
            if (Misc::StringUtils::ciEqual(it->first, mAccumRoot->getName()))
            {
                velocity = calcAnimVelocity(keys, it->second, mAccumulate, groupname);
                break;
            }
        }

        // If there's no velocity, keep looking
        if (!(velocity > 1.0f))
        {
            AnimSourceList::const_reverse_iterator animiter = mAnimSources.rbegin();
            while (*animiter != *animsrc)
                ++animiter;

            while (!(velocity > 1.0f) && ++animiter != mAnimSources.rend())
            {
                const SceneUtil::TextKeyMap& keys2 = (*animiter)->getTextKeys();

                const AnimSource::ControllerMap& ctrls2 = (*animiter)->mControllerMap[0];
                for (AnimSource::ControllerMap::const_iterator it = ctrls2.begin(); it != ctrls2.end(); ++it)
                {
                    const auto* controller = dynamic_cast<const NifOsg::KeyframeController*>(it->second.get());
                    if (controller != nullptr && !controller->hasTransformChannels())
                        continue;
                    if (Misc::StringUtils::ciEqual(it->first, mAccumRoot->getName()))
                    {
                        velocity = calcAnimVelocity(keys2, it->second, mAccumulate, groupname);
                        break;
                    }
                }
            }
        }

        mAnimVelocities.emplace(groupname, velocity);

        return velocity;
    }

    void Animation::updatePosition(float oldtime, float newtime, osg::Vec3f& position)
    {
        // Get the difference from the last update, and move the position
        osg::Vec3f off = osg::componentMultiply(mAccumCtrl->getTranslation(newtime), mAccumulate);
        position += off - osg::componentMultiply(mAccumCtrl->getTranslation(oldtime), mAccumulate);
    }

    osg::Vec3f Animation::runAnimation(float duration)
    {
        osg::Vec3f movement(0.f, 0.f, 0.f);
        const bool esm4Npc = isFalloutNpc(mPtr);
        const bool falloutNpc = isFalloutNpcAnimationContext(mPtr);
        const bool falloutActor = falloutNpc || isFalloutCreature(mPtr);
        if (falloutNpc)
        {
            const int boneLodLevel = getBethesdaBoneLodLevel();
            if (boneLodLevel != mBethesdaBoneLodLevel && !shouldDeferBethesdaBoneLodChange())
            {
                mBethesdaBoneLodLevel = boneLodLevel;
                resetActiveGroups();
            }
        }
        AnimStateMap::iterator stateiter = mStates.begin();
        while (stateiter != mStates.end())
        {
            AnimState& state = stateiter->second;
            const float previousStateTime = state.getTime();
            if (mPlayScriptedOnly && !state.mPriority.contains(MWMechanics::Priority_Scripted))
            {
                ++stateiter;
                continue;
            }

            const SceneUtil::TextKeyMap& textkeys = state.mSource->getTextKeys();
            auto textkey = textkeys.upperBound(state.getTime());

            float timepassed = duration * state.mSpeedMult;
            if (falloutActor && isFalloutSeededIdleGroup(state.mGroupname) && shouldFreezeFalloutIdleAnimation())
                timepassed = 0.f;
            while (state.mPlaying)
            {
                if (!state.shouldLoop())
                {
                    float targetTime = state.getTime() + timepassed;
                    if (textkey == textkeys.end() || textkey->first > targetTime)
                    {
                        if (mAccumCtrl && state.mTime == mAnimationTimePtr[0]->getTimePtr())
                            updatePosition(state.getTime(), targetTime, movement);
                        state.setTime(std::min(targetTime, state.mStopTime));
                    }
                    else
                    {
                        if (mAccumCtrl && state.mTime == mAnimationTimePtr[0]->getTimePtr())
                            updatePosition(state.getTime(), textkey->first, movement);
                        state.setTime(textkey->first);
                    }

                    state.mPlaying = (state.getTime() < state.mStopTime);
                    timepassed = targetTime - state.getTime();

                    while (textkey != textkeys.end() && textkey->first <= state.getTime())
                    {
                        handleTextKey(state, stateiter->first, textkey, textkeys);
                        ++textkey;
                    }
                }
                if (state.shouldLoop())
                {
                    state.mLoopCount--;
                    state.setTime(state.mLoopStartTime);
                    state.mPlaying = true;

                    textkey = textkeys.lowerBound(state.getTime());
                    while (textkey != textkeys.end() && textkey->first <= state.getTime())
                    {
                        handleTextKey(state, stateiter->first, textkey, textkeys);
                        ++textkey;
                    }

                    if (state.getTime() >= state.mLoopStopTime)
                        break;
                }

                if (timepassed <= 0.0f)
                    break;
            }

            if (!state.mPlaying && state.mAutoDisable)
            {
                mStates.erase(stateiter++);

                resetActiveGroups();
            }
            else
            {
                if (falloutActor && state.mGroupname == "idle" && duration > 0.f
                    && state.getTime() != previousStateTime)
                {
                    static std::unordered_map<std::string, unsigned int> sFalloutIdleAdvanceLogs;
                    const std::string refId = mPtr.getCellRef().getRefId().serializeText();
                    unsigned int& logs = sFalloutIdleAdvanceLogs[refId];
                    if (logs < 3)
                    {
                        ++logs;
                        Log(Debug::Verbose) << "FNV/ESM4 diag: idle time advanced for " << mPtr.getCellRef().getRefId()
                                         << " duration=" << duration << " from=" << previousStateTime
                                         << " to=" << state.getTime();
                    }
                }
                ++stateiter;
            }
        }

        if (falloutNpc && !shouldUseNativeFalloutAnimationCallbacks())
        {
            std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> duplicateTransformTargets;
            std::unordered_set<osg::MatrixTransform*> riggedPartTransforms;
            if (mObjectRoot != nullptr)
            {
                FalloutRiggedPartTransformVisitor riggedPartVisitor(riggedPartTransforms);
                mObjectRoot->accept(riggedPartVisitor);
                FalloutTransformTargetVisitor targetVisitor(duplicateTransformTargets);
                mObjectRoot->accept(targetVisitor);
            }

            size_t appliedControllers = 0;
            size_t mirroredDuplicateTransforms = 0;
            size_t mirroredDuplicatePoseOnlyTransforms = 0;
            size_t mirroredRigBoneTransforms = 0;
            size_t matchedRigBoneTransforms = 0;
            size_t missingRigBoneTransforms = 0;
            size_t skippedRiggedDuplicateTransforms = 0;
            size_t skippedHelperControllers = 0;
            float maxAppliedMatrixDelta = 0.f;
            std::string maxAppliedMatrixDeltaBone;
            float maxMirroredDuplicateMatrixDelta = 0.f;
            std::string maxMirroredDuplicateMatrixDeltaBone;
            float maxVisibleMatrixDelta = 0.f;
            std::string maxVisibleMatrixDeltaBone;
            size_t visibleMatrixDeltaCount = 0;
            size_t skippedBoneTranslations = 0;
            size_t appliedBoneTranslations = 0;
            float maxBoneTranslationDelta = 0.f;
            std::string maxBoneTranslationDeltaBone;
            float maxArmMatrixDelta = 0.f;
            std::string maxArmMatrixDeltaBone;
            float maxTorsoMatrixDelta = 0.f;
            std::string maxTorsoMatrixDeltaBone;
            float maxNativeLocalMatrixDelta = 0.f;
            std::string maxNativeLocalMatrixDeltaBone;
            float maxNativeWorldMatrixDelta = 0.f;
            std::string maxNativeWorldMatrixDeltaBone;
            const std::string refIdText = mPtr.getCellRef().getRefId().serializeText();
            const bool actorBasisAudit = std::getenv("OPENMW_FNV_ACTOR_BASIS_AUDIT") != nullptr;
            const bool matrixAuditRequested = std::getenv("OPENMW_FNV_MATRIX_AUDIT") != nullptr
                || std::getenv("OPENMW_FNV_PART_MATRIX_AUDIT") != nullptr;
            bool matrixAuditTarget
                = refIdText.find("4104c7f") != std::string::npos || refIdText == "player";
            if (const char* requestedRef = std::getenv("OPENMW_FNV_MATRIX_AUDIT_TARGET_REF"))
                matrixAuditTarget = Misc::StringUtils::ciEqual(refIdText, requestedRef);
            const bool matrixAudit = actorBasisAudit || (matrixAuditRequested && matrixAuditTarget);
            unsigned int matrixAuditLines = 0;
            constexpr unsigned int matrixAuditLineLimit = 96;
            for (size_t blendMask = 0; blendMask < sNumBlendMasks; ++blendMask)
            {
                AnimStateMap::const_iterator active = mStates.end();
                for (AnimStateMap::const_iterator state = mStates.begin(); state != mStates.end(); ++state)
                {
                    if (!state->second.blendMaskContains(blendMask))
                        continue;

                    if (active == mStates.end()
                        || active->second.mPriority[(BoneGroup)blendMask] < state->second.mPriority[(BoneGroup)blendMask])
                        active = state;
                }

                if (active == mStates.end())
                    continue;

                std::shared_ptr<AnimSource> animsrc = active->second.mSource;
                const bool falloutProcedureIdle = animsrc && animsrc->mFalloutProcedureIdle;
                for (AnimSource::ControllerMap::const_iterator it = animsrc->mControllerMap[blendMask].begin();
                     it != animsrc->mControllerMap[blendMask].end(); ++it)
                {
                    const auto* nifController = dynamic_cast<const NifOsg::KeyframeController*>(it->second.get());
                    // Property-bearing compound controllers stay on the native callback path even when the legacy
                    // manual Fallout transform path is selected, so their StateSet is composed exactly once.
                    if (nifController != nullptr && nifController->hasPropertyChannels())
                        continue;
                    auto nodeIt = getNodeMap().find(it->first);
                    if (nodeIt == getNodeMap().end())
                        continue;

                    osg::MatrixTransform* transform = nodeIt->second.get();
                    if (transform == nullptr)
                        continue;
                    if (isBethesdaBoneLodSuppressed(transform))
                        continue;

                    const std::string lowerAppliedBone = Misc::StringUtils::lowerCase(it->first);
                    const osg::Matrixf before = transform->getMatrix();
                    SceneUtil::KeyframeController::KfTransform keyframe = it->second->getCurrentTransformation(nullptr);
                    if (actorBasisAudit && matrixAuditLines < matrixAuditLineLimit
                        && isFalloutMatrixAuditBone(lowerAppliedBone))
                    {
                        if (auto* nifController = dynamic_cast<NifOsg::KeyframeController*>(it->second.get()))
                        {
                            const SceneUtil::KeyframeController::KfTransform rawKeyframe
                                = nifController->getCurrentTransformationWithoutFalloutActorBasis(nullptr);
                            float rotationDeltaDegrees = 0.f;
                            if (keyframe.mRotation && rawKeyframe.mRotation)
                                rotationDeltaDegrees = quatAngleDeltaDegrees(*keyframe.mRotation, *rawKeyframe.mRotation);
                            float translationDelta = 0.f;
                            if (keyframe.mTranslation && rawKeyframe.mTranslation)
                                translationDelta = (*keyframe.mTranslation - *rawKeyframe.mTranslation).length();

                            Log(Debug::Info) << "FNV/ESM4 ACTOR BASIS AUDIT " << mPtr.getCellRef().getRefId()
                                             << " source=" << (animsrc ? animsrc->mSourceName : std::string())
                                             << " blendMask=" << blendMask
                                             << " bone=" << it->first
                                             << " appliedHasRotation=" << static_cast<bool>(keyframe.mRotation)
                                             << " rawHasRotation=" << static_cast<bool>(rawKeyframe.mRotation)
                                             << " rotationDeltaDegrees=" << rotationDeltaDegrees
                                             << " appliedHasTranslation=" << static_cast<bool>(keyframe.mTranslation)
                                             << " rawHasTranslation=" << static_cast<bool>(rawKeyframe.mTranslation)
                                             << " translationDelta=" << translationDelta;
                            ++matrixAuditLines;
                        }
                    }
                    if (std::getenv("OPENMW_FNV_ENABLE_CHAIR_LOWER_LEG_DONOR") != nullptr
                        && falloutProcedureIdle && blendMask == BoneGroup_LowerBody && animsrc
                        && isFalloutCalfFootBone(lowerAppliedBone)
                        && (isFalloutDynamicChairSitSource(animsrc->mSourceName)
                            || isFalloutDynamicSitSource(animsrc->mSourceName)))
                    {
                        const bool activeChairSit = isFalloutDynamicChairSitSource(animsrc->mSourceName);
                        for (AnimSourceList::const_iterator source = mAnimSources.begin(); source != mAnimSources.end();
                             ++source)
                        {
                            if (!*source)
                                continue;
                            const bool donorMatches = activeChairSit ? isFalloutDynamicSitSource((*source)->mSourceName)
                                                                      : isFalloutDynamicChairSitSource((*source)->mSourceName);
                            if (!donorMatches)
                                continue;

                            const auto& donorControllers = (*source)->mControllerMap[blendMask];
                            const auto donor = donorControllers.find(it->first);
                            if (donor == donorControllers.end())
                                continue;

                            keyframe = donor->second->getCurrentTransformation(nullptr);
                            if (matrixAudit && matrixAuditLines < matrixAuditLineLimit)
                            {
                                Log(Debug::Info) << "FNV/ESM4 MATRIX AUDIT " << mPtr.getCellRef().getRefId()
                                                 << " chairLowerLegDonor=" << (*source)->mSourceName
                                                 << " activeSource=" << animsrc->mSourceName
                                                 << " bone=" << it->first;
                                ++matrixAuditLines;
                            }
                            break;
                        }
                    }
                    const bool accumulationBone = isFalloutAccumulationBone(lowerAppliedBone);
                    const bool allowAccumulationTranslation = !accumulationBone || falloutProcedureIdle;
                    const bool applyBoneTranslation = (falloutProcedureIdle ? shouldApplyFalloutProcedureBoneTranslations()
                                                                            : shouldApplyFalloutBoneTranslations())
                        && allowAccumulationTranslation
                        && keyframe.mTranslation
                        && isSafeFalloutBoneTranslation(*keyframe.mTranslation, before.getTrans());
                    if (accumulationBone && !falloutProcedureIdle && !shouldApplyFalloutAccumulationRotation())
                        keyframe.mRotation.reset();
                    if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(transform))
                    {
                        osg::Matrixf candidate = before;
                        if (keyframe.mRotation)
                        {
                            osg::Quat rotation = composeFalloutBindRelativeRotation(
                                nifTransform, *keyframe.mRotation, lowerAppliedBone, falloutProcedureIdle);
                            if (normalizeFiniteQuat(rotation))
                            {
                                candidate = osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(before.getTrans());
                                if (matrixAudit && matrixAuditLines < matrixAuditLineLimit
                                    && isFalloutMatrixAuditBone(lowerAppliedBone))
                                {
                                    const osg::Quat rawKey = *keyframe.mRotation;
                                    const osg::Quat bind = getFalloutBindRotation(transform);
                                    if (std::getenv("OPENMW_FNV_PROCEDURE_MATRIX_AUDIT") != nullptr)
                                        logFalloutProcedureMatrixAudit(mPtr, transform, it->first,
                                            active->second.mGroupname, animsrc ? animsrc->mSourceName : std::string(),
                                            rawKey, bind, before, candidate, maxNativeLocalMatrixDelta,
                                            maxNativeLocalMatrixDeltaBone, maxNativeWorldMatrixDelta,
                                            maxNativeWorldMatrixDeltaBone);
                                    Log(Debug::Info)
                                        << "FNV/ESM4 MATRIX AUDIT " << mPtr.getCellRef().getRefId()
                                        << " transform=NifOsg"
                                        << " blendMask=" << blendMask
                                        << " group=" << active->second.mGroupname
                                        << " source=" << (animsrc ? animsrc->mSourceName : std::string())
                                        << " bone=" << it->first
                                        << " rawKeyQuat=" << formatQuat(rawKey)
                                        << " bindQuat=" << formatQuat(bind)
                                        << " finalQuat=" << formatQuat(candidate.getRotate())
                                        << " beforeTrans=" << formatVec3(before.getTrans())
                                        << " finalTrans=" << formatVec3(candidate.getTrans())
                                        << " basisLen=("
                                        << osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2)).length() << ","
                                        << osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2)).length() << ","
                                        << osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)).length() << ")"
                                        << " basisDotXY="
                                        << (osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2))
                                               * osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2)))
                                        << " basisDotXZ="
                                        << (osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2))
                                               * osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)))
                                        << " basisDotYZ="
                                        << (osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2))
                                               * osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)))
                                        << " handedness=" << basisHandedness(candidate)
                                        << " mode=" << getFalloutRotationMode()
                                        << " procedureMode=" << getFalloutProcedureRotationMode()
                                        << " procedureIdle=" << falloutProcedureIdle;
                                    ++matrixAuditLines;
                                }
                                nifTransform->setRotation(rotation);
                            }
                        }
                        if (keyframe.mTranslation)
                        {
                            if (applyBoneTranslation)
                            {
                                nifTransform->setTranslation(*keyframe.mTranslation);
                                ++appliedBoneTranslations;
                                const float translationDelta = (*keyframe.mTranslation - before.getTrans()).length();
                                if (translationDelta > maxBoneTranslationDelta)
                                {
                                    maxBoneTranslationDelta = translationDelta;
                                    maxBoneTranslationDeltaBone = it->first;
                                }
                            }
                            else
                                ++skippedBoneTranslations;
                        }
                        if (keyframe.mScale)
                            nifTransform->setScale(*keyframe.mScale);
                    }
                    else
                    {
                        osg::Matrixf candidate = before;
                        osg::Vec3f translation = before.getTrans();
                        if (keyframe.mTranslation)
                        {
                            if (applyBoneTranslation)
                            {
                                translation = *keyframe.mTranslation;
                                ++appliedBoneTranslations;
                                const float translationDelta = (translation - before.getTrans()).length();
                                if (translationDelta > maxBoneTranslationDelta)
                                {
                                    maxBoneTranslationDelta = translationDelta;
                                    maxBoneTranslationDeltaBone = it->first;
                                }
                            }
                            else
                                ++skippedBoneTranslations;
                        }

                        osg::Quat rotation;
                        bool hasRotation = false;
                        if (keyframe.mRotation)
                        {
                            rotation = composeFalloutBindRelativeRotation(
                                transform, *keyframe.mRotation, lowerAppliedBone, falloutProcedureIdle);
                            hasRotation = normalizeFiniteQuat(rotation);
                        }

                        if (hasRotation)
                            candidate = osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(translation);
                        else if (translation != before.getTrans())
                            candidate = osg::Matrixf::translate(translation);

                        if (matrixAudit && matrixAuditLines < matrixAuditLineLimit
                            && isFalloutMatrixAuditBone(lowerAppliedBone))
                        {
                            const osg::Quat rawKey = keyframe.mRotation ? *keyframe.mRotation : osg::Quat();
                            const osg::Quat bind = getFalloutBindRotation(transform);
                            if (keyframe.mRotation && std::getenv("OPENMW_FNV_PROCEDURE_MATRIX_AUDIT") != nullptr)
                                logFalloutProcedureMatrixAudit(mPtr, transform, it->first, active->second.mGroupname,
                                    animsrc ? animsrc->mSourceName : std::string(), rawKey, bind, before, candidate,
                                    maxNativeLocalMatrixDelta, maxNativeLocalMatrixDeltaBone, maxNativeWorldMatrixDelta,
                                    maxNativeWorldMatrixDeltaBone);
                            Log(Debug::Info)
                                << "FNV/ESM4 MATRIX AUDIT " << mPtr.getCellRef().getRefId()
                                << " transform=osg"
                                << " blendMask=" << blendMask
                                << " group=" << active->second.mGroupname
                                << " source=" << (animsrc ? animsrc->mSourceName : std::string())
                                << " bone=" << it->first
                                << " rawKeyQuat=" << formatQuat(rawKey)
                                << " bindQuat=" << formatQuat(bind)
                                << " finalQuat=" << formatQuat(candidate.getRotate())
                                << " beforeTrans=" << formatVec3(before.getTrans())
                                << " finalTrans=" << formatVec3(candidate.getTrans())
                                << " basisLen=("
                                << osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2)).length() << ","
                                << osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2)).length() << ","
                                << osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)).length() << ")"
                                << " basisDotXY="
                                << (osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2))
                                       * osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2)))
                                << " basisDotXZ="
                                << (osg::Vec3f(candidate(0, 0), candidate(0, 1), candidate(0, 2))
                                       * osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)))
                                << " basisDotYZ="
                                << (osg::Vec3f(candidate(1, 0), candidate(1, 1), candidate(1, 2))
                                       * osg::Vec3f(candidate(2, 0), candidate(2, 1), candidate(2, 2)))
                                << " handedness=" << basisHandedness(candidate)
                                << " mode=" << getFalloutRotationMode()
                                << " procedureMode=" << getFalloutProcedureRotationMode()
                                << " procedureIdle=" << falloutProcedureIdle;
                            ++matrixAuditLines;
                        }

                        if (!isSaneFalloutHelperMatrix(candidate))
                        {
                            ++skippedHelperControllers;
                            continue;
                        }

                        transform->setMatrix(candidate);
                    }
                    const osg::Matrixf after = transform->getMatrix();
                    osg::MatrixTransform* rigBoneTransform = nullptr;
                    if (mSkeleton != nullptr && Misc::StringUtils::ciStartsWith(lowerAppliedBone, "bip01"))
                    {
                        if (SceneUtil::Bone* bone = mSkeleton->getBone(it->first))
                            rigBoneTransform = bone->mNode;
                        else
                            ++missingRigBoneTransforms;
                    }
                    if (rigBoneTransform != nullptr && rigBoneTransform != transform)
                    {
                        const osg::Matrixf beforeRigBone = rigBoneTransform->getMatrix();
                        rigBoneTransform->setMatrix(after);
                        rigBoneTransform->dirtyBound();
                        ++mirroredRigBoneTransforms;

                        const float rigBoneDelta = matrixDifference(beforeRigBone, after);
                        if (rigBoneDelta > maxMirroredDuplicateMatrixDelta)
                        {
                            maxMirroredDuplicateMatrixDelta = rigBoneDelta;
                            maxMirroredDuplicateMatrixDeltaBone = it->first;
                        }
                    }
                    else if (rigBoneTransform == transform)
                        ++matchedRigBoneTransforms;
                    auto duplicateIt = duplicateTransformTargets.find(lowerAppliedBone);
                    if (duplicateIt != duplicateTransformTargets.end())
                    {
                        for (osg::MatrixTransform* duplicate : duplicateIt->second)
                        {
                            if (duplicate == nullptr || duplicate == transform)
                                continue;
                            if (!shouldMirrorFalloutDuplicatePoses())
                            {
                                ++skippedRiggedDuplicateTransforms;
                                continue;
                            }
                            if (Misc::StringUtils::ciStartsWith(lowerAppliedBone, "bip01")
                                && !shouldMirrorFalloutSkinnedDuplicateBone(lowerAppliedBone))
                            {
                                ++skippedRiggedDuplicateTransforms;
                                continue;
                            }

                            const osg::Matrixf beforeDuplicate = duplicate->getMatrix();
                            osg::Matrixf duplicateAfter = beforeDuplicate;
                            bool poseOnlyDuplicate = false;
                            if (Misc::StringUtils::ciStartsWith(lowerAppliedBone, "bip01"))
                            {
                                if (auto* nifDuplicate = dynamic_cast<NifOsg::MatrixTransform*>(duplicate))
                                {
                                    if (keyframe.mRotation)
                                        nifDuplicate->setRotation(
                                            composeFalloutBindRelativeRotation(
                                                nifDuplicate, *keyframe.mRotation, lowerAppliedBone, falloutProcedureIdle));
                                    if (keyframe.mScale)
                                        nifDuplicate->setScale(*keyframe.mScale);
                                    duplicateAfter = duplicate->getMatrix();
                                    poseOnlyDuplicate = true;
                                }
                                else
                                {
                                    osg::Quat rotation;
                                    bool hasRotation = false;
                                    if (keyframe.mRotation)
                                    {
                                        rotation = composeFalloutBindRelativeRotation(
                                            duplicate, *keyframe.mRotation, lowerAppliedBone, falloutProcedureIdle);
                                        hasRotation = normalizeFiniteQuat(rotation);
                                    }

                                    osg::Matrixf localPose;
                                    bool hasLocalPose = false;
                                    if (keyframe.mScale)
                                    {
                                        localPose = osg::Matrixf::scale(
                                            osg::Vec3f(*keyframe.mScale, *keyframe.mScale, *keyframe.mScale));
                                        hasLocalPose = true;
                                    }
                                    if (hasRotation)
                                    {
                                        localPose = hasLocalPose ? (localPose * osg::Matrixf::rotate(rotation))
                                                                 : osg::Matrixf::rotate(rotation);
                                        hasLocalPose = true;
                                    }
                                    if (hasLocalPose)
                                    {
                                        duplicateAfter = localPose * osg::Matrixf::translate(beforeDuplicate.getTrans());
                                        duplicate->setMatrix(duplicateAfter);
                                        poseOnlyDuplicate = true;
                                    }
                                }
                            }
                            if (!poseOnlyDuplicate)
                            {
                                duplicate->setMatrix(after);
                                duplicateAfter = after;
                            }
                            duplicate->dirtyBound();
                            ++mirroredDuplicateTransforms;
                            if (poseOnlyDuplicate)
                                ++mirroredDuplicatePoseOnlyTransforms;

                            const float duplicateDelta = matrixDifference(beforeDuplicate, duplicateAfter);
                            if (duplicateDelta > maxMirroredDuplicateMatrixDelta)
                            {
                                maxMirroredDuplicateMatrixDelta = duplicateDelta;
                                maxMirroredDuplicateMatrixDeltaBone = it->first;
                            }
                        }
                    }
                    float matrixDelta = 0.f;
                    const float* beforePtr = before.ptr();
                    const float* afterPtr = after.ptr();
                    for (int i = 0; i < 16; ++i)
                        matrixDelta = std::max(matrixDelta, std::abs(beforePtr[i] - afterPtr[i]));
                    if (matrixDelta > maxAppliedMatrixDelta)
                    {
                        maxAppliedMatrixDelta = matrixDelta;
                        maxAppliedMatrixDeltaBone = it->first;
                    }
                    if (matrixDelta > 0.0001f && it->first.find("nonaccum") == std::string::npos)
                    {
                        ++visibleMatrixDeltaCount;
                        if (matrixDelta > maxVisibleMatrixDelta)
                        {
                            maxVisibleMatrixDelta = matrixDelta;
                            maxVisibleMatrixDeltaBone = it->first;
                        }
                    }
                    if (lowerAppliedBone.find("upperarm") != std::string::npos
                        || lowerAppliedBone.find("forearm") != std::string::npos
                        || lowerAppliedBone.find("clavicle") != std::string::npos
                        || lowerAppliedBone.find("hand") != std::string::npos)
                    {
                        if (matrixDelta > maxArmMatrixDelta)
                        {
                            maxArmMatrixDelta = matrixDelta;
                            maxArmMatrixDeltaBone = it->first;
                        }
                    }
                    if (lowerAppliedBone.find("spine") != std::string::npos
                        || lowerAppliedBone.find("pelvis") != std::string::npos)
                    {
                        if (matrixDelta > maxTorsoMatrixDelta)
                        {
                            maxTorsoMatrixDelta = matrixDelta;
                            maxTorsoMatrixDeltaBone = it->first;
                        }
                    }
                    ++appliedControllers;
                }
            }

            if (hasFalloutRootUpCorrection())
            {
                const osg::Quat correction = getFalloutRootUpCorrectionRotation();
                std::unordered_set<osg::MatrixTransform*> correctedRoots;
                const auto correctRoot = [&](osg::MatrixTransform* root) {
                    if (root == nullptr || !correctedRoots.insert(root).second)
                        return;
                    applyFalloutRootUpCorrection(root, correction);
                };

                Animation::NodeMap::const_iterator rootIt = getNodeMap().find("bip01");
                if (rootIt != getNodeMap().end())
                    correctRoot(rootIt->second.get());
                if (mSkeleton != nullptr)
                {
                    if (SceneUtil::Bone* rootBone = mSkeleton->getBone("Bip01"))
                        correctRoot(rootBone->mNode);
                    else if (SceneUtil::Bone* rootBone = mSkeleton->getBone("bip01"))
                        correctRoot(rootBone->mNode);
                }
                auto duplicateRootIt = duplicateTransformTargets.find("bip01");
                if (duplicateRootIt != duplicateTransformTargets.end())
                {
                    for (osg::MatrixTransform* duplicateRoot : duplicateRootIt->second)
                        correctRoot(duplicateRoot);
                }

                static std::unordered_map<std::string, unsigned int> sFalloutRootCorrectionLogs;
                unsigned int& rootCorrectionLogs = sFalloutRootCorrectionLogs[refIdText];
                if (rootCorrectionLogs < 3)
                {
                    ++rootCorrectionLogs;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: applied root up correction for "
                                     << mPtr.getCellRef().getRefId()
                                     << " mode=" << std::getenv("OPENMW_FNV_ROOT_UP_CORRECTION")
                                     << " roots=" << correctedRoots.size();
                }
            }

            applyFalloutSeatedHumanIk(duplicateTransformTargets, mPtr);
            applyPostManualFalloutActorPose();

            if (mSkeleton)
                mSkeleton->markBoneMatriceDirty();

            static std::unordered_map<std::string, unsigned int> sFalloutManualApplyLogs;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& logs = sFalloutManualApplyLogs[refId];
            const unsigned int maxManualApplyLogs = refId.find("4104c7f") != std::string::npos ? 20 : 3;
            const bool bindPoseProofAudit = appliedControllers == 0
                && std::getenv("OPENMW_FNV_BIND_POSE_PROOF") != nullptr;
            if ((appliedControllers > 0 || bindPoseProofAudit) && logs < maxManualApplyLogs)
            {
                ++logs;
                const FalloutPoseSemanticSample poseSemantic = sampleFalloutPoseSemantics(duplicateTransformTargets);
                std::ostringstream activeGroups;
                for (size_t blendMask = 0; blendMask < sNumBlendMasks; ++blendMask)
                {
                    AnimStateMap::const_iterator active = mStates.end();
                    for (AnimStateMap::const_iterator state = mStates.begin(); state != mStates.end(); ++state)
                    {
                        if (!state->second.blendMaskContains(blendMask))
                            continue;

                        if (active == mStates.end()
                            || active->second.mPriority[(BoneGroup)blendMask] < state->second.mPriority[(BoneGroup)blendMask])
                            active = state;
                    }

                    if (active == mStates.end())
                        continue;

                    if (activeGroups.tellp() > 0)
                        activeGroups << " | ";
                    activeGroups << blendMask << ":" << active->second.mGroupname << "@t=" << active->second.getTime();
                    if (active->second.mSource)
                        activeGroups << " src=" << active->second.mSource->mSourceName;
                }
                Log(Debug::Verbose) << "FNV/ESM4 diag: manually applied " << appliedControllers
                                 << " active keyframe controller(s) for " << mPtr.getCellRef().getRefId()
                                 << " bindPoseProofAudit=" << bindPoseProofAudit
                                 << " activeGroups=[" << activeGroups.str() << "]"
                                 << " skippedUnsafeHelpers=" << skippedHelperControllers
                                 << " mirroredDuplicateTransforms=" << mirroredDuplicateTransforms
                                 << " mirroredDuplicatePoseOnlyTransforms=" << mirroredDuplicatePoseOnlyTransforms
                                 << " mirroredRigBoneTransforms=" << mirroredRigBoneTransforms
                                 << " matchedRigBoneTransforms=" << matchedRigBoneTransforms
                                 << " missingRigBoneTransforms=" << missingRigBoneTransforms
                                 << " skippedRiggedDuplicateTransforms=" << skippedRiggedDuplicateTransforms
                                 << " maxMirroredDuplicateDelta=" << maxMirroredDuplicateMatrixDelta
                                 << " maxMirroredDuplicateBone=" << maxMirroredDuplicateMatrixDeltaBone
                                 << " appliedBoneTranslations=" << appliedBoneTranslations
                                 << " skippedBoneTranslations=" << skippedBoneTranslations
                                 << " falloutRotationMode=" << getFalloutRotationMode()
                                 << " falloutProcedureRotationMode=" << getFalloutProcedureRotationMode()
                                 << " maxBoneTranslationDelta=" << maxBoneTranslationDelta
                                 << " maxBoneTranslationBone=" << maxBoneTranslationDeltaBone
                                 << " maxMatrixDelta=" << maxAppliedMatrixDelta
                                 << " maxDeltaBone=" << maxAppliedMatrixDeltaBone
                                 << " visibleDeltaCount=" << visibleMatrixDeltaCount
                                 << " maxVisibleDelta=" << maxVisibleMatrixDelta
                                 << " maxVisibleBone=" << maxVisibleMatrixDeltaBone
                                 << " maxArmDelta=" << maxArmMatrixDelta
                                 << " maxArmBone=" << maxArmMatrixDeltaBone
                                 << " maxTorsoDelta=" << maxTorsoMatrixDelta
                                 << " maxTorsoBone=" << maxTorsoMatrixDeltaBone
                                 << " maxNativeLocalMatrixDelta=" << maxNativeLocalMatrixDelta
                                 << " maxNativeLocalMatrixDeltaBone=" << maxNativeLocalMatrixDeltaBone
                                 << " maxNativeWorldMatrixDelta=" << maxNativeWorldMatrixDelta
                                 << " maxNativeWorldMatrixDeltaBone=" << maxNativeWorldMatrixDeltaBone;
                if (std::getenv("OPENMW_FNV_PROCEDURE_MATRIX_AUDIT") != nullptr)
                {
                    Log(Debug::Info) << "FNV/ESM4 PROCEDURE MATRIX AUDIT SUMMARY "
                                     << mPtr.getCellRef().getRefId()
                                     << " maxNativeLocalMatrixDelta=" << maxNativeLocalMatrixDelta
                                     << " maxNativeLocalMatrixDeltaBone=" << maxNativeLocalMatrixDeltaBone
                                     << " maxNativeWorldMatrixDelta=" << maxNativeWorldMatrixDelta
                                     << " maxNativeWorldMatrixDeltaBone=" << maxNativeWorldMatrixDeltaBone;
                }
                Log(Debug::Verbose) << "FNV/ESM4 diag: semantic pose for " << mPtr.getCellRef().getRefId()
                                 << " headDeg=" << poseSemantic.mHead
                                 << " spine2Deg=" << poseSemantic.mSpine2
                                 << " lUpperArmDeg=" << poseSemantic.mLeftUpperArm
                                 << " rUpperArmDeg=" << poseSemantic.mRightUpperArm
                                 << " lForearmDeg=" << poseSemantic.mLeftForearm
                                 << " rForearmDeg=" << poseSemantic.mRightForearm
                                 << " lThighDeg=" << poseSemantic.mLeftThigh
                                 << " rThighDeg=" << poseSemantic.mRightThigh
                                 << " maxMajorDeg=" << poseSemantic.mMaxMajor
                                 << " maxMajorBone=" << poseSemantic.mMaxMajorBone
                                 << " verdict=" << (poseSemantic.mBad ? "BAD" : "OK")
                                 << " reason=" << poseSemantic.mReason;
                auditFalloutDuplicateBoneDeltas(duplicateTransformTargets, mPtr);
                auditFalloutMirrorSymmetry(duplicateTransformTargets, mPtr);
                if (std::getenv("OPENMW_FNV_SEATED_POSTURE_AUDIT") != nullptr)
                {
                    auditFalloutSeatedPlacement(duplicateTransformTargets, mPtr);
                    auditFalloutSeatedLegChain(duplicateTransformTargets, mPtr);
                    auditFalloutSeatedUpperBody(duplicateTransformTargets, mPtr);
                }
                if (shouldAuditProofPreviewGameplay())
                {
                    auditFalloutWorldPosture(duplicateTransformTargets, mPtr);
                    auditFalloutRuntimeParts(mObjectRoot.get(), duplicateTransformTargets, mPtr,
                        shouldAuditFalloutActorRenderLiveGeometry());
                }
            }
        }

        if (falloutNpc
            && (shouldApplyFalloutSeatedHumanIk() || shouldApplyFalloutStandingLegIk(mPtr)
                || shouldApplyFalloutStandingArmIk(mPtr))
            && mObjectRoot != nullptr)
        {
            std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
            FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
            mObjectRoot->accept(targetVisitor);
            if (applyFalloutSeatedHumanIk(runtimeTargets, mPtr) && mSkeleton)
                mSkeleton->markBoneMatriceDirty();
            applyPostManualFalloutActorPose();
        }

        if (falloutNpc && mObjectRoot != nullptr)
        {
            applyPostManualFalloutActorPose();
            if (mSkeleton != nullptr)
            {
                mSkeleton->markBoneMatriceDirty();
                mSkeleton->updateBoneMatrices(0);
            }
        }

        if (shouldAuditProofPreviewGameplay() && falloutNpc && shouldAuditFalloutStandingUpperBody() && mObjectRoot != nullptr)
        {
            static std::unordered_map<std::string, unsigned int> sFalloutStandingUpperRuntimeAuditSamples;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& samples = sFalloutStandingUpperRuntimeAuditSamples[refId];
            if (samples < 6)
            {
                ++samples;
                std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
                FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
                mObjectRoot->accept(targetVisitor);
                auditFalloutStandingUpperBody(runtimeTargets, mPtr);
            }
        }

        if (shouldAuditProofPreviewGameplay() && falloutNpc && shouldAuditFalloutRootAttachment() && mObjectRoot != nullptr)
        {
            static std::unordered_map<std::string, unsigned int> sFalloutRootAttachmentRuntimeAuditSamples;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& samples = sFalloutRootAttachmentRuntimeAuditSamples[refId];
            if (samples < 6)
            {
                ++samples;
                std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
                FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
                mObjectRoot->accept(targetVisitor);
                auditFalloutRootAttachmentFrame(mObjectRoot.get(), runtimeTargets, mPtr);
            }
        }

        if (shouldAuditProofPreviewGameplay() && falloutNpc && mObjectRoot != nullptr)
        {
            static std::unordered_map<std::string, unsigned int> sFalloutSkeletonAnchorRuntimeAuditSamples;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& samples = sFalloutSkeletonAnchorRuntimeAuditSamples[refId];
            if (samples < 6)
            {
                ++samples;
                std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
                FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
                mObjectRoot->accept(targetVisitor);
                Log(Debug::Verbose) << "FNV/ESM4 diag: skeleton animation state " << mPtr.getCellRef().getRefId()
                                 << " sample=" << samples
                                 << " activeGroups=[" << describeActiveFalloutAnimationStates() << "]";
                auditFalloutSkeletonBounds(runtimeTargets, mPtr);
            }
        }

        if (shouldAuditProofPreviewGameplay() && falloutNpc && shouldAuditFalloutActorRenderLiveGeometry()
            && mObjectRoot != nullptr)
        {
            static std::unordered_map<std::string, unsigned int> sFalloutPartMatrixRuntimeAuditSamples;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& samples = sFalloutPartMatrixRuntimeAuditSamples[refId];
            if (samples < 6)
            {
                ++samples;
                std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
                FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
                mObjectRoot->accept(targetVisitor);
                auditFalloutRuntimeParts(mObjectRoot.get(), runtimeTargets, mPtr, true);
            }
        }

        updateEffects();

        const float epsilon = 0.001f;
        float yawOffset = 0;
        if (mRootController)
        {
            bool enable = std::abs(mLegsYawRadians) > epsilon || std::abs(mBodyPitchRadians) > epsilon;
            mRootController->setEnabled(enable);
            if (enable)
            {
                osg::Quat legYaw = osg::Quat(mLegsYawRadians, osg::Vec3f(0, 0, 1));
                mRootController->setRotate(legYaw * osg::Quat(mBodyPitchRadians, osg::Vec3f(1, 0, 0)));
                yawOffset = mLegsYawRadians;
                // When yawing the root, also update the accumulated movement.
                movement = legYaw * movement;
            }
        }
        if (mSpineController)
        {
            float yaw = mUpperBodyYawRadians - yawOffset;
            bool enable = std::abs(yaw) > epsilon;
            mSpineController->setEnabled(enable);
            if (enable)
            {
                mSpineController->setRotate(osg::Quat(yaw, osg::Vec3f(0, 0, 1)));
                yawOffset = mUpperBodyYawRadians;
            }
        }
        if (mHeadController)
        {
            float yaw = mHeadYawRadians - yawOffset;
            bool enable = (std::abs(mHeadPitchRadians) > epsilon || std::abs(yaw) > epsilon);
            mHeadController->setEnabled(enable);
            if (enable)
                mHeadController->setRotate(
                    osg::Quat(mHeadPitchRadians, osg::Vec3f(1, 0, 0)) * osg::Quat(yaw, osg::Vec3f(0, 0, 1)));
        }

        if (falloutNpc && mObjectRoot != nullptr)
        {
            applyPostManualFalloutActorPose();
            if (mSkeleton != nullptr)
            {
                mSkeleton->markBoneMatriceDirty();
                mSkeleton->updateBoneMatrices(0);
            }
        }

        if (esm4Npc && mObjectRoot != nullptr
            && std::getenv("OPENMW_ESM4_TRANSFORM_ORACLE_OUTPUT") != nullptr)
        {
            std::unordered_map<std::string, std::vector<osg::MatrixTransform*>> runtimeTargets;
            FalloutTransformTargetVisitor targetVisitor(runtimeTargets);
            mObjectRoot->accept(targetVisitor);
            std::optional<osg::Vec3f> accumulationTranslation;
            if (mAccumCtrl != nullptr)
            {
                const SceneUtil::KeyframeController::KfTransform transform
                    = mAccumCtrl->getCurrentTransformation(nullptr);
                accumulationTranslation = transform.mTranslation;
            }
            writeFalloutTransformOracleFrame(
                mObjectRoot.get(), runtimeTargets, mPtr, describeActiveFalloutAnimationStates(),
                accumulationTranslation);
        }

        if (shouldAuditProofPreviewGameplay())
            auditGenericProofPosture(mObjectRoot.get(), mPtr);

        return movement;
    }

    void Animation::setLoopingEnabled(std::string_view groupname, bool enabled)
    {
        AnimStateMap::iterator state(mStates.find(groupname));
        if (state != mStates.end())
            state->second.mLoopingEnabled = enabled;
    }

    void loadBonesFromFile(
        osg::ref_ptr<osg::Node>& baseNode, VFS::Path::NormalizedView model, Resource::ResourceSystem* resourceSystem)
    {
        const osg::Node* node = resourceSystem->getSceneManager()->getTemplate(model).get();
        osg::ref_ptr<osg::Node> sheathSkeleton(
            const_cast<osg::Node*>(node)); // const-trickery required because there is no const version of NodeVisitor

        GetExtendedBonesVisitor getBonesVisitor;
        sheathSkeleton->accept(getBonesVisitor);
        for (auto& nodePair : getBonesVisitor.mFoundBones)
        {
            SceneUtil::FindByNameVisitor findVisitor(nodePair.second->getName());
            baseNode->accept(findVisitor);

            osg::Group* sheathParent = findVisitor.mFoundNode;
            if (sheathParent)
            {
                osg::Node* copy = static_cast<osg::Node*>(nodePair.first->clone(osg::CopyOp::DEEP_COPY_NODES));
                sheathParent->addChild(copy);
            }
        }
    }

    void injectCustomBones(
        osg::ref_ptr<osg::Node>& node, const std::string& model, Resource::ResourceSystem* resourceSystem)
    {
        if (model.empty())
            return;

        std::string animationPath = model;
        if (animationPath.find("meshes") == 0)
        {
            animationPath.replace(0, 6, "animations");
        }
        animationPath.replace(animationPath.size() - 4, 4, "/");

        for (const VFS::Path::Normalized& name : resourceSystem->getVFS()->getRecursiveDirectoryIterator(animationPath))
        {
            if (Misc::getFileExtension(name) == "nif")
                loadBonesFromFile(node, name, resourceSystem);
        }
    }

    osg::ref_ptr<osg::Node> getModelInstance(Resource::ResourceSystem* resourceSystem, const std::string& model,
        bool baseonly, bool inject, const std::string& defaultSkeleton)
    {
        Resource::SceneManager* sceneMgr = resourceSystem->getSceneManager();
        if (baseonly)
        {
            typedef std::map<std::string, osg::ref_ptr<osg::Node>> Cache;
            static Cache cache;
            Cache::iterator found = cache.find(model);
            if (found == cache.end())
            {
                osg::ref_ptr<osg::Node> created = sceneMgr->getInstance(VFS::Path::toNormalized(model));

                if (inject)
                {
                    injectCustomBones(created, defaultSkeleton, resourceSystem);
                    injectCustomBones(created, model, resourceSystem);
                }

                SceneUtil::CleanObjectRootVisitor removeDrawableVisitor;
                created->accept(removeDrawableVisitor);
                removeDrawableVisitor.remove();

                cache.insert(std::make_pair(model, created));

                return sceneMgr->getInstance(created);
            }
            else
                return sceneMgr->getInstance(found->second);
        }
        else
        {
            osg::ref_ptr<osg::Node> created = sceneMgr->getInstance(VFS::Path::toNormalized(model));

            if (inject)
            {
                injectCustomBones(created, defaultSkeleton, resourceSystem);
                injectCustomBones(created, model, resourceSystem);
            }

            return created;
        }
    }

    void Animation::setObjectRoot(const std::string& model, bool forceskeleton, bool baseonly, bool isCreature)
    {
        osg::ref_ptr<osg::StateSet> previousStateset;
        if (mObjectRoot)
        {
            detachActiveControllers();
            if (mLightListCallback)
                mObjectRoot->removeCullCallback(mLightListCallback);
            if (mTransparencyUpdater)
                mObjectRoot->removeCullCallback(mTransparencyUpdater);
            previousStateset = mObjectRoot->getStateSet();
            mObjectRoot->getParent(0)->removeChild(mObjectRoot);
        }
        mObjectRoot = nullptr;
        mSkeleton = nullptr;

        mNodeMap.clear();
        mNodeMapCreated = false;
        mAccumRoot = nullptr;
        mAccumCtrl = nullptr;

        std::string defaultSkeleton;
        bool inject = false;

        if (Settings::game().mUseAdditionalAnimSources && mPtr.getClass().isActor())
        {
            if (isCreature)
            {
                MWWorld::LiveCellRef<ESM::Creature>* ref = mPtr.get<ESM::Creature>();
                if (ref->mBase->mFlags & ESM::Creature::Bipedal)
                {
                    defaultSkeleton = Settings::models().mXbaseanim.get().value();
                    inject = true;
                }
            }
            else
            {
                inject = true;
                MWWorld::LiveCellRef<ESM::NPC>* ref = mPtr.get<ESM::NPC>();
                if (!ref->mBase->mModel.empty())
                {
                    // If NPC has a custom animation model attached, we should inject bones from default skeleton for
                    // given race and gender as well Since it is a quite rare case, there should not be a noticable
                    // performance loss Note: consider that player and werewolves have no custom animation files
                    // attached for now
                    const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
                    const ESM::Race* race = store.get<ESM::Race>().find(ref->mBase->mRace);

                    const bool firstPerson = false;
                    const bool isBeast = (race->mData.mFlags & ESM::Race::Beast) != 0;
                    const bool isFemale = !ref->mBase->isMale();
                    const bool werewolf = false;

                    defaultSkeleton = Misc::ResourceHelpers::correctActorModelPath(
                        VFS::Path::toNormalized(getActorSkeleton(firstPerson, isFemale, isBeast, werewolf)),
                        mResourceSystem->getVFS());
                }
            }
        }

        const auto isDefaultActorModel = [](std::string_view path) {
            return VFS::Path::pathEqual(Settings::models().mXbaseanim.get(), path)
                || VFS::Path::pathEqual(Settings::models().mXbaseanim1st.get(), path)
                || VFS::Path::pathEqual(Settings::models().mXbaseanimfemale.get(), path)
                || VFS::Path::pathEqual(Settings::models().mXargonianswimkna.get(), path)
                || VFS::Path::pathEqual(Settings::models().mBaseanim.get(), path)
                || VFS::Path::pathEqual(Settings::models().mBaseanimkna.get(), path)
                || VFS::Path::pathEqual(Settings::models().mBaseanimkna1st.get(), path)
                || VFS::Path::pathEqual(Settings::models().mBaseanimfemale.get(), path)
                || VFS::Path::pathEqual(Settings::models().mBaseanimfemale1st.get(), path)
                || VFS::Path::pathEqual("characters/_male/skeleton.nif", path)
                || VFS::Path::pathEqual("actors/character/character assets/skeleton.nif", path)
                || VFS::Path::pathEqual("actors/character/_1stperson/skeleton.nif", path);
        };
        const bool useEmptyMissingDefaultActorRoot
            = !model.empty() && isDefaultActorModel(model) && !mResourceSystem->getVFS()->exists(VFS::Path::toNormalized(model));

        if (!forceskeleton)
        {
            osg::ref_ptr<osg::Node> created;
            if (useEmptyMissingDefaultActorRoot)
            {
                Log(Debug::Info) << "FNV/ESM4: skipped missing default actor model " << model;
                created = new osg::Group;
            }
            else
                created = getModelInstance(mResourceSystem, model, baseonly, inject, defaultSkeleton);
            mInsert->addChild(created);
            mObjectRoot = created->asGroup();
            if (!mObjectRoot)
            {
                mInsert->removeChild(created);
                mObjectRoot = new osg::Group;
                mObjectRoot->addChild(created);
                mInsert->addChild(mObjectRoot);
            }
            osg::ref_ptr<SceneUtil::Skeleton> skel = dynamic_cast<SceneUtil::Skeleton*>(mObjectRoot.get());
            if (skel)
                mSkeleton = skel.get();
        }
        else
        {
            osg::ref_ptr<osg::Node> created;
            if (useEmptyMissingDefaultActorRoot)
            {
                Log(Debug::Info) << "FNV/ESM4: skipped missing default actor skeleton " << model;
                created = new SceneUtil::Skeleton;
            }
            else
                created = getModelInstance(mResourceSystem, model, baseonly, inject, defaultSkeleton);
            osg::ref_ptr<SceneUtil::Skeleton> skel = dynamic_cast<SceneUtil::Skeleton*>(created.get());
            if (!skel)
            {
                skel = new SceneUtil::Skeleton;
                skel->addChild(created);
            }
            mSkeleton = skel.get();
            mObjectRoot = skel;
            mInsert->addChild(mObjectRoot);
        }

        if (osg::ref_ptr<osg::Group> correctedRoot = wrapFalloutActorRootIfRequested(mObjectRoot, mPtr))
        {
            if (correctedRoot.get() != mObjectRoot.get())
            {
                mInsert->removeChild(mObjectRoot);
                mObjectRoot = correctedRoot;
                mInsert->addChild(mObjectRoot);
            }
        }

        if (osg::ref_ptr<osg::Group> correctedRoot = correctFalloutCreatureForwardAxis(mObjectRoot, mPtr))
        {
            if (correctedRoot.get() != mObjectRoot.get())
            {
                mInsert->removeChild(mObjectRoot);
                mObjectRoot = correctedRoot;
                mInsert->addChild(mObjectRoot);
            }
        }

        // osgAnimation formats with skeletons should have their nodemap be bone instances
        // FIXME: better way to detect osgAnimation here instead of relying on extension?
        mRequiresBoneMap = mSkeleton != nullptr && !Misc::StringUtils::ciEndsWith(model, ".nif");

        if (previousStateset)
            mObjectRoot->setStateSet(previousStateset);

        if (isCreature)
        {
            SceneUtil::RemoveTriBipVisitor removeTriBipVisitor;
            mObjectRoot->accept(removeTriBipVisitor);
            removeTriBipVisitor.remove();
        }

        if (!mLightListCallback)
            mLightListCallback = new SceneUtil::LightListCallback;
        mObjectRoot->addCullCallback(mLightListCallback);
        if (mTransparencyUpdater)
            mObjectRoot->addCullCallback(mTransparencyUpdater);
    }

    osg::Group* Animation::getObjectRoot()
    {
        return mObjectRoot.get();
    }

    osg::Group* Animation::getOrCreateObjectRoot()
    {
        if (mObjectRoot)
            return mObjectRoot.get();

        mObjectRoot = new osg::Group;
        mInsert->addChild(mObjectRoot);
        return mObjectRoot.get();
    }

    void Animation::addSpellCastGlow(const osg::Vec4f& color, float glowDuration)
    {
        if (!mGlowUpdater || (mGlowUpdater->isDone() || (mGlowUpdater->isPermanentGlowUpdater() == true)))
        {
            if (mGlowUpdater && mGlowUpdater->isDone())
                mObjectRoot->removeUpdateCallback(mGlowUpdater);

            if (mGlowUpdater && mGlowUpdater->isPermanentGlowUpdater())
            {
                mGlowUpdater->setColor(color);
                mGlowUpdater->setDuration(glowDuration);
            }
            else if (mObjectRoot)
                mGlowUpdater = SceneUtil::addEnchantedGlow(mObjectRoot, mResourceSystem, color, glowDuration);
        }
    }

    void Animation::addExtraLight(osg::ref_ptr<osg::Group> parent, const SceneUtil::LightCommon& esmLight)
    {
        bool exterior = mPtr.isInCell() && mPtr.getCell()->getCell()->isExterior();

        mExtraLightSource = SceneUtil::addLight(parent, esmLight, Mask_Lighting, exterior);
        mExtraLightSource->setActorFade(mAlpha);
    }

    void Animation::addEffect(std::string_view model, std::string_view effectId, bool loop, std::string_view bonename,
        std::string_view texture, bool useAmbientLight)
    {
        if (!mObjectRoot.get())
            return;

        // Early out if we already have this effect
        FindVfxCallbacksVisitor visitor(effectId);
        mInsert->accept(visitor);

        for (std::vector<UpdateVfxCallback*>::iterator it = visitor.mCallbacks.begin(); it != visitor.mCallbacks.end();
             ++it)
        {
            UpdateVfxCallback* callback = *it;

            if (loop && !callback->mFinished && callback->mParams.mLoop && callback->mParams.mBoneName == bonename)
                return;
        }

        EffectParams params;
        params.mModelName = model;
        osg::ref_ptr<osg::Group> parentNode;
        if (bonename.empty())
            parentNode = mInsert;
        else
        {
            NodeMap::const_iterator found = getNodeMap().find(bonename);
            if (found == getNodeMap().end())
                throw std::runtime_error("Can't find bone " + std::string{ bonename });

            parentNode = found->second;
        }

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> trans = new SceneUtil::PositionAttitudeTransform;
        if (!mPtr.getClass().isNpc())
        {
            osg::Vec3f bounds(MWBase::Environment::get().getWorld()->getHalfExtents(mPtr) * 2.f);
            float scale = std::max({ bounds.x(), bounds.y(), bounds.z() / 2.f }) / 64.f;
            if (scale > 1.f)
                trans->setScale(osg::Vec3f(scale, scale, scale));
            float offset = 0.f;
            if (bounds.z() < 128.f)
                offset = bounds.z() - 128.f;
            else if (bounds.z() < bounds.x() + bounds.y())
                offset = 128.f - bounds.z();
            if (MWBase::Environment::get().getWorld()->isFlying(mPtr))
                offset /= 20.f;
            trans->setPosition(osg::Vec3f(0.f, 0.f, offset * scale));
        }
        parentNode->addChild(trans);

        osg::ref_ptr<osg::Node> node
            = mResourceSystem->getSceneManager()->getInstance(VFS::Path::toNormalized(model), trans);

        if (useAmbientLight)
        {
            // Morrowind has a white ambient light attached to the root VFX node of the scenegraph
            node->getOrCreateStateSet()->setAttributeAndModes(
                getVFXLightModelInstance(), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }

        mResourceSystem->getSceneManager()->setUpNormalsRTForStateSet(node->getOrCreateStateSet(), false);

        SceneUtil::FindMaxControllerLengthVisitor findMaxLengthVisitor;
        node->accept(findMaxLengthVisitor);

        node->setNodeMask(Mask_Effect);

        MarkDrawablesVisitor markVisitor(Mask_Effect);
        node->accept(markVisitor);

        params.mMaxControllerLength = findMaxLengthVisitor.getMaxLength();
        params.mLoop = loop;
        params.mEffectId = effectId;
        params.mBoneName = bonename;
        params.mAnimTime = std::make_shared<EffectAnimationTime>();
        trans->addUpdateCallback(new UpdateVfxCallback(params));

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(
            std::shared_ptr<SceneUtil::ControllerSource>(params.mAnimTime));
        node->accept(assignVisitor);

        // Notify that this animation has attached magic effects
        mHasMagicEffects = true;

        overrideFirstRootTexture(texture, mResourceSystem, *node);
    }

    void Animation::removeEffect(std::string_view effectId)
    {
        RemoveCallbackVisitor visitor(effectId);
        mInsert->accept(visitor);
        visitor.remove();
        mHasMagicEffects = visitor.mHasMagicEffects;
    }

    void Animation::removeEffects()
    {
        removeEffect("");
    }

    std::vector<std::string_view> Animation::getLoopingEffects() const
    {
        if (!mHasMagicEffects)
            return {};

        FindVfxCallbacksVisitor visitor;
        mInsert->accept(visitor);

        std::vector<std::string_view> out;

        for (std::vector<UpdateVfxCallback*>::iterator it = visitor.mCallbacks.begin(); it != visitor.mCallbacks.end();
             ++it)
        {
            UpdateVfxCallback* callback = *it;

            if (callback->mParams.mLoop && !callback->mFinished)
                out.push_back(callback->mParams.mEffectId);
        }
        return out;
    }

    void Animation::updateEffects()
    {
        // We do not need to visit scene every frame.
        // We can use a bool flag to check in spellcasting effect found.
        if (!mHasMagicEffects)
            return;

        // TODO: objects without animation still will have
        // transformation nodes with finished callbacks
        RemoveFinishedCallbackVisitor visitor;
        mInsert->accept(visitor);
        visitor.remove();
        mHasMagicEffects = visitor.mHasMagicEffects;
    }

    bool Animation::upperBodyReady() const
    {
        for (AnimStateMap::const_iterator stateiter = mStates.begin(); stateiter != mStates.end(); ++stateiter)
        {
            if (stateiter->second.mPriority.contains(int(MWMechanics::Priority_Hit))
                || stateiter->second.mPriority.contains(int(MWMechanics::Priority_Weapon))
                || stateiter->second.mPriority.contains(int(MWMechanics::Priority_Knockdown))
                || stateiter->second.mPriority.contains(int(MWMechanics::Priority_Death)))
                return false;
        }
        return true;
    }

    const osg::Node* Animation::getNode(std::string_view name) const
    {
        const NodeMap& nodeMap = getNodeMap();
        NodeMap::const_iterator found = nodeMap.find(name);
        if (found == nodeMap.end() && isFalloutActor(mPtr))
        {
            std::string resolvedName;
            found = findFonvAnimationBone(
                nodeMap, Misc::StringUtils::lowerCase(std::string(name)), resolvedName);
        }
        if (found == nodeMap.end())
            return nullptr;
        else
            return found->second;
    }

    void Animation::setAlpha(float alpha)
    {
        if (alpha == mAlpha || !mObjectRoot)
            return;
        mAlpha = alpha;

        // TODO: we use it to fade actors away too, but it would be nice to have a dithering shader instead.
        if (alpha != 1.f)
        {
            if (mTransparencyUpdater == nullptr)
            {
                mTransparencyUpdater = new TransparencyUpdater(alpha);
                mObjectRoot->addCullCallback(mTransparencyUpdater);
            }
            else
                mTransparencyUpdater->setAlpha(alpha);
        }
        else
        {
            mObjectRoot->removeCullCallback(mTransparencyUpdater);
            mTransparencyUpdater = nullptr;
        }
        if (mExtraLightSource)
            mExtraLightSource->setActorFade(alpha);
    }

    void Animation::setLightEffect(float effect)
    {
        if (effect == 0)
        {
            if (mGlowLight)
            {
                mInsert->removeChild(mGlowLight);
                mGlowLight = nullptr;
            }
        }
        else
        {
            // 1 pt of Light magnitude corresponds to 1 foot of radius
            float radius = effect * std::ceil(Constants::UnitsPerFoot);
            // Arbitrary multiplier used to make the obvious cut-off less obvious
            float cutoffMult = 3;

            if (!mGlowLight || (radius * cutoffMult) != mGlowLight->getRadius())
            {
                if (mGlowLight)
                {
                    mInsert->removeChild(mGlowLight);
                    mGlowLight = nullptr;
                }

                osg::ref_ptr<osg::Light> light(new osg::Light);
                light->setDiffuse(osg::Vec4f(0, 0, 0, 0));
                light->setSpecular(osg::Vec4f(0, 0, 0, 0));
                light->setAmbient(osg::Vec4f(1.5f, 1.5f, 1.5f, 1.f));

                bool isExterior = mPtr.isInCell() && mPtr.getCell()->getCell()->isExterior();
                SceneUtil::configureLight(light, radius, isExterior);

                mGlowLight = new SceneUtil::LightSource;
                mGlowLight->setNodeMask(Mask_Lighting);
                mInsert->addChild(mGlowLight);
                mGlowLight->setLight(light);
            }

            mGlowLight->setRadius(radius * cutoffMult);
        }
    }

    void Animation::addControllers()
    {
        mHeadController = addRotateController("bip01 head");
        mSpineController = addRotateController("bip01 spine1");
        mRootController = addRotateController("bip01");
    }

    osg::ref_ptr<RotateController> Animation::addRotateController(std::string_view bone)
    {
        auto iter = getNodeMap().find(bone);
        if (iter == getNodeMap().end())
            return nullptr;
        osg::MatrixTransform* node = iter->second;

        bool foundKeyframeCtrl = false;
        osg::Callback* cb = node->getUpdateCallback();
        while (cb)
        {
            if (dynamic_cast<NifAnimBlendController*>(cb) || dynamic_cast<BoneAnimBlendController*>(cb)
                || dynamic_cast<SceneUtil::KeyframeController*>(cb))
            {
                foundKeyframeCtrl = true;
                break;
            }
            cb = cb->getNestedCallback();
        }
        // Note: AnimBlendController also does the reset so if one is present - we should add the rotation node
        // Without KeyframeController the orientation will not be reseted each frame, so
        // RotateController shouldn't be used for such nodes.
        if (!foundKeyframeCtrl)
            return nullptr;

        osg::ref_ptr<RotateController> controller(new RotateController(mObjectRoot.get()));
        node->addUpdateCallback(controller);
        mActiveControllers.emplace_back(node, controller);
        return controller;
    }

    void Animation::setHeadPitch(float pitchRadians)
    {
        mHeadPitchRadians = pitchRadians;
    }

    void Animation::setHeadYaw(float yawRadians)
    {
        mHeadYawRadians = yawRadians;
    }

    float Animation::getHeadPitch() const
    {
        return mHeadPitchRadians;
    }

    float Animation::getHeadYaw() const
    {
        return mHeadYawRadians;
    }

    void Animation::removeFromScene()
    {
        removeFromSceneImpl();
    }

    bool Animation::useSmoothAnimationTransitions() const
    {
        return Settings::game().mSmoothAnimTransitions && !(VR::getVR() && mPtr == MWMechanics::getPlayer());
    }

    void Animation::removeFromSceneImpl()
    {
        // External keyframe callbacks hold animation/controller state that belongs to this Animation instance.
        // Detach them while both the scene nodes and controller sources are still alive.  Leaving an upper-body
        // overlay callback on the skeleton until member destruction can make OSG tear the callback chain down after
        // its AnimationTime source has already gone away (observed as a ucrtbase FAST_FAIL_INVALID_ARG on shutdown).
        detachActiveControllers();

        if (mGlowLight != nullptr)
            mInsert->removeChild(mGlowLight);

        if (mObjectRoot != nullptr)
            mInsert->removeChild(mObjectRoot);
    }

    MWWorld::MovementDirectionFlags Animation::getSupportedMovementDirections(
        std::span<const std::string_view> prefixes) const
    {
        MWWorld::MovementDirectionFlags result = 0;
        for (const std::string_view prefix : prefixes)
        {
            auto it = std::find_if(mSupportedDirections.begin(), mSupportedDirections.end(),
                [prefix](const auto& direction) { return direction.first == prefix; });
            if (it == mSupportedDirections.end())
            {
                mSupportedDirections.emplace_back(prefix, 0);
                it = mSupportedDirections.end() - 1;
                for (const std::string_view animation : mSupportedAnimations)
                {
                    if (!animation.starts_with(prefix))
                        continue;
                    if (animation.ends_with("forward"))
                        it->second |= MWWorld::MovementDirectionFlag_Forward;
                    else if (animation.ends_with("back"))
                        it->second |= MWWorld::MovementDirectionFlag_Back;
                    else if (animation.ends_with("left"))
                        it->second |= MWWorld::MovementDirectionFlag_Left;
                    else if (animation.ends_with("right"))
                        it->second |= MWWorld::MovementDirectionFlag_Right;
                }
            }
            result |= it->second;
        }
        return result;
    }

    // ------------------------------------------------------

    float Animation::AnimationTime::getValue(osg::NodeVisitor*)
    {
        if (mTimePtr)
            return *mTimePtr;
        return 0.f;
    }

    float EffectAnimationTime::getValue(osg::NodeVisitor*)
    {
        return mTime;
    }

    void EffectAnimationTime::addTime(float duration)
    {
        mTime += duration;
    }

    void EffectAnimationTime::resetTime(float time)
    {
        mTime = time;
    }

    float EffectAnimationTime::getTime() const
    {
        return mTime;
    }

    // --------------------------------------------------------------------------------

    ObjectAnimation::ObjectAnimation(const MWWorld::Ptr& ptr, const std::string& model,
        Resource::ResourceSystem* resourceSystem, bool animated, bool allowLight)
        : Animation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
    {
        if (!model.empty())
        {
            setObjectRoot(model, false, false, false);
            if (animated)
                addAnimSource(model, model);

            if (!ptr.getClass().getEnchantment(ptr).empty())
                mGlowUpdater = SceneUtil::addEnchantedGlow(
                    mObjectRoot, mResourceSystem, ptr.getClass().getEnchantmentColor(ptr));
        }
        if (ptr.getType() == ESM::Light::sRecordId && allowLight)
            addExtraLight(getOrCreateObjectRoot(), SceneUtil::LightCommon(*ptr.get<ESM::Light>()->mBase));
        if (ptr.getType() == ESM4::Light::sRecordId && allowLight)
            addExtraLight(getOrCreateObjectRoot(), SceneUtil::LightCommon(*ptr.get<ESM4::Light>()->mBase));

        if (!allowLight && mObjectRoot)
        {
            RemoveParticlesVisitor visitor;
            mObjectRoot->accept(visitor);
            visitor.remove();
        }

        if (Settings::game().mDayNightSwitches && SceneUtil::hasUserDescription(mObjectRoot, Constants::NightDayLabel))
        {
            AddSwitchCallbacksVisitor visitor;
            mObjectRoot->accept(visitor);
        }

        if (Settings::game().mGraphicHerbalism && ptr.getRefData().getCustomData() != nullptr
            && ObjectAnimation::canBeHarvested())
        {
            harvest(ptr);
        }
    }

    void ObjectAnimation::harvest(const MWWorld::Ptr& ptr)
    {
        const MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
        if (!store.hasVisibleItems())
        {
            HarvestVisitor visitor;
            mObjectRoot->accept(visitor);
        }
    }

    bool ObjectAnimation::canBeHarvested() const
    {
        if (mPtr.getType() != ESM::Container::sRecordId)
            return false;

        const MWWorld::LiveCellRef<ESM::Container>* ref = mPtr.get<ESM::Container>();
        if (!(ref->mBase->mFlags & ESM::Container::Organic))
            return false;

        return SceneUtil::hasUserDescription(mObjectRoot, Constants::HerbalismLabel);
    }

    // ------------------------------

    PartHolder::PartHolder(osg::ref_ptr<osg::Node> node)
        : mNode(std::move(node))
    {
    }

    PartHolder::~PartHolder()
    {
        if (mNode.get() && !mNode->getNumParents())
            Log(Debug::Verbose) << "Part \"" << mNode->getName() << "\" has no parents";

        if (mNode.get() && mNode->getNumParents())
        {
            if (mNode->getNumParents() > 1)
                Log(Debug::Verbose) << "Part \"" << mNode->getName() << "\" has multiple (" << mNode->getNumParents()
                                    << ") parents";
            mNode->getParent(0)->removeChild(mNode);
        }
    }
}
