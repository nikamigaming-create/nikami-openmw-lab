#ifndef MWINPUT_ACTIONMANAGER_H
#define MWINPUT_ACTIONMANAGER_H

#include <osg/ref_ptr>
<<<<<<< HEAD
#include <osgViewer/ViewerEventHandlers>

=======
#include <osg/Vec3f>
#include <osgViewer/ViewerEventHandlers>

#include <optional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../mwmechanics/falloutcombat.hpp"
#include "../mwworld/ptr.hpp"

>>>>>>> origin/main
namespace osgViewer
{
    class Viewer;
    class ScreenCaptureHandler;
}

namespace MWInput
{
    class BindingsManager;

    class ActionManager
    {
    public:
        ActionManager(BindingsManager* bindingsManager, osg::ref_ptr<osgViewer::Viewer> viewer,
            osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler);

        void update(float dt);

        void executeAction(int action);

        bool checkAllowedToUseItems() const;

<<<<<<< HEAD
=======
//## VR_PATCH BEGIN
        bool checkIsRunning();

//## VR_PATCH END
>>>>>>> origin/main
        void toggleMainMenu();
        void toggleConsole();
        void screenshot();
        void activate();
        void rest();
        void quickLoad();
        void quickSave();

        void quickKey(int index);

        void resetIdleTime();
        float getIdleTime() const { return mTimeIdle; }

        bool isSneaking() const;

    private:
<<<<<<< HEAD
        void handleGuiArrowKey(int action);
=======
        enum class FalloutVatsProofMode
        {
            Vats,
            OrdinaryRanged,
            OrdinaryMelee,
        };

        void handleGuiArrowKey(int action);
        bool isFalloutContent() const;
        void toggleFalloutVats();
        bool selectFalloutVatsTarget(const MWWorld::Ptr& target);
        void cycleFalloutVatsTarget(int direction);
        bool selectFalloutVatsBodyPart(std::size_t index);
        void cycleFalloutVatsBodyPart(int direction);
        void updateFalloutVatsCamera();
        void updateFalloutVatsHighlight();
        void clearFalloutVatsHighlight();
        void restoreFalloutVatsView();
        std::size_t getFalloutVatsAvailableShots() const;
        void queueFalloutVatsAttack();
        void executeFalloutVatsQueue();
        void updateFalloutVatsExecution(float dt);
        bool executeNextFalloutVatsAction();
        void finishFalloutVatsExecution(bool interrupted);
        void updateFalloutVatsPointerSelection();
        void updateFalloutVatsHud();
        void updateFalloutVatsProof();
        void captureFalloutVatsProofFrame();
>>>>>>> origin/main

        BindingsManager* mBindingsManager;
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;

        float mTimeIdle;
<<<<<<< HEAD
=======
        MWMechanics::FalloutVatsRuntime mFalloutVats;
        std::optional<MWMechanics::FalloutVatsWeaponContract> mFalloutVatsWeapon;
        MWWorld::Ptr mFalloutVatsTarget;
        std::vector<MWWorld::Ptr> mFalloutVatsTargets;
        std::size_t mFalloutVatsTargetIndex = 0;
        std::vector<MWMechanics::FalloutVatsBodyPartContract> mFalloutVatsBodyParts;
        std::size_t mFalloutVatsBodyPartIndex = 0;
        std::string mFalloutVatsTargetName;
        std::string mFalloutVatsBodyPartName;
        std::string mFalloutVatsBodyPartTargetNode;
        unsigned int mFalloutVatsHitChance = 0;
        bool mFalloutPlayerUseDown = false;
        int mFalloutVatsPreviousCameraMode = -1;
        float mFalloutVatsPreviousCameraDistance = 0.f;
        float mFalloutVatsPreviousCameraPitch = 0.f;
        float mFalloutVatsPreviousCameraYaw = 0.f;
        float mFalloutVatsPreviousCameraRoll = 0.f;
        float mFalloutVatsPreviousPlayerYaw = 0.f;
        bool mFalloutVatsPlayerYawChanged = false;
        float mFalloutVatsPreviousSimulationScale = 1.f;
        osg::Vec3f mFalloutVatsTargetFramingForward{ 0.f, 1.f, 0.f };
        osg::Vec3f mFalloutVatsExecutionCameraEye;
        osg::Vec3f mFalloutVatsExecutionCameraFocus;
        bool mFalloutVatsExecutionCameraInitialized = false;
        unsigned int mFalloutVatsExecutionCameraPhase = 0;
        float mFalloutVatsExecutionTimer = 0.f;
        float mFalloutVatsExecutionApBefore = 0.f;
        float mFalloutVatsExecutionPlannedApAfter = 0.f;
        float mFalloutVatsExecutionApSpent = 0.f;
        float mFalloutVatsExecutionDamage = 0.f;
        std::size_t mFalloutVatsExecutionQueued = 0;
        std::size_t mFalloutVatsExecutionShotsAttempted = 0;
        std::size_t mFalloutVatsExecutionShotsFired = 0;
        std::size_t mFalloutVatsExecutionRolledHits = 0;
        bool mFalloutVatsExecutionVisualPrepared = false;
        std::vector<std::pair<ESM::FormId, float>> mFalloutVatsExecutionTargetHealthBefore;
        bool mFalloutVatsProofEnabled = false;
        bool mFalloutVatsProofFinished = false;
        bool mFalloutVatsProofWeaponSelected = false;
        FalloutVatsProofMode mFalloutVatsProofMode = FalloutVatsProofMode::Vats;
        bool mFalloutVatsProofUseDown = false;
        unsigned int mFalloutVatsProofStage = 0;
        unsigned int mFalloutVatsProofFrame = 0;
        unsigned int mFalloutVatsProofCaptures = 0;
        unsigned int mFalloutVatsProofAttacksIssued = 0;
        unsigned int mFalloutVatsProofLastAttackFrame = 0;
        unsigned int mFalloutVatsProofAttackReleaseFrame = 0;
        unsigned int mFalloutVatsProofCaptureStep = 3;
        unsigned int mFalloutVatsProofPostFrames = 180;
        std::size_t mFalloutVatsProofShotsFired = 0;
        std::uint32_t mFalloutVatsProofWeaponFormId = 0x0000434f;
        std::uint32_t mFalloutVatsProofTargetReference = 0;
        std::string mFalloutVatsProofTargetName;
        std::string mFalloutVatsProofStartCell = "Goodsprings";
        osg::Vec3f mFalloutVatsProofTargetPosition;
        float mFalloutVatsProofTargetYaw = 0.f;
        float mFalloutVatsProofTargetDistance = 512.f;
        bool mFalloutVatsProofAuthoredStartConfigured = false;
        bool mFalloutVatsProofAuthoredStartApplied = false;
        MWWorld::Ptr mFalloutVatsProofTarget;
        float mFalloutVatsProofHealthBefore = 0.f;
        float mFalloutVatsProofPlayerHealthBefore = 0.f;
        bool mFalloutVatsProofPlayerHealthRecorded = false;
        float mFalloutVatsProofCameraPitchBefore = 0.f;
        float mFalloutVatsProofCameraYawBefore = 0.f;
        float mFalloutVatsProofCameraRollBefore = 0.f;
        float mFalloutVatsProofPlayerYawBefore = 0.f;
        int mFalloutVatsProofCameraModeBefore = -1;
>>>>>>> origin/main
    };
}
#endif
