#include "myguitexture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <osg/Image>
#include <osg/StateSet>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/manager.hpp>

namespace
{
    using Colour = std::array<unsigned char, 4>;

    constexpr Colour sTransparent{ 0, 0, 0, 0 };
    constexpr Colour sDark{ 3, 15, 8, 224 };
    constexpr Colour sDimGreen{ 25, 92, 43, 220 };
    constexpr Colour sGreen{ 82, 238, 108, 255 };
    constexpr Colour sBrightGreen{ 170, 255, 180, 255 };
    constexpr Colour sBlack{ 0, 0, 0, 255 };

    std::string normalizeName(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
        });
        return result;
    }

    std::string_view getFileName(std::string_view path)
    {
        const std::size_t separator = path.find_last_of('/');
        return separator == std::string_view::npos ? path : path.substr(separator + 1);
    }

    bool contains(std::string_view value, std::string_view needle)
    {
        return value.find(needle) != std::string_view::npos;
    }

    bool isFallbackCandidate(std::string_view requestedName, const osg::Image& image)
    {
        // Some compatibility profiles use uniform 8x8 placeholders for the
        // Morrowind chrome. Treat those as missing so stretching them cannot
        // turn the whole menu into a blank or broken surface.
        if (image.s() > 8 || image.t() > 8)
            return false;
        const std::string name = normalizeName(requestedName);
        const std::string_view fileName = getFileName(name);
        return fileName.starts_with("menu_") || fileName.starts_with("tx_menubook")
            || contains(fileName, "cursor") || fileName == "scroll.dds" || fileName == "target.dds";
    }

    osg::ref_ptr<osg::Image> makeImage(int width, int height, const Colour& colour, std::string_view name)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setFileName("generated-mygui:" + std::string(name));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                std::copy(colour.begin(), colour.end(), image->data(x, y));
        return image;
    }

    void putPixel(osg::Image& image, int x, int y, const Colour& colour)
    {
        if (x < 0 || y < 0 || x >= image.s() || y >= image.t())
            return;
        std::copy(colour.begin(), colour.end(), image.data(x, y));
    }

    void fillRect(osg::Image& image, int left, int top, int right, int bottom, const Colour& colour)
    {
        for (int y = std::max(0, top); y <= std::min(image.t() - 1, bottom); ++y)
            for (int x = std::max(0, left); x <= std::min(image.s() - 1, right); ++x)
                putPixel(image, x, y, colour);
    }

    void drawLine(osg::Image& image, int x0, int y0, int x1, int y1, const Colour& colour)
    {
        const int dx = std::abs(x1 - x0);
        const int sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0);
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true)
        {
            putPixel(image, x0, y0, colour);
            if (x0 == x1 && y0 == y1)
                break;
            const int error2 = 2 * error;
            if (error2 >= dy)
            {
                error += dy;
                x0 += sx;
            }
            if (error2 <= dx)
            {
                error += dx;
                y0 += sy;
            }
        }
    }

    void drawRect(osg::Image& image, int left, int top, int right, int bottom, const Colour& colour)
    {
        drawLine(image, left, top, right, top, colour);
        drawLine(image, right, top, right, bottom, colour);
        drawLine(image, right, bottom, left, bottom, colour);
        drawLine(image, left, bottom, left, top, colour);
    }

    std::array<unsigned char, 7> glyph(char value)
    {
        switch (value)
        {
            case 'A': return { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 };
            case 'C': return { 0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f };
            case 'D': return { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e };
            case 'E': return { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f };
            case 'G': return { 0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f };
            case 'I': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f };
            case 'L': return { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f };
            case 'M': return { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 };
            case 'N': return { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 };
            case 'O': return { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'P': return { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 };
            case 'R': return { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 };
            case 'S': return { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e };
            case 'T': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
            case 'U': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'V': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 };
            case 'W': return { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a };
            case 'X': return { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 };
            case 'Y': return { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 };
            default: return { 0, 0, 0, 0, 0, 0, 0 };
        }
    }

    void drawText(osg::Image& image, std::string_view text)
    {
        const int scale = std::max(1, std::min(3, (image.s() - 8) / std::max(1, static_cast<int>(text.size()) * 6)));
        const int width = (static_cast<int>(text.size()) * 6 - 1) * scale;
        const int left = std::max(0, (image.s() - width) / 2);
        const int top = std::max(0, (image.t() - 7 * scale) / 2);
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const auto rows = glyph(text[index]);
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    if ((rows[y] & (1 << (4 - x))) != 0)
                        fillRect(image, left + static_cast<int>(index) * 6 * scale + x * scale, top + y * scale,
                            left + static_cast<int>(index) * 6 * scale + (x + 1) * scale - 1,
                            top + (y + 1) * scale - 1, sBrightGreen);
        }
    }

    std::string_view mainMenuLabel(std::string_view fileName)
    {
        if (fileName == "menu_return.dds") return "RETURN";
        if (fileName == "menu_newgame.dds") return "NEW GAME";
        if (fileName == "menu_savegame.dds") return "SAVE GAME";
        if (fileName == "menu_loadgame.dds") return "LOAD GAME";
        if (fileName == "menu_options.dds") return "OPTIONS";
        if (fileName == "menu_credits.dds") return "CREDITS";
        if (fileName == "menu_exitgame.dds") return "EXIT GAME";
        return {};
    }

    void logFallbackOnce(std::string_view name)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> logged;
        const std::lock_guard<std::mutex> lock(mutex);
        if (logged.emplace(name).second)
            Log(Debug::Info) << "Fallout UI: generated fallback for absent MyGUI texture '" << name << "'";
    }
}

namespace MyGUIPlatform
{

    osg::ref_ptr<osg::Image> createMissingTextureFallback(std::string_view requestedName)
    {
        const std::string name = normalizeName(requestedName);
        const std::string_view fileName = getFileName(name);
        osg::ref_ptr<osg::Image> image;

        if (const std::string_view label = mainMenuLabel(fileName); !label.empty())
        {
            image = makeImage(256, 64, sDark, name);
            fillRect(*image, 4, 4, 251, 59, Colour{ 7, 31, 15, 245 });
            drawRect(*image, 2, 2, 253, 61, sDimGreen);
            drawRect(*image, 4, 4, 251, 59, sGreen);
            drawText(*image, label);
        }
        else if (fileName == "target.dds")
        {
            image = makeImage(27, 27, sTransparent, name);
            drawLine(*image, 2, 13, 9, 13, sGreen);
            drawLine(*image, 17, 13, 24, 13, sGreen);
            drawLine(*image, 13, 2, 13, 9, sGreen);
            drawLine(*image, 13, 17, 13, 24, sGreen);
            putPixel(*image, 13, 13, sBrightGreen);
        }
        else if (contains(fileName, "cursor"))
        {
            image = makeImage(32, 32, sTransparent, name);
            for (int y = 2; y <= 22; ++y)
                fillRect(*image, 2, y, 2 + y / 2, y, y < 19 ? sBrightGreen : sGreen);
            drawLine(*image, 3, 2, 13, 22, sGreen);
            fillRect(*image, 9, 18, 13, 28, sGreen);
        }
        else if (fileName == "compass.dds")
        {
            image = makeImage(32, 32, sTransparent, name);
            drawLine(*image, 16, 4, 16, 27, sGreen);
            drawLine(*image, 16, 27, 10, 19, sGreen);
            drawLine(*image, 16, 27, 22, 19, sGreen);
        }
        else if (fileName == "menu_morrowind.dds")
        {
            image = makeImage(256, 256, Colour{ 2, 12, 6, 255 }, name);
            drawRect(*image, 8, 8, 247, 247, sDimGreen);
        }
        else if (contains(fileName, "menu_") || contains(fileName, "tx_menubook") || fileName == "scroll.dds")
        {
            image = makeImage(16, 16, sDark, name);
            drawRect(*image, 0, 0, 15, 15, sGreen);
            if (contains(fileName, "center") || contains(fileName, "middle"))
                fillRect(*image, 2, 2, 13, 13, sDimGreen);
        }
        else
        {
            image = makeImage(16, 16, Colour{ 2, 12, 6, 192 }, name);
            drawRect(*image, 0, 0, 15, 15, sDimGreen);
            drawLine(*image, 2, 2, 13, 13, sGreen);
            drawLine(*image, 13, 2, 2, 13, sGreen);
        }

        // MyGUI authoring uses top-left origin while osg::Image uses the
        // opposite row order at this boundary.
        image->flipVertical();
        logFallbackOnce(name);
        return image;
    }

    OSGTexture::OSGTexture(
        const std::string& name, Resource::ImageManager* imageManager, bool useMissingTextureFallback)
        : mName(name)
        , mImageManager(imageManager)
        , mUseMissingTextureFallback(useMissingTextureFallback)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
        , mNumElemBytes(0)
        , mWidth(0)
        , mHeight(0)
    {
    }

    OSGTexture::OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState)
        : mImageManager(nullptr)
        , mUseMissingTextureFallback(false)
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
        const VFS::Manager* vfs = mImageManager->getVFS();
        if (mUseMissingTextureFallback && vfs != nullptr && !vfs->exists(path))
            image = createMissingTextureFallback(path.value());
        else
        {
            image = mImageManager->getImage(path);
            if (mUseMissingTextureFallback && image.valid() && isFallbackCandidate(path.value(), *image))
                image = createMissingTextureFallback(path.value());
        }
        if (!image.valid())
            throw std::runtime_error("Unable to create MyGUI texture: " + fname);
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
