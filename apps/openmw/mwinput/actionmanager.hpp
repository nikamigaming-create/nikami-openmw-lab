#ifndef MWINPUT_ACTIONMANAGER_H
#define MWINPUT_ACTIONMANAGER_H

#include <osg/ref_ptr>
#include <osgViewer/ViewerEventHandlers>

#include <optional>
#include <string>

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
        void queueFalloutVatsAttack();
        void executeFalloutVatsQueue();
        void updateFalloutVatsHud();

        BindingsManager* mBindingsManager;
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;

        float mTimeIdle;
        MWMechanics::FalloutVatsRuntime mFalloutVats;
        std::optional<MWMechanics::FalloutVatsWeaponContract> mFalloutVatsWeapon;
        MWWorld::Ptr mFalloutVatsTarget;
        std::string mFalloutVatsTargetName;
        std::string mFalloutVatsBodyPartName;
        unsigned int mFalloutVatsHitChance = 0;
        float mFalloutVatsCaptureTimer = 0.f;
        bool mFalloutVatsCaptureQueued = false;
    };
}
#endif
