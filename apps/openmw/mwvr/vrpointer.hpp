#ifndef MWVR_POINTER_H
#define MWVR_POINTER_H

#include "../mwrender/renderingmanager.hpp"

#include <components/vr/space.hpp>

#include <osg/Node>
#include <osg/ref_ptr>

#include <memory>

#include <openxr/openxr.h>

namespace SceneUtil
{
    class PositionAttitudeTransform;
}

namespace osg
{
    class Geometry;
    class Group;
    class Transform;
    class MatrixTransform;
}

namespace MWVR
{
    class Crosshair
    {
    public:
        Crosshair(osg::ref_ptr<osg::Group> parent, osg::Vec3f color, float farAlpha, float nearAlpha, bool pyramid);
        ~Crosshair();

        void hide();
        void show();
        void setStretch(float stretch);
        void setWidth(float width);
        void setOffset(float distance);
        void setParent(osg::ref_ptr<osg::Group> parent);
        void updateMatrix();

        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<osg::Geometry> mGeometry;
        osg::ref_ptr<osg::PositionAttitudeTransform> mTransform;
        float mStretch;
        float mWidth;
        float mOffset;
        bool mVisible;
    };

    //! Controls the beam used to target/select objects.
    class UserPointer
    {

    public:
        UserPointer(osg::Group* root);
        ~UserPointer();

        // void updatePointerTarget();
        const MWRender::RayResult& getPointerRay() const;
        bool canPlaceObject() const;
        void setSource(std::shared_ptr<VR::Space> space);
        /// Sample the active pointer space during Session::updateSpaces, the legal OpenXR locate phase.
        void updateTrackingPose();
        // bool enabled() const { return !!mSource; };
        float distanceToPointerTarget() const { return mDistanceToPointerTarget; }
        void activate();
        void update();

    private:
        bool tryProbePick(MWWorld::Ptr target);

        std::shared_ptr<VR::Space> mSpace = 0;
        osg::ref_ptr<osg::MatrixTransform> mSpaceTransform;
        osg::ref_ptr<osg::Group> mRoot;

        Stereo::Pose mTrackingPose;
        bool mTrackingPoseValid = false;
        bool mTrackingStateInitialized = false;
        bool mLastLoggedTrackingPoseValid = false;

        MWRender::RayResult mPointerRay = {};
        MWRender::RayResult mLastPipBoyRay = {};
        osg::ref_ptr<osg::Node> mPointerTarget;
        osg::ref_ptr<osg::Node> mLastPipBoyTarget;
        osg::Vec3f mLastPipBoyDirection;
        unsigned int mPipBoyMissFrames = 0;
        bool mLastPipBoyRayValid = false;
        unsigned int mTrackingPoseMissFrames = 0;
        float mDistanceToPointerTarget = -1.f;
        bool mCanPlaceObject = false;
        bool mVisualStateInitialized = false;
        bool mLastVisualVisible = false;
        bool mLastVisualGuiMode = false;
        bool mLastVisualGuiFocus = false;
        int mVisualStateLogCount = 0;
        //bool mLeftHandEnabled = true;
        //bool mRightHandEnabled = true;
        std::unique_ptr<Crosshair> mCrosshair;
    };
}

#endif
