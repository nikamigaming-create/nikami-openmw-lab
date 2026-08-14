#ifndef OPENMW_COMPONENTS_SCENEUTIL_KEYFRAME_HPP
#define OPENMW_COMPONENTS_SCENEUTIL_KEYFRAME_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>

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

        KeyframeController(const KeyframeController& copy, const osg::CopyOp& copyop)
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

        virtual KfTransform getCurrentTransformation(osg::NodeVisitor* nv) { return KfTransform(); }

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
            , mFalloutHeadAnimTracks(copy.mFalloutHeadAnimTracks)
            , mNamedSequences(copy.mNamedSequences)
        {
        }

        TextKeyMap mTextKeys;

        META_Object(SceneUtil, KeyframeHolder)

        /// Controllers mapped to node name.
        typedef std::map<std::string, osg::ref_ptr<const KeyframeController>> KeyframeControllerMap;
        KeyframeControllerMap mKeyframeControllers;

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

        /// Bethesda object NIFs can keep multiple activation animations in an embedded
        /// NiControllerManager instead of separate KF files. Each sequence needs its own
        /// controller map because sequences such as Open and Close commonly target the
        /// same scene nodes with different tracks.
        typedef std::map<std::string, osg::ref_ptr<const KeyframeHolder>, std::less<>> NamedSequenceMap;
        NamedSequenceMap mNamedSequences;
    };

}

#endif
