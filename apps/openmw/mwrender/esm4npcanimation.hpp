#ifndef GAME_RENDER_ESM4NPCANIMATION_H
#define GAME_RENDER_ESM4NPCANIMATION_H

#include "animation.hpp"

#include <components/nifosg/matrixtransform.hpp>

#include <osg/MatrixTransform>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ESM4
{
    struct Npc;
    struct Race;
    struct Weapon;
}

namespace MWRender
{
    class ESM4NpcAnimation : public Animation
    {
    public:
        struct FirstPersonState
        {
            float mFieldOfView = 55.f;
            std::vector<std::string> mSaveWornArmorModels;
            std::string mSaveWornLeftHandModel;
            bool mPipBoy = false;
            bool mPipBoyGlove = false;
        };

        struct WeaponAttachmentState
        {
            bool mApplied = false;
            bool mAttached = false;
            bool mVisible = false;
            std::string mFrameName;
            std::string mParentName;
            std::array<float, 9> mRotation{};
            std::array<float, 3> mTranslation{};
            float mScale = 0.f;
        };

        ESM4NpcAnimation(
            const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem);
        ESM4NpcAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
            Resource::ResourceSystem* resourceSystem, std::optional<FirstPersonState> firstPerson);
        osg::Vec3f runAnimation(float duration) override;
        bool getWeaponsShown() const override { return mFalloutWeaponsShown; }
        void showWeapons(bool showWeapon) override;
        void emitFalloutFirstPersonWeaponPostKfAudit();
        osg::Node* getEquippedWeaponNode() override;
        bool prepareFalloutWeaponAnimation(
            std::uint8_t animationType, std::uint8_t reloadAnimation, FonvWeaponAction action) override;
        bool setFalloutAnimatedObject(std::string_view model, std::string_view activeGroup) override;
        bool setWeaponHolsterAttachment(std::string_view frameName, std::string_view parentName,
            const std::array<float, 9>& rotation, const std::array<float, 3>& translation, float scale);
        WeaponAttachmentState getWeaponHolsterAttachmentState() const;
        bool supportsProceduralHumanoidLocomotion() const;
        bool applyProceduralHumanoidLocomotion(std::string_view group, float elapsed);
        std::size_t getFirstPersonAttachedPartCount() const { return mFirstPersonAttachedPartCount; }
        bool hasPipBoyPresentation() const { return mPipBoyPresentationRoot != nullptr; }
        bool setPipBoyScreenTexture(osg::Texture2D* screenTexture, osg::Texture2D* mapTexture = nullptr,
            bool showMap = false, float mapZoom = 1.f, float mapPanX = 0.f, float mapPanY = 0.f);
        void setPipBoyPresentationProgress(float progress, bool interactionPoseActive);
        void setPipBoyInteractionProgress(float progress);
        void setPipBoyControlState(int pane, int submenu, int listOffset, bool worldMap, float mapZoom,
            float mapPanX, float mapPanY, float interactionPulse);

    private:
        struct ProceduralPoseBone
        {
            osg::ref_ptr<osg::MatrixTransform> mNode;
            osg::Matrix mRootRelative;
        };

        struct PipBoyPhysicalControl
        {
            osg::ref_ptr<osg::MatrixTransform> mRoot;
            osg::Vec3f mPivot;
            osg::Vec3f mAxis;
        };

        std::vector<ProceduralPoseBone> mFo4ProceduralPoseBones;
        bool mFo4ProceduralPoseInitialized = false;
        std::string mFo4ProceduralGroup;
        bool mFo4ProceduralAdvancedLogged = false;

        osg::ref_ptr<osg::Node> mFalloutWeaponPart;
        osg::ref_ptr<osg::MatrixTransform> mFalloutWeaponCameraFrame;
        osg::ref_ptr<NifOsg::MatrixTransform> mFalloutWeaponDrawFrame;
        bool mFalloutWeaponUsesWorldModelFallback = false;
        osg::ref_ptr<NifOsg::MatrixTransform> mFalloutWeaponHolsterFrame;
        std::string mFalloutWeaponDrawBone = "Weapon";
        std::string mFalloutWeaponHolsterBone;
        bool mFalloutWeaponsShown = false;
        bool mFalloutWeaponShownTelemetryLogged = false;
        bool mFalloutWeaponPostKfAuditLogged = false;
        bool mFirstPersonView = false;
        std::size_t mFirstPersonAttachedPartCount = 0;
        osg::ref_ptr<osg::Node> mPipBoyArmPart;
        osg::ref_ptr<osg::Node> mFirstPersonArmorArmsPart;
        osg::ref_ptr<osg::Node> mFirstPersonLeftHandPart;
        osg::ref_ptr<osg::Node> mFirstPersonRightHandPart;
        osg::ref_ptr<osg::MatrixTransform> mPipBoyPresentationRoot;
        PipBoyPhysicalControl mPipBoyTabKnob;
        PipBoyPhysicalControl mPipBoyScrollKnob;
        std::array<PipBoyPhysicalControl, 3> mPipBoyButtons;
        std::array<PipBoyPhysicalControl, 3> mPipBoyGlows;
        std::vector<osg::ref_ptr<osg::StateSet>> mPipBoyScreenStateSets;
        float mPipBoyPresentationProgress = 0.f;
        float mPipBoyInteractionProgress = 0.f;
        int mPipBoyArmTargetVariant = 0;
        int mPipBoyLastPane = -1;
        int mPipBoyLastSubmenu = -1;
        int mPipBoyLastListOffset = -1;
        bool mPipBoyLastWorldMap = false;
        float mPipBoyLastMapZoom = 1.f;
        float mPipBoyLastMapPanX = 0.f;
        float mPipBoyLastMapPanY = 0.f;
        float mPipBoyLastControlPulse = 0.f;
        float mPipBoyScrollStartAngle = 1.57079632679f;
        float mPipBoyScrollTargetAngle = 1.57079632679f;
        float mPipBoyScrollDisplayAngle = 1.57079632679f;
        bool mPipBoyRetailInteractionBound = false;
        bool mPipBoyRetailWaverBound = false;
        bool mPipBoyRetailBaseIdleBound = false;
        bool mPipBoyRetailBaseAimBound = false;
        bool mPipBoyControlsInitialized = false;
        bool mPipBoyControlsInitializationAttempted = false;
        bool mPipBoyRetailInteractionPoseHeld = false;
        bool mPipBoyHeldCompositionAuditLogged = false;
        // The game may refresh the first-person weapon after the Pip-Boy
        // renderer has opened. Keep the visual holster state authoritative
        // until the wrist has lowered again.
        bool mPipBoyWeaponSuppressed = false;
        const ESM4::Weapon* mFalloutActionWeapon = nullptr;
        osg::ref_ptr<osg::Node> mFalloutAnimatedObjectPart;
        std::string mFalloutAnimatedObjectModel;
        std::string mFalloutAnimatedObjectGroup;

        osg::ref_ptr<osg::Node> insertPart(
            std::string_view model, const osg::Vec4f* tint = nullptr, std::string_view diffuseTexture = {},
            std::string_view preferredBone = {}, bool forceActorSpace = false);
        osg::ref_ptr<osg::Node> insertAttachedPart(
            std::string_view model, std::string_view preferredBone, std::string* authoredParent = nullptr);
        void initializeFirstPerson(const FirstPersonState& state);
        void initializePipBoyPhysicalControls();

        // Works for FO3/FONV/TES5
        unsigned int insertHeadParts(const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds,
            std::set<uint32_t>& usedHeadPartTypes, std::set<uint32_t>* attachedHeadPartTypes = nullptr,
            unsigned int* attachedRequestedPartCount = nullptr, const ESM4::Race* faceGenRace = nullptr,
            bool faceGenFemale = false);

        void updateParts();
        bool applyRetailWeaponHolsterContract(const ESM4::Weapon& weapon);
        bool refreshFalloutWeaponPart();
        std::string resolveFalloutWeaponViewModel(const ESM4::Weapon& weapon) const;
        void updatePartsTES4(const ESM4::Npc& traits);
        void updatePartsFONV(const ESM4::Npc& traits);
        void updatePartsTES5(const ESM4::Npc& traits);
        void applyPostManualFalloutActorPose() override;
    };
}

#endif // GAME_RENDER_ESM4NPCANIMATION_H
