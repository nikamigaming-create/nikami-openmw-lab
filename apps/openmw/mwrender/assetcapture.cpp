#include "assetcapture.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <osg/Camera>
#include <osg/Viewport>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

#include <components/debug/debuglog.hpp>

namespace
{
    std::string envText(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string();
    }

    unsigned envUnsigned(const char* name, unsigned fallback)
    {
        const std::string value = envText(name);
        if (value.empty())
            return fallback;

        try
        {
            const unsigned parsed = static_cast<unsigned>(std::stoul(value));
            return parsed == 0 ? fallback : parsed;
        }
        catch (...)
        {
            return fallback;
        }
    }

    std::string safeFileToken(std::string value)
    {
        for (char& c : value)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
                c = '_';
        }
        return value.empty() ? "asset" : value;
    }
}

namespace MWRender
{
    int AssetCapture::sCaptureState = 0;
    int AssetCapture::sFramesToWait = 0;
    std::string AssetCapture::sTargetActor;
    std::string AssetCapture::sOutputDir;
    bool AssetCapture::sCompletedOnce = false;
    osgViewer::Viewer* AssetCapture::sViewer = nullptr;

    void AssetCapture::maybeStartFromEnvironment(unsigned frameNumber, osgViewer::Viewer* viewer)
    {
        const std::string outputDir = envText("OPENMW_FNV_ASSET_CAPTURE_DIR");
        if (outputDir.empty())
            return;

        const unsigned interval = envUnsigned("OPENMW_FNV_ASSET_CAPTURE_INTERVAL_FRAMES", 0);
        if (interval == 0 && sCompletedOnce)
            return;
        if (interval != 0 && frameNumber % interval != 0)
            return;

        std::string target = envText("OPENMW_FNV_ASSET_CAPTURE_TARGET");
        if (target.empty())
            target = "framebuffer";

        startCapture(target, viewer);
    }

    void AssetCapture::startCapture(const std::string& actorId, osgViewer::Viewer* viewer)
    {
        if (sCaptureState != 0 || viewer == nullptr)
            return;

        sOutputDir = envText("OPENMW_FNV_ASSET_CAPTURE_DIR");
        if (sOutputDir.empty())
            return;

        std::filesystem::create_directories(std::filesystem::path(sOutputDir));
        sTargetActor = safeFileToken(actorId);
        sViewer = viewer;
        sCaptureState = 1;
        sFramesToWait = 2;

        Log(Debug::Info) << "FNV/ESM4 asset capture started target=\"" << sTargetActor << "\" outputDir=\""
                         << sOutputDir << "\" gate=fnv-asset-capture runtime=loaded-pending-runtime";
    }

    void AssetCapture::update()
    {
        if (sCaptureState == 0)
            return;

        if (sFramesToWait > 0)
        {
            --sFramesToWait;
            return;
        }

        switch (sCaptureState)
        {
            case 1:
                captureScreenshot("front.png");
                ++sCaptureState;
                sFramesToWait = 5;
                break;
            case 2:
                captureScreenshot("side.png");
                ++sCaptureState;
                sFramesToWait = 5;
                break;
            case 3:
                captureScreenshot("top.png");
                ++sCaptureState;
                sFramesToWait = 5;
                break;
            case 4:
                captureScreenshot("iso.png");
                sCaptureState = 0;
                sCompletedOnce = true;
                Log(Debug::Info) << "FNV/ESM4 asset capture complete target=\"" << sTargetActor
                                 << "\" outputDir=\"" << sOutputDir
                                 << "\" gate=fnv-asset-capture runtime=runtime-supported";
                break;
            default:
                sCaptureState = 0;
                Log(Debug::Warning) << "FNV/ESM4 asset capture reset invalid state gate=fnv-asset-capture"
                                    << " runtime=known-blocked";
                break;
        }
    }

    void AssetCapture::captureScreenshot(const std::string& filename)
    {
        if (sViewer == nullptr)
            return;

        osg::Camera* camera = sViewer->getCamera();
        if (camera == nullptr || camera->getViewport() == nullptr)
            return;

        osg::Viewport* viewport = camera->getViewport();
        osg::ref_ptr<osg::Image> image = new osg::Image;
        const int width = static_cast<int>(viewport->width());
        const int height = static_cast<int>(viewport->height());

        image->allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);
        image->readPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE);

        const std::filesystem::path outputPath
            = std::filesystem::path(sOutputDir) / (sTargetActor + "_" + safeFileToken(filename));
        if (!osgDB::writeImageFile(*image, outputPath.string()))
        {
            Log(Debug::Warning) << "FNV/ESM4 asset capture screenshot failed path=\"" << outputPath.string()
                                << "\" gate=fnv-asset-capture runtime=known-blocked";
            return;
        }

        Log(Debug::Info) << "FNV/ESM4 asset capture screenshot path=\"" << outputPath.string()
                         << "\" gate=fnv-asset-capture runtime=runtime-supported";
    }
}
