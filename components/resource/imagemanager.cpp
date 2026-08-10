#include "imagemanager.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>
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

}

namespace Resource
{

    ImageManager::ImageManager(const VFS::Manager* vfs, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mWarningImage(createWarningImage())
        , mOptions(new osgDB::Options("dds_flip dds_dxt1_detect_rgba ignoreTga2Fields"))
        , mOptionsNoFlip(new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields"))
    {
    }

    ImageManager::~ImageManager() {}

    bool checkSupported(osg::Image* image)
    {
        switch (image->getPixelFormat())
        {
            case (GL_COMPRESSED_RGB_S3TC_DXT1_EXT):
            case (GL_COMPRESSED_RGBA_S3TC_DXT1_EXT):
            case (GL_COMPRESSED_RGBA_S3TC_DXT3_EXT):
            case (GL_COMPRESSED_RGBA_S3TC_DXT5_EXT):
            {
                if (!SceneUtil::glExtensionsReady())
                    return true; // hashtag yolo (CS might not have context when loading assets)
                osg::GLExtensions& exts = SceneUtil::getGLExtensions();
                if (!exts.isTextureCompressionS3TCSupported
                    // This one works too. Should it be included in isTextureCompressionS3TCSupported()? Submitted as a
                    // patch to OSG.
                    && !osg::isGLExtensionSupported(exts.contextID, "GL_S3_s3tc"))
                {
                    return false;
                }
                break;
            }
            // not bothering with checks for other compression formats right now
            default:
                return true;
        }
        return true;
    }

    osg::ref_ptr<osg::Image> ImageManager::getImage(VFS::Path::NormalizedView path, bool disableFlip)
    {
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (obj)
            return osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(obj.get()));
        else
        {
            Files::IStreamPtr stream;
            try
            {
                stream = mVFS->get(path);
            }
            catch (std::exception& e)
            {
                // Fallout's menu XML names logical interface textures that are
                // packed into InterfaceShared*.dds.  The authored .tai file is
                // the mapping contract; resolve and crop that atlas entry
                // before treating the logical DDS as missing.
                const std::string requested(path.value());
                const std::size_t slash = requested.find_last_of('/');
                const std::string fileName = slash == std::string::npos ? requested : requested.substr(slash + 1);
                if (requested.starts_with("textures/interface/") && fileName.ends_with(".dds"))
                {
                    try
                    {
                        const VFS::Path::Normalized taiPath("textures/interface/interfaceshared.tai");
                        Files::IStreamPtr taiStream = mVFS->get(taiPath);
                        std::string line;
                        while (std::getline(*taiStream, line))
                        {
                            if (!line.starts_with(fileName))
                                continue;
                            std::string fieldsText = line.substr(fileName.size());
                            std::vector<std::string> fields;
                            std::size_t begin = 0;
                            while (begin <= fieldsText.size())
                            {
                                const std::size_t comma = fieldsText.find(',', begin);
                                std::string field = fieldsText.substr(begin,
                                    comma == std::string::npos ? std::string::npos : comma - begin);
                                const std::size_t first = field.find_first_not_of(" \t\r\n");
                                const std::size_t last = field.find_last_not_of(" \t\r\n");
                                fields.push_back(first == std::string::npos ? std::string()
                                    : field.substr(first, last - first + 1));
                                if (comma == std::string::npos)
                                    break;
                                begin = comma + 1;
                            }
                            if (fields.size() < 8 || fields[2] != "2D")
                                break;

                            const VFS::Path::Normalized atlasPath(
                                "textures/interface/" + fields[0]);
                            osg::ref_ptr<osg::Image> atlas = getImage(atlasPath, disableFlip);
                            if (atlas == nullptr || atlas == mWarningImage || atlas->s() <= 0 || atlas->t() <= 0)
                                break;

                            const float u = std::stof(fields[3]);
                            const float v = std::stof(fields[4]);
                            const float width = std::stof(fields[6]);
                            const float height = std::stof(fields[7]);
                            const int left = std::clamp(
                                static_cast<int>(std::lround(u * atlas->s() - 0.5f)), 0, atlas->s() - 1);
                            const int top = std::clamp(
                                static_cast<int>(std::lround(v * atlas->t() - 0.5f)), 0, atlas->t() - 1);
                            const int right = std::clamp(
                                static_cast<int>(std::lround((u + width) * atlas->s() - 0.5f)), left + 1, atlas->s());
                            const int bottom = std::clamp(
                                static_cast<int>(std::lround((v + height) * atlas->t() - 0.5f)), top + 1, atlas->t());

                            osg::ref_ptr<osg::Image> image = new osg::Image;
                            image->allocateImage(right - left, bottom - top, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                            image->setFileName(requested);
                            for (int y = 0; y < image->t(); ++y)
                                for (int x = 0; x < image->s(); ++x)
                                    image->setColor(atlas->getColor(left + x, top + y), x, y);

                            Log(Debug::Info) << "Resolved Fallout interface texture " << path
                                             << " from " << taiPath << " atlas=" << atlasPath
                                             << " rect=" << left << ',' << top << ',' << image->s() << ','
                                             << image->t();
                            mCache->addEntryToObjectCache(path.value(), image);
                            return image;
                        }
                    }
                    catch (const std::exception& atlasError)
                    {
                        Log(Debug::Warning) << "Failed Fallout interface atlas lookup for " << path
                                            << ": " << atlasError.what();
                    }
                }
                Log(Debug::Error) << "Failed to open image: " << e.what();
                mCache->addEntryToObjectCache(path.value(), mWarningImage);
                return mWarningImage;
            }

            const std::string ext(Misc::getFileExtension(path.value()));
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

            osgDB::ReaderWriter::ReadResult result
                = reader->readImage(*stream, disableFlip ? mOptionsNoFlip : mOptions);
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
                newImage->allocateImage(image->s(), image->t(), image->r(), GL_RGB, GL_UNSIGNED_BYTE);
                // OSG just won't write the alpha as there's nowhere to put it.
                for (int s = 0; s < image->s(); ++s)
                    for (int t = 0; t < image->t(); ++t)
                        for (int r = 0; r < image->r(); ++r)
                            newImage->setColor(image->getColor(s, t, r), s, t, r);
                image = newImage;
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
