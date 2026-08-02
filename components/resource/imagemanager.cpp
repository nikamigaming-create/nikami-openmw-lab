#include "imagemanager.hpp"

#include <cassert>
#include <cstdlib>
#include <string_view>
#include <osgDB/Registry>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "objectcache.hpp"

#ifdef OSG_LIBRARY_STATIC
// This list of plugins should match with the list in the top-level CMakelists.txt.
USE_OSGPLUGIN(png)
USE_OSGPLUGIN(tga)
USE_OSGPLUGIN(dds)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(bmp)
USE_OSGPLUGIN(osg)
USE_SERIALIZER_WRAPPER_LIBRARY(osg)
#endif

namespace
{
    constexpr VFS::Path::NormalizedView sFalloutCursor("textures/interface/icons/misc/cursor.dds");
    constexpr VFS::Path::NormalizedView sFalloutUiChrome("textures/interface/shared/button/frame_idle.dds");
    constexpr VFS::Path::NormalizedView sFalloutCrosshair("textures/interface/hud/crosshair.dds");
    constexpr VFS::Path::NormalizedView sFalloutCompass("textures/interface/hud/compass_cropped.dds");
    constexpr VFS::Path::NormalizedView sFalloutStealthIndicator("textures/interface/hud/stealth_indicator.dds");
    constexpr VFS::Path::NormalizedView sFalloutHitGradient("textures/interface/hud/hitgradientleft.dds");

    bool isLegacyCursorPath(VFS::Path::NormalizedView path)
    {
        return path == "textures/tx_cursor.dds" || path == "textures/tx_cursormove.dds"
            || path == "textures/cursor_drop_ground.dds";
    }

    bool isLegacyMorrowindUiPath(VFS::Path::NormalizedView path)
    {
        const std::string_view value = path.value();
        return value == "textures/door_icon.dds" || value == "textures/target.dds" || value == "textures/compass.dds"
            || value == "textures/scroll.dds" || value == "textures/player_hit_01.dds"
            || value == "icons/tx_goldicon.dds" || value.starts_with("textures/menu_")
            || value.starts_with("textures/tx_menubook") || value.starts_with("icons/k/");
    }

    bool resolveFalloutUiAsset(VFS::Path::NormalizedView path, const VFS::Manager& vfs,
        VFS::Path::NormalizedView& resolvedPath)
    {
        if (!isLegacyMorrowindUiPath(path) || !vfs.exists(sFalloutUiChrome))
            return false;

        if (path == "textures/target.dds" && vfs.exists(sFalloutCrosshair))
            resolvedPath = sFalloutCrosshair;
        else if (path == "textures/compass.dds" && vfs.exists(sFalloutCompass))
            resolvedPath = sFalloutCompass;
        else if (path == "icons/k/stealth_sneak.dds" && vfs.exists(sFalloutStealthIndicator))
            resolvedPath = sFalloutStealthIndicator;
        else if (path == "textures/player_hit_01.dds" && vfs.exists(sFalloutHitGradient))
            resolvedPath = sFalloutHitGradient;
        else
            resolvedPath = sFalloutUiChrome;
        return true;
    }

    osg::ref_ptr<osg::Image> createWarningImage()
    {
        osg::ref_ptr<osg::Image> warningImage = new osg::Image;

        int width = 8, height = 8;
        warningImage->allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);
        assert(warningImage->isDataContiguous());
        unsigned char* data = warningImage->data();
        const bool neutralWorldViewerFallback
            = std::getenv("OPENMW_WORLD_VIEWER_NEUTRAL_MISSING_TEXTURES") != nullptr;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int i = y * width + x;
                if (neutralWorldViewerFallback)
                {
                    const unsigned char shade = ((x / 2 + y / 2) % 2) == 0 ? 150 : 96;
                    data[3 * i] = shade;
                    data[3 * i + 1] = shade;
                    data[3 * i + 2] = shade;
                }
                else
                {
                    data[3 * i] = (255);
                    data[3 * i + 1] = (0);
                    data[3 * i + 2] = (255);
                }
            }
        }
        return warningImage;
    }

    bool isS3TC(osg::Image* image)
    {
        switch (image->getPixelFormat())
        {
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return true;
        }
        return false;
    }

    bool checkSupported(osg::Image* image)
    {
        // not bothering with checks for other compression formats right now
        if (!isS3TC(image))
            return true;

        // hashtag yolo (CS might not have context when loading assets)
        if (!SceneUtil::glExtensionsReady())
            return true;

        return SceneUtil::getGLExtensions().isTextureCompressionS3TCSupported;
    }

}

namespace Resource
{

    ImageManager::ImageManager(const VFS::Manager* vfs, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mWarningImage(createWarningImage())
        , mOptions(new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields"))
    {
    }

    ImageManager::~ImageManager() {}

    osg::ref_ptr<osg::Image> ImageManager::getImage(VFS::Path::NormalizedView path, bool disableFlip)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (obj)
            return osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(obj.get()));
        else
        {
            Files::IStreamPtr stream;
            VFS::Path::NormalizedView resolvedPath = path;
            if (!mVFS->exists(path) && resolveFalloutUiAsset(path, *mVFS, resolvedPath))
            {
                // The generic MyGUI layouts are still used while the FNV UI
                // renderer is being assembled. Resolve their legacy chrome to
                // installed Fallout interface art rather than requesting
                // Morrowind-only texture names from Fallout archives.
            }
            else if (!mVFS->exists(path) && isLegacyCursorPath(path) && mVFS->exists(sFalloutCursor))
            {
                resolvedPath = sFalloutCursor;
                Log(Debug::Info) << "Using native Fallout cursor " << resolvedPath << " for " << path;
            }
            try
            {
                stream = mVFS->get(resolvedPath);
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to open image: " << e.what();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            const std::string ext(Misc::getFileExtension(resolvedPath.value()));
            osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension(ext);
            if (!reader)
            {
                Log(Debug::Error) << "Error loading " << path << ": no readerwriter for '" << ext << "' found";
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            bool killAlpha = false;
            if (reader->supportedExtensions().count("tga"))
            {
                // Morrowind ignores the alpha channel of 16bpp TGA files even when the header says not to
                unsigned char header[18];
                stream->read((char*)header, 18);
                if (stream->gcount() != 18)
                {
                    Log(Debug::Error) << "Error loading " << path << ": couldn't read TGA header";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                int type = header[2];
                int depth;
                if (type == 1 || type == 9)
                    depth = header[7];
                else
                    depth = header[16];
                int alphaBPP = header[17] & 0x0F;
                killAlpha = depth == 16 && alphaBPP == 1;
                stream->seekg(0);
            }

            osgDB::ReaderWriter::ReadResult result = reader->readImage(*stream, mOptions);
            if (!result.success())
            {
                Log(Debug::Error) << "Error loading " << path << ": " << result.message() << " code "
                                  << result.status();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            osg::ref_ptr<osg::Image> image = result.getImage();

            image->setFileName(std::string(path.value()));
            if (!checkSupported(image))
            {
                static bool uncompress = (getenv("OPENMW_DECOMPRESS_TEXTURES") != nullptr);
                if (!uncompress)
                {
                    Log(Debug::Error) << "Error loading " << path << ": no S3TC texture compression support installed";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }
                else
                {
                    // decompress texture in software if not supported by GPU
                    // requires update to getColor() to be released with OSG 3.6
                    osg::ref_ptr<osg::Image> newImage = new osg::Image;
                    newImage->setFileName(image->getFileName());
                    newImage->setOrigin(image->getOrigin());
                    newImage->allocateImage(image->s(), image->t(), image->r(),
                        image->isImageTranslucent() ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
                    for (int s = 0; s < image->s(); ++s)
                        for (int t = 0; t < image->t(); ++t)
                            for (int r = 0; r < image->r(); ++r)
                                newImage->setColor(image->getColor(s, t, r), s, t, r);
                    image = newImage;
                }
            }
            else if (killAlpha)
            {
                osg::ref_ptr<osg::Image> newImage = new osg::Image;
                newImage->setFileName(image->getFileName());
                newImage->setOrigin(image->getOrigin());
                newImage->allocateImage(image->s(), image->t(), image->r(), GL_RGB, GL_UNSIGNED_BYTE);
                // OSG just won't write the alpha as there's nowhere to put it.
                for (int s = 0; s < image->s(); ++s)
                    for (int t = 0; t < image->t(); ++t)
                        for (int r = 0; r < image->r(); ++r)
                            newImage->setColor(image->getColor(s, t, r), s, t, r);
                image = newImage;
            }

            // OSG might not set the right origin for DDS
            if (ext == "dds")
                image->setOrigin(osg::Image::TOP_LEFT);

            // Convert the image to the convention we expect
            if (image->getOrigin() == osg::Image::BOTTOM_LEFT && !disableFlip)
            {
                if (image->isCompressed() && !isS3TC(image))
                {
                    // This is most likely a KTX texture that OSG can't flip
                    // We don't want it to be corrupted or displayed incorrectly, so bail
                    // OSGoS *can* flip RGTC, but we can't verify that (yet?)
                    Log(Debug::Error) << "Error loading " << path << ": cannot flip non-S3TC compressed texture";
                    mCache->addEntryToObjectCache(path.value(), mWarningImage);
                    return mWarningImage;
                }

                image->flipVertical();
                image->setOrigin(osg::Image::TOP_LEFT);
            }

            mCache->addEntryToObjectCache(path.value(), image);
            return image;
        }
    }

    osg::Image* ImageManager::getWarningImage()
    {
        return mWarningImage;
    }

    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);
    }

}
