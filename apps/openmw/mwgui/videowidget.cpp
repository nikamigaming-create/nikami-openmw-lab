#include "videowidget.hpp"

#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
#include <osg-ffmpeg-videoplayer/videoplayer.hpp>
#endif

#include <MyGUI_RenderManager.h>

#include <osg/Texture2D>

#include <algorithm>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
#include "../mwsound/movieaudiofactory.hpp"
#endif

namespace MWGui
{
    namespace
    {
        VFS::Path::Normalized normalizeVideoPath(std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            return VFS::Path::Normalized(path);
        }

        VFS::Path::Normalized resolveVideoPath(const VFS::Manager& vfs, const std::string& requested)
        {
            const VFS::Path::Normalized direct = normalizeVideoPath(requested);
            if (vfs.exists(direct))
            {
                Log(Debug::Info) << "FNV/ESM4 proof: video source " << direct << " archive=" << vfs.getArchive(direct);
                return direct;
            }

            const bool hasFalloutMenu = vfs.exists(VFS::Path::NormalizedView("menus/options/main_menu.xml"));
            const std::string requestedName = direct.value().substr(direct.value().find_last_of('/') + 1);
            const VFS::Path::Normalized fnvIntro("video/fnvintro.bik");
            if (hasFalloutMenu && Misc::StringUtils::ciEqual(requestedName, "Fallout INTRO Vsk.bik") && vfs.exists(fnvIntro))
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: requested INI intro movie " << direct
                                    << " is missing; using installed loose FNV intro " << fnvIntro
                                    << " archive=" << vfs.getArchive(fnvIntro);
                return fnvIntro;
            }

            if (hasFalloutMenu && vfs.exists(fnvIntro))
                Log(Debug::Warning) << "FNV/ESM4 diag: video request " << direct
                                    << " is missing; installed loose FNV intro candidate " << fnvIntro
                                    << " archive=" << vfs.getArchive(fnvIntro);
            return direct;
        }
    }

    VideoWidget::VideoWidget()
        : mVFS(nullptr)
    {
#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
        mPlayer = std::make_unique<Video::VideoPlayer>();
#endif
        setNeedKeyFocus(true);
    }

    VideoWidget::~VideoWidget() = default;

    void VideoWidget::setVFS(const VFS::Manager* vfs)
    {
        mVFS = vfs;
    }

    void VideoWidget::playVideo(const std::string& video)
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        Log(Debug::Info) << "Skipping video playback on Android bootstrap build: " << video;
#else
        mPlayer->setAudioFactory(new MWSound::MovieAudioFactory());
        const VFS::Path::Normalized resolvedVideo = resolveVideoPath(*mVFS, video);

        Files::IStreamPtr videoStream;
        try
        {
            videoStream = mVFS->get(resolvedVideo);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Failed to open video " << resolvedVideo << ": " << e.what();
            return;
        }

        mPlayer->playVideo(std::move(videoStream), resolvedVideo.value());
        Log(Debug::Info) << "FNV/ESM4 proof: video playback opened requested=" << video
                         << " resolved=" << resolvedVideo.value() << " archive=" << mVFS->getArchive(resolvedVideo);

        osg::ref_ptr<osg::Texture2D> texture = mPlayer->getVideoTexture();
        if (!texture)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: video playback opened without texture resolved="
                                << resolvedVideo.value();
            return;
        }
        Log(Debug::Info) << "FNV/ESM4 proof: video texture ready " << resolvedVideo.value() << " size="
                         << mPlayer->getVideoWidth() << "x" << mPlayer->getVideoHeight();

        mTexture = std::make_unique<MyGUIPlatform::OSGTexture>(texture);

        setRenderItemTexture(mTexture.get());
        // Both the widget and the video frame are Y-down, so this UV is not inverted
        getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
#endif
    }

    int VideoWidget::getVideoWidth()
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        return 0;
#else
        return mPlayer->getVideoWidth();
#endif
    }

    int VideoWidget::getVideoHeight()
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        return 0;
#else
        return mPlayer->getVideoHeight();
#endif
    }

    bool VideoWidget::update()
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        return false;
#else
        return mPlayer->update();
#endif
    }

    void VideoWidget::stop()
    {
#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
        mPlayer->close();
#endif
    }

    void VideoWidget::pause()
    {
#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
        mPlayer->pause();
#endif
    }

    void VideoWidget::resume()
    {
#ifndef OPENMW_ANDROID_DISABLE_FFMPEG
        mPlayer->play();
#endif
    }

    bool VideoWidget::isPaused() const
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        return false;
#else
        return mPlayer->isPaused();
#endif
    }

    bool VideoWidget::hasAudioStream()
    {
#ifdef OPENMW_ANDROID_DISABLE_FFMPEG
        return false;
#else
        return mPlayer->hasAudioStream();
#endif
    }

    void VideoWidget::autoResize(bool stretch)
    {
        MyGUI::IntSize screenSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (getParent())
            screenSize = getParent()->getSize();

        if (getVideoHeight() > 0 && !stretch)
        {
            double imageaspect = static_cast<double>(getVideoWidth()) / getVideoHeight();

            int leftPadding = std::max(0, static_cast<int>(screenSize.width - screenSize.height * imageaspect) / 2);
            int topPadding = std::max(0, static_cast<int>(screenSize.height - screenSize.width / imageaspect) / 2);

            setCoord(leftPadding, topPadding, screenSize.width - leftPadding * 2, screenSize.height - topPadding * 2);
        }
        else
            setCoord(0, 0, screenSize.width, screenSize.height);
    }

}
