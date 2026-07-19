#include "myguitexture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <osg/Image>
#include <osg/StateSet>
#include <osg/Texture2D>
//## VR_PATCH BEGIN
// Change members from osg::Texture2D to osg::Texture
#include <osg/Texture>
//## VR_PATCH END

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

    bool contains(std::string_view value, std::string_view needle)
    {
        return value.find(needle) != std::string_view::npos;
    }

    std::string normalizeName(std::string_view value)
    {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            if (c == '\\')
                return '/';
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }

    std::string_view getFileName(std::string_view path)
    {
        const std::size_t separator = path.find_last_of('/');
        return separator == std::string_view::npos ? path : path.substr(separator + 1);
    }

    osg::ref_ptr<osg::Image> makeImage(int width, int height, const Colour& colour, std::string_view name)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setFileName("generated-mygui:" + std::string(name));
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                unsigned char* pixel = image->data(x, y);
                std::copy(colour.begin(), colour.end(), pixel);
            }
        }
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
            case 'B': return { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e };
            case 'C': return { 0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f };
            case 'D': return { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e };
            case 'E': return { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f };
            case 'F': return { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 };
            case 'G': return { 0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f };
            case 'H': return { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 };
            case 'I': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f };
            case 'J': return { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c };
            case 'K': return { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
            case 'L': return { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f };
            case 'M': return { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 };
            case 'N': return { 0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11 };
            case 'O': return { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'P': return { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 };
            case 'Q': return { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d };
            case 'R': return { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 };
            case 'S': return { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e };
            case 'T': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
            case 'U': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'V': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 };
            case 'W': return { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a };
            case 'X': return { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 };
            case 'Y': return { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 };
            case 'Z': return { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f };
            case '?': return { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 };
            default: return { 0, 0, 0, 0, 0, 0, 0 };
        }
    }

    void drawGlyph(osg::Image& image, char value, int left, int top, int scale, const Colour& colour)
    {
        const auto rows = glyph(value);
        for (int y = 0; y < 7; ++y)
        {
            for (int x = 0; x < 5; ++x)
            {
                if ((rows[y] & (1 << (4 - x))) == 0)
                    continue;
                fillRect(image, left + x * scale, top + y * scale, left + (x + 1) * scale - 1,
                    top + (y + 1) * scale - 1, colour);
            }
        }
    }

    void drawText(osg::Image& image, std::string_view text, const Colour& colour)
    {
        if (text.empty())
            return;
        const int maxScaleX = std::max(1, (image.s() - 8) / std::max(1, static_cast<int>(text.size()) * 6 - 1));
        const int maxScaleY = std::max(1, (image.t() - 8) / 7);
        const int scale = std::max(1, std::min({ 4, maxScaleX, maxScaleY }));
        const int textWidth = (static_cast<int>(text.size()) * 6 - 1) * scale;
        const int left = std::max(0, (image.s() - textWidth) / 2);
        const int top = std::max(0, (image.t() - 7 * scale) / 2);
        for (std::size_t i = 0; i < text.size(); ++i)
            drawGlyph(image, text[i], left + static_cast<int>(i) * 6 * scale + 1, top + 1, scale, sBlack);
        for (std::size_t i = 0; i < text.size(); ++i)
            drawGlyph(image, text[i], left + static_cast<int>(i) * 6 * scale, top, scale, colour);
    }

    std::string_view mainMenuLabel(std::string_view fileName)
    {
        if (fileName == "menu_return.dds")
            return "RETURN";
        if (fileName == "menu_newgame.dds")
            return "NEW GAME";
        if (fileName == "menu_savegame.dds")
            return "SAVE GAME";
        if (fileName == "menu_loadgame.dds")
            return "LOAD GAME";
        if (fileName == "menu_options.dds")
            return "OPTIONS";
        if (fileName == "menu_credits.dds")
            return "CREDITS";
        if (fileName == "menu_exitgame.dds")
            return "EXIT GAME";
        return {};
    }

    std::string_view bookButtonLabel(std::string_view fileName)
    {
        if (contains(fileName, "_take_"))
            return "TAKE";
        if (contains(fileName, "_close_"))
            return "CLOSE";
        if (contains(fileName, "_journal_"))
            return "LOG";
        if (contains(fileName, "_topics_"))
            return "TOPICS";
        if (contains(fileName, "_quests_"))
            return "QUESTS";
        if (contains(fileName, "_cancel_"))
            return "CANCEL";
        return {};
    }

    void logFallbackOnce(std::string_view name, std::string_view category)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> logged;
        const std::lock_guard<std::mutex> lock(mutex);
        if (logged.emplace(name).second)
            Log(Debug::Info) << "FNV UI: generated " << category << " fallback for absent MyGUI texture '" << name
                             << "'";
    }

    osg::ref_ptr<osg::Image> makeChrome(std::string_view name, int size)
    {
        osg::ref_ptr<osg::Image> image = makeImage(size, size, sDark, name);
        const std::string_view fileName = getFileName(name);
        const bool top = contains(fileName, "top");
        const bool bottom = contains(fileName, "bottom");
        const bool left = contains(fileName, "left");
        const bool right = contains(fileName, "right");
        const bool centre = contains(fileName, "center") || contains(fileName, "middle");
        if (centre)
        {
            fillRect(*image, 0, 0, size - 1, size - 1, sDimGreen);
            drawRect(*image, 0, 0, size - 1, size - 1, sGreen);
            return image;
        }

        if (top)
            drawLine(*image, 0, 0, size - 1, 0, sGreen);
        if (bottom)
            drawLine(*image, 0, size - 1, size - 1, size - 1, sGreen);
        if (left)
            drawLine(*image, 0, 0, 0, size - 1, sGreen);
        if (right)
            drawLine(*image, size - 1, 0, size - 1, size - 1, sGreen);
        if (!top && !bottom && !left && !right)
            drawRect(*image, 0, 0, size - 1, size - 1, sGreen);
        return image;
    }
}

namespace MyGUIPlatform
{

    osg::ref_ptr<osg::Image> createMissingTextureFallback(std::string_view requestedName)
    {
        const std::string name = normalizeName(requestedName);
        const std::string_view fileName = getFileName(name);
        std::string_view category = "generic UI";
        osg::ref_ptr<osg::Image> image;

        if (const std::string_view label = mainMenuLabel(fileName); !label.empty())
        {
            category = "main-menu button";
            image = makeImage(256, 64, sDark, name);
            fillRect(*image, 4, 4, 251, 59, Colour{ 7, 31, 15, 245 });
            drawRect(*image, 2, 2, 253, 61, sDimGreen);
            drawRect(*image, 4, 4, 251, 59, sGreen);
            drawText(*image, label, sBrightGreen);
        }
        else if (fileName == "target.dds")
        {
            category = "crosshair";
            image = makeImage(27, 27, sTransparent, name);
            drawLine(*image, 2, 13, 9, 13, sGreen);
            drawLine(*image, 17, 13, 24, 13, sGreen);
            drawLine(*image, 13, 2, 13, 9, sGreen);
            drawLine(*image, 13, 17, 13, 24, sGreen);
            putPixel(*image, 13, 13, sBrightGreen);
        }
        else if (fileName == "compass.dds")
        {
            category = "compass";
            image = makeImage(32, 32, sTransparent, name);
            drawLine(*image, 16, 4, 16, 27, sGreen);
            drawLine(*image, 16, 27, 10, 19, sGreen);
            drawLine(*image, 16, 27, 22, 19, sGreen);
            drawLine(*image, 15, 5, 15, 19, sDimGreen);
        }
        else if (contains(fileName, "cursor"))
        {
            category = "cursor";
            image = makeImage(32, 32, sTransparent, name);
            if (contains(fileName, "move"))
            {
                drawLine(*image, 4, 16, 27, 16, sBrightGreen);
                drawLine(*image, 16, 4, 16, 27, sBrightGreen);
                drawLine(*image, 4, 16, 9, 12, sGreen);
                drawLine(*image, 4, 16, 9, 20, sGreen);
                drawLine(*image, 27, 16, 22, 12, sGreen);
                drawLine(*image, 27, 16, 22, 20, sGreen);
                drawLine(*image, 16, 4, 12, 9, sGreen);
                drawLine(*image, 16, 4, 20, 9, sGreen);
                drawLine(*image, 16, 27, 12, 22, sGreen);
                drawLine(*image, 16, 27, 20, 22, sGreen);
            }
            else if (contains(fileName, "drop_ground"))
            {
                drawLine(*image, 16, 4, 16, 25, sBrightGreen);
                drawLine(*image, 16, 25, 8, 17, sGreen);
                drawLine(*image, 16, 25, 24, 17, sGreen);
            }
            else
            {
                for (int y = 2; y <= 22; ++y)
                    fillRect(*image, 2, y, 2 + y / 2, y, y < 19 ? sBrightGreen : sGreen);
                drawLine(*image, 3, 2, 13, 22, sGreen);
                fillRect(*image, 9, 18, 13, 28, sGreen);
            }
        }
        else if (fileName == "menu_bar_gray.dds")
        {
            category = "progress-bar fill";
            image = makeImage(16, 8, Colour{ 220, 220, 220, 255 }, name);
            fillRect(*image, 0, 0, 15, 1, Colour{ 255, 255, 255, 255 });
            fillRect(*image, 0, 6, 15, 7, Colour{ 145, 145, 145, 255 });
        }
        else if (contains(fileName, "menu_thin_border_"))
        {
            category = "thin menu chrome";
            image = makeChrome(name, 2);
        }
        else if (contains(fileName, "menu_thick_border_") || contains(fileName, "menu_button_frame_"))
        {
            category = "menu chrome";
            image = makeChrome(name, 4);
        }
        else if (contains(fileName, "menu_head_block_") || contains(fileName, "menu_rightbutton"))
        {
            category = "window-control chrome";
            image = makeChrome(name, 2);
        }
        else if (contains(fileName, "menu_small_energy_bar_"))
        {
            category = "progress-bar frame";
            image = makeChrome(name, 4);
        }
        else if (fileName == "tx_menubook.dds" || fileName == "tx_menubook_bookmark.dds"
            || fileName == "scroll.dds")
        {
            category = "readable paper background";
            image = makeImage(256, 128, Colour{ 202, 185, 139, 255 }, name);
            for (int y = 0; y < image->t(); ++y)
            {
                const unsigned char shade = static_cast<unsigned char>((y / 8) % 2 == 0 ? 8 : 0);
                for (int x = 0; x < image->s(); ++x)
                {
                    unsigned char* pixel = image->data(x, y);
                    pixel[0] = static_cast<unsigned char>(pixel[0] - shade);
                    pixel[1] = static_cast<unsigned char>(pixel[1] - shade);
                    pixel[2] = static_cast<unsigned char>(pixel[2] - shade);
                }
            }
            drawRect(*image, 1, 1, 254, 126, Colour{ 91, 67, 35, 255 });
            if (fileName == "tx_menubook.dds")
                drawLine(*image, 128, 2, 128, 125, Colour{ 128, 100, 58, 180 });
        }
        else if (contains(fileName, "tx_menubook_"))
        {
            category = "book control";
            image = makeImage(96, 32, Colour{ 178, 157, 108, 245 }, name);
            drawRect(*image, 1, 1, 94, 30, Colour{ 91, 67, 35, 255 });
            if (contains(fileName, "_prev_"))
            {
                drawLine(*image, 58, 7, 38, 16, Colour{ 45, 35, 20, 255 });
                drawLine(*image, 38, 16, 58, 25, Colour{ 45, 35, 20, 255 });
            }
            else if (contains(fileName, "_next_"))
            {
                drawLine(*image, 38, 7, 58, 16, Colour{ 45, 35, 20, 255 });
                drawLine(*image, 58, 16, 38, 25, Colour{ 45, 35, 20, 255 });
            }
            else
            {
                const std::string_view label = bookButtonLabel(fileName);
                drawText(*image, label.empty() ? std::string_view("MENU") : label, Colour{ 45, 35, 20, 255 });
            }
        }
        else if (fileName == "menu_morrowind.dds")
        {
            category = "menu background";
            image = makeImage(256, 256, Colour{ 2, 12, 6, 255 }, name);
            for (int y = 0; y < image->t(); ++y)
            {
                const unsigned char green = static_cast<unsigned char>(12 + y / 8);
                fillRect(*image, 0, y, image->s() - 1, y, Colour{ 2, green, 7, 255 });
            }
            drawRect(*image, 8, 8, 247, 247, sDimGreen);
        }
        else if (contains(name, "icons/") || fileName == "door_icon.dds" || contains(fileName, "map_marker"))
        {
            category = "HUD icon";
            image = makeImage(32, 32, sTransparent, name);
            if (contains(fileName, "stealth"))
            {
                drawLine(*image, 4, 16, 11, 10, sGreen);
                drawLine(*image, 11, 10, 20, 10, sGreen);
                drawLine(*image, 20, 10, 27, 16, sGreen);
                drawLine(*image, 27, 16, 20, 22, sGreen);
                drawLine(*image, 20, 22, 11, 22, sGreen);
                drawLine(*image, 11, 22, 4, 16, sGreen);
                fillRect(*image, 14, 13, 18, 19, sBrightGreen);
            }
            else if (contains(fileName, "gold"))
            {
                drawRect(*image, 8, 8, 23, 23, Colour{ 245, 210, 80, 255 });
                drawText(*image, "G", Colour{ 245, 210, 80, 255 });
            }
            else
            {
                drawLine(*image, 16, 3, 28, 16, sGreen);
                drawLine(*image, 28, 16, 16, 28, sGreen);
                drawLine(*image, 16, 28, 3, 16, sGreen);
                drawLine(*image, 3, 16, 16, 3, sGreen);
                drawText(*image, "?", sBrightGreen);
            }
        }
        else if (fileName == "player_hit_01.dds")
        {
            category = "damage overlay";
            image = makeImage(8, 8, Colour{ 120, 10, 4, 48 }, name);
        }
        else
        {
            image = makeImage(16, 16, Colour{ 2, 12, 6, 192 }, name);
            drawRect(*image, 0, 0, 15, 15, sDimGreen);
            drawLine(*image, 2, 2, 13, 13, sGreen);
            drawLine(*image, 13, 2, 2, 13, sGreen);
        }

        logFallbackOnce(name, category);
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

//## VR_PATCH BEGIN
    OSGTexture::OSGTexture(osg::Texture* texture, osg::StateSet* injectState)
//## VR_PATCH END
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

//## VR_PATCH BEGIN
        auto* texture2D = new osg::Texture2D();
        mTexture = texture2D;
        texture2D->setTextureSize(width, height);
        texture2D->setSourceFormat(glfmt);
        texture2D->setSourceType(GL_UNSIGNED_BYTE);

        mWidth = width;
        mHeight = height;

        texture2D->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture2D->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture2D->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture2D->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
//## VR_PATCH END

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
            image = mImageManager->getImage(path);
//## VR_PATCH BEGIN
        auto* texture2D = new osg::Texture2D(image);
        mTexture = texture2D;
        texture2D->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture2D->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture2D->setTextureWidth(image->s());
        texture2D->setTextureHeight(image->t());
        // disable mip-maps
        texture2D->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
//## VR_PATCH END

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

        mLockedImage->flipVertical();

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
