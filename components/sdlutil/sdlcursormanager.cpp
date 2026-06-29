#include "sdlcursormanager.hpp"

#include <algorithm>
#include <stdexcept>

#include <SDL_endian.h>
#include <SDL_hints.h>
#include <SDL_mouse.h>
#include <SDL_render.h>

#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Version>
#include <osgViewer/GraphicsWindow>

#include <components/debug/debuglog.hpp>

#include "imagetosurface.hpp"

#if defined(OSG_LIBRARY_STATIC) && (!defined(ANDROID) || OSG_VERSION_GREATER_THAN(3, 6, 5))
// Sets the default windowing system interface according to the OS.
// Necessary for OpenSceneGraph to do some things, like decompression.
USE_GRAPHICSWINDOW()
#endif

namespace SDLUtil
{
    namespace
    {
        bool isLikelyMarkerMagentaCursor(osg::Image* image)
        {
            if (image == nullptr || image->s() <= 0 || image->t() <= 0)
                return false;

            const int stepX = std::max(1, image->s() / 16);
            const int stepY = std::max(1, image->t() / 16);
            int opaqueSamples = 0;
            int magentaSamples = 0;
            for (int s = 0; s < image->s(); s += stepX)
            {
                for (int t = 0; t < image->t(); t += stepY)
                {
                    const osg::Vec4 color = image->getColor(s, t, 0);
                    if (color.a() < 0.35f)
                        continue;

                    ++opaqueSamples;
                    if (color.r() > 0.7f && color.g() < 0.35f && color.b() > 0.7f)
                        ++magentaSamples;
                }
            }

            return opaqueSamples >= 8 && magentaSamples * 2 >= opaqueSamples;
        }

        SDL_Cursor* createSystemCursor(std::string_view name)
        {
            if (name == "hresize")
                return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
            if (name == "vresize")
                return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENS);
            if (name == "dresize")
                return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENESW);
            if (name == "dresize2")
                return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZENWSE);
            return SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
        }
    }

    SDLCursorManager::SDLCursorManager()
        : mEnabled(false)
        , mInitialized(false)
    {
    }

    SDLCursorManager::~SDLCursorManager()
    {
        CursorMap::const_iterator cursIter = mCursorMap.begin();

        while (cursIter != mCursorMap.end())
        {
            SDL_FreeCursor(cursIter->second);
            ++cursIter;
        }

        mCursorMap.clear();
    }

    void SDLCursorManager::setEnabled(bool enabled)
    {
        if (mInitialized && enabled == mEnabled)
            return;

        mInitialized = true;
        mEnabled = enabled;

        // turn on hardware cursors
        if (enabled)
        {
            _setGUICursor(mCurrentCursor);
        }
        // turn off hardware cursors
        else
        {
            SDL_ShowCursor(SDL_FALSE);
        }
    }

    void SDLCursorManager::cursorChanged(std::string_view name)
    {
        mCurrentCursor = name;
        _setGUICursor(mCurrentCursor);
    }

    void SDLCursorManager::_setGUICursor(std::string_view name)
    {
        auto it = mCursorMap.find(name);
        if (it != mCursorMap.end())
            SDL_SetCursor(it->second);
    }

    void SDLCursorManager::createCursor(std::string_view name, int rotDegrees, osg::Image* image, Uint8 hotspotX,
        Uint8 hotspotY, int cursorWidth, int cursorHeight)
    {
#ifndef ANDROID
        _createCursorFromResource(name, rotDegrees, image, hotspotX, hotspotY, cursorWidth, cursorHeight);
#endif
    }

    SDLUtil::SurfaceUniquePtr decompress(
        osg::ref_ptr<osg::Image> source, float rotDegrees, int cursorWidth, int cursorHeight)
    {
        int width = source->s();
        int height = source->t();
        bool useAlpha = source->isImageTranslucent();

        osg::ref_ptr<osg::Image> decompressedImage = new osg::Image;
        decompressedImage->setFileName(source->getFileName());
        decompressedImage->allocateImage(width, height, 1, useAlpha ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
        for (int s = 0; s < width; ++s)
            for (int t = 0; t < height; ++t)
                decompressedImage->setColor(source->getColor(s, t, 0), s, t, 0);

        Uint32 redMask = 0x000000ff;
        Uint32 greenMask = 0x0000ff00;
        Uint32 blueMask = 0x00ff0000;
        Uint32 alphaMask = useAlpha ? 0xff000000 : 0;

        SDL_Surface* cursorSurface = SDL_CreateRGBSurfaceFrom(decompressedImage->data(), width, height,
            decompressedImage->getPixelSizeInBits(), decompressedImage->getRowSizeInBytes(), redMask, greenMask,
            blueMask, alphaMask);

        SDL_Surface* targetSurface
            = SDL_CreateRGBSurface(0, cursorWidth, cursorHeight, 32, redMask, greenMask, blueMask, alphaMask);
        SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(targetSurface);

        SDL_RenderClear(renderer);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_Texture* cursorTexture = SDL_CreateTextureFromSurface(renderer, cursorSurface);

        SDL_RenderCopyEx(renderer, cursorTexture, nullptr, nullptr, -rotDegrees, nullptr, SDL_FLIP_NONE);

        SDL_DestroyTexture(cursorTexture);
        SDL_FreeSurface(cursorSurface);
        SDL_DestroyRenderer(renderer);

        return SDLUtil::SurfaceUniquePtr(targetSurface, SDL_FreeSurface);
    }

    void SDLCursorManager::_createCursorFromResource(std::string_view name, int rotDegrees, osg::Image* image,
        Uint8 hotspotX, Uint8 hotspotY, int cursorWidth, int cursorHeight)
    {
        if (mCursorMap.find(name) != mCursorMap.end())
            return;

        try
        {
            if (isLikelyMarkerMagentaCursor(image))
            {
                SDL_Cursor* cursor = createSystemCursor(name);
                if (cursor != nullptr)
                {
                    mCursorMap.emplace(name, cursor);
                    Log(Debug::Warning) << "Rejected marker-magenta cursor image '" << image->getFileName()
                                        << "' for cursor '" << name << "'; using SDL system cursor.";
                    return;
                }
            }

            auto surface = decompress(image, static_cast<float>(rotDegrees), cursorWidth, cursorHeight);

            // set the cursor and store it for later
            SDL_Cursor* curs = SDL_CreateColorCursor(surface.get(), hotspotX, hotspotY);

            mCursorMap.emplace(name, curs);
        }
        catch (std::exception& e)
        {
            Log(Debug::Warning) << e.what();
            Log(Debug::Warning) << "Using default cursor.";
            return;
        }
    }

}
