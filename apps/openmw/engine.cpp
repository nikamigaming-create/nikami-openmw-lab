Warning: truncated output (original token count: 217168)
Total output lines: 15598

#include "engine.hpp"
#include "fnvsidecaripc.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/mathutil.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

// ## VR_PATCH BEGIN
#include "mwrender/camera.hpp"
#include "mwvr/vrgui.hpp"
#include "mwvr/vrinputmanager.hpp"
#include "mwvr/vranimation.hpp"
#include <components/misc/callbackmanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vr/session.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
#include <components/xr/instance.hpp>
#include <components/xr/interactionprofiles.hpp>
#include <components/xr/session.hpp>
// ## VR_PATCH END

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/esm/position.hpp>
#include <components/esm/esmcommon.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadlvln.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadotft.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadtact.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/esm4/loadwrld.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>
#include <components/misc/strings/lower.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"
#include "mwinput/actions.hpp"

#include "mwgui/inventorywindow.hpp"
#include "mwgui/itemmodel.hpp"
#include "mwgui/sortfilteritemmodel.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwgui/confirmationdialog.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/action.hpp"
#include "mwworld/actionequip.hpp"
#include "mwworld/actorfacing.hpp"
#include "mwworld/cellstore.hpp"
#include "mwworld/containerstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/esmstore.hpp"
#include "mwworld/esm4questruntime.hpp"
#include "mwworld/inventorystore.hpp"
#include "mwworld/manualref.hpp"
#include "mwworld/worldimp.hpp"
#include "mwworld/worldmodel.hpp"

#include "mwphysics/collisiontype.hpp"
#include "mwphysics/raycasting.hpp"

#include "mwrender/characterpreview.hpp"
#include "mwrender/animation.hpp"
#include "mwrender/esm4npcanimation.hpp"
#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"
#include "mwclass/esm4base.hpp"
#include "mwclass/esm4npc.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"
#include "mwmechanics/actorutil.hpp"
#include "mwmechanics/movement.hpp"
#include "mwmechanics/stat.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    osg::Camera* createFalloutNeutralActorPreviewComposite(
        const std::vector<std::unique_ptr<MWRender::FalloutActorPreview>>& previews)
    {
        osg::ref_ptr<osg::Camera> camera = new osg::Camera;
        camera->setName("FNV Neutral Actor Preview Composite");
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setClearMask(0);
        camera->setRenderOrder(osg::Camera::POST_RENDER, 20);
        camera->setAllowEventFocus(false);
        camera->setNodeMask(MWRender::Mask_RenderToTexture);

        constexpr float width = 0.42f;
        constexpr float height = 0.92f;
        constexpr std::array<float, 3> centers = { -0.56f, 0.f, 0.56f };
        const std::size_t count = std::min<std::size_t>(previews.size(), centers.size());
        for (std::size_t i = 0; i < count; ++i)
        {
            osg::ref_ptr<osg::Texture2D> texture = previews[i] != nullptr ? previews[i]->getTexture() : nullptr;
            if (texture == nullptr)
                continue;

            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

            const float left = centers[i] - width * 0.5f;
            const float bottom = -height * 0.5f;
            osg::ref_ptr<osg::Geometry> geom = osg::createTexturedQuadGeometry(
                osg::Vec3f(left, bottom, 0.f), osg::Vec3f(width, 0.f, 0.f), osg::Vec3f(0.f, height, 0.f));
            osg::ref_ptr<osg::Vec2Array> texCoords = new osg::Vec2Array;
            texCoords->push_back(osg::Vec2f(0.f, 1.f));
            texCoords->push_back(osg::Vec2f(0.f, 0.f));
            texCoords->push_back(osg::Vec2f(1.f, 0.f));
            texCoords->push_back(osg::Vec2f(1.f, 1.f));
            geom->setTexCoordArray(0, texCoords.get(), osg::Array::BIND_PER_VERTEX);
            osg::StateSet* stateset = geom->getOrCreateStateSet();
            stateset->setTextureAttributeAndModes(0, texture.get(), osg::StateAttribute::ON);
            stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
            stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            stateset->setAttributeAndModes(
                new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));
            camera->addChild(geom);
        }

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview composite panes=" << count
                         << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
        return camera.release();
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            std::string messageFormat
                = MWBase::Environment::get().getL10nManager()->getMessage("OMWEngine", "ScreenshotMade");

            std::string message = Misc::StringUtils::format(messageFormat, filePath);

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    void writeProofJsonString(std::ostream& stream, std::string_view value)
    {
        stream << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '"': stream << "\\\""; break;
                case '\\': stream << "\\\\"; break;
                case '\b': stream << "\\b"; break;
                case '\f': stream << "\\f"; break;
                case '\n': stream << "\\n"; break;
                case '\r': stream << "\\r"; break;
                case '\t': stream << "\\t"; break;
                default:
                    if (ch < 0x20)
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
                    else
                        stream << static_cast<char>(ch);
                    break;
            }
        }
        stream << '"';
    }

    struct FNVSidecarScreenshot
    {
        std::filesystem::path mPath;
        std::filesystem::file_time_type mWriteTime{};
        std::uintmax_t mSize = 0;
        bool mValid = false;
    };

    FNVSidecarScreenshot newestSidecarScreenshot(const std::filesystem::path& directory)
    {
        FNVSidecarScreenshot result;
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error)
            return result;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (error)
                return {};
            if (!entry.is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }
            const std::string fileName = entry.path().filename().string();
            if (fileName.rfind("screenshot", 0) != 0)
                continue;
            const std::filesystem::file_time_type writeTime = entry.last_write_time(error);
            if (error)
            {
                error.clear();
                continue;
            }
            const std::uintmax_t size = entry.file_size(error);
            if (error)
            {
                error.clear();
                continue;
            }
            if (!result.mValid || writeTime > result.mWriteTime
                || (writeTime == result.mWriteTime && entry.path().native() > result.mPath.native()))
            {
                result.mPath = entry.path();
                result.mWriteTime = writeTime;
                result.mSize = size;
                result.mValid = true;
            }
        }
        return result;
    }

    bool isNewSidecarScreenshot(const FNVSidecarScreenshot& baseline, const FNVSidecarScreenshot& candidate)
    {
        if (!candidate.mValid || candidate.mSize == 0)
            return false;
        if (!baseline.mValid)
            return true;
        return candidate.mPath != baseline.mPath && candidate.mWriteTime >= baseline.mWriteTime;
    }

    enum class FNVSidecarOpenMwPhase
    {
        WaitingRetail,
        Staging,
        Settling,
        Ready,
        SettlingCaptureState,
        Capturing,
        WaitingAdvance,
        Complete,
        Failed,
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }

    int readProofInt(const char* name, int fallback)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return fallback;

        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value)
            return fallback;

        return static_cast<int>(parsed);
    }

    float readProofFloat(const char* name, float fallback)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return fallback;

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value)
            return fallback;

        return parsed;
    }

    bool parseProofFloatBits(std::string_view text, std::uint32_t& bits)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            text.remove_prefix(2);
        if (text.empty())
            return false;

        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), bits, 16);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    }

    float proofFloatFromBits(std::uint32_t bits)
    {
        return std::bit_cast<float>(bits);
    }

    std::string formatProofFloatBits(std::span<const std::uint32_t> bits)
    {
        std::ostringstream stream;
        stream << "[";
        for (std::size_t index = 0; index < bits.size(); ++index)
        {
            if (index != 0)
                stream << ",";
            stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << std::uppercase << bits[index]
                   << std::dec;
        }
        stream << "]";
        return stream.str();
    }

    constexpr std::array<std::uint32_t, 16> FalloutRetailD3DProjectionBits = {
        0x3F8B02BE, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x3FDE6AC9, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x3F800077, 0x3F800000,
        0x00000000, 0x00000000, 0xC0A00094, 0x00000000,
    };

    // D3D9 uses a [0,1] left-handed depth clip while OSG/OpenGL uses a [-1,1]
    // right-handed clip. These are the exact float words produced by applying
    // that coordinate conversion to the captured retail D3D9 matrix, not a
    // second trigonometric reconstruction of the same nominal FOV.
    constexpr std::array<std::uint32_t, 16> FalloutRetailOpenGLProjectionBits = {
        0x3F8B02BE, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x3FDE6AC9, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0xBF8000EE, 0xBF800000,
        0x00000000, 0x00000000, 0xC1200094, 0x00000000,
    };

    constexpr std::uint32_t FalloutRetailVerticalFovBits = 0x426F5C9D;
    constexpr std::uint32_t FalloutRetailNearClipBits = 0x40A00000;
    constexpr std::uint32_t FalloutRetailFarClipBits = 0x48ACC600;
    constexpr std::array<std::uint32_t, 1> FalloutRetailVerticalFovBitArray = {
        FalloutRetailVerticalFovBits,
    };
    constexpr std::array<std::uint32_t, 2> FalloutRetailNearFarBits = {
        FalloutRetailNearClipBits,
        FalloutRetailFarClipBits,
    };

    osg::Matrixf makeFalloutRetailProjectionMatrix()
    {
        osg::Matrixf matrix;
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
                matrix(row, column) = proofFloatFromBits(FalloutRetailOpenGLProjectionBits[row * 4 + column]);
        }
        return matrix;
    }

    std::array<std::uint32_t, 16> getProofMatrixBits(const osg::Matrixd& matrix)
    {
        std::array<std::uint32_t, 16> bits{};
        for (std::size_t row = 0; row < 4; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
                bits[row * 4 + column]
                    = std::bit_cast<std::uint32_t>(static_cast<float>(matrix(row, column)));
        }
        return bits;
    }

    std::vector<float> readWorldViewerFloatList(const char* name)
    {
        std::vector<float> values;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return values;

        std::string_view remaining(env);
        while (!remaining.empty())
        {
            const std::size_t comma = remaining.find(',');
            std::string_view token = remaining.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                const std::string text(token);
                char* end = nullptr;
                const float parsed = std::strtof(text.c_str(), &end);
                if (end != text.c_str() && *end == '\0')
                    values.push_back(parsed);
            }
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }

        return values;
    }

    std::vector<int> readWorldViewerIntList(const char* name)
    {
        std::vector<int> values;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return values;

        std::string_view remaining(env);
        while (!remaining.empty())
        {
            const std::size_t comma = remaining.find(',');
            std::string_view token = remaining.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                const std::string text(token);
                char* end = nullptr;
                const long parsed = std::strtol(text.c_str(), &end, 10);
                if (end != text.c_str() && *end == '\0')
                    values.push_back(static_cast<int>(parsed));
            }
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }

        return values;
    }

    std::vector<std::string> readWorldViewerStringList(const char* name)
    {
        std::vector<std::string> values;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return values;

        std::string_view remaining(env);
        while (!remaining.empty())
        {
            const std::size_t comma = remaining.find(',');
            std::string_view token = remaining.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
                values.emplace_back(token);
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }
        return values;
    }

    struct WorldViewerCameraKeyframe
    {
        int mFrame = 0;
        osg::Vec3d mEye;
        osg::Vec3d mTarget;
    };

    std::vector<WorldViewerCameraKeyframe> getWorldViewerCameraSequence()
    {
        const std::vector<int> frames = readWorldViewerIntList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_FRAMES");
        if (frames.empty())
            return {};

        const std::vector<float> eyeX = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_EYE_X");
        const std::vector<float> eyeY = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_EYE_Y");
        const std::vector<float> eyeZ = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_EYE_Z");
        const std::vector<float> targetX = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_TARGET_X");
        const std::vector<float> targetY = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_TARGET_Y");
        const std::vector<float> targetZ = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_TARGET_Z");

        std::size_t count = frames.size();
        count = std::min(count, eyeX.size());
        count = std::min(count, eyeY.size());
        count = std::min(count, eyeZ.size());
        count = std::min(count, targetX.size());
        count = std::min(count, targetY.size());
        count = std::min(count, targetZ.size());
        if (count == 0)
            return {};

        std::vector<WorldViewerCameraKeyframe> sequence;
        sequence.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            sequence.push_back({ frames[i], osg::Vec3d(eyeX[i], eyeY[i], eyeZ[i]),
                osg::Vec3d(targetX[i], targetY[i], targetZ[i]) });
        return sequence;
    }

    struct WorldViewerTimeKeyframe
    {
        int mFrame = 0;
        float mHour = 0.f;
    };

    std::vector<WorldViewerTimeKeyframe> getWorldViewerTimeSequence()
    {
        const std::vector<int> frames = readWorldViewerIntList("OPENMW_WORLD_VIEWER_TIME_SEQUENCE_FRAMES");
        const std::vector<float> hours = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_TIME_SEQUENCE_HOURS");
        const std::size_t count = std::min(frames.size(), hours.size());
        if (count == 0)
            return {};

        std::vector<WorldViewerTimeKeyframe> sequence;
        sequence.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (frames[i] >= 0 && std::isfinite(hours[i]))
                sequence.push_back({ frames[i], hours[i] });
        }
        std::stable_sort(sequence.begin(), sequence.end(), [](const auto& left, const auto& right) {
            return left.mFrame < right.mFrame;
        });
        return sequence;
    }

    struct WorldViewerCameraAngleKeyframe
    {
        int mFrame = 0;
        float mPitch = 0.f;
        float mYaw = 0.f;
    };

    std::vector<WorldViewerCameraAngleKeyframe> getWorldViewerCameraAngleSequence()
    {
        const std::vector<int> frames
            = readWorldViewerIntList("OPENMW_WORLD_VIEWER_CAMERA_ANGLE_SEQUENCE_FRAMES");
        const std::vector<float> pitches
            = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_ANGLE_SEQUENCE_PITCHES");
        const std::vector<float> yaws
            = readWorldViewerFloatList("OPENMW_WORLD_VIEWER_CAMERA_ANGLE_SEQUENCE_YAWS");
        const std::size_t count = std::min({ frames.size(), pitches.size(), yaws.size() });
        if (count == 0)
            return {};

        std::vector<WorldViewerCameraAngleKeyframe> sequence;
        sequence.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (frames[i] >= 0 && std::isfinite(pitches[i]) && std::isfinite(yaws[i]))
                sequence.push_back({ frames[i], pitches[i], yaws[i] });
        }
        std::stable_sort(sequence.begin(), sequence.end(), [](const auto& left, const auto& right) {
            return left.mFrame < right.mFrame;
        });
        return sequence;
    }

    bool proofEnvEnabled(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0' && std::string(value) != "0";
    }

    bool worldViewerStaticCameraRequested()
    {
        const char* value = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_MODE");
        const char* sequenceFrames = std::getenv("OPENMW_WORLD_VIEWER_CAMERA_SEQUENCE_FRAMES");
        const bool sequenceRequested = sequenceFrames != nullptr && *sequenceFrames != '\0';
        return sequenceRequested
            || (value != nullptr && (std::string(value) == "static" || std::string(value) == "orbit-raycast"));
    }

    bool worldViewerNonStaticStartCameraRequested()
    {
        const char* value = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_MODE");
        if (value == nullptr || *value == '\0')
            return false;

        const std::string mode(value);
        return mode != "static" && mode != "orbit-raycast";
    }

    bool worldViewerOrbitRaycastRequested()
    {
        const char* value = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_MODE");
        return proofEnvEnabled("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_RAYCAST")
            || (value != nullptr && std::string(value) == "orbit-raycast");
    }

    std::string safeWorldViewerPtrText(const MWWorld::Ptr& ptr);
    std::string safeWorldViewerPtrBase(const MWWorld::Ptr& ptr);
    std::string safeWorldViewerPtrType(const MWWorld::Ptr& ptr);
    std::string safeWorldViewerPtrName(const MWWorld::Ptr& ptr);

    bool snapProofActorToRenderGround(
        MWWorld::World& world, MWWorld::Ptr& actor, const char* target, bool& freshBoundsLatched)
    {
        if (actor.isEmpty())
            return false;

        const ESM::Position& current = actor.getRefData().getPosition();
        osg::BoundingBox bounds;
        if (actor.getRefData().getBaseNode() != nullptr)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            boundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
            actor.getRefData().getBaseNode()->accept(boundsVisitor);
            bounds = boundsVisitor.getBoundingBox();
        }

        if (!bounds.valid())
        {
            freshBoundsLatched = false;
            Log(Debug::Info) << "FNV/ESM4 proof: render-ground snap waiting for valid actor bounds target=\""
                             << (target != nullptr ? target : "") << "\" actor=" << actor.toString();
            return false;
        }
        const osg::Vec3f boundsSize(
            bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(), bounds.zMax() - bounds.zMin());
        const osg::Vec3f actorPosition(current.pos[0], current.pos[1], current.pos[2]);
        const float boundsCenterDistance = (bounds.center() - actorPosition).length();
        const float boundsDiagonal = boundsSize.length();
        const float maxBoundsCenterDistance
            = readProofFloat("OPENMW_PROOF_RENDER_GROUND_MAX_BOUNDS_CENTER_DISTANCE", 1000.f);
        const float maxBoundsCenterDiagonals
            = readProofFloat("OPENMW_PROOF_RENDER_GROUND_MAX_BOUNDS_CENTER_DIAGONALS", 2.f);
        const float minRelativeAllowance
            = readProofFloat("OPENMW_PROOF_RENDER_GROUND_MIN_RELATIVE_BOUNDS_ALLOWANCE", 64.f);
        const float relativeBoundsAllowance
            = std::max(minRelativeAllowance, boundsDiagonal * maxBoundsCenterDiagonals);
        const float allowedBoundsCenterDistance
            = std::min(maxBoundsCenterDistance, relativeBoundsAllowance);
        const float maxBoundsExtent = readProofFloat("OPENMW_PROOF_RENDER_GROUND_MAX_BOUNDS_EXTENT", 1000.f);
        if (boundsCenterDistance > allowedBoundsCenterDistance || boundsSize.x() > maxBoundsExtent
            || boundsSize.y() > maxBoundsExtent || boundsSize.z() > maxBoundsExtent)
        {
            freshBoundsLatched = false;
            Log(Debug::Info) << "FNV/ESM4 proof: render-ground snap waiting for settled actor bounds target=\""
                             << (target != nullptr ? target : "") << "\" actor=" << actor.toString()
                             << " centerDistance=" << boundsCenterDistance << " size=(" << boundsSize.x() << ","
                             << boundsSize.y() << "," << boundsSize.z() << ") maxCenterDistance="
                             << maxBoundsCenterDistance << " boundsDiagonal=" << boundsDiagonal
                             << " maxCenterDiagonals=" << maxBoundsCenterDiagonals
                             << " allowedCenterDistance=" << allowedBoundsCenterDistance
                             << " maxExtent=" << maxBoundsExtent;
            return false;
        }

        // A staged reference and its render node do not become coherent in the same update.  Seeing one
        // plausible box is not enough: it can be the previous actor's final cull result translated through a
        // just-moved root.  Latch one fresh observation and require the following proof update to agree before
        // using the visual bottom to move the reference.  This is completion driven (not a capture-window
        // delay), and the relative-diagonal test above resets the latch whenever the node falls out of sync.
        if (!freshBoundsLatched)
        {
            freshBoundsLatched = true;
            Log(Debug::Info) << "FNV/ESM4 proof: render-ground snap latched fresh actor bounds; waiting for next update target=\""
                             << (target != nullptr ? target : "") << "\" actor=" << actor.toString()
                             << " centerDistance=" << boundsCenterDistance << " boundsDiagonal="
                             << boundsDiagonal << " allowedCenterDistance=" << allowedBoundsCenterDistance;
            return false;
        }

        const float visualBottom = bounds.zMin();
        const float rayUp = readProofFloat("OPENMW_PROOF_RENDER_GROUND_RAY_UP", 512.f);
        const float rayDown = readProofFloat("OPENMW_PROOF_RENDER_GROUND_RAY_DOWN", 4096.f);
        const float offset = readProofFloat("OPENMW_PROOF_RENDER_GROUND_OFFSET_Z", 0.f);
        MWPhysics::RayCastingResult renderGround {};
        osg::Vec3f bestSample(current.pos[0], current.pos[1], 0.f);
        float bestScore = std::numeric_limits<float>::infinity();
        int bestSampleIndex = -1;
        int sampleIndex = 0;
        const float searchRadius = std::max(0.f, readProofFloat("OPENMW_PROOF_RENDER_GROUND_SEARCH_RADIUS", 0.f));
        const float searchStep = std::max(1.f, readProofFloat("OPENMW_PROOF_RENDER_GROUND_SEARCH_STEP", 32.f));
        const float minNormalZ = readProofFloat("OPENMW_PROOF_RENDER_GROUND_MIN_NORMAL_Z", 0.15f);
        bool sawRejectedNormal = false;
        auto tryGroundSample = [&](float x, float y, bool acceptAnyNormal) {
            const osg::Vec3f from(x, y, current.pos[2] + rayUp);
            const osg::Vec3f to(x, y, current.pos[2] - rayDown);
            MWPhysics::RayCastingResult candidate {};
            world.castRenderingRay(candidate, from, to, true, true, std::span<const MWWorld::Ptr> { &actor, 1 });
            if (!candidate.mHit)
            {
                ++sampleIndex;
                return;
            }

            const bool normalAccepted = acceptAnyNormal || candidate.mHitNormal.z() >= minNormalZ;
            if (!normalAccepted)
                sawRejectedNormal = true;
            if (!normalAccepted)
            {
                ++sampleIndex;
                return;
            }

            const float dx = x - current.pos[0];
            const float dy = y - current.pos[1];
            const float score = std::sqrt(dx * dx + dy * dy) + std::max(0.f, current.pos[2] - candidate.mHitPos.z()) * 0.05f;
            if (score < bestScore)
            {
                bestScore = score;
                renderGround = candidate;
                bestSample.set(x, y, 0.f);
                bestSampleIndex = sampleIndex;
            }
            ++sampleIndex;
        };

        tryGroundSample(current.pos[0], current.pos[1], false);
        if (searchRadius > 0.f)
        {
            const int sampleSteps = static_cast<int>(std::ceil(searchRadius / searchStep));
            for (int ix = -sampleSteps; ix <= sampleSteps; ++ix)
            {
                for (int iy = -sampleSteps; iy <= sampleSteps; ++iy)
                {
                    if (ix == 0 && iy == 0)
                        continue;
                    const float dx = static_cast<float>(ix) * searchStep;
                    const float dy = static_cast<float>(iy) * searchStep;
                    if (std::sqrt(dx * dx + dy * dy) > searchRadius)
                        continue;
                    tryGroundSample(current.pos[0] + dx, current.pos[1] + dy, false);
                }
            }
            if (!renderGround.mHit && sawRejectedNormal)
            {
                sampleIndex = 0;
                tryGroundSample(current.pos[0], current.pos[1], true);
                for (int ix = -sampleSteps; ix <= sampleSteps; ++ix)
                {
                    for (int iy = -sampleSteps; iy <= sampleSteps; ++iy)
                    {
                        if (ix == 0 && iy == 0)
                            continue;
                        const float dx = static_cast<float>(ix) * searchStep;
                        const float dy = static_cast<float>(iy) * searchStep;
                        if (std::sqrt(dx * dx + dy * dy) > searchRadius)
                            continue;
                        tryGroundSample(current.pos[0] + dx, current.pos[1] + dy, true);
                    }
                }
            }
        }

        if (!renderGround.mHit)
        {
            const osg::Vec3f from(current.pos[0], current.pos[1], current.pos[2] + rayUp);
            const osg::Vec3f to(current.pos[0], current.pos[1], current.pos[2] - rayDown);
            Log(Debug::Warning) << "FNV/ESM4 proof: render-ground snap missed target=\""
                                << (target != nullptr ? target : "") << "\" actor=" << actor.toString()
                                << " from=(" << from.x() << "," << from.y() << "," << from.z()
                                << ") to=(" << to.x() << "," << to.y() << "," << to.z()
                                << ") visualBottom=" << visualBottom
                                << " searchRadius=" << searchRadius << " searchStep=" << searchStep
                                << " minNormalZ=" << minNormalZ << " sawRejectedNormal=" << sawRejectedNormal;
            return false;
        }

        const float delta = (renderGround.mHitPos.z() + offset) - visualBottom;
        const bool moveXY = proofEnvEnabled("OPENMW_PROOF_RENDER_GROUND_MOVE_XY");
        const float snappedX = moveXY ? bestSample.x() : current.pos[0];
        const float snappedY = moveXY ? bestSample.y() : current.pos[1];
        const bool xyAlreadyGrounded = std::abs(snappedX - current.pos[0]) < 0.001f
            && std::abs(snappedY - current.pos[1]) < 0.001f;
        if (std::abs(delta) < 0.001f && xyAlreadyGrounded)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: render-ground snap already grounded target=\""
                             << (target != nullptr ? target : "") << "\" actor=" << actor.toString()
                             << " ground=(" << renderGround.mHitPos.x() << "," << renderGround.mHitPos.y()
                             << "," << renderGround.mHitPos.z() << ") visualBottom=" << visualBottom
                             << " sample=(" << bestSample.x() << "," << bestSample.y() << ")"
                             << " sampleIndex=" << bestSampleIndex << " moveXY=" << moveXY
                             << " hitBase=" << safeWorldViewerPtrBase(renderGround.mHitObject)
                             << " hitType=\"" << safeWorldViewerPtrType(renderGround.mHitObject) << "\"";
            return true;
        }

        const osg::Vec3f snapped(snappedX, snappedY, current.pos[2] + delta);
        actor = world.moveObject(actor, snapped, true, true);
        Log(Debug::Info) << "FNV/ESM4 proof: render-ground snapped actor target=\""
                         << (target != nullptr ? target : "") << "\" oldPos=(" << current.pos[0] << ","
                         << current.pos[1] << "," << current.pos[2] << ") newPos=(" << snapped.x() << ","
                         << snapped.y() << "," << snapped.z() << ") ground=(" << renderGround.mHitPos.x()
                         << "," << renderGround.mHitPos.y() << "," << renderGround.mHitPos.z()
                         << ") visualBottom=" << visualBottom << " delta=" << delta << " offset=" << offset
                         << " sample=(" << bestSample.x() << "," << bestSample.y() << ")"
                         << " sampleIndex=" << bestSampleIndex << " moveXY=" << moveXY
                         << " searchRadius=" << searchRadius << " searchStep=" << searchStep
                         << " minNormalZ=" << minNormalZ
                         << " hitBase=" << safeWorldViewerPtrBase(renderGround.mHitObject)
                         << " hitType=\"" << safeWorldViewerPtrType(renderGround.mHitObject) << "\""
                         << " hitName=\"" << safeWorldViewerPtrName(renderGround.mHitObject) << "\""
                         << " hitPtr=" << safeWorldViewerPtrText(renderGround.mHitObject);
        return true;
    }

    bool stageProofActorForCamera(MWWorld::World& world, MWWorld::Ptr& actor, const char* target)
    {
        if (actor.isEmpty())
            return false;

        const ESM::Position& current = actor.getRefData().getPosition();
        const osg::Vec3f stagedPos(
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", current.pos[0]),
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", current.pos[1]),
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", current.pos[2]));
        const osg::Vec3f stagedRot(
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", current.rot[0]),
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", current.rot[1]),
            readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", current.rot[2]));
        // The position-only overload derives a destination worldspace from
        // the actor's source cell.  That leaves authored interior references
        // (and actors from another worldspace) in an inactive cell even when
        // moveToActive is requested.  A one-session proof sweep must stage
        // every exact authored reference in the same active render cell.
        MWWorld::Ptr player = MWMechanics::getPlayer();
        if (!player.isEmpty() && player.isInCell())
            actor = world.moveObject(actor, player.getCell(), stagedPos, true, true);
        else
            actor = world.moveObject(actor, stagedPos, true, true);
        world.rotateObject(actor, stagedRot);
        Log(Debug::Info) << "FNV/ESM4 proof: staged actor target=\""
                         << (target != nullptr ? target : "") << "\" oldPos=(" << current.pos[0] << ","
                         << current.pos[1] << "," << current.pos[2] << ") pos=(" << stagedPos.x() << ","
                         << stagedPos.y() << "," << stagedPos.z() << ") rot=(" << stagedRot.x() << ","
                         << stagedRot.y() << "," << stagedRot.z() << ") ptr=" << actor.toString();
        return true;
    }

    void logProofActorRenderBounds(const MWWorld::Ptr& actor, const char* target, const char* phase)
    {
        if (actor.isEmpty())
            return;

        const ESM::Position& pos = actor.getRefData().getPosition();
        osg::BoundingBox bounds;
        if (actor.getRefData().getBaseNode() != nullptr)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            boundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
            actor.getRefData().getBaseNode()->accept(boundsVisitor);
            bounds = boundsVisitor.getBoundingBox();
        }

        if (!bounds.valid())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: actor render bounds invalid phase=\""
                                << (phase != nullptr ? phase : "") << "\" target=\""
                                << (target != nullptr ? target : "") << "\" pos=(" << pos.pos[0] << ","
                                << pos.pos[1] << "," << pos.pos[2] << ") ptr=" << actor.toString();
            return;
        }

        const double height = bounds.zMax() - bounds.zMin();
        const double width = bounds.xMax() - bounds.xMin();
        const double depth = bounds.yMax() - bounds.yMin();
        Log(Debug::Info) << "FNV/ESM4 proof: actor render bounds phase=\""
                         << (phase != nullptr ? phase : "") << "\" target=\""
                         << (target != nullptr ? target : "") << "\" pos=(" << pos.pos[0] << ","
                         << pos.pos[1] << "," << pos.pos[2] << ") min=(" << bounds.xMin() << ","
                         << bounds.yMin() << "," << bounds.zMin() << ") max=(" << bounds.xMax() << ","
                         << bounds.yMax() << "," << bounds.zMax() << ") center=(" << bounds.center().x()
                         << "," << bounds.center().y() << "," << bounds.center().z() << ") size=("
                         << width << "," << depth << "," << height << ") bottomDelta="
                         << (pos.pos[2] - bounds.zMin()) << " ptr=" << actor.toString();
    }

    bool adjustProofActorCameraByRenderRay(MWWorld::World& world, const MWWorld::Ptr& actor, const char* target,
        const osg::Vec3f& focus, osg::Vec3f& targetPos)
    {
        if (!proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_RAYCAST_BACKOFF")
            && !proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_RENDER_RAYCAST_BACKOFF"))
            return false;

        const osg::Vec3f ray = targetPos - focus;
        const float rayLength = ray.length();
        if (rayLength <= 1e-3f)
            return false;

        const osg::Vec3f rayDirection = ray / rayLength;
        MWPhysics::RayCastingResult renderRay {};
        world.castRenderingRay(renderRay, focus, targetPos, true, true, std::span<const MWWorld::Ptr> { &actor, 1 });
        if (!renderRay.mHit)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera raycast clear target=\""
                             << (target != nullptr ? target : "") << "\" mode=\"render\"";
            return false;
        }

        const float clearance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RENDER_RAYCAST_CLEARANCE",
            readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RAYCAST_CLEARANCE", 24.f));
        const float minDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RENDER_RAYCAST_MIN_DISTANCE", 0.75f);
        const osg::Vec3f adjusted = renderRay.mHitPos - rayDirection * clearance;
        const float adjustedDistance = (adjusted - focus).length();
        if (adjustedDistance <= minDistance)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: actor orbit camera raycast hit too close target=\""
                                << (target != nullptr ? target : "") << "\" mode=\"render\" hit=("
                                << renderRay.mHitPos.x() << "," << renderRay.mHitPos.y() << ","
                                << renderRay.mHitPos.z() << ") hitBase="
                                << safeWorldViewerPtrBase(renderRay.mHitObject) << " hitType=\""
                                << safeWorldViewerPtrType(renderRay.mHitObject) << "\"";
            return false;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera raycast adjusted target=\""
                         << (target != nullptr ? target : "") << "\" mode=\"render\" hit=("
                         << renderRay.mHitPos.x() << "," << renderRay.mHitPos.y() << ","
                         << renderRay.mHitPos.z() << ") from=(" << targetPos.x() << "," << targetPos.y()
                         << "," << targetPos.z() << ") to=(" << adjusted.x() << "," << adjusted.y()
                         << "," << adjusted.z() << ") hitBase=" << safeWorldViewerPtrBase(renderRay.mHitObject)
                         << " hitType=\"" << safeWorldViewerPtrType(renderRay.mHitObject) << "\" hitName=\""
                         << safeWorldViewerPtrName(renderRay.mHitObject) << "\" hitPtr="
                         << safeWorldViewerPtrText(renderRay.mHitObject);
        targetPos = adjusted;
        return true;
    }

    int raiseProofActorCameraForClearVisibility(MWWorld::World& world, const MWWorld::Ptr& actor,
        const char* target, const osg::BoundingBox& bounds, osg::Vec3f& targetPos)
    {
        if (!proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST") || !bounds.valid())
            return -1;

        const float boundsHeight = std::max(bounds.zMax() - bounds.zMin(), 1.f);
        const osg::Vec3f center = bounds.center();
        const float stagedGroundZ = actor.isEmpty()
            ? bounds.zMin()
            : actor.getRefData().getPosition().pos[2];
        std::vector<osg::Vec3f> samples;
        samples.reserve(7);
        // Animated/collision helper bounds may extend below the staged ground plane.  A ray to that hidden
        // mathematical corner necessarily intersects terrain and would reject a retail camera even though all
        // renderable actor pixels are visible.  Clamp the low visibility sample to just above the actor's exact
        // staged ground while retaining the full AABB for the projection/containment gate.
        samples.emplace_back(center.x(), center.y(),
            std::max(bounds.zMin() + boundsHeight * 0.08f, stagedGroundZ + 1.f));
        samples.emplace_back(center.x(), center.y(), bounds.zMin() + boundsHeight * 0.45f);
        samples.emplace_back(center.x(), center.y(), bounds.zMin() + boundsHeight * 0.82f);
        samples.emplace_back(bounds.xMin(), center.y(), center.z());
        samples.emplace_back(bounds.xMax(), center.y(), center.z());
        samples.emplace_back(center.x(), bounds.yMin(), center.z());
        samples.emplace_back(center.x(), bounds.yMax(), center.z());

        const float hitTolerance
            = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST_HIT_TOLERANCE", 0.75f);
        const float heightStep = std::max(
            readProofFloat("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST_MIN_HEIGHT_STEP", 16.f),
            boundsHeight
                * readProofFloat("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST_HEIGHT_STEP_FACTOR", 0.25f));
        const int steps = proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST_GATE_ONLY")
            ? 0
            : std::max(0, readProofInt("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST_STEPS", 6));
        const auto countBlockers = [&](const osg::Vec3f& candidate) {
            int blockers = 0;
            for (const osg::Vec3f& sample : samples)
            {
                const float sampleDistance = (sample - candidate).length();
                if (sampleDistance <= 1e-3f)
                    continue;
                MWPhysics::RayCastingResult renderRay {};
                world.castRenderingRay(
                    renderRay, candidate, sample, true, true, std::span<const MWWorld::Ptr> { &actor, 1 });
                if (!renderRay.mHit)
                    continue;
                const float hitDistance = (renderRay.mHitPos - candidate).length();
                if (hitDistance + hitTolerance < sampleDistance)
                    ++blockers;
            }
            return blockers;
        };

        osg::Vec3f best = targetPos;
        int bestBlockers = std::numeric_limits<int>::max();
        int bestStep = 0;
        for (int step = 0; step <= steps; ++step)
        {
            osg::Vec3f candidate = targetPos;
            candidate.z() += heightStep * static_cast<float>(step);
            const int blockers = countBlockers(candidate);
            Log(Debug::Info) << "FNV/ESM4 proof: actor visibility camera candidate target=\""
                             << (target != nullptr ? target : "") << "\" step=" << step << " pos=("
                             << candidate.x() << "," << candidate.y() << "," << candidate.z()
                             << ") blockers=" << blockers << " samples=" << samples.size();
            if (blockers < bestBlockers)
            {
                best = candidate;
                bestBlockers = blockers;
                bestStep = step;
            }
            if (blockers == 0)
                break;
        }

        if (bestStep != 0)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: actor visibility camera raised target=\""
                             << (target != nullptr ? target : "") << "\" fromZ=" << targetPos.z()
                             << " toZ=" << best.z() << " step=" << bestStep
                             << " blockers=" << bestBlockers;
            targetPos = best;
        }
        return bestBlockers;
    }

    bool selectProofActorCameraByOrbitRays(MWWorld::World& world, const MWWorld::Ptr& actor, const char* target,
        const osg::Vec3f& focus, osg::Vec3f& targetPos)
    {
        if (!proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST")
            && !proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_RENDER_RAYCAST"))
            return false;

        const osg::Vec3f seedOffset = targetPos - focus;
        const float seedDistance = seedOffset.length();
        if (seedDistance <= 1e-3f)
            return false;

        osg::BoundingBox bounds;
        if (!actor.isEmpty() && actor.getRefData().getBaseNode() != nullptr)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            boundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
            actor.getRefData().getBaseNode()->accept(boundsVisitor);
            bounds = boundsVisitor.getBoundingBox();
        }

        std::vector<osg::Vec3f> actorSamples;
        actorSamples.push_back(focus);
        if (bounds.valid())
        {
            const float height = bounds.zMax() - bounds.zMin();
            const float focusZ = focus.z();
            actorSamples.emplace_back(bounds.center().x(), bounds.center().y(), bounds.zMin() + height * 0.08f);
            actorSamples.emplace_back(bounds.center().x(), bounds.center().y(), bounds.zMin() + height * 0.45f);
            actorSamples.emplace_back(bounds.center().x(), bounds.center().y(), bounds.zMin() + height * 0.82f);
            actorSamples.emplace_back(bounds.xMin(), bounds.center().y(), focusZ);
            actorSamples.emplace_back(bounds.xMax(), bounds.center().y(), focusZ);
            actorSamples.emplace_back(bounds.center().x(), bounds.yMin(), focusZ);
            actorSamples.emplace_back(bounds.center().x(), bounds.yMax(), focusZ);
        }

        struct CandidateScore
        {
            osg::Vec3f mPos;
            float mAngle = 0.f;
            int mBlockers = 0;
            int mFrameBlockers = 0;
            float mNearestBlocker = std::numeric_limits<float>::max();
            float mNearestFrameBlocker = std::numeric_limits<float>::max();
            float mClosestSampleDistance = std::numeric_limits<float>::max();
            std::string mFirstBlockerBase;
            std::string mFirstBlockerType;
            std::string mFirstBlockerName;
        };

        const float stepDegrees = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_STEP_DEGREES", 35.f);
        const int rings = std::max(0, readProofInt("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_RINGS", 4));
        const float hitTolerance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_HIT_TOLERANCE", 0.75f);
        std::vector<float> angleOffsets;
        angleOffsets.push_back(0.f);
        for (int ring = 1; ring <= rings; ++ring)
        {
            angleOffsets.push_back(stepDegrees * static_cast<float>(ring));
            angleOffsets.push_back(-stepDegrees * static_cast<float>(ring));
        }
        if (proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_INCLUDE_REVERSE"))
            angleOffsets.push_back(180.f);

        const auto rotateOffset = [](const osg::Vec3f& offset, float degrees) {
            const float radians = degrees * static_cast<float>(osg::PI) / 180.f;
            const float cosAngle = std::cos(radians);
            const float sinAngle = std::sin(radians);
            return osg::Vec3f(offset.x() * cosAngle - offset.y() * sinAngle,
                offset.x() * sinAngle + offset.y() * cosAngle, offset.z());
        };

        const auto scoreCandidate = [&](const osg::Vec3f& candidatePos, float angle) {
            CandidateScore score;
            score.mPos = candidatePos;
            score.mAngle = angle;
            const auto scoreSample = [&](const osg::Vec3f& sample, bool frameSample) {
                {
                    const float sampleDistance = (sample - candidatePos).length();
                    if (sampleDistance <= 1e-3f)
                        return;
                    MWPhysics::RayCastingResult renderRay {};
                    world.castRenderingRay(
                        renderRay, candidatePos, sample, true, true, std::span<const MWWorld::Ptr> { &actor, 1 });
                    if (!renderRay.mHit)
                        return;

                    const float hitDistance = (renderRay.mHitPos - candidatePos).length();
                    if (hitDistance + hitTolerance >= sampleDistance)
                        return;

                    if (frameSample)
                    {
                        ++score.mFrameBlockers;
                        score.mNearestFrameBlocker = std::min(score.mNearestFrameBlocker, hitDistance);
                        return;
                    }

                    ++score.mBlockers;
                    if (hitDistance < score.mNearestBlocker)
                    {
                        score.mNearestBlocker = hitDistance;
                        score.mClosestSampleDistance = sampleDistance;
                        score.mFirstBlockerBase = safeWorldViewerPtrBase(renderRay.mHitObject);
                        score.mFirstBlockerType = safeWorldViewerPtrType(renderRay.mHitObject);
                        score.mFirstBlockerName = safeWorldViewerPtrName(renderRay.mHitObject);
                    }
                }
            };

            for (const osg::Vec3f& sample : actorSamples)
                scoreSample(sample, false);

            osg::Vec3f view = focus - candidatePos;
            view.z() = 0.f;
            if (view.length2() <= 1e-4f)
                view = osg::Vec3f(0.f, 1.f, 0.f);
            else
                view.normalize();
            osg::Vec3f right(view.y(), -view.x(), 0.f);
            if (right.length2() <= 1e-4f)
                right = osg::Vec3f(1.f, 0.f, 0.f);
            else
                right.normalize();

            float frameWidth = 1.5f;
            float frameHeight = 2.2f;
            if (bounds.valid())
            {
                frameWidth = std::max(std::max(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin()), 1.f);
                frameHeight = std::max(bounds.zMax() - bounds.zMin(), 2.f);
            }
            const float frameSide = frameWidth
                * readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_FRAME_WIDTH_MULT", 2.35f);
            const float frameUp = frameHeight
                * readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_FRAME_UP_MULT", 0.72f);
            const float frameDown = frameHeight
                * readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST_FRAME_DOWN_MULT", 0.38f);
            std::vector<osg::Vec3f> frameSamples;
            frameSamples.reserve(6);
            frameSamples.push_back(focus + right * frameSide);
            frameSamples.push_back(focus - right * frameSide);
            frameSamples.push_back(focus + osg::Vec3f(0.f, 0.f, frameUp));
            frameSamples.push_back(focus - osg::Vec3f(0.f, 0.f, frameDown));
            frameSamples.push_back(focus + right * frameSide + osg::Vec3f(0.f, 0.f, frameUp));
            frameSamples.push_back(focus - right * frameSide + osg::Vec3f(0.f, 0.f, frameUp));
            for (const osg::Vec3f& sample : frameSamples)
      …187168 tokens truncated…        }
        if (proofRequiresActorForScreenshot && !proofActorCameraAligned)
        {
            if (!proofActorScreenshotWaitLogged)
            {
                const char* proofSayActor = std::getenv("OPENMW_PROOF_SAY_ACTOR");
                Log(Debug::Info) << "FNV/ESM4 proof: waiting to capture screenshot until actor is resolved target=\""
                                 << (proofSayActor != nullptr ? proofSayActor : "") << "\" frame=" << frameNumber
                                 << " screenshotIndex=" << proofScreenshotFrameIndex;
                proofActorScreenshotWaitLogged = true;
            }
            worldViewerTrace(frameNumber, "actor-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "actor-wait-render.end");
            worldViewerTrace(frameNumber, "actor-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "actor-wait-lua-finish.end");
            return true;
        }
        if (!worldViewerCameraReadyForScreenshot)
        {
            worldViewerTrace(frameNumber, "camera-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "camera-wait-render.end");
            worldViewerTrace(frameNumber, "camera-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "camera-wait-lua-finish.end");
            return true;
        }
        if (!proofActorVisualReady)
        {
            static int proofActorVisualLastWaitLogFrame = -1000000;
            if (static_cast<int>(frameNumber) - proofActorVisualLastWaitLogFrame >= 30
                || frameNumber == static_cast<unsigned int>(std::max(0, proofActorCameraAlignedFrame)))
            {
                proofActorVisualLastWaitLogFrame = static_cast<int>(frameNumber);
                Log(Debug::Info) << "FNV/ESM4 proof: waiting for target draw traversal target=\""
                                 << (proofSayActor != nullptr ? proofSayActor : "") << "\" frame=" << frameNumber
                                 << " alignedFrame=" << proofActorCameraAlignedFrame
                                 << " rootParents=" << proofActorRootParents << " rootMask=0x" << std::hex
                                 << proofActorRootMask << std::dec << " visibleDrawables="
                                 << proofActorVisibleDrawables << " visibleRigs=" << proofActorVisibleRigs
                                 << " resolvedRigs=" << proofActorResolvedRigs << " cullReadyRigs="
                                 << proofActorCullReadyRigs << " maximumCullTraversal="
                                 << proofActorMaximumCullTraversal;
            }
            worldViewerTrace(frameNumber, "actor-draw-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "actor-draw-wait-render.end");
            worldViewerTrace(frameNumber, "actor-draw-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "actor-draw-wait-lua-finish.end");
            return true;
        }
        const bool skeletalExportBypassesPortraitGates
            = std::getenv("OPENMW_OPENNV_ACTOR_EXPORT_NO_SCREENSHOT") != nullptr;
        if (!proofActorPoseReadyForScreenshot && !skeletalExportBypassesPortraitGates)
        {
            static int proofActorPoseLastWaitLogFrame = -1000000;
            if (static_cast<int>(frameNumber) - proofActorPoseLastWaitLogFrame >= 30)
            {
                proofActorPoseLastWaitLogFrame = static_cast<int>(frameNumber);
                Log(Debug::Info) << "FNV/ESM4 actor pose gate: waiting before baseline screenshot actorIndex="
                                 << proofActorBatchIndex << " poseIndex=" << proofActorPoseIndex << "/"
                                 << proofActorActivePoseGroups.size() << " frame=" << frameNumber;
            }
            worldViewerTrace(frameNumber, "actor-pose-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "actor-pose-wait-render.end");
            worldViewerTrace(frameNumber, "actor-pose-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "actor-pose-wait-lua-finish.end");
            return true;
        }
        if (!proofPortraitClear && !skeletalExportBypassesPortraitGates)
        {
            worldViewerTrace(frameNumber, "portrait-clear-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "portrait-clear-wait-render.end");
            worldViewerTrace(frameNumber, "portrait-clear-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "portrait-clear-wait-lua-finish.end");
            return true;
        }

        if (viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_TELEMETRY") && mWorld != nullptr)
        {
            worldViewerTrace(frameNumber, "screenshot-telemetry.begin");
            logWorldViewerTelemetry(*mWorld, *mViewer, frameNumber, static_cast<int>(mStateManager->getState()),
                proofLoadingGui, proofWorldReady, proofWorldReadyFrames);
            worldViewerTelemetryLastFrame = frameNumber;
            worldViewerTelemetryLogged = true;
            worldViewerTrace(frameNumber, "screenshot-telemetry.end");
        }

        worldViewerTrace(frameNumber, "screenshot-queue.begin");
        if (!proofActorBatchPrevious.isEmpty())
        {
            const bool exported = exportOpenNvActorMesh(proofActorBatchPrevious, proofActorBatchIndex);
            if (exported && std::getenv("OPENMW_OPENNV_ACTOR_EXPORT_EXIT_AFTER_BATCH") != nullptr
                && proofActorBatchIndex + 1 >= proofActorBatchTargets.size())
            {
                Log(Debug::Info) << "OpenNV skeletal actor export batch complete; exiting cleanly actors="
                                 << proofActorBatchTargets.size();
                mStateManager->requestQuit();
            }
        }
        const bool suppressActorExportScreenshot
            = std::getenv("OPENMW_OPENNV_ACTOR_EXPORT_NO_SCREENSHOT") != nullptr;
        if (suppressActorExportScreenshot)
        {
            if (proofScreenshotFrameReached)
                ++proofScreenshotFrameIndex;
            Log(Debug::Info) << "OpenNV skeletal actor export suppressed native screenshot frame="
                             << frameNumber << " nextActor=" << proofScreenshotFrameIndex;
        }
        else
        {
        if (proofPortraitClearRequired)
        {
            Log(Debug::Info) << "World viewer portrait capture accepted: frame=" << frameNumber
                             << " screenshotIndex=" << proofScreenshotFrameIndex << " headNormalized=("
                             << proofPortraitHeadX << "," << proofPortraitHeadY << ") handOffsetZ=("
                             << proofPortraitLeftHandOffsetZ << "," << proofPortraitRightHandOffsetZ
                             << ") headMotion=" << proofPortraitHeadMotion << " forwardDot="
                             << proofPortraitForwardDot << " clearFrames=" << proofPortraitClearFrames
                             << " status=pass";
        }
        Log(Debug::Info) << "FNV/ESM4 proof: queuing GUI-inclusive native screenshot at frame " << frameNumber
                         << " hour=" << mWorld->getTimeStamp().getHour()
                         << " weatherId=" << mWorld->getCurrentWeatherScriptId()
                         << " weatherTransition=" << mWorld->getWeatherTransition();
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
        if (proofScreenshotFrameReached)
            ++proofScreenshotFrameIndex;
        if (proofScreenshotReadyFramesReached)
            proofScreenshotReadyQueued = true;
        if (proofActorAlignedScreenshotReached)
            proofActorAlignedScreenshotQueued = true;
        if (proofPortraitClearRequired)
            proofPortraitClearFrames = 0;
        }
        worldViewerTrace(frameNumber, "screenshot-queue.end");
    }

    if (playableSessionFinished && playableSessionOrbitNextFrame != 0
        && frameNumber >= playableSessionOrbitNextFrame && !playableSessionEndScreenshotPending)
    {
        if (setPlayableSessionFrontPortraitCamera)
            setPlayableSessionFrontPortraitCamera(playableSessionOrbitScreenshotIndex);
        playableSessionEndScreenshotPending = true;
        playableSessionOrbitNextFrame = 0;
    }

    const bool playableSessionScreenshotPending = playableSessionStartScreenshotPending
        || playableSessionMidpointScreenshotPending || playableSessionEndScreenshotPending;
    if (playableSessionScreenshotPending && proofWorldReady && mScreenCaptureHandler != nullptr)
    {
        const char* phase = playableSessionStartScreenshotPending
            ? "start-third-person"
            : (playableSessionMidpointScreenshotPending ? "midpoint-first-person" : "end-portrait");
        Log(Debug::Info) << "Playable session: queuing native screenshot phase=" << phase
                         << " frame=" << frameNumber;
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
        if (playableSessionStartScreenshotPending)
            playableSessionStartScreenshotPending = false;
        else if (playableSessionMidpointScreenshotPending)
            playableSessionMidpointScreenshotPending = false;
        else
        {
            playableSessionEndScreenshotPending = false;
            const int portraitCount
                = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_CLOSEUPS") ? 6 : 4;
            if (proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_ORBIT")
                && playableSessionOrbitScreenshotIndex + 1 < portraitCount)
            {
                ++playableSessionOrbitScreenshotIndex;
                const int portraitFrameGap
                    = std::max(3, readProofInt("OPENMW_PLAYABLE_SESSION_PORTRAIT_FRAME_GAP", 3));
                playableSessionOrbitNextFrame = frameNumber + portraitFrameGap;
                Log(Debug::Info) << "Playable session: scheduled native portrait orbit index="
                                 << playableSessionOrbitScreenshotIndex << " frame="
                                 << playableSessionOrbitNextFrame << " frameGap=" << portraitFrameGap;
            }
            else
                playableSessionExitFrame = frameNumber
                    + std::max(3, readProofInt("OPENMW_PLAYABLE_SESSION_PORTRAIT_FRAME_GAP", 3));
        }
    }

    if (deferProofLuaWorker)
    {
        worldViewerTrace(frameNumber, "lua-worker-deferred-allow.begin");
        mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);
        worldViewerTrace(frameNumber, "lua-worker-deferred-allow.end");
    }

    worldViewerTrace(frameNumber, "rendering-traversals.begin");
    mViewer->renderingTraversals();
    worldViewerTrace(frameNumber, "rendering-traversals.end");

    if (proofActorStagedForCamera && !proofActorBatchPrevious.isEmpty()
        && proofActorBatchPrevious.getType() == ESM4::Npc::sRecordId)
    {
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(proofActorBatchPrevious);
        if (traits != nullptr && traits->mIsFONV)
        {
            worldViewerTrace(frameNumber, "actor-skin-state.begin");
            FalloutProofSkinState sampledSkinState
                = inspectFalloutProofSkinState(proofActorBatchPrevious, proofActorBatchIndex, frameNumber);
            proofActorSkinState = sampledSkinState;

            FalloutProofSkinState stableSample = sampledSkinState;
            stableSample.mSampleFrame = 0;
            std::ostringstream stableKey;
            stableKey << (proofSidecarEnabled ? proofSidecarGeneration : 0) << '|';
            writeFalloutProofSkinStateJson(stableKey, stableSample);
            if (proofActorSkinStateLastLogKey != stableKey.str())
            {
                proofActorSkinStateLastLogKey = stableKey.str();
                std::ostringstream payload;
                writeFalloutProofSkinStateJson(payload, sampledSkinState);
                Log(sampledSkinState.mPass ? Debug::Info : Debug::Warning)
                    << "FNV/ESM4 actor skin state gate: " << payload.str();
            }
            worldViewerTrace(frameNumber, "actor-skin-state.end");
        }
    }

    worldViewerTrace(frameNumber, "lua-worker-finish.begin");
    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
    worldViewerTrace(frameNumber, "lua-worker-finish.end");

    if (proofActorBaseRosterExpanded && !proofActorBatchCompletionLogged
        && proofScreenshotFrameIndex >= proofActorBatchTargets.size())
    {
        proofActorBatchCompletionLogged = true;
        proofActorBatchCompleteFrame = static_cast<int>(frameNumber);
        Log(Debug::Info) << "FNV/ESM4 actor batch: complete actors=" << proofActorBatchTargets.size()
                         << " screenshots=" << proofScreenshotFrameIndex << " frame=" << frameNumber;
    }
    if (!proofActorBatchQuitRequested && proofActorBatchCompletionLogged
        && proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_EXIT_AFTER_COMPLETE") && proofActorBatchCompleteFrame >= 0)
    {
        const int exitDelayEnv = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_EXIT_DELAY_FRAMES");
        const int exitDelay = exitDelayEnv >= 1 ? exitDelayEnv : 30;
        if (static_cast<int>(frameNumber) >= proofActorBatchCompleteFrame + exitDelay)
        {
            proofActorBatchQuitRequested = true;
            Log(Debug::Info) << "FNV/ESM4 actor batch: native captures flushed; exiting cleanly frame="
                             << frameNumber;
            mStateManager->requestQuit();
        }
    }

    if (!playableSessionQuitRequested && playableSessionFinished
        && proofEnvEnabled("OPENMW_PLAYABLE_SESSION_EXIT_AFTER_COMPLETE")
        && !playableSessionStartScreenshotPending && !playableSessionMidpointScreenshotPending
        && !playableSessionEndScreenshotPending && playableSessionOrbitNextFrame == 0
        && playableSessionExitFrame != 0
        && frameNumber >= playableSessionExitFrame)
    {
        playableSessionQuitRequested = true;
        Log(Debug::Info) << "Playable session: background validation complete; exiting cleanly at frame "
                         << frameNumber;
        mStateManager->requestQuit();
    }

    worldViewerTrace(frameNumber, "frame.end");
    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(false)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
    // ## VR_PATCH BEGIN
    , mVrGUIManager(nullptr)
    , mXrInstance(nullptr)
// ## VR_PATCH END
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
        mScreenCaptureOperation->stop();

    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown begin";
    mMechanicsManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown mechanics";
    mDialogueManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown dialogue";
    mJournal = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown journal";
    mWindowManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown window";
    mScriptManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown script";
    mWorld = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown world";
    mStereoManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown stereo";
    mSoundManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown sound";
    mInputManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown input";
    mStateManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown state";
    mLuaWorker = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown lua-worker";
    mLuaManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown lua";
    mL10nManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown l10n";

    mScriptContext = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown script-context";

    mUnrefQueue = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown unref-queue";
    mWorkQueue = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown work-queue";

    // Drop resource caches while the viewer/window context is still alive. ESM4 proof runs
    // load a lot of actor NIFs quickly, and tearing those caches down after viewer shutdown
    // can trip post-capture CRT fail-fast paths in OSG/GL resource cleanup.
    mResourceSystem.reset();
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown resource-system";

    mViewer = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown viewer";
    // ## VR_PATCH BEGIN
    mVrViewer = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown vr-viewer";
    mCallbackManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown callback-manager";
    mVrGUIManager = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown vr-gui";
    mXrSession = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown xr-session";
    mXrInstance = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown xr-instance";
    // ## VR_PATCH END

    mEncoder = nullptr;
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown encoder";

    if (mWindow)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: engine teardown destroy-window begin";
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
        Log(Debug::Info) << "FNV/ESM4 proof: engine teardown destroy-window end";
    }

    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown sdl-quit begin";
    SDL_Quit();
    Log(Debug::Info) << "FNV/ESM4 proof: engine teardown sdl-quit end";

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool backgroundPlayableSession = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_BACKGROUND");
    // The unattended Save330 recorder needs a real, titled SDL surface while
    // retaining the background-session simulation behavior.  This only
    // changes window visibility; the production world/input path is unchanged.
    const bool captureKeepWindowVisible = proofEnvEnabled("OPENMW_PROOF_CAPTURE_KEEP_WINDOW_VISIBLE");
    const bool hiddenBackgroundWindow = backgroundPlayableSession && !captureKeepWindowVisible;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    // ## VR_PATCH BEGIN
    if (VR::getVR())
        // MSAA needs to happen in offscreen buffers.
        antialiasing = 0;
    // ## VR_PATCH END


    if (!backgroundPlayableSession
        && (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen))
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
        | (hiddenBackgroundWindow ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (!backgroundPlayableSession && windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (!backgroundPlayableSession && windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    if (hiddenBackgroundWindow)
    {
        Log(Debug::Info) << "Playable session: creating a hidden flat OpenGL window for background native capture";
    }
    else if (backgroundPlayableSession)
    {
        Log(Debug::Info) << "Playable session: creating a visible flat OpenGL window for exact-title native capture";
    }

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    // ## VR_PATCH BEGIN
    if (VR::getVR())
        realizeOperations->add(new InitializeVrOperation(this));
    // ## VR_PATCH END

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = Stereo::Position::fromMWUnits(leftEyeOffset),
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = Stereo::Position::fromMWUnits(rightEyeOffset),
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(mViewer, stereoEnabled, Settings::camera().mNearClip,
        Settings::camera().mViewingDistance, static_cast<unsigned>(Settings::video().mAntialiasing));

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

    // ## VR_PATCH BEGIN
    mCallbackManager = std::make_unique<Misc::CallbackManager>(mViewer);
    // ## VR_PATCH END

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    auto generatedFiles = std::make_unique<VFS::InMemoryArchive>("engine-generated files");
    mGeneratedFiles = generatedFiles.get();
    mVFS->addArchive(std::move(generatedFiles));
    mVFS->buildIndex();

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap, Settings::general().mAnisotropy);
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();
    bool shadersSupported = exts.glslLanguageVersion >= 1.2f;

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), shadersSupported, mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        configureVRPreScene(keybinderUser, keybinderUserExists, userGameControllerdb, gameControllerdb);
    }
    else
    {
        mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
            keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    }
    // ## VR_PATCH END
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // ## VR_PATCH BEGIN
    // In VR, the MWRender::Camera object needs to be created right away to apply tracking updates even before the scene and
    // RenderingManager has been created.
    auto camera = std::make_unique<MWRender::Camera>(mViewer->getCamera());
    // ## VR_PATCH END
    //  Create the world
    if (hasFalloutNvContent(mContentFiles) && !Settings::models().mLoadUnsupportedNifFiles)
    {
        // Retail Fallout assets use Gamebryo 20.2.0.7 NIF/KF files. The generic OpenMW default rejects formats
        // newer than Morrowind unless this compatibility mode is enabled, which otherwise makes a correctly
        // configured FNV session fail as soon as the first authored animation is loaded.
        Settings::models().mLoadUnsupportedNifFiles.set(true);
        Log(Debug::Info) << "FNV/ESM4: enabled unsupported NIF/KF compatibility for FalloutNV.esm";
    }
    if (hasFalloutNvContent(mContentFiles))
    {
        // The generic default points at Morrowind's two-layer cloud mesh. FNV WTHR drives four named surfaces on
        // the retail clouds.nif; leaving the default selected loads the textures but keeps every native layer hidden.
        Settings::models().mSkyclouds.set(VFS::Path::Normalized("meshes/sky/clouds.nif"));
    }
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    if (proofEnvEnabled("OPENMW_FNV_FIRST_SMOKE"))
    {
        if (mScreenCaptureHandler == nullptr)
            Log(Debug::Error) << "FNV first smoke: loading-screen native capture handler unavailable";
        else
        {
            mScreenCaptureHandler->setFramesToCapture(1);
            mScreenCaptureHandler->captureNextFrame(*mViewer);
            Log(Debug::Info) << "FNV first smoke: queued native loading-screen capture";
        }
    }
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine data load complete";
    listener->loadingOff();

    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine world init begin";
    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue, std::move(camera));
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine world init complete";
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine world scene registered";
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine setupPlayer begin";
    mWorld->setupPlayer();
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine setupPlayer complete";
    mWorld->setRandomSeed(mRandomSeed);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine random seed set";

    // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        configureVRScene();
    }
    // ## VR_PATCH END

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine gmst loader begin";
    mL10nManager->setGmstLoader(
        [gmst, misses = std::set<std::string, std::less<>>()](std::string_view gmstName) mutable {
            const ESM::GameSetting* res = gmst->search(gmstName);
            if (res && res->mValue.getType() == ESM::VT_String)
                return res->mValue.getString();
            else
            {
                if (misses.count(gmstName) == 0)
                {
                    misses.emplace(gmstName);
                    Log(Debug::Error) << "GMST " << gmstName << " not found";
                }
                return std::string("GMST:") + std::string(gmstName);
            }
        });
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine gmst loader ready";

    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine window store begin";
    mWindowManager->setStore(mWorld->getStore());
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine window initUI begin";
    mWindowManager->initUI();
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine window initUI complete";

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine translation load begin";
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine translation load complete";

    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine compiler extensions begin";
    Compiler::registerExtensions(mExtensions);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine compiler extensions complete";

    // Create script system
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine script system begin";
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine script system ready";

    // Create game mechanics system
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine mechanics begin";
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine mechanics ready";

    // Create dialog system
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine dialogue begin";
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine dialogue ready";

    if (mGeneratedFiles != nullptr)
    {
        mLuaManager->compileObScripts(*mVFS, *mGeneratedFiles);
    }

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua permanent storage begin";
    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua permanent storage complete";
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua init begin";
    mLuaManager->init();
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua init complete";

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine lua worker ready";
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    Log(Debug::Info) << "FNV/ESM4 proof: entering prepareEngine skipMenu=" << mSkipMenu << " newGame=" << mNewGame
                     << " saveFile=\"" << mSaveGameFile.string() << "\"";
    try
    {
        prepareEngine();
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "FNV/ESM4 proof: prepareEngine failed: " << e.what();
        throw;
    }
    Log(Debug::Info) << "FNV/ESM4 proof: prepareEngine complete skipMenu=" << mSkipMenu << " newGame=" << mNewGame
                     << " saveFile=\"" << mSaveGameFile.string() << "\"";

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

           // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        // Mask_GUI gets re-enabled at some point.
        mViewer->getCamera()->setCullMask(mViewer->getCamera()->getCullMask() & ~(MWRender::VisMask::Mask_GUI));
    }

           // ## VR_PATCH END
    //  Start the game
    if (!mSaveGameFile.empty())
    {
        Log(Debug::Info) << "FNV/ESM4 proof: loading save from command line \"" << mSaveGameFile.string() << "\"";
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        Log(Debug::Info) << "FNV/ESM4 proof: starting command-line game bypass=" << (!mNewGame);
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && std::getenv("OPENMW_PROOF_DELAY_STARTUP_SCRIPT") == nullptr
        && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, dt))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}

// ## VR_PATCH BEGIN
void OMW::Engine::configureVRGraphics(osg::GraphicsContext* gc)
{
    // Interaction profiles need to be configured before XR::Instance, to enable all relevant extensions
    configureVRInputProfiles();

    mXrInstance = std::make_unique<XR::Instance>(gc, mWindow);
    mXrSession = mXrInstance->createSession();
    if (mXrSession->appShouldShareDepthInfo())
        mSelectDepthFormatOperation->setSupportedFormats(mXrInstance->platform().supportedDepthFormats());
    mSelectColorFormatOperation->setSupportedFormats({ mXrInstance->platform().supportedColorFormats() });
}

void OMW::Engine::configureVRInputProfiles()
{
    const std::string xrinputuserdefault = mCfgMgr.getUserConfigPath().string() + "/openxrinteractionprofiles.xml";
    const std::string xrinputlocaldefault = mCfgMgr.getLocalPath().string() + "/openxrinteractionprofiles.xml";
    const std::string xrinputglobaldefault = mCfgMgr.getGlobalPath().string() + "/openxrinteractionprofiles.xml";

    std::string xrInteractionProfiles;
    if (std::filesystem::exists(xrinputuserdefault))
        xrInteractionProfiles = xrinputuserdefault;
    else if (std::filesystem::exists(xrinputlocaldefault))
        xrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        xrInteractionProfiles = xrinputglobaldefault;
    else
        xrInteractionProfiles = ""; // if it doesn't exist, pass in an empty string

    std::string defaulXrInteractionProfiles;
    if (std::filesystem::exists(xrinputlocaldefault))
        defaulXrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        defaulXrInteractionProfiles = xrinputglobaldefault;
    else
        defaulXrInteractionProfiles = ""; // if it doesn't exist, pass in an empty string

    Log(Debug::Verbose) << "xrinteractionprofiles user: " << xrinputuserdefault;
    Log(Debug::Verbose) << "xrinteractionprofiles local: " << xrinputlocaldefault;
    Log(Debug::Verbose) << "xrinteractionprofiles global: " << xrinputglobaldefault;

    XR::loadInteractionProfiles(xrInteractionProfiles, defaulXrInteractionProfiles);
}

void OMW::Engine::configureVRPreScene(const std::filesystem::path& userFile, bool userFileExists,
    const std::filesystem::path& userControllerBindingsFile, const std::filesystem::path& controllerBindingsFile)
{
    VR::setLeftHandedMode(Settings::vr().mLeftHandedMode);

    // Set up enough of VR to view the intro cinematic/loading screen
    mVrViewer = std::make_unique<VR::Viewer>(mXrSession, mViewer);
    mVrViewer->configureCallbacks();
    auto cullMask = ~(MWRender::VisMask::Mask_UpdateVisitor | MWRender::VisMask::Mask_SimpleWater);
    cullMask &= ~MWRender::VisMask::Mask_GUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI_NonIntersectable;
    mViewer->getCamera()->setCullMask(cullMask);
    mViewer->getCamera()->setCullMaskLeft(cullMask);
    mViewer->getCamera()->setCullMaskRight(cullMask);

    mInputManager = std::make_unique<MWVR::VRInputManager>(mWindow, mViewer, mScreenCaptureHandler, userFile,
        userFileExists, userControllerBindingsFile, controllerBindingsFile, mGrab);
    mVrGUIManager = std::make_unique<MWVR::VRGUIManager>(mViewer->getSceneData()->asGroup());

    // Before the RenderingManager and associated infrastructure is created, we need to render directly into the stereo framebuffer
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(true);
}

void OMW::Engine::configureVRScene() 
{
    // Rendering should now be done in the post-processor FBOs
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(false);
    // Fully initialize with integration into the rendering manager
    mVrGUIManager->initScene();
}
// ## VR_PATCH END

