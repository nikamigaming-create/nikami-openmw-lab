#include "myguitexture.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <osg/StateSet>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/manager.hpp>

namespace MyGUIPlatform
{
    namespace
    {
        struct FalloutTaiAtlasEntry
        {
            std::string mAtlasPath;
            std::string mAtlasType;
            float mU = 0.f;
            float mV = 0.f;
            float mWidth = 0.f;
            float mHeight = 0.f;
        };

        bool shouldDisableReaderFlip(const std::string& fname)
        {
            std::string normalized = fname;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            return normalized.rfind("textures/interface/loading/", 0) == 0
                || normalized.rfind("splash/", 0) == 0;
        }

        std::string lowerSlashes(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string trim(std::string value)
        {
            const auto first = std::find_if_not(value.begin(), value.end(),
                [](unsigned char c) { return std::isspace(c) != 0; });
            const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                  [](unsigned char c) { return std::isspace(c) != 0; })
                                  .base();
            if (first >= last)
                return {};
            return std::string(first, last);
        }

        std::string basenameLower(std::string value)
        {
            value = lowerSlashes(std::move(value));
            const std::size_t slash = value.find_last_of('/');
            if (slash != std::string::npos)
                value.erase(0, slash + 1);
            return value;
        }

        std::vector<std::string> splitCsv(const std::string& value)
        {
            std::vector<std::string> result;
            std::istringstream stream(value);
            std::string field;
            while (std::getline(stream, field, ','))
                result.push_back(trim(field));
            return result;
        }

        const std::map<std::string, FalloutTaiAtlasEntry>& loadFalloutTaiAtlas(const VFS::Manager* vfs)
        {
            using AtlasMap = std::map<std::string, FalloutTaiAtlasEntry>;
            static const VFS::Manager* sLoadedVfs = nullptr;
            static AtlasMap sAtlas;
            if (sLoadedVfs == vfs)
                return sAtlas;

            sLoadedVfs = vfs;
            sAtlas.clear();
            if (vfs == nullptr)
                return sAtlas;

            const VFS::Path::Normalized taiPath("textures/interface/interfaceshared.tai");
            if (!vfs->exists(taiPath))
                return sAtlas;

            try
            {
                Files::IStreamPtr stream = vfs->get(taiPath);
                std::string line;
                while (std::getline(*stream, line))
                {
                    line = trim(line);
                    if (line.empty() || line[0] == '#')
                        continue;

                    std::istringstream row(line);
                    std::string source;
                    row >> source;
                    std::string rest;
                    std::getline(row, rest);
                    const std::vector<std::string> fields = splitCsv(rest);
                    if (source.empty() || fields.size() < 8)
                        continue;

                    FalloutTaiAtlasEntry entry;
                    entry.mAtlasPath = "textures/interface/" + lowerSlashes(fields[0]);
                    entry.mAtlasType = fields[2];
                    if (entry.mAtlasType != "2D")
                        continue;

                    entry.mU = std::stof(fields[3]);
                    entry.mV = std::stof(fields[4]);
                    entry.mWidth = std::stof(fields[6]);
                    entry.mHeight = std::stof(fields[7]);
                    if (entry.mWidth <= 0.f || entry.mHeight <= 0.f)
                        continue;

                    sAtlas.emplace(basenameLower(source), std::move(entry));
                }

                Log(Debug::Info) << "FNV/ESM4 diag: loaded TAI atlas " << taiPath << " rows=" << sAtlas.size();
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: failed to parse TAI atlas " << taiPath << ": " << e.what();
                sAtlas.clear();
            }

            return sAtlas;
        }

        osg::ref_ptr<osg::Image> cropFalloutTaiAtlasImage(
            Resource::ImageManager* imageManager, const std::string& requestedName)
        {
            if (imageManager == nullptr)
                return nullptr;

            const std::map<std::string, FalloutTaiAtlasEntry>& atlas = loadFalloutTaiAtlas(imageManager->getVFS());
            const std::string key = basenameLower(requestedName);
            const auto found = atlas.find(key);
            if (found == atlas.end())
                return nullptr;

            const FalloutTaiAtlasEntry& entry = found->second;
            osg::ref_ptr<osg::Image> atlasImage = imageManager->getImage(VFS::Path::Normalized(entry.mAtlasPath));
            if (atlasImage == nullptr || atlasImage == imageManager->getWarningImage() || atlasImage->s() <= 0
                || atlasImage->t() <= 0)
                return nullptr;

            const int atlasWidth = atlasImage->s();
            const int atlasHeight = atlasImage->t();
            const int x0 = std::clamp(static_cast<int>(std::floor(entry.mU * atlasWidth)), 0, atlasWidth - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(entry.mV * atlasHeight)), 0, atlasHeight - 1);
            const int x1 = std::clamp(static_cast<int>(std::ceil((entry.mU + entry.mWidth) * atlasWidth)), x0 + 1, atlasWidth);
            const int y1 = std::clamp(static_cast<int>(std::ceil((entry.mV + entry.mHeight) * atlasHeight)), y0 + 1, atlasHeight);

            osg::ref_ptr<osg::Image> cropped = new osg::Image;
            cropped->allocateImage(x1 - x0, y1 - y0, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            cropped->setOrigin(osg::Image::TOP_LEFT);
            cropped->setFileName(requestedName);

            for (int y = 0; y < cropped->t(); ++y)
                for (int x = 0; x < cropped->s(); ++x)
                    cropped->setColor(atlasImage->getColor(x0 + x, y0 + y), x, y);

            Log(Debug::Info) << "FNV/ESM4 diag: resolved TAI atlas texture " << requestedName << " from "
                             << entry.mAtlasPath << " rect=(" << x0 << ", " << y0 << ", " << cropped->s() << ", "
                             << cropped->t() << ")";
            return cropped;
        }
    }

    OSGTexture::OSGTexture(const std::string& name, Resource::ImageManager* imageManager)
        : mName(name)
        , mImageManager(imageManager)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
        , mNumElemBytes(0)
        , mWidth(0)
        , mHeight(0)
    {
    }

    OSGTexture::OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState)
        : mImageManager(nullptr)
        , mTexture(texture)
        , mInjectState(injectState)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
        , mNumElemBytes(0)
        , mWidth(texture->getTextureWidth())
        , mHeight(texture->getTextureHeight())
    {
    }

    OSGTexture::~OSGTexture() {}

    void OSGTexture::createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format)
    {
        GLenum glfmt = GL_NONE;
        size_t numelems = 0;
        switch (format.getValue())
        {
            case MyGUI::PixelFormat::L8:
                glfmt = GL_LUMINANCE;
                numelems = 1;
                break;
            case MyGUI::PixelFormat::L8A8:
                glfmt = GL_LUMINANCE_ALPHA;
                numelems = 2;
                break;
            case MyGUI::PixelFormat::R8G8B8:
                glfmt = GL_RGB;
                numelems = 3;
                break;
            case MyGUI::PixelFormat::R8G8B8A8:
                glfmt = GL_RGBA;
                numelems = 4;
                break;
        }
        if (glfmt == GL_NONE)
            throw std::runtime_error("Texture format not supported");

        mTexture = new osg::Texture2D();
        mTexture->setTextureSize(width, height);
        mTexture->setSourceFormat(glfmt);
        mTexture->setSourceType(GL_UNSIGNED_BYTE);

        mWidth = width;
        mHeight = height;

        mTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        mFormat = format;
        mUsage = usage;
        mNumElemBytes = numelems;
    }

    void OSGTexture::destroy()
    {
        mTexture = nullptr;
        mFormat = MyGUI::PixelFormat::Unknow;
        mUsage = MyGUI::TextureUsage::Default;
        mNumElemBytes = 0;
        mWidth = 0;
        mHeight = 0;
    }

    void OSGTexture::loadFromFile(const std::string& fname)
    {
        if (!mImageManager)
            throw std::runtime_error("No imagemanager set");

        const VFS::Path::Normalized path(fname);
        osg::ref_ptr<osg::Image> image;
        if (mImageManager->getVFS()->exists(path))
            image = mImageManager->getImage(path, shouldDisableReaderFlip(fname));
        else
            image = cropFalloutTaiAtlasImage(mImageManager, fname);
        if (image == nullptr)
            image = mImageManager->getImage(path, shouldDisableReaderFlip(fname));

        mTexture = new osg::Texture2D(image);
        mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mTexture->setTextureWidth(image->s());
        mTexture->setTextureHeight(image->t());
        // disable mip-maps
        mTexture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::LINEAR);

        mWidth = image->s();
        mHeight = image->t();

        mUsage = MyGUI::TextureUsage::Static;
    }

    void OSGTexture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    void* OSGTexture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (!mTexture.valid())
            throw std::runtime_error("Texture is not created");
        if (mLockedImage.valid())
            throw std::runtime_error("Texture already locked");

        mLockedImage = new osg::Image();
        mLockedImage->allocateImage(mTexture->getTextureWidth(), mTexture->getTextureHeight(),
            mTexture->getTextureDepth(), mTexture->getSourceFormat(), mTexture->getSourceType());

        return mLockedImage->data();
    }

    void OSGTexture::unlock()
    {
        if (!mLockedImage.valid())
            throw std::runtime_error("Texture not locked");

        // mTexture might be in use by the draw thread, so create a new texture instead and use that.
        osg::ref_ptr<osg::Texture2D> newTexture = new osg::Texture2D;
        newTexture->setTextureSize(getWidth(), getHeight());
        newTexture->setSourceFormat(mTexture->getSourceFormat());
        newTexture->setSourceType(mTexture->getSourceType());
        newTexture->setFilter(osg::Texture::MIN_FILTER, mTexture->getFilter(osg::Texture::MIN_FILTER));
        newTexture->setFilter(osg::Texture::MAG_FILTER, mTexture->getFilter(osg::Texture::MAG_FILTER));
        newTexture->setWrap(osg::Texture::WRAP_S, mTexture->getWrap(osg::Texture::WRAP_S));
        newTexture->setWrap(osg::Texture::WRAP_T, mTexture->getWrap(osg::Texture::WRAP_T));
        newTexture->setImage(mLockedImage.get());
        // Tell the texture it can get rid of the image for static textures (since
        // they aren't expected to update much at all).
        newTexture->setUnRefImageDataAfterApply(mUsage.isValue(MyGUI::TextureUsage::Static) ? true : false);

        mTexture = newTexture;

        mLockedImage = nullptr;
    }

    // Render-to-texture not currently implemented.
    MyGUI::IRenderTarget* OSGTexture::getRenderTarget()
    {
        return nullptr;
    }

    void OSGTexture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "OSGTexture::setShader is not implemented";
    }
}
