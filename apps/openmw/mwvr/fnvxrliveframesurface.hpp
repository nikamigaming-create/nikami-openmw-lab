#ifndef OPENMW_MWVR_FNVXRLIVEFRAMESURFACE_H
#define OPENMW_MWVR_FNVXRLIVEFRAMESURFACE_H

#include <cstdint>
#include <chrono>
#include <array>
#include <memory>
#include <vector>

#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/PositionAttitudeTransform>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <components/stereo/types.hpp>
#include <components/vr/layer.hpp>
#include <components/vr/session.hpp>

namespace MWVR
{
    class FNVXRLiveFrameSurface : private VR::Session::Listener
    {
    public:
        static FNVXRLiveFrameSurface& instance();

        void init(osg::Group* geometryRoot);
        void update(osg::NodeVisitor* nv);

        bool updateFocus(osg::Node* focusNode, osg::Vec3f hitPoint);
        bool hasFocus() const { return mFocused; }
        bool visible() const { return mVisible; }
        bool modalInputActive() const { return mGripMenuOverride || (mVisible && !mRetailWorldMode); }
        void updateAimPointer();
        bool injectMouseClick();
        bool recenterMenuAnchor();
        bool recenterMenuPortal();
        void setGripMenuOverride(bool active);
        bool gripMenuOverride() const { return mGripMenuOverride; }

    private:
        FNVXRLiveFrameSurface() = default;
        ~FNVXRLiveFrameSurface();

        bool enabled() const;
        bool ensureVideoMapping();
        bool readFrame();
        void ensureSceneObjects();
        void updateTexture();
        void updateStereoTextures();
        void updatePlacement(bool retailWorldMode);
        void setVisible(bool visible);
        bool retailPanelAllowed();
        bool retailWorldActive();
        bool ensureRuntimeMapping();
        std::uint32_t retailRuntimePhase();
        bool retailRuntimeWorldReady();
        bool retailStereoWorldReady();
        bool ensureCameraMapping();
        bool ensurePlayerMapping();
        bool ensureStereoMapping();
        bool ensureVrPoseMapping();
        void publishVrPose(std::uint64_t predictedDisplayTime);
        void onSpaceUpdate() override;
        void onFrameUpdate(VR::Frame& frame) override;
        void onFrameEnd(osg::RenderInfo& info, VR::Frame& frame) override;
        bool projectionLayerEnabled() const;
        void requestProjectionLayer(bool active);
        bool ensureProjectionLayer();
        void uploadProjectionLayer(osg::RenderInfo& info);
        void markProjectionLayerUploadFailed(const char* reason);

        bool ensureDInputMapping();
        void publishPointer(float u, float v);
        void clearPointer();

        osg::ref_ptr<osg::Group> mGeometryRoot;
        osg::ref_ptr<osg::PositionAttitudeTransform> mTransform;
        osg::ref_ptr<osg::Geometry> mDebugGeometry;
        osg::ref_ptr<osg::Geometry> mGeometry;
        osg::ref_ptr<osg::Image> mImage;
        osg::ref_ptr<osg::Image> mStereoLeftImage;
        osg::ref_ptr<osg::Image> mStereoRightImage;
        osg::ref_ptr<osg::Texture2D> mTexture;
        osg::ref_ptr<osg::Texture2D> mStereoLeftTexture;
        osg::ref_ptr<osg::Texture2D> mStereoRightTexture;
        osg::ref_ptr<osg::StateSet> mDebugStateSet;
        osg::ref_ptr<osg::StateSet> mStateSet;

        std::vector<std::uint8_t> mPixels;
        std::vector<std::uint8_t> mStereoLeftPixels;
        std::vector<std::uint8_t> mStereoRightPixels;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::uint32_t mStereoWidth = 0;
        std::uint32_t mStereoHeight = 0;
        std::int32_t mLastSequence = 0;
        std::chrono::steady_clock::time_point mWorldReadySince{};
        std::chrono::steady_clock::time_point mRetailWorldLatchUntil{};
        bool mFocused = false;
        bool mVisible = false;
        bool mHudHiddenForRetailSurface = false;
        bool mHaveAnchorPose = false;
        bool mPointerActive = false;
        bool mWorldReadyTimerRunning = false;
        bool mRetailPanelArmed = false;
        bool mRetailWorldMode = false;
        bool mGripMenuOverride = false;
        bool mStereoFrameReady = false;
        bool mStereoFrameFresh = false;
        bool mUseStereoTextures = false;
        bool mCapturedStereoViewsValid = false;
        std::array<Stereo::View, 2> mCapturedStereoViews;
        bool mProjectionLayerRequested = false;
        bool mProjectionLayerReady = false;
        bool mProjectionLayerInserted = false;
        bool mRetailProjectionTakeoverActive = false;
        bool mLoggedProjectionLayer = false;
        bool mLoggedProjectionLayerUpload = false;
        int mRetailWorldReadyFrames = 0;
        bool mLoggedFrame = false;
        bool mLoggedDInput = false;
        bool mLoggedCamera = false;
        bool mLoggedRuntime = false;
        bool mLoggedStereo = false;
        bool mLoggedStereoReady = false;
        bool mLoggedStereoStale = false;
        bool mLoggedStereoWaiting = false;
        bool mLoggedCameraReadFailure = false;
        bool mLoggedCameraInvalid = false;
        bool mLoggedPlayer = false;
        bool mLoggedPlayerReadFailure = false;
        bool mLoggedPlayerInvalid = false;
        bool mLoggedPlayerSyncDisabled = false;
        bool mLoggedPlayerSyncBlocked = false;
        bool mLoggedPlayerSyncApplied = false;
        std::uint64_t mLastLoggedPlayerFrame = 0;
        std::uint32_t mLastLoggedPlayerFlags = 0;
        std::uint64_t mLastSyncedPlayerFrame = 0;
        std::uint32_t mLastSyncedCellFormId = 0;
        float mLastSyncedPlayerPos[3] {};
        int mFocusLogCount = 0;
        int mPointerLogCount = 0;
        Stereo::Pose mAnchorPose;
        std::shared_ptr<VR::ProjectionLayer> mProjectionLayer;
        std::array<std::shared_ptr<VR::Swapchain>, 2> mProjectionColorSwapchains;
        std::uint32_t mProjectionWidth = 0;
        std::uint32_t mProjectionHeight = 0;
        std::array<std::vector<std::uint8_t>, 2> mProjectionUploadPixels;
        std::chrono::steady_clock::time_point mLastStereoFreshTime{};
        std::uint64_t mLastPredictedDisplayTime = 0;

#ifdef _WIN32
        void* mVideoMapping = nullptr;
        std::uint8_t* mVideoView = nullptr;
        void* mDInputMapping = nullptr;
        void* mDInputView = nullptr;
        void* mCameraMapping = nullptr;
        void* mCameraView = nullptr;
        void* mPlayerMapping = nullptr;
        void* mPlayerView = nullptr;
        void* mRuntimeMapping = nullptr;
        void* mRuntimeView = nullptr;
        void* mStereoMapping = nullptr;
        std::uint8_t* mStereoView = nullptr;
        void* mVrPoseMapping = nullptr;
        void* mVrPoseView = nullptr;
        std::uint64_t mVrPoseFrame = 0;
        std::int32_t mLastStereoSequence = 0;
        std::int32_t mCurrentStereoSequence = 0;
        std::int32_t mUploadedStereoSequence = 0;
#endif
    };
}

#endif
