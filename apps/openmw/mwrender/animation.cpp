#include "animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Switch>

#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystem>

#include <osgAnimation/Bone>
#include <osgAnimation/UpdateBone>

#include <components/debug/debuglog.hpp>

#include <components/esm/defs.hpp>

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

#include <components/misc/constants.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/nifosg/matrixtransform.hpp>

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

    float matrixDifference(const osg::Matrixf& left, const osg::Matrixf& right)
    {
        float result = 0.f;
        const float* leftPtr = left.ptr();
        const float* rightPtr = right.ptr();
        for (int i = 0; i < 16; ++i)
            result = std::max(result, std::abs(leftPtr[i] - rightPtr[i]));
        return result;
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
            if (!node.getName().empty())
                mTargets[Misc::StringUtils::lowerCase(node.getName())].push_back(&node);
            traverse(node);
        }

    private:
        std::unordered_map<std::string, std::vector<osg::MatrixTransform*>>& mTargets;
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
            position = osg::componentMultiply(mResetAxes, position);
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

    private:
        osg::Vec3f mResetAxes;
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
    {
        for (size_t i = 0; i < sNumBlendMasks; i++)
            mAnimationTimePtr[i] = std::make_shared<AnimationTime>();

        mLightListCallback = new SceneUtil::LightListCallback;
    }

    Animation::~Animation()
    {
        removeFromSceneImpl();
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
            "Bip01 Spine1", /* Torso */
            "Bip01 L Clavicle", /* Left arm */
            "Bip01 R Clavicle", /* Right arm */
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

        return {};
    }

    Animation::NodeMap::const_iterator findNodeMapBone(
        const Animation::NodeMap& nodeMap, const std::string& name, std::string& resolvedName)
    {
        Animation::NodeMap::const_iterator found = nodeMap.find(name);
        if (found != nodeMap.end())
        {
            resolvedName = found->first;
            return found;
        }

        for (Animation::NodeMap::const_iterator it = nodeMap.begin(); it != nodeMap.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->first, name))
            {
                resolvedName = it->first;
                return it;
            }
        }

        return nodeMap.end();
    }

    Animation::NodeMap::const_iterator findFonvAnimationBone(
        const Animation::NodeMap& nodeMap, const std::string& name, std::string& resolvedName)
    {
        for (const std::string& alias : getFonvBoneAliases(name))
        {
            Animation::NodeMap::const_iterator found = findNodeMapBone(nodeMap, alias, resolvedName);
            if (found != nodeMap.end())
                return found;
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
            return std::string("splitKeyXZThenBind");
        }();
        return sMode;
    }

    float getFalloutIdleSeedSeconds()
    {
        if (std::getenv("OPENMW_FNV_DISABLE_IDLE_SEED") != nullptr)
            return -1.f;

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
            if (isFalloutLowerBodyBone(lowerBone))
                return rotation;
            if (isFalloutSpineBone(lowerBone))
                return bindRotation * rotation;
            return rotation * falloutHalfTurn('x') * bindRotation;
        }

        const std::string& mode = getFalloutRotationMode();
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
        if (mode == "rawKey")
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
        return rotation * bindRotation;
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
        return lowerBone == "bip01" || lowerBone == "bip01 pelvis" || lowerBone == "bip01 spine"
            || lowerBone == "bip01 spine1" || lowerBone == "bip01 spine2" || lowerBone == "bip01 neck"
            || lowerBone == "bip01 head" || lowerBone == "bip01 l upperarm" || lowerBone == "bip01 r upperarm"
            || lowerBone == "bip01 l forearm" || lowerBone == "bip01 r forearm" || lowerBone == "bip01 l hand"
            || lowerBone == "bip01 r hand" || lowerBone == "bip01 l thigh" || lowerBone == "bip01 r thigh";
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

        if (sample.mSpine2 > 105.f)
            sample.mReason = "spine2";
        else if (sample.mHead > 105.f)
            sample.mReason = "head";
        else if (sample.mLeftThigh > 125.f || sample.mRightThigh > 125.f)
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

    bool shouldApplyFalloutAccumulationRotation()
    {
        return std::getenv("OPENMW_FNV_APPLY_ACCUM_ROTATION") != nullptr;
    }

    bool shouldUseNativeFalloutAnimationCallbacks()
    {
        return std::getenv("OPENMW_FNV_NATIVE_ANIM_CALLBACKS") != nullptr;
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

        if (stem == "mtidle" || stem == "idle" || Misc::StringUtils::ciEndsWith(stem, "idle"))
            return "idle";
        if (Misc::StringUtils::ciEndsWith(stem, "turnleft"))
            return "turnleft";
        if (Misc::StringUtils::ciEndsWith(stem, "turnright"))
            return "turnright";
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
        if (stem.find("attack") != std::string::npos)
            return "attack1";

        return {};
    }

    void addSyntheticLoopingTextKeys(SceneUtil::TextKeyMap& textkeys, const std::string& group)
    {
        constexpr float start = 0.f;
        constexpr float stop = 4.f;
        textkeys.emplace(start, group + ": start");
        textkeys.emplace(start, group + ": loop start");
        textkeys.emplace(stop, group + ": loop stop");
        textkeys.emplace(stop, group + ": stop");
    }

    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(
        const std::string& kfname, const std::string& baseModel, bool falloutProcedureIdle)
    {
        if (!mResourceSystem->getVFS()->exists(kfname))
            return nullptr;

        auto animsrc = std::make_shared<AnimSource>();
        animsrc->mKeyframes = mResourceSystem->getKeyframeManager()->get(VFS::Path::toNormalized(kfname));

        std::string lowerKf = Misc::StringUtils::lowerCase(kfname);
        std::string lowerBaseModel = Misc::StringUtils::lowerCase(baseModel);
        const bool isFonvActorAnim = lowerKf.find("meshes/characters/_male/") != std::string::npos
            || lowerBaseModel.find("characters\\_male\\") != std::string::npos
            || lowerBaseModel.find("characters/_male/") != std::string::npos;
        const bool isFonvCreatureAnim = lowerKf.find("meshes/creatures/") != std::string::npos
            || lowerBaseModel.find("meshes\\creatures\\") != std::string::npos
            || lowerBaseModel.find("meshes/creatures/") != std::string::npos;
        const bool isFonvAnim = isFonvActorAnim || isFonvCreatureAnim;

        if (animsrc->mKeyframes && animsrc->mKeyframes->mTextKeys.empty()
            && !animsrc->mKeyframes->mKeyframeControllers.empty() && isFonvCreatureAnim)
        {
            const std::string group = getFalloutSyntheticGroupFromKf(kfname);
            if (!group.empty())
            {
                osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                    = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                addSyntheticLoopingTextKeys(keyframes->mTextKeys, group);
                animsrc->mKeyframes = keyframes;
                Log(Debug::Info) << "FNV/ESM4 diag: synthesized creature KF text key group '" << group
                                 << "' for " << kfname;
            }
        }

        if (!animsrc->mKeyframes || animsrc->mKeyframes->mTextKeys.empty()
            || animsrc->mKeyframes->mKeyframeControllers.empty())
            return nullptr;

        const NodeMap& nodeMap = getNodeMap();
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        unsigned int matchedControllers = 0;
        unsigned int missingControllers = 0;
        for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
             it != controllerMap.end(); ++it)
        {
            std::string bonename = Misc::StringUtils::lowerCase(it->first);
            NodeMap::const_iterator found = isFonvAnim ? findNodeMapBone(nodeMap, bonename, bonename)
                                                       : nodeMap.find(bonename);
            if (found == nodeMap.end() && isFonvActorAnim)
            {
                const std::string originalName = bonename;
                found = findFonvAnimationBone(nodeMap, originalName, bonename);
                if (found != nodeMap.end())
                    Log(Debug::Info) << "FNV/ESM4 diag: aliased KF bone '" << originalName << "' to '" << bonename
                                     << "' for " << kfname;
            }
            if (found == nodeMap.end())
            {
                ++missingControllers;
                Log(Debug::Warning) << "Warning: addAnimSource: can't find bone '" + bonename << "' in " << baseModel
                                    << " (referenced by " << kfname << ")";
                continue;
            }
            ++matchedControllers;

            osg::Node* node = found->second;

            size_t blendMask = detectBlendMask(node, it->second->getName());

            // clone the controller, because each Animation needs its own ControllerSource
            osg::ref_ptr<SceneUtil::KeyframeController> cloned
                = osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY);
            cloned->setSource(mAnimationTimePtr[blendMask]);

            animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
        }
        if (isFonvAnim)
        {
            const bool isProcedureIdle = falloutProcedureIdle && lowerKf.find("idleanims/") != std::string::npos
                && (lowerKf.find("dynamicidle_sit") != std::string::npos
                    || lowerKf.find("sitchair") != std::string::npos
                    || lowerKf.find("sittablechair") != std::string::npos
                    || lowerKf.find("dynamicidle_sleep") != std::string::npos);
            if (isProcedureIdle && !animsrc->getTextKeys().hasGroupStart("idle"))
            {
                osg::ref_ptr<SceneUtil::KeyframeHolder> keyframes
                    = new SceneUtil::KeyframeHolder(*animsrc->mKeyframes, osg::CopyOp::SHALLOW_COPY);
                keyframes->mTextKeys.emplace(0.f, "idle: start");
                keyframes->mTextKeys.emplace(0.f, "idle: loop start");
                keyframes->mTextKeys.emplace(4.f, "idle: loop stop");
                keyframes->mTextKeys.emplace(4.f, "idle: stop");
                animsrc->mKeyframes = keyframes;
                Log(Debug::Info) << "FNV/ESM4 diag: synthesized idle text keys for procedure source " << kfname;
            }
            animsrc->mFalloutProcedureIdle = isProcedureIdle;

            Log(Debug::Info) << "FNV/ESM4 diag: animation source " << kfname << " bound " << matchedControllers << "/"
                             << controllerMap.size() << " controller(s) to " << baseModel << ", missing "
                             << missingControllers;
            std::ostringstream groups;
            unsigned int groupCount = 0;
            for (const std::string& group : animsrc->getTextKeys().getGroups())
            {
                if (groupCount != 0)
                    groups << ",";
                groups << group;
                ++groupCount;
            }
            Log(Debug::Info) << "FNV/ESM4 diag: animation source " << kfname << " groups=[" << groups.str() << "]";
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
                Log(Debug::Info) << "FNV/ESM4 diag: animation source " << kfname << " headAnimTracks="
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
        const bool falloutNpc = isFalloutActor(mPtr);
        if (!mObjectRoot || mAnimSources.empty())
        {
            if (falloutNpc)
                Log(Debug::Warning) << "FNV/ESM4 diag: play request for " << mPtr.getCellRef().getRefId()
                                    << " group '" << groupname << "' ignored objectRoot=" << static_cast<bool>(mObjectRoot)
                                    << " animSources=" << mAnimSources.size();
            return;
        }

        if (falloutNpc)
            Log(Debug::Info) << "FNV/ESM4 diag: play request for " << mPtr.getCellRef().getRefId() << " group '"
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
                Log(Debug::Info) << "FNV/ESM4 diag: play reused active group '" << groupname << "' for "
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
                if (isFalloutNpc(mPtr) && groupname == "idle" && startpoint == 0.f
                    && state.mStopTime > state.mStartTime)
                {
                    const float idleSeedSeconds = getFalloutIdleSeedSeconds();
                    if (idleSeedSeconds >= 0.f)
                    {
                        const float seededIdleTime
                            = std::min(state.mStartTime + idleSeedSeconds, state.mStopTime - 0.01f);
                        if (seededIdleTime > state.getTime())
                        {
                            Log(Debug::Info) << "FNV/ESM4 diag: seeding Fallout idle pose for "
                                             << mPtr.getCellRef().getRefId() << " from=" << state.getTime()
                                             << " to=" << seededIdleTime << " seconds=" << idleSeedSeconds;
                            state.setTime(seededIdleTime);
                            state.mPlaying = (state.getTime() < state.mStopTime);
                        }
                    }
                    else
                        Log(Debug::Info) << "FNV/ESM4 diag: Fallout idle seed disabled for "
                                         << mPtr.getCellRef().getRefId();
                }
                mStates[std::string{ groupname }] = state;

                if (falloutNpc)
                {
                    size_t controllerCount = 0;
                    for (size_t mask = 0; mask < sNumBlendMasks; ++mask)
                        if (state.blendMaskContains(mask))
                            controllerCount += state.mSource->mControllerMap[mask].size();

                    Log(Debug::Info) << "FNV/ESM4 diag: play matched " << mPtr.getCellRef().getRefId() << " group '"
                                     << groupname << "' checkedSources=" << checkedSources << " controllers="
                                     << controllerCount << " startTime=" << state.mStartTime
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
                if (isFalloutNpc(mPtr))
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

    void Animation::resetActiveGroups()
    {
//## VR_PATCH BEGIN
        const bool isPlayer = (mPtr == MWMechanics::getPlayer());

//## VR_PATCH END
        const bool falloutNpc = isFalloutNpc(mPtr);
        size_t falloutAddedControllers = 0;
        // remove all previous external controllers from the scene graph
        for (auto it = mActiveControllers.begin(); it != mActiveControllers.end(); ++it)
        {
            osg::Node* node = it->first;
            node->removeUpdateCallback(it->second);

            // Should be no longer needed with OSG 3.4
            it->second->setNestedCallback(nullptr);
        }

        mActiveControllers.clear();

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

                    const bool useSmoothAnims = useSmoothAnimationTransitions();

                    osg::Callback* callback = it->second->getAsCallback();
                    if (useSmoothAnims)
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
                    const bool addSceneGraphCallback = (!falloutNpc || shouldUseNativeFalloutAnimationCallbacks())
                        && (!isPlayer || !vrOverride(active->first, it->first));
                    if (addSceneGraphCallback)
//## VR_PATCH END
                    {
                        node->addUpdateCallback(callback);
                        mActiveControllers.emplace_back(node, callback);
                        if (falloutNpc)
                            ++falloutAddedControllers;
                    }

                    if (blendMask == 0 && node == mAccumRoot)
                    {
                        mAccumCtrl = it->second;

                        if (!falloutNpc)
                        {
                            // make sure reset is last in the chain of callbacks
                            if (!mResetAccumRootCallback)
                            {
                                mResetAccumRootCallback = new ResetAccumRootCallback;
                                mResetAccumRootCallback->setAccumulate(mAccumulate);
                            }
                            mAccumRoot->addUpdateCallback(mResetAccumRootCallback);
                            mActiveControllers.emplace_back(mAccumRoot, mResetAccumRootCallback);
                        }
                    }
                }
            }
        }

        if (falloutNpc)
            Log(Debug::Info) << "FNV/ESM4 diag: active animation group reset for " << mPtr.getCellRef().getRefId()
                             << " states=" << mStates.size() << " callbacks=" << falloutAddedControllers
                             << " activeControllers=" << mActiveControllers.size();

        addControllers();
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
        const bool falloutNpc = isFalloutNpc(mPtr);
        const bool falloutActor = isFalloutActor(mPtr);
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
            if (falloutActor && state.mGroupname == "idle" && shouldFreezeFalloutIdleAnimation())
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
                        Log(Debug::Info) << "FNV/ESM4 diag: idle time advanced for " << mPtr.getCellRef().getRefId()
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
            const std::string refIdText = mPtr.getCellRef().getRefId().serializeText();
            const bool matrixAudit = std::getenv("OPENMW_FNV_MATRIX_AUDIT") != nullptr
                && (refIdText.find("4104c7f") != std::string::npos || refIdText == "player");
            unsigned int matrixAuditLines = 0;
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
                    auto nodeIt = getNodeMap().find(it->first);
                    if (nodeIt == getNodeMap().end())
                        continue;

                    osg::MatrixTransform* transform = nodeIt->second.get();
                    if (transform == nullptr)
                        continue;

                    const std::string lowerAppliedBone = Misc::StringUtils::lowerCase(it->first);
                    const osg::Matrixf before = transform->getMatrix();
                    SceneUtil::KeyframeController::KfTransform keyframe = it->second->getCurrentTransformation(nullptr);
                    const bool accumulationBone = isFalloutAccumulationBone(lowerAppliedBone);
                    const bool applyBoneTranslation = shouldApplyFalloutBoneTranslations() && !accumulationBone
                        && keyframe.mTranslation
                        && isSafeFalloutBoneTranslation(*keyframe.mTranslation, before.getTrans());
                    if (accumulationBone && !shouldApplyFalloutAccumulationRotation())
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
                                if (matrixAudit && matrixAuditLines < 40 && isFalloutMatrixAuditBone(lowerAppliedBone))
                                {
                                    const osg::Quat rawKey = *keyframe.mRotation;
                                    const osg::Quat bind = getFalloutBindRotation(transform);
                                    Log(Debug::Info)
                                        << "FNV/ESM4 MATRIX AUDIT " << mPtr.getCellRef().getRefId()
                                        << " transform=NifOsg"
                                        << " blendMask=" << blendMask
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

                        if (matrixAudit && matrixAuditLines < 40 && isFalloutMatrixAuditBone(lowerAppliedBone))
                        {
                            const osg::Quat rawKey = keyframe.mRotation ? *keyframe.mRotation : osg::Quat();
                            const osg::Quat bind = getFalloutBindRotation(transform);
                            Log(Debug::Info)
                                << "FNV/ESM4 MATRIX AUDIT " << mPtr.getCellRef().getRefId()
                                << " transform=osg"
                                << " blendMask=" << blendMask
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

            if (mSkeleton)
                mSkeleton->markBoneMatriceDirty();

            static std::unordered_map<std::string, unsigned int> sFalloutManualApplyLogs;
            const std::string refId = mPtr.getCellRef().getRefId().serializeText();
            unsigned int& logs = sFalloutManualApplyLogs[refId];
            if (appliedControllers > 0 && logs < 3)
            {
                ++logs;
                const FalloutPoseSemanticSample poseSemantic = sampleFalloutPoseSemantics(duplicateTransformTargets);
                Log(Debug::Info) << "FNV/ESM4 diag: manually applied " << appliedControllers
                                 << " active keyframe controller(s) for " << mPtr.getCellRef().getRefId()
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
                                 << " maxTorsoBone=" << maxTorsoMatrixDeltaBone;
                Log(Debug::Info) << "FNV/ESM4 diag: semantic pose for " << mPtr.getCellRef().getRefId()
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
        mActiveControllers.clear();
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

        if (!forceskeleton)
        {
            osg::ref_ptr<osg::Node> created
                = getModelInstance(mResourceSystem, model, baseonly, inject, defaultSkeleton);
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
            osg::ref_ptr<osg::Node> created
                = getModelInstance(mResourceSystem, model, baseonly, inject, defaultSkeleton);
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
        NodeMap::const_iterator found = getNodeMap().find(name);
        if (found == getNodeMap().end())
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
