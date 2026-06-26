#ifndef OPENMW_MWRENDER_ASSETCAPTURE_H
#define OPENMW_MWRENDER_ASSETCAPTURE_H

#include "../mwworld/ptr.hpp"
#include <string>

namespace osgViewer {
    class Viewer;
}

namespace MWRender
{
    class AssetCapture
    {
    public:
        static void maybeStartFromEnvironment(unsigned frameNumber, osgViewer::Viewer* viewer);
        static void startCapture(const std::string& actorId, osgViewer::Viewer* viewer);
        static void update(); // Called every frame to process the state machine

    private:
        static int sCaptureState;
        static int sFramesToWait;
        static std::string sTargetActor;
        static std::string sOutputDir;
        static bool sCompletedOnce;
        static osgViewer::Viewer* sViewer;

        static void captureScreenshot(const std::string& filename);
    };
}

#endif
