#ifndef GAME_MWRENDER_STANDALONEMODELPREVIEW_H
#define GAME_MWRENDER_STANDALONEMODELPREVIEW_H

#include <cstdint>
#include <string>

#include <osg/BoundingBox>
#include <osg/PositionAttitudeTransform>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Group;
    class Texture2D;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class StandaloneModelDrawOnceCallback;
    class StandaloneModelRTTNode;

    struct StandaloneModelPreviewSettings
    {
        std::string mModel;
        osg::Vec3f mRotation = osg::Vec3f(0.f, 0.f, 0.f);
        float mScale = 1.f;
        std::uint32_t mWidth = 1280;
        std::uint32_t mHeight = 720;
        osg::Vec4f mClearColor = osg::Vec4f(0.22f, 0.23f, 0.24f, 1.f);
        osg::Vec3f mCameraDirection = osg::Vec3f(0.f, 1.f, 0.f);
        float mCameraDistanceMultiplier = 1.f;
    };

    struct StandaloneModelPreviewState
    {
        VFS::Path::Normalized mCorrectedModel;
        bool mBoundsValid = false;
        osg::BoundingBox mBounds;
        osg::Vec3f mBoundsCenter = osg::Vec3f(0.f, 0.f, 0.f);
        osg::Vec3f mBoundsSize = osg::Vec3f(0.f, 0.f, 0.f);
        float mFrameRadius = 16.f;
        osg::Vec3f mCameraPosition = osg::Vec3f(0.f, 0.f, 0.f);
        osg::Vec3f mLookAt = osg::Vec3f(0.f, 0.f, 0.f);
    };

    class StandaloneModelPreview
    {
    public:
        StandaloneModelPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
            const StandaloneModelPreviewSettings& settings);
        ~StandaloneModelPreview();

        void rebuild(const StandaloneModelPreviewSettings& settings);
        void redraw();

        osg::Texture2D* getTexture();
        const StandaloneModelPreviewState& getState() const { return mState; }

    private:
        StandaloneModelPreview(const StandaloneModelPreview&) = delete;
        StandaloneModelPreview& operator=(const StandaloneModelPreview&) = delete;

        void setupScene();

        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        StandaloneModelPreviewSettings mSettings;
        StandaloneModelPreviewState mState;
        osg::ref_ptr<StandaloneModelRTTNode> mRTTNode;
        osg::ref_ptr<StandaloneModelDrawOnceCallback> mDrawOnceCallback;
        osg::ref_ptr<osg::PositionAttitudeTransform> mModelNode;
    };
}

#endif
