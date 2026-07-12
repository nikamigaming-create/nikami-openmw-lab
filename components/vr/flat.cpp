#include "actionset.hpp"
#include "frame.hpp"
#include "layer.hpp"
#include "session.hpp"
#include "space.hpp"
#include "viewer.hpp"
#include "vr.hpp"

#include <components/xr/action.hpp>
#include <components/xr/actionset.hpp>
#include <components/xr/debug.hpp>
#include <components/xr/extensions.hpp>
#include <components/xr/instance.hpp>
#include <components/xr/interactionprofiles.hpp>
#include <components/xr/platform.hpp>
#include <components/xr/session.hpp>

#include <osg/FrameBufferObject>
#include <osg/State>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace VR
{
    namespace
    {
        bool sShouldRecenterXY = false;
        bool sShouldRecenterZ = false;

        class FlatSession final : public Session
        {
        public:
            std::shared_ptr<Swapchain> createSwapchain(
                uint32_t, uint32_t, uint32_t, uint32_t, Swapchain::Attachment, const std::string&) override
            {
                return {};
            }

            std::array<Stereo::View, 2> locateViews(int64_t, Space&) override { return {}; }

            std::array<SwapchainConfig, 2> getRecommendedSwapchainConfig() const override { return {}; }

            std::vector<ReferenceSpace> getSupportedReferenceSpaceTypes() const override { return {}; }

            void setReferenceWorldPose(Stereo::Pose) override {}

            std::shared_ptr<Space> getReferenceSpace(ReferenceSpace) override { return {}; }

        protected:
            void newFrame(uint64_t, bool& shouldSyncFrame, bool& shouldSyncInput) override
            {
                shouldSyncFrame = false;
                shouldSyncInput = false;
            }

            void syncFrameUpdate(
                uint64_t, bool& shouldRender, uint64_t& predictedDisplayTime, uint64_t& predictedDisplayPeriod) override
            {
                shouldRender = false;
                predictedDisplayTime = 0;
                predictedDisplayPeriod = 0;
            }

            void syncFrameRender(Frame&) override {}
            void syncFrameEnd(Frame&) override {}
        };
    }

    XrPath stringToXrPath(const std::string&)
    {
        return XR_NULL_PATH;
    }

    std::string xrPathToString(XrPath)
    {
        return {};
    }

    bool getVR()
    {
        return false;
    }

    bool getKBMouseModeActive()
    {
        return true;
    }

    bool getSteamVR()
    {
        return false;
    }

    bool getControllerActive(XrPath)
    {
        return false;
    }

    XrPath getControllerInteractionProfile(XrPath)
    {
        return XR_NULL_PATH;
    }

    bool getLeftControllerActive()
    {
        return false;
    }

    bool getRightControllerActive()
    {
        return false;
    }

    bool getLocatingSpacesAllowed()
    {
        return false;
    }

    bool getLeftHandedMode()
    {
        return false;
    }

    Stereo::Unit getPlayerHeight()
    {
        return {};
    }

    DisplayTime getPredictedDisplayTime()
    {
        return 0;
    }

    DisplayTime getPredictedDisplayPeriod()
    {
        return 0;
    }

    std::string getRuntimeName()
    {
        return "FLAT";
    }

    const char* getPreferredAimPath()
    {
        return Paths::RIGHT_HAND_AIM;
    }

    void setVR(bool) {}
    void setControllerActive(XrPath, XrPath, bool) {}
    void setSteamVR(bool) {}
    void setSneakOffsetEnabled(bool) {}
    void setPredictedDisplayTime(DisplayTime) {}
    void setPredictedDisplayPeriod(DisplayTime) {}
    void setLocatingSpacesAllowed(bool) {}
    void setRuntimeName(std::string) {}
    void setLeftHandedMode(bool) {}

    void recenterXY()
    {
        sShouldRecenterXY = true;
    }
    void recenterZ()
    {
        sShouldRecenterZ = true;
    }
    bool getShouldRecenterXY()
    {
        return sShouldRecenterXY;
    }
    bool getShouldRecenterZ()
    {
        return sShouldRecenterZ;
    }
    void setShouldRecenterXY(bool value)
    {
        sShouldRecenterXY = value;
    }
    void setShouldRecenterZ(bool value)
    {
        sShouldRecenterZ = value;
    }

    Frame::Frame() = default;
    Frame::~Frame() = default;

    ProjectionLayerView::ProjectionLayerView() = default;
    ProjectionLayerView::~ProjectionLayerView() = default;
    ProjectionLayer::ProjectionLayer() = default;
    ProjectionLayer::~ProjectionLayer() = default;
    QuadLayer::QuadLayer() = default;
    QuadLayer::~QuadLayer() = default;

    Session& Session::instance()
    {
        static FlatSession session;
        return session;
    }

    Session::Session() = default;
    Session::~Session() = default;

    void Session::processChangedSettings(const std::set<std::pair<std::string, std::string>>&) {}
    Frame Session::newFrame()
    {
        return {};
    }
    void Session::frameBeginUpdate(Frame&) {}
    void Session::updateSpaces() {}
    void Session::frameBeginRender(Frame&) {}
    void Session::frameEnd(osg::RenderInfo&, Frame&) {}
    void Session::swapBuffers(osg::GraphicsContext*, Frame&) {}
    void Session::computePlayerScale() {}
    void Session::setCharHeight(Stereo::Unit height)
    {
        mCharHeight = height;
    }
    void Session::instantTransition() {}
    void Session::setInteractionProfileActive(XrPath, XrPath, bool) {}
    bool Session::getInteractionProfileActive(XrPath) const
    {
        return false;
    }
    void Session::setSneak(bool) {}
    void Session::addListener(Listener*) {}
    void Session::removeListener(Listener*) {}
    void Session::recenter()
    {
        mRecenter = false;
    }
    void Session::readSettings() {}

    Session::Listener::Listener() = default;
    Session::Listener::~Listener() = default;

    Space::Space() = default;
    TrackingPose Space::locate(ReferenceSpace)
    {
        return {};
    }

    DerivedSpace::DerivedSpace(std::shared_ptr<Space> reference, Stereo::Pose pose)
        : mReference(std::move(reference))
        , mPose(pose)
    {
    }

    TrackingPose DerivedSpace::locate(Space&)
    {
        return {};
    }
    TrackingPose DerivedSpace::locateInWorld()
    {
        return {};
    }

    SpaceTransform::SpaceTransform() = default;

    SpaceTransform::SpaceTransform(std::shared_ptr<Space> space)
        : mSpace(std::move(space))
    {
    }

    SpaceTransform::SpaceTransform(const SpaceTransform& transform, const osg::CopyOp& copyOp)
        : osg::Transform(transform, copyOp)
        , mSpace(transform.mSpace)
    {
    }

    void SpaceTransform::setSpace(std::shared_ptr<Space> space)
    {
        mSpace = std::move(space);
    }

    bool SpaceTransform::computeLocalToWorldMatrix(osg::Matrix& matrix, osg::NodeVisitor*) const
    {
        if (_referenceFrame == RELATIVE_RF)
            matrix.preMult(mMatrix);
        else
            matrix = mMatrix;
        return true;
    }

    bool SpaceTransform::computeWorldToLocalMatrix(osg::Matrix& matrix, osg::NodeVisitor*) const
    {
        const osg::Matrix inverse = osg::Matrix::inverse(mMatrix);
        if (_referenceFrame == RELATIVE_RF)
            matrix.postMult(inverse);
        else
            matrix = inverse;
        return true;
    }

    void SpaceTransform::onSpaceUpdate()
    {
        mMatrix.makeIdentity();
    }

    struct SwapBuffersCallback : osg::Referenced
    {
    };
    struct InitialDrawCallback : osg::Referenced
    {
    };
    struct FinaldrawCallback : osg::Referenced
    {
    };
    struct UpdateViewCallback : osg::Referenced
    {
    };

    namespace
    {
        Viewer* sViewer = nullptr;
    }

    Viewer& Viewer::instance()
    {
        if (!sViewer)
            throw std::logic_error("VR viewer is unavailable in a flat build");
        return *sViewer;
    }

    Viewer::Viewer(std::shared_ptr<Session> session, osg::ref_ptr<osgViewer::Viewer> viewer)
        : mSession(std::move(session))
        , mViewer(std::move(viewer))
    {
        sViewer = this;
    }

    Viewer::~Viewer()
    {
        if (sViewer == this)
            sViewer = nullptr;
    }

    void Viewer::configureCallbacks() {}
    void Viewer::processChangedSettings(const std::set<std::pair<std::string, std::string>>&) {}
    void Viewer::insertLayer(std::shared_ptr<Layer>) {}
    void Viewer::removeLayer(std::shared_ptr<Layer>) {}
    void Viewer::setPrimaryProjectionLayerEnabled(bool enabled)
    {
        mPrimaryProjectionLayerEnabled = enabled;
    }
    osg::ref_ptr<osg::FrameBufferObject> Viewer::getFboForView(Stereo::Eye)
    {
        return {};
    }
    void Viewer::submitDepthForView(osg::State&, osg::FrameBufferObject*, Stereo::Eye) {}
}

namespace XR
{
    struct PlatformPrivate
    {
    };

    XrResult CheckXrResult(XrResult result, const char*, const char*)
    {
        return result;
    }

    Action::~Action() = default;

    ActionSet::ActionSet(const std::string& actionSetName)
        : mLocalizedName(actionSetName)
        , mInternalName(actionSetName)
    {
    }

    ActionSet::~ActionSet() = default;
    bool ActionSet::update()
    {
        return false;
    }
    void ActionSet::applyHaptics(const std::string&, float) {}
    std::vector<XrActionSuggestedBinding> ActionSet::suggestBindings()
    {
        return {};
    }
    void ActionSet::createBoolAction(const std::string&, const std::string&, const std::string&) {}
    void ActionSet::createAxisAction(const std::string&, const std::string&, const std::string&) {}
    void ActionSet::createFloatAction(const std::string&, const std::string&, const std::string&) {}
    void ActionSet::createPoseAction(const std::string&, const std::string&, const std::string&) {}
    void ActionSet::createHapticsAction(const std::string&, const std::string&, const std::string&) {}
    std::optional<InputAction::Value> ActionSet::getValue(const std::string&) const
    {
        return std::nullopt;
    }
    void ActionSet::suggestBinding(const std::string&, const std::string&) {}
    std::shared_ptr<VR::Space> ActionSet::createActionSpace(const std::string&, const std::string&, Stereo::Pose)
    {
        return {};
    }

    Extensions& Extensions::instance()
    {
        throw std::logic_error("OpenXR extensions are unavailable in a flat build");
    }

    Extensions::~Extensions() = default;
    bool Extensions::extensionEnabled(const std::string&) const
    {
        return false;
    }

    Instance& Instance::instance()
    {
        throw std::logic_error("OpenXR is unavailable in a flat build");
    }

    Instance::Instance(osg::GraphicsContext*, SDL_Window*) {}
    Instance::~Instance() = default;

    Platform& Instance::platform()
    {
        throw std::logic_error("OpenXR is unavailable in a flat build");
    }

    std::shared_ptr<Session> Instance::createSession()
    {
        return {};
    }

    Platform::~Platform() = default;

    Session& Session::instance()
    {
        throw std::logic_error("OpenXR session is unavailable in a flat build");
    }

    const char* ValueTypeToString(Interaction::ValueType valueType)
    {
        switch (valueType)
        {
            case Interaction::BOOLEAN:
                return "BOOLEAN";
            case Interaction::FLOAT:
                return "FLOAT";
            case Interaction::AXIS:
                return "AXIS";
            case Interaction::POSE:
                return "POSE";
        }
        return "UNKNOWN";
    }

    const InteractionProfiles& getAllKnownInteractionProfiles()
    {
        static const InteractionProfiles profiles;
        return profiles;
    }

    void loadInteractionProfiles(const std::filesystem::path&, const std::filesystem::path&) {}
}

extern "C"
{
    XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char*, XrPath* path)
    {
        if (path)
            *path = XR_NULL_PATH;
        return XR_ERROR_RUNTIME_UNAVAILABLE;
    }

    XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
        XrInstance, const XrInteractionProfileSuggestedBinding*)
    {
        return XR_ERROR_RUNTIME_UNAVAILABLE;
    }

    XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*)
    {
        return XR_ERROR_RUNTIME_UNAVAILABLE;
    }
}
