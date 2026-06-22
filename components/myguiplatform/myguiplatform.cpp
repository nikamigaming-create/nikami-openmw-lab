#include "myguiplatform.hpp"

#include "myguidatamanager.hpp"
#include "myguiloglistener.hpp"
#include "myguirendermanager.hpp"

#include <MyGUI_LogManager.h>

namespace MyGUIPlatform
{

    Platform::Platform(osgViewer::Viewer* viewer, osg::Group* guiRoot, Resource::ImageManager* imageManager,
        const VFS::Manager* vfs, float uiScalingFactor, VFS::Path::NormalizedView resourcePath,
        const std::filesystem::path& logName)
        : mLogManager(MyGUI::LogManager::getInstancePtr() == nullptr ? std::make_unique<MyGUI::LogManager>() : nullptr)
        , mDataManager(std::make_unique<DataManager>(resourcePath, vfs))
        , mRenderManager(std::make_unique<RenderManager>(viewer, guiRoot, imageManager, uiScalingFactor))
    {
#ifndef __ANDROID__
        mLogFacility = logName.empty() ? nullptr : std::make_unique<LogFacility>(logName, false);
        if (mLogFacility != nullptr)
            MyGUI::LogManager::getInstance().addLogSource(mLogFacility->getSource());
#endif

        mRenderManager->initialise();
    }

    Platform::~Platform() = default;

    void Platform::shutdown()
    {
        mRenderManager->shutdown();
    }

    RenderManager* Platform::getRenderManagerPtr()
    {
        return mRenderManager.get();
    }

    DataManager* Platform::getDataManagerPtr()
    {
        return mDataManager.get();
    }

}
