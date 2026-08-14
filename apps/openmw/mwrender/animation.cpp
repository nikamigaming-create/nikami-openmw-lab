Warning: truncated output (original token count: 100107)
Total output lines: 8269

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
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/PolygonMode>
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
#include <components/nifosg/falloutkf.hpp>

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
#include "../mwmechanics/creaturestats.hpp"
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

    bool isFalloutDeathFallbackContext(const MWWorld::Ptr& ptr)
    {
        if (isFalloutNpcAnimationContext(ptr))
            return true;
        if (ptr.getType() != ESM4::Creature::sRecordId)
            return false;

        const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>();
        return ref != nullptr && ref->mBase != nullptr && ref->mBase->mIsFONV;
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
        coâ€¦70107 tokens truncatedâ€¦ne->mNode;
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

        applyFalloutDeathPoseFallback();

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

    void Animation::applyFalloutDeathPoseFallback()
    {
        if (mFalloutCorpseTransform == nullptr || !isFalloutDeathFallbackContext(mPtr)
            || !mPtr.getClass().isActor())
            return;

        const bool dead = mPtr.getClass().getCreatureStats(mPtr).isDead();
        if (!dead)
        {
            if (mFalloutCorpsePoseApplied)
            {
                mFalloutCorpseTransform->setMatrix(osg::Matrixf::identity());
                mFalloutCorpseTransform->dirtyBound();
                mFalloutCorpsePoseApplied = false;
                Log(Debug::Info) << "FNV death fallback: actor=" << mPtr.getCellRef().getRefId()
                                 << " state=resurrected pose=identity";
            }
            return;
        }

        static constexpr std::array<std::string_view, 5> sDeathGroups{
            "death1", "death2", "death3", "death4", "death5"
        };
        if (std::any_of(sDeathGroups.begin(), sDeathGroups.end(),
                [&](std::string_view group) { return hasAnimation(group); }))
            return;
        if (mFalloutCorpsePoseApplied || mFalloutCorpseTransform->getNumChildren() == 0)
            return;

        osg::ComputeBoundsVisitor boundsVisitor;
        mFalloutCorpseTransform->getChild(0)->accept(boundsVisitor);
        const osg::BoundingBox bounds = boundsVisitor.getBoundingBox();
        if (!bounds.valid())
        {
            Log(Debug::Warning) << "FNV death fallback: actor=" << mPtr.getCellRef().getRefId()
                                << " state=failed reason=invalid-bounds";
            return;
        }

        const osg::Vec3f pivot = bounds.center();
        const std::string refId = mPtr.getCellRef().getRefId().serializeText();
        const float direction = !refId.empty() && (static_cast<unsigned char>(refId.back()) & 1u) ? -1.f : 1.f;
        osg::Matrixf pose = osg::Matrixf::translate(-pivot)
            * osg::Matrixf::rotate(direction * osg::PI_2, osg::Vec3f(0.f, 1.f, 0.f))
            * osg::Matrixf::translate(pivot);

        float transformedMinZ = std::numeric_limits<float>::max();
        for (unsigned int corner = 0; corner < 8; ++corner)
            transformedMinZ = std::min(transformedMinZ, (bounds.corner(corner) * pose).z());
        const float groundShift = bounds.zMin() - transformedMinZ;
        pose = pose * osg::Matrixf::translate(0.f, 0.f, groundShift);

        mFalloutCorpseTransform->setMatrix(pose);
        mFalloutCorpseTransform->dirtyBound();
        mFalloutCorpsePoseApplied = true;
        Log(Debug::Info) << "FNV combat death: actor=" << mPtr.getCellRef().getRefId()
                         << " dead=1 visual=procedural-grounded-side-pose status=pass angle="
                         << direction * 90.f << " groundShift=" << groundShift
                         << " boundsMinZ=" << bounds.zMin() << " boundsMaxZ=" << bounds.zMax();
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
        mFalloutCorpseTransform = nullptr;
        mFalloutCorpsePoseApplied = false;

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

        if (isFalloutDeathFallbackContext(mPtr))
        {
            mInsert->removeChild(mObjectRoot);
            mFalloutCorpseTransform = new osg::MatrixTransform;
            mFalloutCorpseTransform->setName("FNV Actor Corpse Pose");
            mFalloutCorpseTransform->addChild(mObjectRoot);
            mObjectRoot = mFalloutCorpseTransform;
            mInsert->addChild(mObjectRoot);
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

    void Animation::setFalloutVatsWireframes(
        std::span<const std::string_view> targetNodes, std::string_view selectedNode, bool enabled)
    {
        class FalloutVatsHighlightVisitor : public osg::NodeVisitor
        {
        public:
            FalloutVatsHighlightVisitor(
                std::span<const std::string_view> targetNodes, std::string_view selectedNode, bool enabled)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mTargetNodes(targetNodes)
                , mSelectedNode(selectedNode)
                , mEnabled(enabled)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
                    highlight(geode.getDrawable(index));
                traverse(geode);
            }

            void apply(osg::Drawable& drawable) override
            {
                highlight(&drawable);
            }

            std::size_t mHighlightedRigs{ 0 };

        private:
            std::span<const std::string_view> mTargetNodes;
            std::string_view mSelectedNode;
            bool mEnabled;
            std::unordered_set<SceneUtil::RigGeometry*> mVisited;

            void highlight(osg::Drawable* drawable)
            {
                SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable);
                if (rig == nullptr || !mVisited.insert(rig).second)
                    return;
                if (rig->setFalloutVatsHighlight(mTargetNodes, mSelectedNode, mEnabled))
                    ++mHighlightedRigs;
            }
        } visitor(targetNodes, selectedNode, enabled);

        if (mObjectRoot)
            mObjectRoot->accept(visitor);
        Log(Debug::Info) << "FNV VATS: skinned highlight enabled=" << enabled
                         << " rigs=" << visitor.mHighlightedRigs
                         << " bodyParts=" << targetNodes.size()
                         << " selected=" << selectedNode;
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
