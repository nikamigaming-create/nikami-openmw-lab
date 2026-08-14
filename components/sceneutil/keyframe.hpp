#ifndef OPENMW_COMPONENTS_SCENEUTIL_KEYFRAME_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_KEYFRAME_HPP

#include <map>
#include <optional>
<<<<<<< HEAD
=======
#include <string>
#include <vector>
>>>>>>> origin/main

#include <osg/Object>

#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/textkeymap.hpp>

namespace SceneUtil
{
    /// @note Derived classes are expected to derive from osg::Callback and implement getAsCallback().
    class KeyframeController : public SceneUtil::Controller, public virtual osg::Object
    {
    public:
        KeyframeController() {}

<<<<<<< HEAD
        KeyframeController(const KeyframeController& copy, const osg::CopyOp& copyop)
=======
        KeyframeController(const KeyframeController& copy)
>>>>>>> origin/main
            : SceneUtil::Controller(copy)
        {
        }

        std::shared_ptr<float> mTime = std::make_shared<float>(0.0f);

        struct KfTransform
        {
            std::optional<osg::Vec3f> mTranslation;
            std::optional<osg::Quat> mRotation;
            std::optional<float> mScale;
        };

        virtual osg::Vec3f getTranslation(float time) const { return osg::Vec3f(); }

<<<<<<< HEAD
        virtual KfTransform getCurrentTransformation(osg::NodeVisitor* nv) { return KfTransform(); }
=======
        virtual KfTransform getCurrentTransformation(osg::NodeVisitor* nv) { return KfTransform(); };
>>>>>>> origin/main

        /// @note We could drop this function in favour of osg::Object::asCallback from OSG 3.6 on.
        virtual osg::Callback* getAsCallback() = 0;
    };

    /// Wrapper object containing an animation track as a ref-countable osg::Object.
    struct TextKeyMapHolder : public osg::Object
    {
    public:
        TextKeyMapHolder() {}
        TextKeyMapHolder(const TextKeyMapHolder& copy, const osg::CopyOp& copyop)
            : osg::Object(copy, copyop)
            , mTextKeys(copy.mTextKeys)
        {
        }

        TextKeyMap mTextKeys;

        META_Object(SceneUtil, TextKeyMapHolder)
    };

    /// Wrapper object containing the animation track and its KeyframeControllers.
    class KeyframeHolder : public osg::Object
    {
    public:
        KeyframeHolder() {}
        KeyframeHolder(const KeyframeHolder& copy, const osg::CopyOp& copyop)
            : mTextKeys(copy.mTextKeys)
            , mKeyframeControllers(copy.mKeyframeControllers)
<<<<<<< HEAD
=======
            , mFalloutHeadAnimTracks(copy.mFalloutHeadAnimTracks)
>>>>>>> origin/main
        {
        }

        TextKeyMap mTextKeys;

        META_Object(SceneUtil, KeyframeHolder)

        /// Controllers mapped to node name.
        typedef std::map<std::string, osg::ref_ptr<const KeyframeController>> KeyframeControllerMap;
        KeyframeControllerMap mKeyframeControllers;
<<<<<<< HEAD
=======

        struct FalloutHeadAnimTrack
        {
            enum class Type
            {
                Float,
                Bool
            };

            Type mType = Type::Float;
            float mDefaultValue = 0.f;
            std::vector<std::pair<float, float>> mKeys;
        };

        /// Fallout 3/New Vegas KF files store FaceGen/head animation channels as HeadAnims float/bool tracks.
        typedef std::map<std::string, FalloutHeadAnimTrack> FalloutHeadAnimTrackMap;
        FalloutHeadAnimTrackMap mFalloutHeadAnimTracks;
>>>>>>> origin/main
    };

}

#endif
