#ifndef MWVR_VRANIMATION_H
#define MWVR_VRANIMATION_H

#include "../mwrender/npcanimation.hpp"
#include "../mwrender/renderingmanager.hpp"
#include <components/vr/vr.hpp>
#include <components/vr/session.hpp>
#include <components/vr/space.hpp>

#include <osg/MatrixTransform>
#include <osg/Drawable>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osg/Vec4f>

#include <string>
#include <optional>
#include <vector>

namespace MWVR
{
    class HandController;
    class FingerController;
    class TrackingController;
    class NativeWeaponBoneController;
    class Crosshair;
    class XrSpaceTransform;

    void updateVrDebugSnapshotControls();

    struct CachedVrControllerPose
    {
        osg::Vec3f mWorldPosition;
        osg::Quat mWorldOrientation;
        bool mValid = false;
    };

    /// Returns the last pose sampled by the tracked-hand scene controller during a valid OpenXR frame.
    std::optional<CachedVrControllerPose> getCachedVrControllerPose(std::string_view side);

    /// Subclassing NpcAnimation to implement VR related behaviour
    class VRAnimation : public MWRender::NpcAnimation, private VR::Session::Listener
    {
    protected:
        virtual void addControllers() override;

    public:
        /**
         * @param ptr
         * @param disableListener  Don't listen for equipment changes and magic effects. InventoryStore only supports
         *                         one listener at a time, so you shouldn't do this if creating several NpcAnimations
         *                         for the same Ptr, eg preview dolls for the player.
         *                         Those need to be manually rendered anyway.
         * @param disableSounds    Same as \a disableListener but for playing items sounds
         * @param xrSession        The XR session that shall be used to track limbs
         */
        VRAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
            Resource::ResourceSystem* resourceSystem, bool disableSounds, osg::ref_ptr<osg::Group> sceneRoot);
        virtual ~VRAnimation();

        /// Overridden to always be false
        void enableHeadAnimation(bool enable) override;

        /// Overridden to always be false
        void setAccurateAiming(bool enabled) override;

        /// Overriden to always be a variant of VM_VR*
        void setViewMode(ViewMode viewMode) override;

        /// Overriden to include VR modifications
        void updateParts() override;

        /// Route Fallout weapon parts through the same tracked aim space used by the OpenMW VR pointer.
        void showWeapons(bool showWeapon) override;

        /// @return world transform that yields the position and orientation of the current weapon
        osg::Node* getWeaponTransform()
        {
            return mFalloutVrWeaponRayWorldValid && mFalloutVrWeaponRayWorldNode != nullptr
                ? mFalloutVrWeaponRayWorldNode.get()
                : mWeaponDirectionTransform.get();
        }
        const osg::Node* getWeaponTransform() const
        {
            return mFalloutVrWeaponRayWorldValid && mFalloutVrWeaponRayWorldNode != nullptr
                ? mFalloutVrWeaponRayWorldNode.get()
                : mWeaponDirectionTransform.get();
        }

        struct FalloutVrNativeWeaponFrame
        {
            const osg::Node* mWeaponPart = nullptr;
            const osg::Node* mProductionRay = nullptr;
            osg::Matrix mProductionRayWorld;
        };

        /// Read-only snapshot for short-lived effects authored beneath the currently bound native weapon. Pointers
        /// remain valid only for the current bind generation; callers must consume the snapshot immediately.
        std::optional<FalloutVrNativeWeaponFrame> getFalloutVrNativeWeaponFrame() const
        {
            const osg::Node* const part = mObjectParts[ESM::PRT_Weapon] != nullptr
                ? mObjectParts[ESM::PRT_Weapon]->getNode()
                : nullptr;
            if (!mFalloutVrWeaponRayWorldValid || part == nullptr || mFalloutVrWeaponRayNode == nullptr
                || mFalloutVrWeaponRayWorldNode == nullptr)
                return std::nullopt;
            return FalloutVrNativeWeaponFrame{
                part, mFalloutVrWeaponRayNode.get(), mFalloutVrWeaponRayWorldNode->getMatrix() };
        }

        /// Authored emission frames for the current bind in deterministic scene traversal order. Most weapons
        /// expose one frame; retail multi-emitter meshes expose one frame per ProjectileNode.
        const std::vector<osg::Matrix>& getFalloutVrWeaponEmissionFrames() const
        {
            return mFalloutVrWeaponRayWorldMatrices;
        }

        /// Enable pointers
        void enablePointers(bool left, bool right);

        void setEnableCrosshairs(bool enable);

        void updateLocalSpaceWorldPose();

        virtual void addAnimSource(std::string_view model, const std::string& baseModel) override;

        void updateSpace();

        /// Publish the current authored muzzle frame from the tracked right-hand branch without choosing an
        /// ambiguous scene-graph parental path.
        void updateFalloutVrWeaponRayWorld();

        void modifyMovement(osg::Vec3& movement);

        struct FalloutVrHandSurface
        {
            enum class Kind
            {
                Hand,
                PipBoy,
            };

            std::string model;
            std::string diffuseTexture;
            std::string source;
            bool left = false;
            Kind kind = Kind::Hand;
        };

        void setFalloutVrHandSurfaces(std::vector<FalloutVrHandSurface> surfaces);
        bool hasFalloutVrPipBoySurface() const { return !mFalloutVrPipBoySurfaceNodes.empty(); }
        osg::Node* getFalloutVrPipBoySurfaceRoot() const
        {
            return mFalloutVrPipBoySurfaceNodes.empty() ? nullptr : mFalloutVrPipBoySurfaceNodes.front().get();
        }
        osg::Matrix getTrackingSpaceWorldMatrix() const;
        const std::vector<osg::ref_ptr<osg::Drawable>>& getFalloutVrPipBoyScreenDrawables() const
        {
            return mFalloutVrPipBoyScreenDrawables;
        }
        bool setFalloutVrPipBoyScreenTexture(osg::Texture2D* screenTexture, osg::Texture2D* mapTexture,
            bool showMap, float mapZoom, float mapPanX, float mapPanY, const osg::Vec4f& mapClip);

    protected:

        void updateCrosshairs() override;

        void updateCharHeight();

        void recenter();
        void onRecenter() override { recenter(); }
        void onInteractionProfileActiveChanged(XrPath topLevelPath, bool isActive) override;

        void updateTrackingControllers();
        void clearFalloutVrHandSurfaces();
        void attachFalloutVrHandSurfaces();
        void updateFalloutVrHandSurfaceVisibility();
        void updateFalloutVrPipBoyInteractionScale();

        void enableTracking(XrPath path);
        void disableTracking(XrPath path);
        void enablePointer(XrPath topLevelPath, bool enable);

        struct TrackingContext
        {
            bool enabled = false;
            XrPath topLevelPath;
            std::string spaceName;
            std::string forearmBone;
            std::unique_ptr<TrackingController> forearmController;
            std::string handBone;
            osg::ref_ptr<HandController> handController;
            struct FingerBinding
            {
                std::string bone;
                osg::ref_ptr<FingerController> controller;
            };
            std::vector<FingerBinding> fingerBindings;
        };

    protected:

        typedef std::map<XrPath, TrackingContext> TrackingContextMap;
        TrackingContextMap mVrControllers;
        osg::ref_ptr<osg::MatrixTransform> mModelOffset;
        osg::ref_ptr<osg::MatrixTransform> mWeaponDirectionTransform;
        osg::ref_ptr<osg::MatrixTransform> mWeaponPointerTransform;
        osg::ref_ptr<NativeWeaponBoneController> mNativeWeaponBoneController;
        // Fallout production activation/combat rays originate at authored ProjectileNode helpers. Melee/thrown
        // families may expose one immutable native family-axis child; firearms never guess a model axis.
        osg::ref_ptr<osg::Node> mFalloutVrWeaponRayNode;
        osg::ref_ptr<osg::MatrixTransform> mFalloutVrSyntheticWeaponRayNode;
        std::vector<osg::ref_ptr<osg::MatrixTransform>> mFalloutVrSyntheticWeaponRayNodes;
        osg::ref_ptr<osg::MatrixTransform> mFalloutVrWeaponRayWorldNode;
        osg::Matrix mFalloutVrWeaponRayInRightHand;
        osg::NodePath mFalloutVrWeaponRayPathFromRightHand;
        std::vector<osg::Matrix> mFalloutVrWeaponRaysInRightHand;
        std::vector<osg::Matrix> mFalloutVrWeaponRayWorldMatrices;
        bool mFalloutVrWeaponRayInRightHandValid = false;
        bool mFalloutVrWeaponRayWorldValid = false;
        std::vector<osg::ref_ptr<osg::Node>> mFalloutVrNativeWeaponDebugNodes;
        // The native OpenMW weapon stays on its authored skeleton Weapon attachment.  This node only publishes
        // independent right-hand landmarks for diagnostics; it never becomes a weapon parent.
        osg::ref_ptr<osg::Node> mFalloutVrRightPalmFrame;

        bool mRecenter = false;
        XrPath mLeftHandPath = XR_NULL_PATH;
        XrPath mRightHandPath = XR_NULL_PATH;
        bool mCrosshairsEnabled;
        bool mFalloutVrHandSurfacesAttached = false;
        std::vector<FalloutVrHandSurface> mFalloutVrHandSurfaces;
        std::vector<osg::ref_ptr<osg::Node>> mFalloutVrHandSurfaceNodes;
        std::vector<osg::ref_ptr<osg::Node>> mFalloutVrPipBoySurfaceNodes;
        struct PipBoyInteractionScaleTarget
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> transform;
            osg::Vec3f basePosition;
            osg::Vec3f baseScale;
            osg::Vec3f socketModel;
        };
        std::vector<PipBoyInteractionScaleTarget> mFalloutVrPipBoyInteractionScaleTargets;
        bool mFalloutVrPipBoyInteractionFocused = false;
        float mFalloutVrPipBoyInteractionScale = 1.f;
        std::vector<osg::ref_ptr<osg::Drawable>> mFalloutVrPipBoyScreenDrawables;
        osg::ref_ptr<osg::Texture2D> mFalloutVrPipBoyScreenTexture;
        osg::ref_ptr<osg::Texture2D> mFalloutVrPipBoyMapTexture;
        bool mFalloutVrPipBoyShowMap = false;
        float mFalloutVrPipBoyMapZoom = 1.f;
        float mFalloutVrPipBoyMapPanX = 0.f;
        float mFalloutVrPipBoyMapPanY = 0.f;
        osg::Vec4f mFalloutVrPipBoyMapClip = osg::Vec4f(0.f, 0.f, 1.f, 1.f);
        bool mRightPointerEnabled = false;
        float mCharHeight = 120.f;
        Stereo::Pose mHeadPoseInLocalSpace;
        Stereo::Pose mCharLocalSpacePose;
        float mCharacterYaw = 0.f;
        std::unique_ptr<MWVR::Crosshair> mCrosshairAmmo;
        std::unique_ptr<MWVR::Crosshair> mCrosshairThrown;
        std::unique_ptr<MWVR::Crosshair> mCrosshairSpell;
        osg::ref_ptr<osg::Transform> mKBMouseCrosshairTransform;
        osg::ref_ptr<osg::Group> mSceneRoot;
    };

}

#endif
