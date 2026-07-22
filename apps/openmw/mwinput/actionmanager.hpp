#ifndef MWINPUT_ACTIONMANAGER_H
#define MWINPUT_ACTIONMANAGER_H

#include <osg/ref_ptr>
#include <osgViewer/ViewerEventHandlers>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../mwmechanics/falloutcombat.hpp"
#include "../mwworld/ptr.hpp"

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

//## VR_PATCH BEGIN
        bool checkIsRunning();

//## VR_PATCH END
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
        void handleGuiArrowKey(int action);
        bool isFalloutContent() const;
        void toggleFalloutVats();
        bool selectFalloutVatsTarget(const MWWorld::Ptr& target);
        void cycleFalloutVatsTarget(int direction);
        bool selectFalloutVatsBodyPart(std::size_t index);
        void cycleFalloutVatsBodyPart(int direction);
        void updateFalloutVatsCamera();
        void restoreFalloutVatsView();
        std::size_t getFalloutVatsAvailableShots() const;
        void queueFalloutVatsAttack();
        void executeFalloutVatsQueue();
        void updateFalloutVatsExecution(float dt);
        bool executeNextFalloutVatsAction();
        void finishFalloutVatsExecution(bool interrupted);
        void updateFalloutVatsHud();

        BindingsManager* mBindingsManager;
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;

        float mTimeIdle;
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
        int mFalloutVatsPreviousCameraMode = -1;
        float mFalloutVatsPreviousCameraDistance = 0.f;
        float mFalloutVatsPreviousSimulationScale = 1.f;
        float mFalloutVatsExecutionTimer = 0.f;
        float mFalloutVatsExecutionApBefore = 0.f;
        float mFalloutVatsExecutionPlannedApAfter = 0.f;
        float mFalloutVatsExecutionApSpent = 0.f;
        float mFalloutVatsExecutionDamage = 0.f;
        std::size_t mFalloutVatsExecutionQueued = 0;
        std::size_t mFalloutVatsExecutionShotsAttempted = 0;
        std::size_t mFalloutVatsExecutionShotsFired = 0;
        std::size_t mFalloutVatsExecutionRolledHits = 0;
        std::vector<std::pair<ESM::FormId, float>> mFalloutVatsExecutionTargetHealthBefore;
        float mFalloutVatsCaptureTimer = 0.f;
        unsigned int mFalloutVatsCaptureFrames = 0;
        unsigned int mFalloutVatsVideoCaptureCount = 0;
        bool mFalloutVatsCapturePrepared = false;
        bool mFalloutVatsCaptureQueued = false;
        bool mFalloutVatsVideoExited = false;
    };
}
#endif
