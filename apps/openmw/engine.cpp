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
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadlvln.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadotft.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadtact.hpp>
#include <components/esm4/loadweap.hpp>

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
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/action.hpp"
#include "mwworld/actionequip.hpp"
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
                scoreSample(sample, true);

            return score;
        };

        const auto betterScore = [](const CandidateScore& left, const CandidateScore& right) {
            if (left.mBlockers != right.mBlockers)
                return left.mBlockers < right.mBlockers;
            if (left.mFrameBlockers != right.mFrameBlockers)
                return left.mFrameBlockers < right.mFrameBlockers;
            if (left.mNearestFrameBlocker != right.mNearestFrameBlocker)
                return left.mNearestFrameBlocker > right.mNearestFrameBlocker;
            return std::abs(left.mAngle) < std::abs(right.mAngle);
        };

        CandidateScore best;
        bool haveBest = false;
        for (float angle : angleOffsets)
        {
            const osg::Vec3f candidatePos = focus + rotateOffset(seedOffset, angle);
            CandidateScore score = scoreCandidate(candidatePos, angle);
            Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera candidate target=\""
                             << (target != nullptr ? target : "") << "\" angle=" << angle << " pos=("
                             << candidatePos.x() << "," << candidatePos.y() << "," << candidatePos.z()
                             << ") blockers=" << score.mBlockers
                             << " frameBlockers=" << score.mFrameBlockers
                             << " nearestBlocker=" << (score.mNearestBlocker == std::numeric_limits<float>::max()
                                        ? -1.f
                                        : score.mNearestBlocker)
                             << " nearestFrameBlocker="
                             << (score.mNearestFrameBlocker == std::numeric_limits<float>::max()
                                        ? -1.f
                                        : score.mNearestFrameBlocker)
                             << " closestSampleDistance="
                             << (score.mClosestSampleDistance == std::numeric_limits<float>::max()
                                        ? -1.f
                                        : score.mClosestSampleDistance)
                             << " blockerBase=" << score.mFirstBlockerBase
                             << " blockerType=\"" << score.mFirstBlockerType << "\""
                             << " blockerName=\"" << score.mFirstBlockerName << "\"";
            if (!haveBest || betterScore(score, best))
            {
                best = score;
                haveBest = true;
            }
        }

        const CandidateScore current = scoreCandidate(targetPos, 0.f);
        if (!haveBest || !betterScore(best, current))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera kept target=\""
                             << (target != nullptr ? target : "") << "\" blockers="
                             << (haveBest ? best.mBlockers : -1)
                             << " frameBlockers=" << (haveBest ? best.mFrameBlockers : -1);
            return false;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera selected target=\""
                         << (target != nullptr ? target : "") << "\" angle=" << best.mAngle << " from=("
                         << targetPos.x() << "," << targetPos.y() << "," << targetPos.z() << ") to=("
                         << best.mPos.x() << "," << best.mPos.y() << "," << best.mPos.z()
                         << ") blockers=" << best.mBlockers << " frameBlockers=" << best.mFrameBlockers;
        targetPos = best.mPos;
        return true;
    }

    void applyWorldViewerStaticCamera(MWRender::Camera* camera, const osg::Vec3d& eye, const osg::Vec3d& target)
    {
        camera->setMode(MWRender::Camera::Mode::Static, true);
        camera->setStaticPosition(eye);
        const osg::Vec3d delta = target - eye;
        const double horizontal = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
        camera->setPitch(static_cast<float>(std::atan2(delta.z(), horizontal)), true);
        camera->setYaw(-static_cast<float>(std::atan2(delta.x(), delta.y())), true);
        camera->setRoll(0.f);
        camera->instantTransition();
        camera->updateCamera();
    }

    osg::Vec3d resolveWorldViewerOrbitCamera(MWWorld::World& world, const osg::Vec3d& seedEye,
        const osg::Vec3d& target)
    {
        const osg::Vec3d seedDelta = seedEye - target;
        const float seedRadius
            = static_cast<float>(std::sqrt(seedDelta.x() * seedDelta.x() + seedDelta.y() * seedDelta.y()));
        const float radius = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_RADIUS",
            std::max(seedRadius, readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_DISTANCE", 420.f)));
        const float height = readProofFloat(
            "OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_HEIGHT", static_cast<float>(seedDelta.z()));
        const int samples = std::max(4, readProofInt("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_SAMPLES", 24));
        const float clearance = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_CLEARANCE", 48.f);
        const float minHitDistance = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_MIN_HIT_DISTANCE", 96.f);
        const float minGroundHeight = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_MIN_GROUND_HEIGHT", 96.f);
        const float rayDistance = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_ORBIT_GROUND_RAY_DISTANCE", 4096.f);
        const float baseAngle = static_cast<float>(std::atan2(seedDelta.x(), seedDelta.y()));
        const int groundMask = MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
            | MWPhysics::CollisionType_Door | MWPhysics::CollisionType_Water;
        const MWPhysics::RayCastingInterface* rayCasting = world.getRayCasting();

        osg::Vec3d bestEye = seedEye;
        float bestScore = -std::numeric_limits<float>::infinity();
        bool bestClear = false;
        int bestIndex = -1;

        for (int i = 0; i < samples; ++i)
        {
            const float angle = baseAngle + (static_cast<float>(i) * 2.f * static_cast<float>(osg::PI) / samples);
            osg::Vec3d candidate(target.x() + std::sin(angle) * radius, target.y() + std::cos(angle) * radius,
                target.z() + height);

            bool groundHit = false;
            float groundZ = 0.f;
            if (rayCasting != nullptr)
            {
                const osg::Vec3f groundFrom(
                    static_cast<float>(candidate.x()), static_cast<float>(candidate.y()),
                    static_cast<float>(candidate.z() + rayDistance * 0.25f));
                const osg::Vec3f groundTo(groundFrom.x(), groundFrom.y(), groundFrom.z() - rayDistance);
                const MWPhysics::RayCastingResult groundRay = rayCasting->castRay(groundFrom, groundTo, groundMask);
                groundHit = groundRay.mHit;
                if (groundHit)
                {
                    groundZ = groundRay.mHitPos.z();
                    if (candidate.z() < groundZ + minGroundHeight)
                        candidate.z() = groundZ + minGroundHeight;
                }
            }

            const osg::Vec3f from(
                static_cast<float>(candidate.x()), static_cast<float>(candidate.y()), static_cast<float>(candidate.z()));
            const osg::Vec3f to(static_cast<float>(target.x()), static_cast<float>(target.y()),
                static_cast<float>(target.z()));
            const float totalDistance = (to - from).length();
            MWPhysics::RayCastingResult renderRay {};
            world.castRenderingRay(renderRay, from, to, true, false, std::span<const MWWorld::Ptr> {});
            const float hitDistance = renderRay.mHit ? (renderRay.mHitPos - from).length() : totalDistance;
            bool actorHit = false;
            if (renderRay.mHit && !renderRay.mHitObject.isEmpty())
            {
                try
                {
                    actorHit = renderRay.mHitObject.getClass().isActor();
                }
                catch (const std::exception&)
                {
                    actorHit = false;
                }
            }
            const bool nearTargetHit = renderRay.mHit && hitDistance >= std::max(0.f, totalDistance - clearance);
            const bool immediateBlock = renderRay.mHit && hitDistance < minHitDistance;
            const bool clear = !renderRay.mHit || actorHit || nearTargetHit;
            float score = hitDistance + (groundHit ? 200.f : 0.f) - (static_cast<float>(i) * 0.01f);
            if (clear)
                score += 100000.f;
            if (actorHit)
                score += 2000.f;
            if (immediateBlock)
                score -= 50000.f;

            Log(Debug::Info) << "World viewer orbit raycast: candidate=" << i << " eye=(" << candidate.x() << ","
                             << candidate.y() << "," << candidate.z() << ") target=(" << target.x() << ","
                             << target.y() << "," << target.z() << ") radius=" << radius << " height=" << height
                             << " groundHit=" << groundHit << " groundZ=" << groundZ << " renderHit="
                             << renderRay.mHit << " actorHit=" << actorHit << " nearTargetHit=" << nearTargetHit
                             << " immediateBlock=" << immediateBlock << " hitDistance=" << hitDistance
                             << " totalDistance=" << totalDistance << " score=" << score << " hitPtr="
                             << (renderRay.mHitObject.isEmpty() ? std::string("<none>")
                                                                : renderRay.mHitObject.toString());

            if (score > bestScore)
            {
                bestScore = score;
                bestEye = candidate;
                bestClear = clear;
                bestIndex = i;
            }
        }

        Log(Debug::Info) << "World viewer orbit raycast: selected candidate=" << bestIndex << " eye=(" << bestEye.x()
                         << "," << bestEye.y() << "," << bestEye.z() << ") target=(" << target.x() << ","
                         << target.y() << "," << target.z() << ") clear=" << bestClear << " score=" << bestScore;
        return bestEye;
    }

    bool resolveFalloutProofHeadPose(const MWWorld::Ptr& actor, osg::Vec3d& center, osg::Vec3d& forward);

    bool enforceWorldViewerStaticCamera(MWWorld::World& world, unsigned frameNumber)
    {
        if (!worldViewerStaticCameraRequested())
            return false;

        MWRender::Camera* camera = world.getCamera();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (camera == nullptr || player.isEmpty())
            return false;

        const ESM::Position& position = player.getRefData().getPosition();
        const osg::Vec3d fallbackTarget(position.pos[0], position.pos[1], position.pos[2] + 128.f);
        osg::Vec3d target(
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_TARGET_X", static_cast<float>(fallbackTarget.x())),
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_TARGET_Y", static_cast<float>(fallbackTarget.y())),
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_TARGET_Z", static_cast<float>(fallbackTarget.z())));
        osg::Vec3d eye(
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_POS_X", position.pos[0] + 2048.f),
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_POS_Y", position.pos[1] - 4096.f),
            readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_POS_Z", position.pos[2] + 2048.f));

        bool headFollowResolved = false;
        std::string headFollowActor;
        osg::Vec3d headFollowCenter;
        osg::Vec3d headFollowForward;
        float headFollowFocus = 0.f;
        float headFollowDistance = 0.f;

        const char* followRefText = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_REF");
        if (followRefText != nullptr && *followRefText != '\0')
        {
            ESM::RefId followRef = ESM::RefId::deserializeText(followRefText);
            if (followRef.empty())
                followRef = ESM::RefId::stringRefId(followRefText);

            MWWorld::Ptr followed;
            if (const ESM::FormId* formId = followRef.getIf<ESM::FormId>())
                followed = MWBase::Environment::get().getWorldModel()->getPtr(*formId);
            if (followed.isEmpty())
                followed = world.searchPtr(followRef, true);
            if (!followed.isEmpty())
            {
                const ESM::Position& followedPosition = followed.getRefData().getPosition();
                eye.set(followedPosition.pos[0]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_EYE_X", 0.f),
                    followedPosition.pos[1]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_EYE_Y", -180.f),
                    followedPosition.pos[2]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_EYE_Z", 115.f));
                target.set(followedPosition.pos[0]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_TARGET_X", 0.f),
                    followedPosition.pos[1]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_TARGET_Y", 0.f),
                    followedPosition.pos[2]
                        + readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_TARGET_Z", 78.f));

                if (std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEADING") != nullptr)
                {
                    const float distance
                        = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEADING_DISTANCE", 180.f);
                    const float heading = followedPosition.rot[2];
                    eye.x() = followedPosition.pos[0] + std::sin(heading) * distance;
                    eye.y() = followedPosition.pos[1] + std::cos(heading) * distance;
                }

                if (std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD") != nullptr)
                {
                    osg::Vec3d headCenter;
                    osg::Vec3d headForward;
                    if (resolveFalloutProofHeadPose(followed, headCenter, headForward))
                    {
                        const float focus
                            = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD_FOCUS", 4.f);
                        const float distance
                            = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD_DISTANCE", 86.f);
                        target = headCenter + headForward * focus;
                        osg::Vec3d cameraDirection = headForward;
                        if (std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD_ORBIT_DEG") != nullptr)
                        {
                            const double radians
                                = osg::DegreesToRadians(readProofFloat(
                                    "OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD_ORBIT_DEG", 0.f));
                            const double c = std::cos(radians);
                            const double s = std::sin(radians);
                            cameraDirection.set(
                                headForward.x() * c - headForward.y() * s,
                                headForward.x() * s + headForward.y() * c,
                                headForward.z());
                            const double planarLength = std::sqrt(cameraDirection.x() * cameraDirection.x()
                                + cameraDirection.y() * cameraDirection.y());
                            if (planarLength > 1e-6)
                            {
                                const double originalPlanarLength
                                    = std::sqrt(headForward.x() * headForward.x() + headForward.y() * headForward.y());
                                cameraDirection.x() *= originalPlanarLength / planarLength;
                                cameraDirection.y() *= originalPlanarLength / planarLength;
                            }
                        }
                        eye = target + cameraDirection * distance;
                        eye.z() += readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_HEAD_EYE_Z", 0.f);
                        headFollowResolved = true;
                        headFollowActor = followRefText;
                        headFollowCenter = headCenter;
                        headFollowForward = headForward;
                        headFollowFocus = focus;
                        headFollowDistance = distance;
                    }
                }
            }
        }

        static const std::vector<WorldViewerCameraKeyframe> cameraSequence = getWorldViewerCameraSequence();
        static int loggedCameraSequenceIndex = -2;
        int cameraSequenceIndex = -1;
        for (std::size_t i = 0; i < cameraSequence.size(); ++i)
        {
            if (frameNumber >= static_cast<unsigned>(cameraSequence[i].mFrame))
                cameraSequenceIndex = static_cast<int>(i);
        }
        if (cameraSequenceIndex >= 0)
        {
            const WorldViewerCameraKeyframe& keyframe = cameraSequence[static_cast<std::size_t>(cameraSequenceIndex)];
            eye = keyframe.mEye;
            target = keyframe.mTarget;
            if (loggedCameraSequenceIndex != cameraSequenceIndex)
            {
                loggedCameraSequenceIndex = cameraSequenceIndex;
                Log(Debug::Info) << "World viewer: applying camera sequence index=" << cameraSequenceIndex
                                 << " frame=" << frameNumber << " eye=(" << eye.x() << "," << eye.y() << ","
                                 << eye.z() << ") target=(" << target.x() << "," << target.y() << ","
                                 << target.z() << ")";
            }
        }
        static bool orbitSolved = false;
        static osg::Vec3d orbitEye;
        static osg::Vec3d orbitTarget;
        osg::Vec3d resolvedEye = eye;
        if (cameraSequenceIndex < 0 && worldViewerOrbitRaycastRequested())
        {
            if (!orbitSolved || (orbitTarget - target).length() > 1.f)
            {
                orbitEye = resolveWorldViewerOrbitCamera(world, eye, target);
                orbitTarget = target;
                orbitSolved = true;
            }
            resolvedEye = orbitEye;
        }

        applyWorldViewerStaticCamera(camera, resolvedEye, target);

        if (headFollowResolved && frameNumber % 30 == 0)
        {
            const osg::Vec3d expectedTarget = headFollowCenter + headFollowForward * headFollowFocus;
            const float targetError = static_cast<float>((target - expectedTarget).length());
            const float eyeDistance = static_cast<float>((resolvedEye - target).length());
            const bool framingPass = cameraSequenceIndex < 0 && targetError <= 0.01f
                && std::abs(eyeDistance - headFollowDistance) <= 0.01f;
            Log(Debug::Info) << "World viewer actor framing: frame=" << frameNumber << " actor=\""
                             << headFollowActor << "\" head=(" << headFollowCenter.x() << ","
                             << headFollowCenter.y() << "," << headFollowCenter.z() << ") forward=("
                             << headFollowForward.x() << "," << headFollowForward.y() << ","
                             << headFollowForward.z() << ") eye=(" << resolvedEye.x() << ","
                             << resolvedEye.y() << "," << resolvedEye.z() << ") target=(" << target.x() << ","
                             << target.y() << "," << target.z() << ") targetError=" << targetError
                             << " eyeDistance=" << eyeDistance << " requestedDistance=" << headFollowDistance
                             << " status=" << (framingPass ? "pass" : "fail");
        }

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log(Debug::Info) << "World viewer: enforcing static survey camera eye=(" << resolvedEye.x() << ","
                             << resolvedEye.y() << "," << resolvedEye.z() << ") target=(" << target.x() << ","
                             << target.y() << ","
                             << target.z() << ") pitch=" << camera->getPitch() << " yaw=" << camera->getYaw();
        }
        return true;
    }

    bool settleWorldViewerNonStaticStartCamera(MWWorld::World& world)
    {
        if (!worldViewerNonStaticStartCameraRequested())
            return false;

        MWRender::Camera* camera = world.getCamera();
        MWWorld::Ptr player = world.getPlayerPtr();
        if (camera == nullptr || player.isEmpty())
            return false;

        const char* cameraMode = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_MODE");
        const bool thirdPerson = cameraMode != nullptr && std::string(cameraMode) == "thirdperson";
        ESM::Position position = player.getRefData().getPosition();
        const float cameraDistance = readProofFloat(
            "OPENMW_WORLD_VIEWER_START_CAMERA_DISTANCE", thirdPerson ? 192.f : 0.f);
        const float cameraPitch = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_PITCH", -position.rot[0]);
        const float cameraYaw = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_YAW", -position.rot[2]);

        const char* cameraPitchEnv = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_PITCH");
        const char* cameraYawEnv = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_YAW");
        const bool explicitFirstPersonAngles = !thirdPerson
            && ((cameraPitchEnv != nullptr && *cameraPitchEnv != '\0')
                || (cameraYawEnv != nullptr && *cameraYawEnv != '\0'));
        if (explicitFirstPersonAngles)
        {
            world.rotateObject(player, osg::Vec3f(-cameraPitch, position.rot[1], -cameraYaw));
            position = player.getRefData().getPosition();
        }

        const float cameraNudgeDistance = readProofFloat("OPENMW_WORLD_VIEWER_START_CAMERA_NUDGE_DISTANCE",
            readProofFloat("OPENMW_FNV_PROOF_CAMERA_NUDGE_DISTANCE", 128.f));

        camera->attachTo(player);
        if (cameraNudgeDistance > 0.f)
        {
            camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
            camera->setPreferredCameraDistance(cameraNudgeDistance);
            camera->processViewChange();
            camera->update(0.f, false);
            camera->instantTransition();
        }
        camera->setMode(thirdPerson ? MWRender::Camera::Mode::ThirdPerson : MWRender::Camera::Mode::FirstPerson, true);
        camera->setPreferredCameraDistance(cameraDistance);
        camera->processViewChange();
        camera->update(0.f, false);
        camera->instantTransition();
        camera->setPitch(cameraPitch, true);
        camera->setYaw(cameraYaw, true);
        camera->setRoll(0.f);
        camera->update(0.f, false);
        camera->instantTransition();
        camera->updateCamera();

        const osg::Vec3d cameraPos = camera->getPosition();
        Log(Debug::Info) << "World viewer: settled delayed non-static start camera mode="
                         << (thirdPerson ? "thirdperson" : "firstperson") << " playerPos=(" << position.pos[0]
                         << "," << position.pos[1] << "," << position.pos[2] << ") playerRot=("
                         << position.rot[0] << "," << position.rot[1] << "," << position.rot[2]
                         << ") cameraPos=(" << cameraPos.x() << "," << cameraPos.y() << "," << cameraPos.z()
                         << ") pitch=" << camera->getPitch() << " yaw=" << camera->getYaw()
                         << " distance=" << cameraDistance;
        return true;
    }

    void enforceWorldViewerDryStart(MWWorld::World& world)
    {
        const char* value = std::getenv("OPENMW_WORLD_VIEWER_START_DRY");
        if (value == nullptr || *value == '\0' || std::string(value) == "0")
            return;

        world.setWaterHeight(-200000.f);

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            Log(Debug::Info) << "World viewer: enforcing dry proof water height";
        }
    }

    bool hasFalloutNvContent(const std::vector<std::string>& contentFiles)
    {
        for (std::string file : contentFiles)
        {
            std::transform(file.begin(), file.end(), file.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            constexpr std::string_view falloutNv = "falloutnv.esm";
            if (file.size() >= falloutNv.size()
                && file.compare(file.size() - falloutNv.size(), falloutNv.size(), falloutNv) == 0)
                return true;
        }

        return false;
    }

    bool isFalloutProofFaceNodeName(std::string_view lowerName)
    {
        if (lowerName.find("starfield generated face ") != std::string_view::npos)
            return true;
        if (lowerName.find("fnv part ") == std::string_view::npos)
            return false;

        return lowerName.find("characters/head/head") != std::string_view::npos
            || lowerName.find("characters/head/mouth") != std::string_view::npos
            || lowerName.find("characters/head/teeth") != std::string_view::npos
            || lowerName.find("characters/head/tongue") != std::string_view::npos
            || lowerName.find("characters/head/eye") != std::string_view::npos
            || lowerName.find("characters/hair/beard") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/malehead") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/femalehead") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/lefteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/righteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/lefteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/righteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/eyebrow") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/eyebrow") != std::string_view::npos
            || lowerName.find("actors/human/mesh/beards") != std::string_view::npos
            || lowerName.find("actors/human/mesh/hairs") != std::string_view::npos;
    }

    bool isFalloutProofFaceHeadNodeName(std::string_view lowerName)
    {
        return lowerName.find("starfield generated face ") != std::string_view::npos
            || (lowerName.find("fnv part ") != std::string_view::npos
            && (lowerName.find("characters/head/head") != std::string_view::npos
                || lowerName.find("actors/human/characterassets/male/malehead") != std::string_view::npos
                || lowerName.find("actors/human/characterassets/female/femalehead") != std::string_view::npos));
    }

    bool isFalloutProofFaceFeatureNodeName(std::string_view lowerName)
    {
        if (lowerName.find("fnv part ") == std::string_view::npos)
            return false;

        return lowerName.find("characters/head/mouth") != std::string_view::npos
            || lowerName.find("characters/head/teeth") != std::string_view::npos
            || lowerName.find("characters/head/tongue") != std::string_view::npos
            || lowerName.find("characters/head/eye") != std::string_view::npos
            || lowerName.find("characters/hair/beard") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/lefteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/righteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/lefteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/righteye") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/male/eyebrow") != std::string_view::npos
            || lowerName.find("actors/human/characterassets/female/eyebrow") != std::string_view::npos
            || lowerName.find("actors/human/mesh/beards") != std::string_view::npos;
    }

    class FalloutProofFaceBoundsVisitor : public osg::NodeVisitor
    {
    public:
        FalloutProofFaceBoundsVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        }

        void apply(osg::Node& node) override
        {
            const std::string lowerName = Misc::StringUtils::lowerCase(node.getName());
            std::string_view semanticName(lowerName);
            const bool bip01Semantic = semanticName.rfind("bip01 ", 0) == 0;
            if (bip01Semantic)
                semanticName.remove_prefix(6);
            const bool explicitCreatureSemantic = bip01Semantic
                || semanticName == "head" || semanticName.rfind("head ", 0) == 0
                || semanticName.rfind("righead", 0) == 0 || semanticName.rfind("neck", 0) == 0
                || semanticName.rfind("rigneck", 0) == 0 || semanticName.rfind("pelvis", 0) == 0
                || semanticName.rfind("rigpelvis", 0) == 0 || semanticName.rfind("gaster", 0) == 0
                || semanticName.rfind("abdomen", 0) == 0 || semanticName == "root"
                || semanticName.rfind("torso", 0) == 0 || semanticName.rfind("rigtorso", 0) == 0
                || semanticName.rfind("spine", 0) == 0 || semanticName.rfind("rigspine", 0) == 0
                || semanticName == "tail" || semanticName.rfind("rigtail01", 0) == 0;
            if (explicitCreatureSemantic)
            {
                // Fallout creature rigs do not share the humanoid Bip01 Head convention.  For example,
                // ravens use RigHead/RigTail, mantises use Head/Gaster, and radscorpions expose Neck1/Pelvis.
                // Preserve those authored semantic anchors so the proof camera can derive the creature's
                // longitudinal axis instead of assuming that actor yaw is also the mesh's front axis.
                const osg::Vec3d center = osg::computeLocalToWorld(getNodePath()).getTrans();
                const SemanticAnchor anchor{ node.getName(), center };
                if (semanticName.find("head") != std::string_view::npos
                    && semanticName.find("brain") == std::string_view::npos
                    && semanticName.find("nub") == std::string_view::npos)
                    mCreatureFrontAnchors.push_back(anchor);
                else if (semanticName.find("neck") != std::string_view::npos)
                    mCreatureNeckAnchors.push_back(anchor);

                if (semanticName.rfind("pelvis", 0) == 0 || semanticName.rfind("rigpelvis", 0) == 0
                    || semanticName.rfind("gaster", 0) == 0 || semanticName.rfind("abdomen", 0) == 0
                    || semanticName == "root" || semanticName.rfind("torso", 0) == 0
                    || semanticName.rfind("rigtorso", 0) == 0 || semanticName.rfind("spine", 0) == 0
                    || semanticName.rfind("rigspine", 0) == 0 || semanticName == "tail"
                    || semanticName.rfind("rigtail01", 0) == 0)
                    mCreatureRearAnchors.push_back(anchor);
            }
            if (lowerName.rfind("screen01", 0) == 0
                || lowerName.rfind("screenreflection01", 0) == 0)
            {
                const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                osg::Vec3d screenForward
                    = osg::Matrixd::transform3x3(osg::Vec3d(0.0, 1.0, 0.0), localToWorld);
                if (screenForward.normalize() > 1e-6)
                    mScreenForward += screenForward;

                osg::Vec3d screenCenter = localToWorld.getTrans();
                const osg::BoundingSphere bound = node.getBound();
                if (bound.valid())
                {
                    osg::NodePath parentPath = getNodePath();
                    if (!parentPath.empty())
                        parentPath.pop_back();
                    screenCenter = bound.center() * osg::computeLocalToWorld(parentPath);
                }
                mScreenCenter += screenCenter;
                ++mScreenMatched;
            }
            if (lowerName == "bip01 head" || lowerName == "bip01 head_nub")
            {
                const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                osg::Vec3d forward
                    = osg::Matrixd::transform3x3(osg::Vec3d(0.0, 1.0, 0.0), localToWorld);
                forward.normalize();
                if (lowerName == "bip01 head")
                {
                    mHeadBoneCenter += localToWorld.getTrans();
                    if (forward.length2() > 1e-12)
                        mHeadForward += forward;
                    ++mHeadBoneMatched;
                }
                else
                {
                    // The retail oracle records Bip01 Head, not its child Head_Nub.
                    // Keep the nub only as a fallback; averaging both shifted the
                    // reported Easy Pete head by four world units and polluted the
                    // orientation gate even when the rendered head was unchanged.
                    mHeadFallbackCenter += localToWorld.getTrans();
                    if (forward.length2() > 1e-12)
                        mHeadFallbackForward += forward;
                    ++mHeadFallbackMatched;
                }
            }

            if (isFalloutProofFaceNodeName(lowerName))
            {
                const osg::BoundingSphere bound = node.getBound();
                if (bound.valid())
                {
                    osg::NodePath parentPath = getNodePath();
                    if (!parentPath.empty())
                        parentPath.pop_back();
                    const osg::Matrixd parentToWorld = osg::computeLocalToWorld(parentPath);
                    const osg::Vec3d center = bound.center() * parentToWorld;
                    const double radius = bound.radius();
                    mBounds.expandBy(osg::Vec3d(center.x() - radius, center.y() - radius, center.z() - radius));
                    mBounds.expandBy(osg::Vec3d(center.x() + radius, center.y() + radius, center.z() + radius));
                    ++mMatched;

                    if (isFalloutProofFaceHeadNodeName(lowerName))
                    {
                        mHeadCenter += center;
                        ++mHeadMatched;
                    }
                    else if (isFalloutProofFaceFeatureNodeName(lowerName))
                    {
                        mFeatureCenter += center;
                        ++mFeatureMatched;
                    }
                }
            }

            traverse(node);
        }

        const osg::BoundingBox& getBounds() const { return mBounds; }
        unsigned int getMatched() const { return mMatched; }
        unsigned int getHeadMatched() const { return mHeadMatched + mHeadBoneMatched; }
        unsigned int getHeadBoneMatched() const { return mHeadBoneMatched + mHeadFallbackMatched; }
        unsigned int getFeatureMatched() const { return mFeatureMatched; }
        unsigned int getScreenMatched() const { return mScreenMatched; }
        osg::Vec3d getScreenCenter() const
        {
            return mScreenMatched > 0 ? mScreenCenter / static_cast<double>(mScreenMatched) : osg::Vec3d();
        }
        osg::Vec3d getScreenForward(const osg::Vec3d& actorOrigin) const
        {
            if (mScreenMatched == 0)
                return osg::Vec3d();

            // A screen normal has two possible signs, and NIFs do not use one universal local winding/axis
            // convention for every actor add-on. Pick the authored normal's outward hemisphere from geometry:
            // the screen center must lie away from the actor origin in the same direction as the portrait camera.
            // This also remains correct when coordinate conversion changes handedness or an add-on is mirrored.
            osg::Vec3d radial = getScreenCenter() - actorOrigin;
            radial.z() = 0.0;
            const bool hasRadial = radial.normalize() > 1e-6;

            osg::Vec3d forward = mScreenForward;
            forward.z() = 0.0;
            if (forward.normalize() <= 1e-6)
                return hasRadial ? radial : osg::Vec3d();
            if (hasRadial && forward * radial < 0.0)
                forward = -forward;
            return forward;
        }
        osg::Vec3d getHeadCenter() const
        {
            if (mHeadBoneMatched > 0)
                return mHeadBoneCenter / static_cast<double>(mHeadBoneMatched);
            if (mHeadFallbackMatched > 0)
                return mHeadFallbackCenter / static_cast<double>(mHeadFallbackMatched);
            return mHeadMatched > 0 ? mHeadCenter / static_cast<double>(mHeadMatched) : osg::Vec3d();
        }
        osg::Vec3d getHeadForward() const
        {
            osg::Vec3d forward = mHeadBoneMatched > 0 ? mHeadForward : mHeadFallbackForward;
            if (forward.normalize() <= 1e-6)
                return osg::Vec3d();
            return forward;
        }
        osg::Vec3d getFeatureCenter() const
        {
            return mFeatureMatched > 0 ? mFeatureCenter / static_cast<double>(mFeatureMatched) : osg::Vec3d();
        }
        bool getCreatureAxis(
            osg::Vec3d& center, osg::Vec3d& forward, std::string& frontName, std::string& rearName) const
        {
            const std::vector<SemanticAnchor>& frontAnchors
                = !mCreatureFrontAnchors.empty() ? mCreatureFrontAnchors : mCreatureNeckAnchors;
            if (frontAnchors.empty() || mCreatureRearAnchors.empty())
                return false;

            // Prefer the semantic pair with the greatest planar separation.  This picks Head/Gaster for
            // insects and RigHead/RigTail for birds, while a curled scorpion tail naturally loses to
            // Neck/Pelvis.  Requiring a real planar baseline rejects vertical head/pelvis pairs that do not
            // encode which way a standing creature faces.
            const SemanticAnchor* bestFront = nullptr;
            const SemanticAnchor* bestRear = nullptr;
            double bestPlanarLength2 = 1.0;
            for (const SemanticAnchor& candidateFront : frontAnchors)
            {
                for (const SemanticAnchor& candidateRear : mCreatureRearAnchors)
                {
                    const double dx = candidateFront.mCenter.x() - candidateRear.mCenter.x();
                    const double dy = candidateFront.mCenter.y() - candidateRear.mCenter.y();
                    const double planarLength2 = dx * dx + dy * dy;
                    if (planarLength2 > bestPlanarLength2)
                    {
                        bestPlanarLength2 = planarLength2;
                        bestFront = &candidateFront;
                        bestRear = &candidateRear;
                    }
                }
            }
            if (bestFront == nullptr || bestRear == nullptr)
                return false;

            center = bestFront->mCenter;
            forward = bestFront->mCenter - bestRear->mCenter;
            forward.z() = 0.0;
            if (forward.normalize() <= 1e-6)
                return false;
            frontName = bestFront->mName;
            rearName = bestRear->mName;
            return true;
        }

    private:
        struct SemanticAnchor
        {
            std::string mName;
            osg::Vec3d mCenter;
        };

        osg::BoundingBox mBounds;
        osg::Vec3d mScreenCenter;
        osg::Vec3d mScreenForward;
        osg::Vec3d mHeadBoneCenter;
        osg::Vec3d mHeadForward;
        osg::Vec3d mHeadFallbackCenter;
        osg::Vec3d mHeadFallbackForward;
        osg::Vec3d mHeadCenter;
        osg::Vec3d mFeatureCenter;
        unsigned int mMatched = 0;
        unsigned int mScreenMatched = 0;
        unsigned int mHeadBoneMatched = 0;
        unsigned int mHeadFallbackMatched = 0;
        unsigned int mHeadMatched = 0;
        unsigned int mFeatureMatched = 0;
        std::vector<SemanticAnchor> mCreatureFrontAnchors;
        std::vector<SemanticAnchor> mCreatureNeckAnchors;
        std::vector<SemanticAnchor> mCreatureRearAnchors;
    };

    class FalloutProofDrawableCullVisitor final : public osg::NodeVisitor
    {
    public:
        explicit FalloutProofDrawableCullVisitor(unsigned int minimumCullTraversal)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mMinimumCullTraversal(minimumCullTraversal)
        {
            setNodeMaskOverride(~0u);
            setTraversalMask(~0u);
        }

        void apply(osg::Drawable& drawable) override
        {
            bool visiblePath = drawable.getNodeMask() != 0;
            for (osg::Node* node : getNodePath())
            {
                if (node != nullptr && node->getNodeMask() == 0)
                {
                    visiblePath = false;
                    break;
                }
            }
            if (!visiblePath)
                return;

            ++mVisibleDrawables;
            auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
                return;

            ++mVisibleRigs;
            if (rig->hasResolvedParentSkeleton())
                ++mResolvedRigs;
            const unsigned int traversal = rig->getLastCullTraversalNumber();
            mMaximumCullTraversal = std::max(mMaximumCullTraversal, traversal);
            if (traversal >= mMinimumCullTraversal)
                ++mCullReadyRigs;
        }

        unsigned int mVisibleDrawables = 0;
        unsigned int mVisibleRigs = 0;
        unsigned int mResolvedRigs = 0;
        unsigned int mCullReadyRigs = 0;
        unsigned int mMaximumCullTraversal = 0;

    private:
        unsigned int mMinimumCullTraversal = 0;
    };

    bool resolveFalloutProofHeadPose(const MWWorld::Ptr& actor, osg::Vec3d& center, osg::Vec3d& forward)
    {
        if (actor.isEmpty() || actor.getRefData().getBaseNode() == nullptr)
            return false;

        FalloutProofFaceBoundsVisitor visitor;
        actor.getRefData().getBaseNode()->accept(visitor);
        if (visitor.getScreenMatched() > 0)
        {
            const ESM::Position& actorPosition = actor.getRefData().getPosition();
            const osg::Vec3d actorOrigin(
                actorPosition.pos[0], actorPosition.pos[1], actorPosition.pos[2]);
            center = visitor.getScreenCenter();
            forward = visitor.getScreenForward(actorOrigin);
            if (forward.length2() > 1e-6)
                return true;
        }
        if (actor.getType() == ESM::REC_CREA4)
        {
            std::string frontName;
            std::string rearName;
            if (visitor.getCreatureAxis(center, forward, frontName, rearName))
            {
                const osg::Vec3d sceneForward = forward;
                // The imported Fallout skeleton path is reflected across the engine/world Y basis.  The node
                // pair therefore supplies the right semantic longitudinal axis but its scene-space Y sign is
                // opposite the authored actor/camera transform recorded by retail.  Convert the direction at
                // the proof-camera boundary; X and Z already share the world basis.
                forward.y() = -forward.y();
                forward.normalize();
                Log(Debug::Info) << "FNV/ESM4 proof: creature semantic camera axis actor=" << actor.toString()
                                 << " frontNode=\"" << frontName << "\" rearNode=\"" << rearName
                                 << "\" frontCenter=(" << center.x() << "," << center.y() << "," << center.z()
                                 << ") sceneForward=(" << sceneForward.x() << "," << sceneForward.y() << ","
                                 << sceneForward.z() << ") retailWorldForward=(" << forward.x() << ","
                                 << forward.y() << "," << forward.z() << ")";
                return true;
            }
        }
        if (visitor.getHeadBoneMatched() == 0)
            return false;

        center = visitor.getHeadCenter();
        forward = visitor.getHeadForward();
        return forward.length2() > 1e-6;
    }

    struct FalloutProofPortraitPose
    {
        osg::Vec3d mHeadCenter;
        osg::Vec3d mHeadForward;
        osg::Vec3d mPelvisCenter;
        osg::Vec3d mLeftHandCenter;
        osg::Vec3d mRightHandCenter;
        osg::Vec3d mLeftFootCenter;
        osg::Vec3d mRightFootCenter;
        bool mHeadResolved = false;
        bool mPelvisResolved = false;
        bool mLeftHandResolved = false;
        bool mRightHandResolved = false;
        bool mLeftFootResolved = false;
        bool mRightFootResolved = false;
    };

    class FalloutProofPortraitPoseVisitor : public osg::NodeVisitor
    {
    public:
        FalloutProofPortraitPoseVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
        }

        void apply(osg::Node& node) override
        {
            const std::string lowerName = Misc::StringUtils::lowerCase(node.getName());
            if (lowerName == "bip01 head" || lowerName == "bip01 head_nub"
                || lowerName == "bip01 pelvis" || lowerName == "bip01 l hand"
                || lowerName == "bip01 r hand" || lowerName == "bip01 l foot"
                || lowerName == "bip01 r foot")
            {
                const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                const osg::Vec3d center = localToWorld.getTrans();
                if (lowerName == "bip01 head" || lowerName == "bip01 head_nub")
                {
                    mPose.mHeadCenter += center;
                    osg::Vec3d forward
                        = osg::Matrixd::transform3x3(osg::Vec3d(0.0, 1.0, 0.0), localToWorld);
                    if (forward.normalize() > 1e-6)
                        mPose.mHeadForward += forward;
                    ++mHeadCount;
                }
                else if (lowerName == "bip01 pelvis")
                {
                    mPose.mPelvisCenter += center;
                    ++mPelvisCount;
                }
                else if (lowerName == "bip01 l hand")
                {
                    mPose.mLeftHandCenter += center;
                    ++mLeftHandCount;
                }
                else if (lowerName == "bip01 r hand")
                {
                    mPose.mRightHandCenter += center;
                    ++mRightHandCount;
                }
                else if (lowerName == "bip01 l foot")
                {
                    mPose.mLeftFootCenter += center;
                    ++mLeftFootCount;
                }
                else
                {
                    mPose.mRightFootCenter += center;
                    ++mRightFootCount;
                }
            }
            traverse(node);
        }

        FalloutProofPortraitPose getPose() const
        {
            FalloutProofPortraitPose result = mPose;
            if (mHeadCount > 0)
            {
                result.mHeadCenter /= static_cast<double>(mHeadCount);
                result.mHeadResolved = result.mHeadForward.normalize() > 1e-6;
            }
            if (mPelvisCount > 0)
            {
                result.mPelvisCenter /= static_cast<double>(mPelvisCount);
                result.mPelvisResolved = true;
            }
            if (mLeftHandCount > 0)
            {
                result.mLeftHandCenter /= static_cast<double>(mLeftHandCount);
                result.mLeftHandResolved = true;
            }
            if (mRightHandCount > 0)
            {
                result.mRightHandCenter /= static_cast<double>(mRightHandCount);
                result.mRightHandResolved = true;
            }
            if (mLeftFootCount > 0)
            {
                result.mLeftFootCenter /= static_cast<double>(mLeftFootCount);
                result.mLeftFootResolved = true;
            }
            if (mRightFootCount > 0)
            {
                result.mRightFootCenter /= static_cast<double>(mRightFootCount);
                result.mRightFootResolved = true;
            }
            return result;
        }

    private:
        FalloutProofPortraitPose mPose;
        unsigned int mHeadCount = 0;
        unsigned int mPelvisCount = 0;
        unsigned int mLeftHandCount = 0;
        unsigned int mRightHandCount = 0;
        unsigned int mLeftFootCount = 0;
        unsigned int mRightFootCount = 0;
    };

    FalloutProofPortraitPose resolveFalloutProofPortraitPose(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || actor.getRefData().getBaseNode() == nullptr)
            return {};
        FalloutProofPortraitPoseVisitor visitor;
        actor.getRefData().getBaseNode()->accept(visitor);
        return visitor.getPose();
    }

    constexpr std::size_t FalloutRetailTransformComponentCount = 13;
    constexpr std::size_t FalloutRetailMatrixComponentCount = 12;

    struct FalloutRetailPoseNode
    {
        std::string mName;
        std::string mParent;
        std::array<std::uint32_t, FalloutRetailTransformComponentCount> mLocalBits{};
        std::array<std::uint32_t, FalloutRetailTransformComponentCount> mWorldBits{};
        osg::Matrixd mLocalMatrix;
        osg::Matrixd mWorldMatrix;
    };

    struct FalloutRetailPoseSnapshot
    {
        std::string mPath;
        std::array<std::uint32_t, 6> mRootBits{};
        std::vector<FalloutRetailPoseNode> mNodes;
        std::unordered_map<std::string, std::size_t> mNodeByLowerName;
    };

    osg::Matrixd makeFalloutRetailPoseMatrix(
        const std::array<std::uint32_t, FalloutRetailTransformComponentCount>& bits)
    {
        osg::Matrixd matrix;
        matrix.makeIdentity();
        const double scale = static_cast<double>(proofFloatFromBits(bits[12]));
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
            {
                matrix(row, column)
                    = static_cast<double>(proofFloatFromBits(bits[column * 3 + row])) * scale;
            }
        }
        matrix(3, 0) = proofFloatFromBits(bits[9]);
        matrix(3, 1) = proofFloatFromBits(bits[10]);
        matrix(3, 2) = proofFloatFromBits(bits[11]);
        return matrix;
    }

    std::array<std::uint32_t, FalloutRetailMatrixComponentCount> getFalloutRetailMatrixBits(
        const osg::Matrixd& matrix)
    {
        std::array<std::uint32_t, FalloutRetailMatrixComponentCount> result{};
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
                result[row * 3 + column] = std::bit_cast<std::uint32_t>(static_cast<float>(matrix(row, column)));
        }
        const osg::Vec3d translation = matrix.getTrans();
        result[9] = std::bit_cast<std::uint32_t>(static_cast<float>(translation.x()));
        result[10] = std::bit_cast<std::uint32_t>(static_cast<float>(translation.y()));
        result[11] = std::bit_cast<std::uint32_t>(static_cast<float>(translation.z()));
        return result;
    }

    bool isFalloutRetailPoseSnapshotRoot(std::string_view lowerName)
    {
        return lowerName == "scene root";
    }

    std::size_t getFalloutRetailPoseReplayNodeCount(const FalloutRetailPoseSnapshot& snapshot)
    {
        return std::count_if(snapshot.mNodes.begin(), snapshot.mNodes.end(), [](const FalloutRetailPoseNode& node) {
            return !isFalloutRetailPoseSnapshotRoot(Misc::StringUtils::lowerCase(node.mName));
        });
    }

    osg::Matrixd getFalloutRetailPoseAdapterLocal(
        const FalloutRetailPoseNode& node, const osg::NodePath& nodePath)
    {
        osg::NodePath parentPath = nodePath;
        if (!parentPath.empty())
            parentPath.pop_back();
        const osg::Matrixd parentWorld = osg::computeLocalToWorld(parentPath);
        return node.mWorldMatrix * osg::Matrixd::inverse(parentWorld);
    }

    float falloutRetailFloatMultiply(float left, float right)
    {
        volatile float rounded = left * right;
        return rounded;
    }

    float falloutRetailFloatAdd(float left, float right)
    {
        volatile float rounded = left + right;
        return rounded;
    }

    float falloutRetailFloatDot3(float left0, float right0, float left1, float right1, float left2, float right2)
    {
        float result = falloutRetailFloatMultiply(left0, right0);
        result = falloutRetailFloatAdd(result, falloutRetailFloatMultiply(left1, right1));
        return falloutRetailFloatAdd(result, falloutRetailFloatMultiply(left2, right2));
    }

    std::array<std::uint32_t, FalloutRetailTransformComponentCount> composeFalloutRetailTransformBits(
        const std::array<std::uint32_t, FalloutRetailTransformComponentCount>& parent,
        const std::array<std::uint32_t, FalloutRetailTransformComponentCount>& local)
    {
        std::array<std::uint32_t, FalloutRetailTransformComponentCount> result{};
        std::array<float, FalloutRetailTransformComponentCount> parentValues{};
        std::array<float, FalloutRetailTransformComponentCount> localValues{};
        for (std::size_t component = 0; component < FalloutRetailTransformComponentCount; ++component)
        {
            parentValues[component] = proofFloatFromBits(parent[component]);
            localValues[component] = proofFloatFromBits(local[component]);
        }

        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 3; ++column)
            {
                const float value = falloutRetailFloatDot3(parentValues[row * 3], localValues[column],
                    parentValues[row * 3 + 1], localValues[3 + column], parentValues[row * 3 + 2],
                    localValues[6 + column]);
                result[row * 3 + column] = std::bit_cast<std::uint32_t>(value);
            }

            const float rotatedTranslation = falloutRetailFloatDot3(parentValues[row * 3], localValues[9],
                parentValues[row * 3 + 1], localValues[10], parentValues[row * 3 + 2], localValues[11]);
            const float scaledTranslation = falloutRetailFloatMultiply(rotatedTranslation, parentValues[12]);
            result[9 + row]
                = std::bit_cast<std::uint32_t>(falloutRetailFloatAdd(scaledTranslation, parentValues[9 + row]));
        }
        result[12]
            = std::bit_cast<std::uint32_t>(falloutRetailFloatMultiply(parentValues[12], localValues[12]));
        return result;
    }

    void auditFalloutRetailPoseSource(const FalloutRetailPoseSnapshot& snapshot)
    {
        std::vector<std::array<std::uint32_t, FalloutRetailTransformComponentCount>> calculated;
        calculated.reserve(snapshot.mNodes.size());
        std::size_t mismatchedNodes = 0;
        std::size_t mismatchedWords = 0;
        for (std::size_t index = 0; index < snapshot.mNodes.size(); ++index)
        {
            const FalloutRetailPoseNode& node = snapshot.mNodes[index];
            std::array<std::uint32_t, FalloutRetailTransformComponentCount> world = node.mLocalBits;
            if (Misc::StringUtils::lowerCase(node.mParent) != "none")
            {
                const auto parent = snapshot.mNodeByLowerName.find(Misc::StringUtils::lowerCase(node.mParent));
                if (parent == snapshot.mNodeByLowerName.end() || parent->second >= index)
                    throw std::runtime_error("retail pose parent missing or out of order for node " + node.mName);
                world = composeFalloutRetailTransformBits(calculated[parent->second], node.mLocalBits);
            }
            calculated.push_back(world);

            bool nodeMismatch = false;
            for (std::size_t component = 0; component < FalloutRetailTransformComponentCount; ++component)
            {
                if (world[component] == node.mWorldBits[component])
                    continue;
                nodeMismatch = true;
                ++mismatchedWords;
            }
            mismatchedNodes += nodeMismatch ? 1 : 0;
        }

        const std::size_t words = snapshot.mNodes.size() * FalloutRetailTransformComponentCount;
        const bool pass = mismatchedNodes == 0;
        Log(pass ? Debug::Info : Debug::Error)
            << "FNV/ESM4 retail pose source audit: snapshot=\"" << snapshot.mPath << "\" nodes="
            << snapshot.mNodes.size() << " transformWords=" << words << " mismatchedNodes=" << mismatchedNodes
            << " mismatchedWords=" << mismatchedWords << " arithmetic=parent-times-local-round-every-float32-op status="
            << (pass ? "pass" : "fail");
        if (!pass)
            throw std::runtime_error("retail pose source arithmetic audit failed: " + snapshot.mPath);
    }

    std::shared_ptr<FalloutRetailPoseSnapshot> loadFalloutRetailPoseSnapshot(const std::string& path)
    {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("failed to open retail pose snapshot: " + path);

        std::string line;
        if (!std::getline(input, line) || line != "NIKAMI_RETAIL_POSE_SNAPSHOT_V1")
            throw std::runtime_error("unsupported retail pose snapshot schema: " + path);

        auto snapshot = std::make_shared<FalloutRetailPoseSnapshot>();
        snapshot->mPath = path;
        bool foundRoot = false;
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;
            std::istringstream row(line);
            std::string kind;
            row >> kind;
            if (kind == "source")
                continue;
            if (kind == "root")
            {
                for (std::uint32_t& bits : snapshot->mRootBits)
                {
                    std::string token;
                    if (!(row >> token) || !parseProofFloatBits(token, bits))
                        throw std::runtime_error("invalid root float bits in retail pose snapshot: " + path);
                }
                foundRoot = true;
                continue;
            }
            if (kind != "node")
                throw std::runtime_error("unknown retail pose snapshot row: " + kind);

            FalloutRetailPoseNode node;
            std::string parentToken;
            std::string localToken;
            std::string worldToken;
            if (!(row >> std::quoted(node.mName) >> parentToken >> std::quoted(node.mParent) >> localToken)
                || parentToken != "parent" || localToken != "local")
            {
                throw std::runtime_error("invalid node header in retail pose snapshot: " + path);
            }
            for (std::uint32_t& bits : node.mLocalBits)
            {
                std::string token;
                if (!(row >> token) || !parseProofFloatBits(token, bits))
                    throw std::runtime_error("invalid local float bits for retail pose node " + node.mName);
            }
            if (!(row >> worldToken) || worldToken != "world")
                throw std::runtime_error("missing world transform for retail pose node " + node.mName);
            for (std::uint32_t& bits : node.mWorldBits)
            {
                std::string token;
                if (!(row >> token) || !parseProofFloatBits(token, bits))
                    throw std::runtime_error("invalid world float bits for retail pose node " + node.mName);
            }
            node.mLocalMatrix = makeFalloutRetailPoseMatrix(node.mLocalBits);
            node.mWorldMatrix = makeFalloutRetailPoseMatrix(node.mWorldBits);
            snapshot->mNodes.push_back(std::move(node));
        }

        if (!foundRoot || snapshot->mNodes.empty())
            throw std::runtime_error("retail pose snapshot has no root or nodes: " + path);
        for (std::size_t index = 0; index < snapshot->mNodes.size(); ++index)
        {
            const std::string lower = Misc::StringUtils::lowerCase(snapshot->mNodes[index].mName);
            if (!snapshot->mNodeByLowerName.emplace(lower, index).second)
                throw std::runtime_error("duplicate retail pose node: " + snapshot->mNodes[index].mName);
        }
        auditFalloutRetailPoseSource(*snapshot);
        return snapshot;
    }

    struct FalloutRetailPoseAuditResult
    {
        std::size_t mMatched = 0;
        std::size_t mDuplicates = 0;
        std::size_t mLocalMismatches = 0;
        std::size_t mWorldMismatches = 0;
        std::vector<std::string> mDetails;
    };

    class FalloutRetailPoseAuditVisitor final : public osg::NodeVisitor
    {
    public:
        explicit FalloutRetailPoseAuditVisitor(const FalloutRetailPoseSnapshot& snapshot)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mSnapshot(snapshot)
            , mSeen(snapshot.mNodes.size(), 0)
        {
        }

        void apply(osg::Node& node) override
        {
            const std::string lowerName = Misc::StringUtils::lowerCase(node.getName());
            const auto found = mSnapshot.mNodeByLowerName.find(lowerName);
            const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node);
            if (found != mSnapshot.mNodeByLowerName.end() && transform != nullptr
                && !isFalloutRetailPoseSnapshotRoot(lowerName))
            {
                const std::size_t index = found->second;
                const FalloutRetailPoseNode& expected = mSnapshot.mNodes[index];
                ++mResult.mMatched;
                if (++mSeen[index] > 1)
                    ++mResult.mDuplicates;

                const auto localBits = getFalloutRetailMatrixBits(transform->getMatrix());
                const auto worldBits = getFalloutRetailMatrixBits(osg::computeLocalToWorld(getNodePath()));
                const auto expectedLocalBits
                    = getFalloutRetailMatrixBits(getFalloutRetailPoseAdapterLocal(expected, getNodePath()));
                const auto expectedWorldBits = getFalloutRetailMatrixBits(expected.mWorldMatrix);
                bool localMismatch = false;
                bool worldMismatch = false;
                for (std::size_t component = 0; component < FalloutRetailMatrixComponentCount; ++component)
                {
                    localMismatch = localMismatch || localBits[component] != expectedLocalBits[component];
                    worldMismatch = worldMismatch || worldBits[component] != expectedWorldBits[component];
                }
                mResult.mLocalMismatches += localMismatch ? 1 : 0;
                mResult.mWorldMismatches += worldMismatch ? 1 : 0;
                if ((localMismatch || worldMismatch) && mResult.mDetails.size() < 24)
                {
                    std::ostringstream detail;
                    detail << "node=\"" << expected.mName << "\" localMismatch=" << localMismatch
                           << " worldMismatch=" << worldMismatch << " localExpected="
                           << formatProofFloatBits(expectedLocalBits)
                           << " localActual=" << formatProofFloatBits(localBits) << " worldExpected="
                           << formatProofFloatBits(expectedWorldBits)
                           << " worldActual=" << formatProofFloatBits(worldBits);
                    mResult.mDetails.push_back(detail.str());
                }
            }
            traverse(node);
        }

        FalloutRetailPoseAuditResult finish()
        {
            for (std::size_t index = 0; index < mSnapshot.mNodes.size(); ++index)
            {
                if (isFalloutRetailPoseSnapshotRoot(Misc::StringUtils::lowerCase(mSnapshot.mNodes[index].mName)))
                    continue;
                if (mSeen[index] == 0 && mResult.mDetails.size() < 24)
                    mResult.mDetails.push_back("missingNode=\"" + mSnapshot.mNodes[index].mName + "\"");
            }
            return std::move(mResult);
        }

    private:
        const FalloutRetailPoseSnapshot& mSnapshot;
        std::vector<unsigned int> mSeen;
        FalloutRetailPoseAuditResult mResult;
    };

    class FalloutRetailPoseApplyVisitor final : public osg::NodeVisitor
    {
    public:
        explicit FalloutRetailPoseApplyVisitor(const FalloutRetailPoseSnapshot& snapshot)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mSnapshot(snapshot)
        {
        }

        void apply(osg::Node& node) override
        {
            const std::string lowerName = Misc::StringUtils::lowerCase(node.getName());
            const auto found = mSnapshot.mNodeByLowerName.find(lowerName);
            auto* transform = dynamic_cast<osg::MatrixTransform*>(&node);
            if (found != mSnapshot.mNodeByLowerName.end() && transform != nullptr
                && !isFalloutRetailPoseSnapshotRoot(lowerName))
            {
                transform->setMatrix(
                    getFalloutRetailPoseAdapterLocal(mSnapshot.mNodes[found->second], getNodePath()));
                ++mApplied;
            }
            traverse(node);
        }

        std::size_t getApplied() const { return mApplied; }

    private:
        const FalloutRetailPoseSnapshot& mSnapshot;
        std::size_t mApplied = 0;
    };

    class FalloutRetailPoseSkeletonRefreshVisitor final : public osg::NodeVisitor
    {
    public:
        explicit FalloutRetailPoseSkeletonRefreshVisitor(unsigned int traversalNumber)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mTraversalNumber(traversalNumber)
        {
        }

        void apply(osg::Group& group) override
        {
            if (auto* skeleton = dynamic_cast<SceneUtil::Skeleton*>(&group))
            {
                skeleton->markBoneMatriceDirty();
                skeleton->updateBoneMatrices(mTraversalNumber);
                ++mSkeletons;
            }
            traverse(group);
        }

        std::size_t getSkeletons() const { return mSkeletons; }

    private:
        unsigned int mTraversalNumber = 0;
        std::size_t mSkeletons = 0;
    };

    class FalloutRetailPoseRigRefreshVisitor final : public osg::NodeVisitor
    {
    public:
        FalloutRetailPoseRigRefreshVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned int index = 0; index < geode.getNumDrawables(); ++index)
                refresh(geode.getDrawable(index));
            traverse(geode);
        }

        void apply(osg::Drawable& drawable) override { refresh(&drawable); }

        std::size_t getRefreshed() const { return mRefreshed; }
        std::size_t getVisited() const { return mVisited; }

    private:
        void refresh(osg::Drawable* drawable)
        {
            auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(drawable);
            if (rig == nullptr || !rig->isFalloutCharacterRig())
                return;
            ++mVisited;
            rig->forceNextUpdate();
            if (rig->refreshFalloutSkinningForCurrentPose())
                ++mRefreshed;
        }

        std::size_t mVisited = 0;
        std::size_t mRefreshed = 0;
    };

    class FalloutRetailPoseReplayCallback final : public osg::NodeCallback
    {
    public:
        FalloutRetailPoseReplayCallback(
            std::shared_ptr<const FalloutRetailPoseSnapshot> snapshot, int applyFrame, int auditFrame)
            : mSnapshot(std::move(snapshot))
            , mApplyFrame(applyFrame)
            , mAuditFrame(auditFrame)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            traverse(node, visitor);
            if (node == nullptr || visitor == nullptr || visitor->getFrameStamp() == nullptr)
                return;
            const unsigned int frame = visitor->getFrameStamp()->getFrameNumber();
            if (frame < static_cast<unsigned int>(std::max(0, mApplyFrame)))
                return;

            if (static_cast<int>(frame) == mAuditFrame)
                audit(*node, frame, "pre-replay");

            FalloutRetailPoseApplyVisitor applyVisitor(*mSnapshot);
            node->accept(applyVisitor);
            FalloutRetailPoseSkeletonRefreshVisitor skeletonRefresh(visitor->getTraversalNumber());
            node->accept(skeletonRefresh);
            FalloutRetailPoseRigRefreshVisitor rigRefresh;
            node->accept(rigRefresh);
            if (!mApplyLogged)
            {
                mApplyLogged = true;
                Log(Debug::Info) << "FNV/ESM4 retail pose replay: frame=" << frame << " snapshot=\""
                                 << mSnapshot->mPath << "\" expectedNodes="
                                 << getFalloutRetailPoseReplayNodeCount(*mSnapshot)
                                  << " appliedNodes=" << applyVisitor.getApplied()
                                  << " refreshedSkeletons=" << skeletonRefresh.getSkeletons()
                                  << " visitedRigs=" << rigRefresh.getVisited()
                                  << " refreshedRigs=" << rigRefresh.getRefreshed();
            }

            if (static_cast<int>(frame) == mAuditFrame)
                audit(*node, frame, "post-replay");
        }

    private:
        void audit(osg::Node& root, unsigned int frame, std::string_view stage)
        {
            FalloutRetailPoseAuditVisitor visitor(*mSnapshot);
            root.accept(visitor);
            FalloutRetailPoseAuditResult result = visitor.finish();
            const std::size_t expected = getFalloutRetailPoseReplayNodeCount(*mSnapshot);
            const std::size_t missing = result.mMatched < expected ? expected - result.mMatched : 0;
            const bool pass = missing == 0 && result.mDuplicates == 0 && result.mLocalMismatches == 0
                && result.mWorldMismatches == 0;
            Log(pass ? Debug::Info : Debug::Error)
                << "FNV/ESM4 retail pose audit: frame=" << frame << " stage=" << stage
                << " expectedNodes=" << expected << " matchedNodes=" << result.mMatched << " missingNodes="
                << missing << " duplicateNodes=" << result.mDuplicates << " localMismatchNodes="
                << result.mLocalMismatches << " worldMismatchNodes=" << result.mWorldMismatches
                << " status=" << (pass ? "pass" : "fail");
            for (const std::string& detail : result.mDetails)
                Log(Debug::Error) << "FNV/ESM4 retail pose mismatch: frame=" << frame << " stage=" << stage
                                  << " " << detail;
        }

        std::shared_ptr<const FalloutRetailPoseSnapshot> mSnapshot;
        int mApplyFrame = 0;
        int mAuditFrame = -1;
        bool mApplyLogged = false;
    };

    template <typename T>
    ESM::RefId findEsm4EditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& typedStore = store.get<T>();
        for (auto it = typedStore.begin(); it != typedStore.end(); ++it)
        {
            if (it->mEditorId == editorId)
                return ESM::RefId::formIdRefId(it->mId);
        }

        return ESM::RefId();
    }

    template <typename T>
    bool addFNVEditorItem(
        MWWorld::ContainerStore& inventory, const MWWorld::ESMStore& store, std::string_view editorId, int count)
    {
        const ESM::RefId id = findEsm4EditorId<T>(store, editorId);
        if (id.empty())
            return false;

        try
        {
            const int existing = inventory.count(id);
            const int missing = std::max(0, count - existing);
            if (missing > 0)
                inventory.add(id, missing, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter inventory " << (missing > 0 ? "added " : "retained ")
                             << editorId << " x" << count << " id=" << id.toDebugString()
                             << " previous=" << existing << " inserted=" << missing;
            return true;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter inventory failed " << editorId << " id="
                                << id.toDebugString() << ": " << e.what();
            return false;
        }
    }

    bool addProofInventoryItem(MWWorld::ContainerStore& inventory, std::string_view id, int count)
    {
        const ESM::RefId refId = ESM::RefId::stringRefId(id);
        try
        {
            const int existing = inventory.count(refId);
            const int missing = std::max(0, count - existing);
            if (missing > 0)
                inventory.add(refId, missing, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter proof inventory "
                             << (missing > 0 ? "added " : "retained ") << id << " x" << count
                             << " previous=" << existing << " inserted=" << missing;
            return true;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter proof inventory failed " << id << ": " << e.what();
            return false;
        }
    }

    template <typename T>
    bool isFNVEditorItemEquipped(
        MWWorld::InventoryStore& inventory, const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const ESM::RefId id = findEsm4EditorId<T>(store, editorId);
        return !id.empty() && inventory.isEquipped(id);
    }

    template <typename T>
    bool equipFNVEditorItem(const MWWorld::Ptr& player, MWWorld::InventoryStore& inventory,
        const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const ESM::RefId id = findEsm4EditorId<T>(store, editorId);
        if (id.empty())
            return false;

        try
        {
            const MWWorld::Ptr item = inventory.search(id);
            if (item.isEmpty())
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: starter equipment missing from inventory " << editorId
                                    << " id=" << id.toDebugString();
                return false;
            }

            MWWorld::ActionEquip(item, true).execute(player);
            const bool equipped = inventory.isEquipped(id);
            Log(equipped ? Debug::Info : Debug::Warning)
                << "FNV/ESM4 proof: starter equipment " << editorId << " id=" << id.toDebugString()
                << " equipped=" << equipped;
            return equipped;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter equipment failed " << editorId << " id="
                                << id.toDebugString() << ": " << e.what();
            return false;
        }
    }

    void applyFNVLevelOneCourierBootstrap(
        MWBase::World& world, MWBase::Journal& journal, bool moveOutsideDoc, bool applyProfile)
    {
        const int vcg01Stage = readProofInt("OPENMW_FNV_BOOTSTRAP_VCG01_STAGE", 200);
        const ESM::RefId vcg01 = ESM::RefId::stringRefId("VCG01");
        journal.setJournalIndex(vcg01, vcg01Stage);
        const float proofHour = readProofFloat("OPENMW_FNV_BOOTSTRAP_HOUR", 12.f);
        world.setGlobalFloat(MWWorld::Globals::sGameHour, proofHour);
        world.advanceTime(0.0, false);
        Log(Debug::Info) << "FNV/ESM4 proof: set quest state VCG01 stage=" << vcg01Stage
                         << " previousLoadSafe=explicit-bootstrap hour=" << proofHour;

        if (moveOutsideDoc)
        {
            ESM::Position outside;
            outside.pos[0] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_X", -67735.f);
            outside.pos[1] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Y", 3204.f);
            outside.pos[2] = readProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Z", 8425.f);
            outside.rot[0] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_X", 0.f);
            outside.rot[1] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Y", 0.f);
            outside.rot[2] = readProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Z", -0.6981317f);
            ESM::Position cellProbe = outside;
            ESM::RefId cellId = world.findExteriorPosition("Goodsprings", cellProbe);
            if (cellId.empty())
            {
                cellId = ESM::RefId::formIdRefId(ESM::FormId::fromUint32(0x010daeb9));
                Log(Debug::Warning) << "FNV/ESM4 proof: Goodsprings cell lookup failed; falling back to "
                                    << cellId.toDebugString();
            }
            else
            {
                Log(Debug::Info) << "FNV/ESM4 proof: resolved Goodsprings bootstrap cell " << cellId.toDebugString()
                                 << " preserving explicit proof pos=(" << outside.pos[0] << "," << outside.pos[1]
                                 << "," << outside.pos[2] << ")";
            }
            world.changeToCell(cellId, outside, false, true);
            MWWorld::Ptr player = world.getPlayerPtr();
            player = world.moveObject(player, outside.asVec3(), true, true);
            if (player.getCell() != nullptr)
                player.getCell()->setWaterLevel(-200000.f);
            world.rotateObject(player, osg::Vec3f(outside.rot[0], outside.rot[1], outside.rot[2]));
            if (MWRender::Camera* camera = world.getCamera())
            {
                const char* cameraMode = std::getenv("OPENMW_FNV_BOOTSTRAP_CAMERA_MODE");
                const bool forceFirstPerson
                    = cameraMode != nullptr && Misc::StringUtils::ciEqual(cameraMode, "firstperson");
                const float cameraDistance = readProofFloat("OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE", 0.f);
                camera->attachTo(player);
                camera->setMode(
                    forceFirstPerson ? MWRender::Camera::Mode::FirstPerson : MWRender::Camera::Mode::ThirdPerson,
                    true);
                camera->setPreferredCameraDistance(cameraDistance);
                camera->update(0.f, false);
                camera->setPitch(-outside.rot[0], true);
                camera->setYaw(-outside.rot[2], true);
                camera->setRoll(0.f);
                camera->updateCamera();
                const ESM::Position& actual = player.getRefData().getPosition();
                const osg::Vec3d cameraPos = camera->getPosition();
                Log(Debug::Info) << "FNV/ESM4 proof: reset player camera mode="
                                 << static_cast<int>(camera->getMode()) << " playerPos=("
                                 << actual.pos[0] << "," << actual.pos[1] << "," << actual.pos[2]
                                 << ") playerRot=(" << actual.rot[0] << "," << actual.rot[1] << ","
                                 << actual.rot[2] << ") cameraPos=(" << cameraPos.x() << "," << cameraPos.y()
                                 << "," << cameraPos.z() << ") cameraPitch=" << camera->getPitch()
                                 << " cameraYaw=" << camera->getYaw() << " cameraDistance=" << cameraDistance;
            }
            Log(Debug::Info) << "FNV/ESM4 proof: moved player outside after Doc handoff activeCell="
                             << world.getCellName(player.getCell()) << " pos=("
                             << outside.pos[0] << "," << outside.pos[1] << "," << outside.pos[2] << ") rotZ="
                             << outside.rot[2] << " rotX=" << outside.rot[0] << " rotY=" << outside.rot[1]
                             << " cell=" << cellId.toDebugString() << " waterLevel=-200000";
        }

        if (!applyProfile)
            return;

        MWWorld::Ptr player = world.getPlayerPtr();
        MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
        stats.setLevel(1);
        stats.setLevelProgress(0);
        stats.setBounty(0);
        stats.setReputation(0);
        stats.setBaseDisposition(50);

        const std::pair<ESM::RefId, float> attributes[] = {
            { ESM::Attribute::Strength, 60.f },
            { ESM::Attribute::Intelligence, 60.f },
            { ESM::Attribute::Willpower, 50.f },
            { ESM::Attribute::Agility, 60.f },
            { ESM::Attribute::Speed, 50.f },
            { ESM::Attribute::Endurance, 60.f },
            { ESM::Attribute::Personality, 50.f },
            { ESM::Attribute::Luck, 60.f },
        };
        for (const auto& [id, value] : attributes)
            stats.setAttribute(id, value);

        for (int i = 0; i < ESM::Skill::Length; ++i)
        {
            MWMechanics::SkillValue skill;
            skill.setBase(35.f);
            skill.setProgress(0.f);
            stats.setSkill(ESM::Skill::indexToRefId(i), skill);
        }

        stats.setHealth(MWMechanics::DynamicStat<float>(125.f, 125.f, 125.f));
        stats.setMagicka(MWMechanics::DynamicStat<float>(60.f, 60.f, 60.f));
        stats.setFatigue(MWMechanics::DynamicStat<float>(220.f, 220.f, 220.f));

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const MWWorld::ESMStore& store = world.getStore();
        int added = 0;
        const bool pistolAdded
            = addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNV9mmPistol", 1);
        const bool rifleAdded
            = addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNVVarmintRifle", 1);
        const bool ammo9mmAdded
            = addFNVEditorItem<ESM4::Ammunition>(inventory, store, "Ammo9mm", 60);
        const bool ammo556Added
            = addFNVEditorItem<ESM4::Ammunition>(inventory, store, "Ammo556mm", 60);
        const bool stimpakAdded = addFNVEditorItem<ESM4::Potion>(inventory, store, "Stimpak", 5);
        const bool bobbyPinAdded = addFNVEditorItem<ESM4::MiscItem>(inventory, store, "BobbyPin", 5);
        const bool capsAdded = addFNVEditorItem<ESM4::MiscItem>(inventory, store, "Caps001", 75);
        const bool outfitAdded = addFNVEditorItem<ESM4::Armor>(inventory, store, "VaultSuit21", 1);
        const bool headgearAdded = addFNVEditorItem<ESM4::Armor>(inventory, store, "CowboyHat02", 1);
        for (const bool present : { pistolAdded, rifleAdded, ammo9mmAdded, ammo556Added, stimpakAdded,
                 bobbyPinAdded, capsAdded, outfitAdded, headgearAdded })
        {
            added += present ? 1 : 0;
        }

        // Audit the loaded save before the idempotent equip pass below. On a fresh bootstrap these flags are
        // expected to be false; on a reload they prove the equipped InventoryStore slots survived serialization.
        const bool outfitPreviouslyEquipped
            = isFNVEditorItemEquipped<ESM4::Armor>(inventory, store, "VaultSuit21");
        const bool headgearPreviouslyEquipped
            = isFNVEditorItemEquipped<ESM4::Armor>(inventory, store, "CowboyHat02");
        const bool weaponPreviouslyEquipped
            = isFNVEditorItemEquipped<ESM4::Weapon>(inventory, store, "WeapNVVarmintRifle");
        const bool ammunitionPreviouslyEquipped
            = isFNVEditorItemEquipped<ESM4::Ammunition>(inventory, store, "Ammo556mm");

        const bool outfitEquipped
            = outfitAdded && equipFNVEditorItem<ESM4::Armor>(player, inventory, store, "VaultSuit21");
        const bool headgearEquipped
            = headgearAdded && equipFNVEditorItem<ESM4::Armor>(player, inventory, store, "CowboyHat02");
        const bool weaponEquipped
            = rifleAdded && equipFNVEditorItem<ESM4::Weapon>(player, inventory, store, "WeapNVVarmintRifle");
        const bool ammunitionEquipped
            = ammo556Added && equipFNVEditorItem<ESM4::Ammunition>(player, inventory, store, "Ammo556mm");
        player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);

        // Fallback records keep the save usable if a partial content stack omits one of the core retail records.
        // Never add both representations: a complete FalloutNV.esm load receives only the authored items above.
        int proofAdded = 0;
        if (!pistolAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_PISTOL", 1) ? 1 : 0;
        if (!rifleAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_VARMINT_RIFLE", 1) ? 1 : 0;
        if (!ammo9mmAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_AMMO", 60) ? 1 : 0;
        if (!stimpakAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_STIMPAK", 5) ? 1 : 0;
        if (!bobbyPinAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_BOBBY_PIN", 5) ? 1 : 0;
        if (!capsAdded)
            proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_CAPS", 75) ? 1 : 0;

        Log(Debug::Info) << "FNV/ESM4 proof: level-1 Courier profile applied level=" << stats.getLevel()
                         << " attributes=8 skills=" << ESM::Skill::Length << " starterItemKinds=" << added
                         << " proofStarterItemKinds=" << proofAdded
                         << " visualOutfit=VaultSuit21 visualHeadgear=CowboyHat02"
                         << " visualWeapon=WeapNVVarmintRifle"
                         << " inventoryCounts={VaultSuit21:" << inventory.count(findEsm4EditorId<ESM4::Armor>(
                                store, "VaultSuit21"))
                         << ",CowboyHat02:" << inventory.count(findEsm4EditorId<ESM4::Armor>(store, "CowboyHat02"))
                         << ",WeapNV9mmPistol:"
                         << inventory.count(findEsm4EditorId<ESM4::Weapon>(store, "WeapNV9mmPistol"))
                         << ",WeapNVVarmintRifle:"
                         << inventory.count(findEsm4EditorId<ESM4::Weapon>(store, "WeapNVVarmintRifle"))
                         << ",Ammo9mm:"
                         << inventory.count(findEsm4EditorId<ESM4::Ammunition>(store, "Ammo9mm"))
                         << ",Ammo556mm:"
                          << inventory.count(findEsm4EditorId<ESM4::Ammunition>(store, "Ammo556mm")) << "}"
                          << " preexistingEquipped={outfit:" << outfitPreviouslyEquipped
                          << ",headgear:" << headgearPreviouslyEquipped << ",weapon:"
                          << weaponPreviouslyEquipped << ",ammunition:" << ammunitionPreviouslyEquipped << "}"
                          << " equipped={outfit:" << outfitEquipped << ",headgear:" << headgearEquipped
                         << ",weapon:" << weaponEquipped << ",ammunition:" << ammunitionEquipped << "}"
                         << " status="
                         << (outfitEquipped && headgearEquipped && weaponEquipped && ammunitionEquipped ? "pass"
                                                                                                     : "fail");
    }

    void resetFNVProofCamera(MWBase::World& world)
    {
        MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return;

        ESM::Position pos = player.getRefData().getPosition();
        pos.rot[0] = readProofFloat("OPENMW_FNV_PROOF_CAMERA_PLAYER_ROT_X", pos.rot[0]);
        pos.rot[1] = readProofFloat("OPENMW_FNV_PROOF_CAMERA_PLAYER_ROT_Y", pos.rot[1]);
        pos.rot[2] = readProofFloat("OPENMW_FNV_PROOF_CAMERA_PLAYER_ROT_Z", -0.6981317f);
        world.rotateObject(player, osg::Vec3f(pos.rot[0], pos.rot[1], pos.rot[2]));

        if (MWRender::Camera* camera = world.getCamera())
        {
            const char* cameraMode = std::getenv("OPENMW_FNV_PROOF_CAMERA_MODE");
            const bool firstPerson = cameraMode == nullptr || *cameraMode == '\0'
                || Misc::StringUtils::ciEqual(cameraMode, "firstperson");
            const float cameraDistance = readProofFloat("OPENMW_FNV_PROOF_CAMERA_DISTANCE", 0.f);
            const float cameraPitch = readProofFloat("OPENMW_FNV_PROOF_CAMERA_PITCH", 0.45f);
            const float cameraYaw = readProofFloat("OPENMW_FNV_PROOF_CAMERA_YAW", -pos.rot[2]);

            camera->attachTo(player);
            camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
            camera->setPreferredCameraDistance(readProofFloat("OPENMW_FNV_PROOF_CAMERA_NUDGE_DISTANCE", 128.f));
            camera->processViewChange();
            camera->update(0.f, false);
            camera->instantTransition();
            camera->setMode(firstPerson ? MWRender::Camera::Mode::FirstPerson : MWRender::Camera::Mode::ThirdPerson,
                true);
            camera->setPreferredCameraDistance(cameraDistance);
            camera->processViewChange();
            camera->update(0.f, false);
            camera->setPitch(cameraPitch, true);
            camera->setYaw(cameraYaw, true);
            camera->setRoll(0.f);
            camera->instantTransition();
            camera->update(0.f, false);
            camera->updateCamera();

            const osg::Vec3d cameraPos = camera->getPosition();
            const ESM::Position& actual = player.getRefData().getPosition();
            Log(Debug::Info) << "FNV/ESM4 proof: reset real-start camera mode=" << static_cast<int>(camera->getMode())
                             << " playerPos=(" << actual.pos[0] << "," << actual.pos[1] << "," << actual.pos[2]
                             << ") playerRot=(" << actual.rot[0] << "," << actual.rot[1] << "," << actual.rot[2]
                             << ") cameraPos=(" << cameraPos.x() << "," << cameraPos.y() << "," << cameraPos.z()
                             << ") cameraPitch=" << camera->getPitch() << " cameraYaw=" << camera->getYaw()
                             << " cameraDistance=" << cameraDistance;
        }
    }

    void settleFNVFlatStartupCamera(MWBase::World& world)
    {
        if (VR::getVR())
            return;

        MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return;

        MWRender::Camera* camera = world.getCamera();
        if (camera == nullptr)
            return;

        const ESM::Position& pos = player.getRefData().getPosition();
        const float cameraPitch = 0.20f;
        const float cameraYaw = -pos.rot[2];

        camera->attachTo(player);
        camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
        camera->setPreferredCameraDistance(128.f);
        camera->processViewChange();
        camera->update(0.f, false);
        camera->instantTransition();
        camera->setMode(MWRender::Camera::Mode::FirstPerson, true);
        camera->setPreferredCameraDistance(0.f);
        camera->processViewChange();
        camera->update(0.f, false);
        camera->setPitch(cameraPitch, true);
        camera->setYaw(cameraYaw, true);
        camera->setRoll(0.f);
        camera->instantTransition();
        camera->update(0.f, false);
        camera->updateCamera();

        const osg::Vec3d cameraPos = camera->getPosition();
        Log(Debug::Verbose) << "FNV/ESM4 diag: settled flat startup camera via zoom-cycle equivalent"
                         << " mode=" << static_cast<int>(camera->getMode()) << " playerPos=(" << pos.pos[0] << ","
                         << pos.pos[1] << "," << pos.pos[2] << ") playerRotZ=" << pos.rot[2] << " cameraPos=("
                         << cameraPos.x() << "," << cameraPos.y() << "," << cameraPos.z()
                         << ") cameraPitch=" << camera->getPitch() << " cameraYaw=" << camera->getYaw();
    }

    bool viewerTelemetryEnabled(const char* name)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return false;

        return !Misc::StringUtils::ciEqual(value, "0") && !Misc::StringUtils::ciEqual(value, "false")
            && !Misc::StringUtils::ciEqual(value, "off") && !Misc::StringUtils::ciEqual(value, "no");
    }

    bool worldViewerTraceEnabled()
    {
        return viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_TRACE")
            || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_TELEMETRY");
    }

    void worldViewerTrace(unsigned int frameNumber, std::string_view phase)
    {
        if (!worldViewerTraceEnabled())
            return;

        Log(Debug::Info) << "World viewer trace: frame=" << frameNumber << " phase=\"" << phase << "\"";
        std::fflush(stdout);
        std::fflush(stderr);
    }

    std::string cleanWorldViewerTelemetryString(std::string value)
    {
        for (char& c : value)
        {
            if (c == '"')
                c = '\'';
            else if (static_cast<unsigned char>(c) < 32)
                c = ' ';
        }
        return value;
    }

    std::string worldViewerOsgObjectType(const osg::Object& object)
    {
        std::string type = object.libraryName();
        if (!type.empty())
            type += "::";
        type += object.className();
        return cleanWorldViewerTelemetryString(type);
    }

    std::string worldViewerCallbackType(const osg::Callback& callback)
    {
        std::string type = worldViewerOsgObjectType(callback);
        if (type == "osg::Callback")
            type = typeid(callback).name();
        return cleanWorldViewerTelemetryString(type);
    }

    std::string worldViewerNodePathText(const osg::NodePath& path)
    {
        if (path.empty())
            return "<none>";

        std::string out;
        const std::size_t start = path.size() > 8 ? path.size() - 8 : 0;
        if (start > 0)
            out = ".../";

        for (std::size_t i = start; i < path.size(); ++i)
        {
            if (i != start)
                out += "/";

            const osg::Node* node = path[i];
            if (node == nullptr)
            {
                out += "<null>";
                continue;
            }

            std::string name = node->getName();
            if (name.empty())
                name = worldViewerOsgObjectType(*node);
            out += cleanWorldViewerTelemetryString(name);
        }

        return out;
    }

    std::string lowerWorldViewerText(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::vector<std::string> readWorldViewerFilterList(const char* name)
    {
        std::vector<std::string> out;
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
            return out;

        std::string current;
        for (const char c : std::string_view(value))
        {
            if (c == ';' || c == ',' || c == '|')
            {
                if (!current.empty())
                    out.push_back(lowerWorldViewerText(current));
                current.clear();
                continue;
            }
            if (!std::isspace(static_cast<unsigned char>(c)))
                current += c;
        }
        if (!current.empty())
            out.push_back(lowerWorldViewerText(current));

        return out;
    }

    class WorldViewerOsgUpdateCallbackAuditVisitor : public osg::NodeVisitor
    {
    public:
        WorldViewerOsgUpdateCallbackAuditVisitor(
            unsigned int frameNumber, bool stripAllNodeCallbacks, bool stripAllStateSetCallbacks,
            std::vector<std::string> stripClassFilters, std::vector<std::string> keepPathFilters, int logLimit)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mFrameNumber(frameNumber)
            , mStripAllNodeCallbacks(stripAllNodeCallbacks)
            , mStripAllStateSetCallbacks(stripAllStateSetCallbacks)
            , mStripClassFilters(std::move(stripClassFilters))
            , mKeepPathFilters(std::move(keepPathFilters))
            , mLogLimit(std::max(0, logLimit))
        {
        }

        void apply(osg::Node& node) override
        {
            ++mNodesVisited;
            auditNode(node);
            traverse(node);
        }

        void report(std::string_view rootName) const
        {
            Log(Debug::Info) << "World viewer osg-update-callback-summary: frame=" << mFrameNumber << " root=\""
                             << rootName << "\" nodesVisited=" << mNodesVisited
                             << " nodeOwners=" << mNodeOwnersSeen << " nodeCallbacks=" << mNodeCallbacksSeen
                             << " nodeOwnersStripped=" << mNodeOwnersStripped
                             << " stateSetOwners=" << mStateSetOwnersSeen
                             << " stateSetCallbacks=" << mStateSetCallbacksSeen
                             << " stateSetOwnersStripped=" << mStateSetOwnersStripped
                             << " logged=" << mLogged;
        }

    private:
        void auditNode(osg::Node& node)
        {
            const std::string pathText = worldViewerNodePathText(getNodePath());
            osg::Callback* nodeCallback = node.getUpdateCallback();
            if (nodeCallback != nullptr)
            {
                const bool stripNodeCallbacks = shouldStripCallbackChain(nodeCallback, mStripAllNodeCallbacks, pathText);
                ++mNodeOwnersSeen;
                mNodeCallbacksSeen += auditCallbackChain("node", node, nodeCallback, stripNodeCallbacks, pathText);
                if (stripNodeCallbacks)
                {
                    node.setUpdateCallback(nullptr);
                    ++mNodeOwnersStripped;
                }
            }

            osg::StateSet* stateSet = node.getStateSet();
            if (stateSet == nullptr || stateSet->getUpdateCallback() == nullptr)
                return;

            const bool stripStateSetCallbacks
                = shouldStripCallbackChain(stateSet->getUpdateCallback(), mStripAllStateSetCallbacks, pathText);
            ++mStateSetOwnersSeen;
            mStateSetCallbacksSeen += auditCallbackChain("stateset", node, stateSet->getUpdateCallback(),
                stripStateSetCallbacks, pathText);
            if (stripStateSetCallbacks)
            {
                stateSet->setUpdateCallback(nullptr);
                ++mStateSetOwnersStripped;
            }
        }

        bool shouldStripCallbackChain(osg::Callback* callback, bool stripAll, std::string_view pathText) const
        {
            const std::string lowerPath = lowerWorldViewerText(std::string(pathText));
            for (const std::string& keep : mKeepPathFilters)
            {
                if (!keep.empty() && lowerPath.find(keep) != std::string::npos)
                    return false;
            }

            if (stripAll)
                return true;

            if (mStripClassFilters.empty())
                return false;

            for (osg::Callback* cb = callback; cb != nullptr; cb = cb->getNestedCallback())
            {
                const std::string callbackType = lowerWorldViewerText(worldViewerCallbackType(*cb));
                for (const std::string& filter : mStripClassFilters)
                {
                    if (!filter.empty() && callbackType.find(filter) != std::string::npos)
                        return true;
                }
            }

            return false;
        }

        int auditCallbackChain(
            const char* owner, osg::Node& node, osg::Callback* callback, bool stripOwner, std::string_view pathText)
        {
            int count = 0;
            for (osg::Callback* cb = callback; cb != nullptr; cb = cb->getNestedCallback())
            {
                if (mLogged < mLogLimit)
                {
                    Log(Debug::Info) << "World viewer osg-update-callback: frame=" << mFrameNumber << " owner=\""
                                     << owner << "\" action=\"" << (stripOwner ? "strip" : "audit")
                                     << "\" link=" << count << " nodeClass=\"" << worldViewerOsgObjectType(node)
                                     << "\" nodeName=\"" << cleanWorldViewerTelemetryString(node.getName())
                                     << "\" callbackClass=\"" << worldViewerCallbackType(*cb) << "\" path=\""
                                     << pathText << "\"";
                    ++mLogged;
                }
                ++count;
            }
            return count;
        }

        unsigned int mFrameNumber = 0;
        bool mStripAllNodeCallbacks = false;
        bool mStripAllStateSetCallbacks = false;
        std::vector<std::string> mStripClassFilters;
        std::vector<std::string> mKeepPathFilters;
        int mLogLimit = 0;
        int mLogged = 0;
        int mNodesVisited = 0;
        int mNodeOwnersSeen = 0;
        int mNodeCallbacksSeen = 0;
        int mNodeOwnersStripped = 0;
        int mStateSetOwnersSeen = 0;
        int mStateSetCallbacksSeen = 0;
        int mStateSetOwnersStripped = 0;
    };

    bool worldViewerOsgUpdateCallbackAuditRequested()
    {
        return viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_AUDIT_OSG_UPDATE_CALLBACKS")
            || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_UPDATE_CALLBACKS")
            || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_NODE_UPDATE_CALLBACKS")
            || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_STATESET_UPDATE_CALLBACKS")
            || std::getenv("OPENMW_WORLD_VIEWER_STRIP_OSG_UPDATE_CALLBACK_CLASS_FILTER") != nullptr
            || std::getenv("OPENMW_WORLD_VIEWER_KEEP_OSG_UPDATE_CALLBACK_PATH_FILTER") != nullptr;
    }

    void auditWorldViewerOsgUpdateCallbacks(osg::Node* sceneData, unsigned int frameNumber)
    {
        if (!worldViewerOsgUpdateCallbackAuditRequested())
            return;

        if (sceneData == nullptr)
        {
            Log(Debug::Info) << "World viewer osg-update-callback-summary: frame=" << frameNumber
                             << " root=\"scene\" nodesVisited=0 nodeOwners=0 nodeCallbacks=0"
                             << " nodeOwnersStripped=0 stateSetOwners=0 stateSetCallbacks=0"
                             << " stateSetOwnersStripped=0 logged=0 reason=\"missing scene data\"";
            return;
        }

        const bool stripAll = viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_UPDATE_CALLBACKS");
        const bool stripNodeCallbacks
            = stripAll || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_NODE_UPDATE_CALLBACKS");
        const bool stripStateSetCallbacks
            = stripAll || viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_STRIP_OSG_STATESET_UPDATE_CALLBACKS");
        std::vector<std::string> stripClassFilters
            = readWorldViewerFilterList("OPENMW_WORLD_VIEWER_STRIP_OSG_UPDATE_CALLBACK_CLASS_FILTER");
        std::vector<std::string> keepPathFilters
            = readWorldViewerFilterList("OPENMW_WORLD_VIEWER_KEEP_OSG_UPDATE_CALLBACK_PATH_FILTER");

        static bool loggedExamples = false;
        const bool logEveryFrame = viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_AUDIT_OSG_UPDATE_CALLBACKS_EVERY_FRAME");
        const int logLimit = (!loggedExamples || logEveryFrame)
            ? readProofInt("OPENMW_WORLD_VIEWER_OSG_UPDATE_CALLBACK_AUDIT_LIMIT", 120)
            : 0;

        WorldViewerOsgUpdateCallbackAuditVisitor visitor(
            frameNumber, stripNodeCallbacks, stripStateSetCallbacks, std::move(stripClassFilters),
            std::move(keepPathFilters), logLimit);
        sceneData->accept(visitor);
        visitor.report("scene");

        if (logLimit > 0)
            loggedExamples = true;

        std::fflush(stdout);
        std::fflush(stderr);
    }

    std::string safeWorldViewerPtrText(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return "<none>";

        try
        {
            return ptr.toString();
        }
        catch (const std::exception& e)
        {
            return std::string("<ptr failed: ") + e.what() + ">";
        }
    }

    std::string safeWorldViewerPtrBase(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return "<none>";

        try
        {
            return ptr.getCellRef().getRefId().toDebugString();
        }
        catch (const std::exception& e)
        {
            return std::string("<base failed: ") + e.what() + ">";
        }
    }

    std::string safeWorldViewerPtrType(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return "<none>";

        try
        {
            return std::string(ptr.getTypeDescription());
        }
        catch (const std::exception& e)
        {
            return std::string("<type failed: ") + e.what() + ">";
        }
    }

    std::string safeWorldViewerPtrName(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return "<none>";

        try
        {
            return std::string(ptr.getClass().getName(ptr));
        }
        catch (const std::exception& e)
        {
            return std::string("<name failed: ") + e.what() + ">";
        }
    }

    bool isWorldViewerActorPtr(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return false;

        try
        {
            return ptr.getClass().isActor();
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void logWorldViewerRayResult(const char* kind, unsigned frameNumber, const osg::Vec3f& from,
        const osg::Vec3f& to, const MWPhysics::RayCastingResult& result)
    {
        const float distance = result.mHit ? (result.mHitPos - from).length() : (to - from).length();
        const MWWorld::Ptr& hitObject = result.mHitObject;

        Log(Debug::Info) << "World viewer ray: kind=" << kind << " frame=" << frameNumber << " hit=" << result.mHit
                         << " distance=" << distance << " actorHit=" << isWorldViewerActorPtr(hitObject)
                         << " from=(" << from.x() << "," << from.y() << "," << from.z() << ") to=(" << to.x()
                         << "," << to.y() << "," << to.z() << ") hitPos=(" << result.mHitPos.x() << ","
                         << result.mHitPos.y() << "," << result.mHitPos.z() << ") hitNormal=("
                         << result.mHitNormal.x() << "," << result.mHitNormal.y() << "," << result.mHitNormal.z()
                         << ") hitBase=" << safeWorldViewerPtrBase(hitObject)
                         << " hitType=\"" << safeWorldViewerPtrType(hitObject) << "\" hitName=\""
                         << safeWorldViewerPtrName(hitObject) << "\" hitPtr=" << safeWorldViewerPtrText(hitObject);
    }

    void logWorldViewerTelemetry(MWWorld::World& world, osgViewer::Viewer& viewer, unsigned frameNumber, int state,
        bool loadingGui, bool worldReady, int worldReadyFrames)
    {
        MWWorld::Ptr player;
        try
        {
            player = world.getPlayerPtr();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "World viewer telemetry: player lookup failed: " << e.what();
        }

        MWWorld::CellStore* playerCell = !player.isEmpty() && player.isInCell() ? player.getCell() : nullptr;
        const bool playerExterior = playerCell != nullptr && playerCell->isExterior();
        ESM::Position playerPos = {};
        if (!player.isEmpty())
            playerPos = player.getRefData().getPosition();

        MWRender::Camera* camera = world.getCamera();
        const osg::Vec3d cameraPos = camera != nullptr ? camera->getPosition() : osg::Vec3d();
        const osg::Vec3f cameraPosF(cameraPos.x(), cameraPos.y(), cameraPos.z());
        const osg::Vec3f playerPosF(playerPos.pos[0], playerPos.pos[1], playerPos.pos[2]);

        Log(Debug::Info) << "World viewer telemetry: frame=" << frameNumber << " state=" << state
                         << " loadingGui=" << loadingGui << " worldReady=" << worldReady
                         << " readyFrames=" << worldReadyFrames
                         << " activeCells=" << world.getWorldScene().getActiveCells().size()
                         << " hour=" << world.getTimeStamp().getHour()
                         << " weatherId=" << world.getCurrentWeatherScriptId()
                         << " weatherTransition=" << world.getWeatherTransition()
                         << " playerCell=\"" << (playerCell != nullptr ? world.getCellName(playerCell) : "<none>")
                         << "\" exterior=" << playerExterior << " grid=("
                         << (playerCell != nullptr && playerExterior ? playerCell->getCell()->getGridX() : 0)
                         << ","
                         << (playerCell != nullptr && playerExterior ? playerCell->getCell()->getGridY() : 0)
                         << ") worldspace="
                         << (playerCell != nullptr ? playerCell->getCell()->getWorldSpace().toDebugString() : "<none>")
                         << " playerPos=(" << playerPos.pos[0] << "," << playerPos.pos[1] << ","
                         << playerPos.pos[2] << ") playerRot=(" << playerPos.rot[0] << "," << playerPos.rot[1]
                         << "," << playerPos.rot[2] << ") cameraMode="
                         << (camera != nullptr ? static_cast<int>(camera->getMode()) : -1)
                         << " cameraPos=(" << cameraPos.x() << "," << cameraPos.y() << "," << cameraPos.z()
                         << ") cameraPitch=" << (camera != nullptr ? camera->getPitch() : 0.f)
                         << " cameraYaw=" << (camera != nullptr ? camera->getYaw() : 0.f)
                         << " cullMask=0x" << std::hex << viewer.getCamera()->getCullMask() << std::dec;

        if (viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_RAY_TELEMETRY"))
        {
            const float rayMaxDistance = static_cast<float>(readProofInt("OPENMW_WORLD_VIEWER_RAY_DISTANCE", 200000));
            const osg::Vec3f rayUpOffset(0.f, 0.f, 512.f);
            const osg::Vec3f rayDown(0.f, 0.f, -rayMaxDistance);
            const MWPhysics::RayCastingInterface* rayCasting = world.getRayCasting();
            const int groundMask = MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                | MWPhysics::CollisionType_Door | MWPhysics::CollisionType_Water;

            if (rayCasting != nullptr)
            {
                const osg::Vec3f playerGroundFrom = playerPosF + rayUpOffset;
                const osg::Vec3f playerGroundTo = playerGroundFrom + rayDown;
                logWorldViewerRayResult("playerGround", frameNumber, playerGroundFrom, playerGroundTo,
                    rayCasting->castRay(playerGroundFrom, playerGroundTo, groundMask));

                const osg::Vec3f cameraGroundFrom = cameraPosF + rayUpOffset;
                const osg::Vec3f cameraGroundTo = cameraGroundFrom + rayDown;
                logWorldViewerRayResult("cameraGround", frameNumber, cameraGroundFrom, cameraGroundTo,
                    rayCasting->castRay(cameraGroundFrom, cameraGroundTo, groundMask));
            }

            if (camera != nullptr)
            {
                osg::Vec3f forward = camera->getOrient() * osg::Vec3f(0.f, 1.f, 0.f);
                forward.normalize();
                const osg::Vec3f centerTo = cameraPosF + (forward * rayMaxDistance);
                MWPhysics::RayCastingResult centerRay {};
                world.castRenderingRay(centerRay, cameraPosF, centerTo, true, false, std::span<const MWWorld::Ptr> {});
                logWorldViewerRayResult("cameraCenterRender", frameNumber, cameraPosF, centerTo, centerRay);
            }
        }

        if (!viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_REF_TELEMETRY"))
            return;

        const int refLimit = readProofInt("OPENMW_WORLD_VIEWER_REF_TELEMETRY_LIMIT", 200);
        const bool unlimitedRefs = refLimit == 0;
        int refsLogged = 0;
        int refsTotal = 0;
        int actorRaysLogged = 0;
        const int actorRayLimit = readProofInt("OPENMW_WORLD_VIEWER_ACTOR_RAY_LIMIT", 8);

        for (MWWorld::CellStore* cellstore : world.getWorldScene().getActiveCells())
        {
            if (cellstore == nullptr)
                continue;

            int refs = 0;
            int enabled = 0;
            int rendered = 0;
            int actors = 0;
            int renderedActors = 0;
            int doors = 0;
            int deleted = 0;
            int modelFailures = 0;
            int nameFailures = 0;

            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                ++refs;
                ++refsTotal;
                const bool isDeleted = ptr.mRef != nullptr && ptr.mRef->isDeleted();
                const bool isEnabled = ptr.getRefData().isEnabled();
                const bool hasNode = ptr.getRefData().getBaseNode() != nullptr;
                const bool isActor = ptr.getClass().isActor();
                const bool isDoor = ptr.getClass().isDoor();
                if (isDeleted)
                    ++deleted;
                if (isEnabled)
                    ++enabled;
                if (hasNode)
                    ++rendered;
                if (isActor)
                    ++actors;
                if (isActor && hasNode)
                    ++renderedActors;
                if (isDoor)
                    ++doors;

                std::string name;
                try
                {
                    name = std::string(ptr.getClass().getName(ptr));
                }
                catch (const std::exception& e)
                {
                    ++nameFailures;
                    name = std::string("<name failed: ") + e.what() + ">";
                }

                std::string model;
                try
                {
                    model = ptr.getClass().getCorrectedModel(ptr).value();
                }
                catch (const std::exception& e)
                {
                    ++modelFailures;
                    model = std::string("<model failed: ") + e.what() + ">";
                }

                if (unlimitedRefs || refsLogged < refLimit)
                {
                    const ESM::Position& pos = ptr.getRefData().getPosition();
                    unsigned int nodeMask = 0;
                    osg::Vec3d scenePos;
                    if (ptr.getRefData().getBaseNode())
                    {
                        nodeMask = ptr.getRefData().getBaseNode()->getNodeMask();
                        scenePos = ptr.getRefData().getBaseNode()->getPosition();
                    }

                    Log(Debug::Info) << "World viewer ref: cell=\"" << world.getCellName(cellstore)
                                     << "\" ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                     << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                     << " type=" << ptr.getTypeDescription() << " enabled=" << isEnabled
                                     << " deleted=" << isDeleted << " rendered=" << hasNode << " actor="
                                     << isActor << " door=" << isDoor << " name=\"" << name << "\" model=\""
                                     << model << "\" pos=(" << pos.pos[0] << "," << pos.pos[1] << ","
                                     << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1] << ","
                                     << pos.rot[2] << ") scenePos=(" << scenePos.x() << "," << scenePos.y()
                                     << "," << scenePos.z() << ") nodeMask=0x" << std::hex << nodeMask << std::dec
                                     << " ptr=" << ptr.toString();
                    ++refsLogged;
                }

                if (viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_RAY_TELEMETRY") && isActor && hasNode
                    && actorRaysLogged < actorRayLimit)
                {
                    const ESM::Position& pos = ptr.getRefData().getPosition();
                    osg::Vec3f actorCenter(pos.pos[0], pos.pos[1], pos.pos[2] + 64.f);
                    if (ptr.getRefData().getBaseNode())
                    {
                        const osg::Vec3d nodePos = ptr.getRefData().getBaseNode()->getPosition();
                        actorCenter = osg::Vec3f(nodePos.x(), nodePos.y(), nodePos.z() + 64.f);
                    }

                    if (camera != nullptr)
                    {
                        MWPhysics::RayCastingResult cameraActorRay {};
                        world.castRenderingRay(
                            cameraActorRay, cameraPosF, actorCenter, true, false, std::span<const MWWorld::Ptr> {});
                        logWorldViewerRayResult("cameraActorRender", frameNumber, cameraPosF, actorCenter,
                            cameraActorRay);
                    }

                    const MWPhysics::RayCastingInterface* rayCasting = world.getRayCasting();
                    if (rayCasting != nullptr)
                    {
                        const osg::Vec3f actorCrossFrom = actorCenter + osg::Vec3f(0.f, -128.f, 0.f);
                        const osg::Vec3f actorCrossTo = actorCenter + osg::Vec3f(0.f, 128.f, 0.f);
                        std::vector<MWWorld::Ptr> targetActor { ptr };
                        logWorldViewerRayResult("actorCrossPhysics", frameNumber, actorCrossFrom, actorCrossTo,
                            rayCasting->castRay(actorCrossFrom, actorCrossTo, {}, targetActor,
                                MWPhysics::CollisionType_Actor));
                    }

                    ++actorRaysLogged;
                }
                return true;
            });

            Log(Debug::Info) << "World viewer cell: name=\"" << world.getCellName(cellstore) << "\" exterior="
                             << cellstore->isExterior() << " grid=("
                             << (cellstore->isExterior() ? cellstore->getCell()->getGridX() : 0) << ","
                             << (cellstore->isExterior() ? cellstore->getCell()->getGridY() : 0) << ") worldspace="
                             << cellstore->getCell()->getWorldSpace().toDebugString() << " refs=" << refs
                             << " enabled=" << enabled << " rendered=" << rendered
                             << " missingRenderNode=" << (enabled - rendered) << " actors=" << actors
                             << " renderedActors=" << renderedActors << " doors=" << doors << " deleted="
                             << deleted << " nameFailures=" << nameFailures << " modelFailures=" << modelFailures;
        }

        if (!unlimitedRefs && refsTotal > refsLogged)
        {
            Log(Debug::Info) << "World viewer telemetry: ref dump truncated refsLogged=" << refsLogged
                             << " refsTotal=" << refsTotal << " limit=" << refLimit;
        }
    }
    // ## VR_PATCH BEGIN

    class InitializeVrOperation : public osg::GraphicsOperation
    {
    public:
        InitializeVrOperation(OMW::Engine* engine)
            : GraphicsOperation("InitializeVrOperation", false)
            , mEngine(engine)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override { mEngine->configureVRGraphics(graphicsContext); }

        OMW::Engine* mEngine;
    };
    // ## VR_PATCH END
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const auto getProofFrame = [](const char* name) {
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return -1;
        char* end = nullptr;
        const long value = std::strtol(env, &end, 10);
        if (end == env || value < 0)
            return -1;
        return static_cast<int>(value);
    };
    const auto getProofFrames = [](const char* name) {
        std::vector<int> frames;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return frames;

        std::string_view value(env);
        while (!value.empty())
        {
            const std::size_t comma = value.find(',');
            std::string_view token = value.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                int frame = -1;
                const auto result = std::from_chars(token.data(), token.data() + token.size(), frame);
                if (result.ec == std::errc() && result.ptr == token.data() + token.size() && frame >= 0)
                    frames.push_back(frame);
            }
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }

        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        return frames;
    };
    const auto getProofFloats = [](const char* name) {
        std::vector<float> values;
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return values;

        std::string_view value(env);
        while (!value.empty())
        {
            const std::size_t comma = value.find(',');
            std::string_view token = value.substr(0, comma);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);
            if (!token.empty())
            {
                const std::string tokenText(token);
                char* end = nullptr;
                const float parsed = std::strtof(tokenText.c_str(), &end);
                if (end != tokenText.c_str() && *end == '\0')
                    values.push_back(parsed);
            }
            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }

        return values;
    };
    static std::vector<int> proofScreenshotFrames = getProofFrames("OPENMW_PROOF_SCREENSHOT_FRAME");
    static const std::vector<WorldViewerTimeKeyframe> worldViewerTimeSequence
        = getWorldViewerTimeSequence();
    static const std::vector<WorldViewerCameraAngleKeyframe> worldViewerCameraAngleSequence
        = getWorldViewerCameraAngleSequence();
    static const std::vector<float> proofActorViewOrbitDegrees
        = getProofFloats("OPENMW_PROOF_ACTOR_VIEW_ORBIT_DEGREES");
    static const std::vector<float> proofActorViewFrontDistances
        = getProofFloats("OPENMW_PROOF_ACTOR_VIEW_FRONT_DISTANCES");
    static const int proofScreenshotReadyFrames = getProofFrame("OPENMW_PROOF_SCREENSHOT_READY_FRAMES");
    static const int proofInventoryFrame = getProofFrame("OPENMW_PROOF_INVENTORY_FRAME");
    static const std::vector<int> proofInventoryPaneFrames = getProofFrames("OPENMW_PROOF_INVENTORY_PANE_FRAME");
    static const std::vector<int> proofInventoryPaneIndices = getProofFrames("OPENMW_PROOF_INVENTORY_PANE_INDEX");
    static const int proofQuickSaveFrame = getProofFrame("OPENMW_PROOF_QUICKSAVE_FRAME");
    static const int proofSayFrame = getProofFrame("OPENMW_PROOF_SAY_FRAME");
    static std::vector<std::string> proofActorBatchTargets
        = readWorldViewerStringList("OPENMW_PROOF_SAY_ACTORS");
    static const std::vector<std::string> proofActorPoseGroups
        = readWorldViewerStringList("OPENMW_PROOF_ACTOR_POSE_GROUPS");
    static const bool proofActorPoseAllAvailable
        = proofEnvEnabled("OPENMW_PROOF_ACTOR_POSE_ALL_AVAILABLE");
    static const bool proofActorPoseSweepEnabled
        = proofActorPoseAllAvailable || !proofActorPoseGroups.empty();
    static std::vector<std::string> proofActorActivePoseGroups = proofActorPoseGroups;
    static const std::vector<std::string> proofSidecarActionIds
        = readWorldViewerStringList("OPENMW_FNV_SIDECAR_ACTION_IDS");
    static FNVSidecar::Client proofSidecarClient;
    const bool proofSidecarEnabled = proofSidecarClient.enabled();
    const FNVSidecar::Snapshot proofSidecarSnapshot
        = proofSidecarEnabled ? proofSidecarClient.snapshot() : FNVSidecar::Snapshot{};
    static const int proofTimedScript1Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_1_FRAME");
    static const int proofTimedScript2Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_2_FRAME");
    static std::size_t proofScreenshotFrameIndex = 0;
    static std::size_t proofInventoryPaneFrameIndex = 0;
    static bool proofScreenshotReadyQueued = false;
    static int worldViewerTimeSequenceIndex = -1;
    static int worldViewerCameraAngleSequenceIndex = -1;
    static bool proofInventoryOpened = false;
    static bool proofQuickSaveQueued = false;
    static bool proofSayQueued = false;
    static bool proofDialogueTopicQueued = false;
    static bool proofTimedScript1Executed = false;
    static bool proofTimedScript2Executed = false;
    static bool proofActorCameraAligned = false;
    static int proofActorCameraAlignedFrame = -1;
    static std::size_t proofActorCameraAlignedScreenshotIndex = static_cast<std::size_t>(-1);
    static bool proofActorAlignedScreenshotQueued = false;
    static bool proofActorNeutralizedForCamera = false;
    static bool proofActorStagedForCamera = false;
    static bool proofActorSnappedToRenderGround = false;
    static bool proofActorRenderGroundFreshBoundsLatched = false;
    static int proofActorStagedFrame = -1;
    static int proofActorSnappedFrame = -1;
    static MWWorld::Ptr proofPinnedStagedActor;
    static osg::Vec3f proofPinnedStagedActorPosition;
    static osg::Vec3f proofPinnedStagedActorRotation;
    static int proofPinnedStagedActorLastLogFrame = -1000000;
    static std::size_t proofActorBatchIndex = static_cast<std::size_t>(-1);
    static MWWorld::Ptr proofActorBatchPrevious;
    static bool proofActorBaseRosterExpanded = false;
    static int proofActorBatchSelectedFrame = -1;
    static std::size_t proofActorPoseBatchIndex = static_cast<std::size_t>(-1);
    static std::size_t proofActorPoseIndex = 0;
    static int proofActorPoseNextFrame = -1;
    static bool proofActorPoseCycleComplete = false;
    static bool proofActorPoseInventoryLogged = false;
    static std::size_t proofActorPosePlayed = 0;
    static std::size_t proofActorPoseSkipped = 0;
    static FNVSidecarOpenMwPhase proofSidecarPhase = FNVSidecarOpenMwPhase::WaitingRetail;
    static std::optional<FNVSidecar::RetailAction> proofSidecarAction;
    static std::uint64_t proofSidecarGeneration = 0;
    static std::uint32_t proofSidecarPreviousActor = 0;
    static std::uint32_t proofSidecarPreviousAction = 0;
    static int proofSidecarActionStartFrame = -1;
    static int proofSidecarCaptureStateStartFrame = -1;
    static bool proofSidecarActionPlayed = false;
    static FNVSidecarScreenshot proofSidecarScreenshotBaseline;
    static FNVSidecarScreenshot proofSidecarScreenshotCandidate;
    static int proofSidecarScreenshotStableFrames = 0;
    static bool proofSidecarCompletionPublished = false;
    static bool proofSidecarPeerErrorLogged = false;
    static int proofActorBatchCompleteFrame = -1;
    static bool proofActorBatchCompletionLogged = false;
    static bool proofActorBatchQuitRequested = false;
    static bool proofActorScreenshotWaitLogged = false;
    static int proofActorScreenshotLastResolveFrame = -1;
    static bool proofNeutralActorPreviewAttempted = false;
    static bool proofNeutralActorPreviewReady = false;
    static bool proofNeutralActorPreviewIsolationApplied = false;
    static osg::ref_ptr<osg::Group> proofNeutralActorPreviewRoot;
    static osg::ref_ptr<osg::Camera> proofNeutralActorPreviewComposite;
    static std::vector<std::unique_ptr<MWRender::FalloutActorPreview>> proofNeutralActorPreviews;
    static bool proofPinnedPlayerToActorView = false;
    static osg::Vec3f proofPinnedPlayerPosition;
    static osg::Vec3f proofPinnedPlayerRotation;
    static int proofPinnedPlayerFirstFrame = -1;
    static int proofPinnedPlayerLastLogFrame = -1000000;
    static bool proofFirstPersonHidden = false;
    static bool proofGuiHidden = false;
    static bool proofLoadingGuiForceCleared = false;
    static bool proofSkyDisabled = false;
    static bool proofGodModeEnabled = false;
    static bool proofDelayedStartupScriptExecuted = false;
    static bool proofFNVBootstrapApplied = false;
    static bool proofFNVCameraResetApplied = false;
    static bool proofRetailProjectionApplied = false;
    static bool proofRetailProjectionAudited = false;
    static bool worldViewerNonStaticStartCameraSettled = false;
    static bool fnvFlatStartupCameraSettled = false;
    static bool proofScreenshotWaitLogged = false;
    static int proofPortraitClearFrames = 0;
    static bool proofPortraitPreviousHeadResolved = false;
    static osg::Vec3d proofPortraitPreviousHead;
    static osg::Vec3d proofPortraitPreviousForward;
    static int proofPortraitLastRejectLogFrame = -1000000;
    static int proofWorldReadyFrames = 0;
    static bool worldViewerTelemetryLogged = false;
    static unsigned int worldViewerTelemetryLastFrame = 0;
    static bool worldViewerCameraWaitLogged = false;
    static bool playableSessionStarted = false;
    static bool playableSessionFinished = false;
    static bool playableSessionEndTelemetryPending = false;
    static bool playableSessionCameraSwitched = false;
    static bool playableSessionStartScreenshotPending = false;
    static bool playableSessionMidpointScreenshotPending = false;
    static bool playableSessionEndScreenshotPending = false;
    static bool playableSessionQuitRequested = false;
    static bool playableSessionActorAiStabilized = false;
    static unsigned int playableSessionExitFrame = 0;
    static int playableSessionOrbitScreenshotIndex = 0;
    static unsigned int playableSessionOrbitNextFrame = 0;
    static float playableSessionElapsed = 0.f;
    static float playableSessionFirstPersonCameraDistance = std::numeric_limits<float>::quiet_NaN();
    static osg::Vec3f playableSessionPlayerStart;
    static osg::Vec3f playableSessionCameraSwitchPosition;
    static osg::Vec3f playableSessionActorStart;
    static float playableSessionActorStartDistance = std::numeric_limits<float>::quiet_NaN();
    static MWWorld::Ptr playableSessionActor;
    static int playableSessionActorCombatSuppressions = 0;
    static int fnvInteractionPhase = 0;
    static unsigned int fnvInteractionPhaseFrame = 0;
    static osg::Timer_t fnvInteractionPhaseStartTime = 0;
    static bool fnvInteractionGreetingAudioSeen = false;
    static bool fnvInteractionTopicAudioSeen = false;
    static bool fnvInteractionActorPass = false;
    static bool fnvInteractionDialoguePass = false;
    static bool fnvInteractionQuestPass = false;
    static bool fnvInteractionDoorInPass = false;
    static bool fnvInteractionInteriorActorsPass = false;
    static bool fnvInteractionRadioPass = false;
    static bool fnvInteractionDoorOutPass = false;
    static MWWorld::Ptr fnvInteractionActor;
    static osg::Vec3f fnvInteractionActorSettledPosition;
    static MWWorld::Ptr fnvInteractionRadio;
    static ESM::RefId fnvInteractionRadioSound;
    static bool fnvInteractionQuestCaptureQueued = false;
    static bool fnvInteractionRadioCaptureQueued = false;
    static int authoredInteractionPhase = 0;
    static unsigned int authoredInteractionPhaseFrame = 0;
    static osg::Timer_t authoredInteractionPhaseStartTime = 0;
    static bool authoredInteractionGreetingAudioSeen = false;
    static bool authoredInteractionTopicAudioSeen = false;
    static bool authoredInteractionActorPass = false;
    static bool authoredInteractionDialoguePass = false;
    static bool authoredInteractionDoorInPass = false;
    static bool authoredInteractionInteriorActorsPass = false;
    static bool authoredInteractionRadioPass = false;
    static bool authoredInteractionDoorOutPass = false;
    static MWWorld::Ptr authoredInteractionActor;
    static osg::Vec3f authoredInteractionActorSettledPosition;
    static MWWorld::Ptr authoredInteractionRadio;
    static ESM::RefId authoredInteractionRadioSound;
    static bool authoredInteractionRadioCaptureQueued = false;
    const auto parseProofFormId = [](std::string_view value) -> std::optional<ESM::FormId> {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);

        constexpr std::string_view prefix = "FormId:0x";
        constexpr std::string_view shortPrefix = "0x";
        std::size_t offset = std::string_view::npos;
        if (value.size() > prefix.size() && Misc::StringUtils::ciEqual(value.substr(0, prefix.size()), prefix))
            offset = prefix.size();
        else if (value.size() > shortPrefix.size() && Misc::StringUtils::ciEqual(value.substr(0, shortPrefix.size()), shortPrefix))
            offset = shortPrefix.size();
        else
            return std::nullopt;

        std::uint32_t formId = 0;
        const auto result = std::from_chars(value.data() + offset, value.data() + value.size(), formId, 16);
        if (result.ec != std::errc() || result.ptr != value.data() + value.size())
        {
            try {
                formId = std::stoul(std::string(value.substr(offset)), nullptr, 16);
            } catch (...) { return std::nullopt; }
        }
        return ESM::FormId::fromUint32(formId);
    };
    const auto makeProofRefId = [&](const char* value) {
        const std::string_view ref(value);
        if (std::optional<ESM::FormId> formId = parseProofFormId(ref))
            return ESM::RefId::formIdRefId(*formId);
        return ESM::RefId::stringRefId(ref);
    };
    const auto compactProofToken = [](std::string_view value) {
        std::string compact;
        compact.reserve(value.size());
        for (char ch : Misc::StringUtils::lowerCase(value))
        {
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                compact.push_back(ch);
        }
        return compact;
    };
    const auto resolveProofActor = [&](const char* value) {
        const std::string_view target(value);
        const ESM::RefId targetRefId = makeProofRefId(value);
        const std::optional<ESM::FormId> targetFormId = parseProofFormId(target);
        const bool targetIsActorBase = mWorld->getStore().get<ESM4::Npc>().search(targetRefId) != nullptr
            || mWorld->getStore().get<ESM4::Creature>().search(targetRefId) != nullptr;
        const bool forceBaseSpawn = targetIsActorBase
            && proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_FORCE_BASE_SPAWN");
        if (forceBaseSpawn && !proofActorBatchPrevious.isEmpty()
            && proofActorBatchPrevious.getCellRef().getRefId() == targetRefId)
        {
            return proofActorBatchPrevious;
        }

        MWWorld::Ptr actor;
        if (!forceBaseSpawn)
            actor = mWorld->searchPtr(targetRefId, false, false);
        if (!actor.isEmpty())
            return actor;

        const std::string targetCompact = compactProofToken(target);
        const bool logActors = std::getenv("OPENMW_PROOF_LOG_ACTORS") != nullptr;
        int scanned = 0;
        int actors = 0;
        MWWorld::Ptr found;

        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
        {
            if (forceBaseSpawn)
                break;
            if (cellstore == nullptr)
                continue;

            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                ++scanned;
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return true;
                ++actors;

                const ESM::RefId baseId = ptr.getCellRef().getRefId();
                const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
                std::string name;
                try
                {
                    name = std::string(ptr.getClass().getName(ptr));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: actor name lookup failed for " << ptr.toString()
                                        << ": " << e.what();
                }

                const std::string baseDebug = baseId.toDebugString();
                const std::string refDebug = refNum.toString("FormId:");
                const std::string baseCompact = compactProofToken(baseDebug);
                const std::string refCompact = compactProofToken(refDebug);
                const std::string nameCompact = compactProofToken(name);
                const ESM::Position& pos = ptr.getRefData().getPosition();

                if (logActors)
                {
                    Log(Debug::Info) << "FNV/ESM4 proof: active actor ref=" << refDebug << " base=" << baseDebug
                                     << " name=\"" << name << "\" pos=(" << pos.pos[0] << "," << pos.pos[1]
                                     << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1]
                                     << "," << pos.rot[2] << ") ptr=" << ptr.toString();
                }

                const bool formIdMatchesRef = targetFormId.has_value() && refNum == *targetFormId;
                const bool refIdMatchesBase = baseId == targetRefId;
                const bool tokenMatches = !targetCompact.empty()
                    && (targetCompact == baseCompact || targetCompact == refCompact || targetCompact == nameCompact
                        || (!nameCompact.empty()
                            && (targetCompact.find(nameCompact) != std::string::npos
                                || nameCompact.find(targetCompact) != std::string::npos)));
                if (formIdMatchesRef || refIdMatchesBase || tokenMatches)
                {
                    found = ptr;
                    Log(Debug::Info) << "FNV/ESM4 proof: active-cell actor match target \"" << value
                                     << "\" ref=" << refDebug << " base=" << baseDebug << " name=\"" << name
                                     << "\" ptr=" << ptr.toString() << " pos=(" << pos.pos[0] << ","
                                     << pos.pos[1] << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << ","
                                     << pos.rot[1] << "," << pos.rot[2] << ") scanned=" << scanned
                                     << " actors=" << actors;
                    return false;
                }
                return true;
            });

            if (!found.isEmpty())
                break;
        }

        if (found.isEmpty())
        {
            if (forceBaseSpawn)
                Log(Debug::Info) << "FNV/ESM4 proof: forcing isolated base-record spawn for \"" << value << "\"";
            else
                Log(Debug::Warning) << "FNV/ESM4 proof: active-cell actor lookup failed for \"" << value
                                    << "\" scanned=" << scanned << " actors=" << actors;

            if (std::getenv("OPENMW_PROOF_PLACE_ACTOR_IF_MISSING") != nullptr)
            {
                try
                {
                    MWWorld::Ptr player = MWMechanics::getPlayer();
                    MWWorld::CellStore* store = nullptr;
                    ESM::RefId worldspace = ESM::Cell::sDefaultWorldspaceId;
                    ESM::Position pos;
                    if (!player.isEmpty() && player.isInCell())
                    {
                        const ESM::Position& playerPos = player.getRefData().getPosition();
                        pos.pos[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", playerPos.pos[0] + 96.f);
                        pos.pos[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", playerPos.pos[1]);
                        pos.pos[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", playerPos.pos[2]);
                        pos.rot[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", 0.f);
                        pos.rot[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", 0.f);
                        pos.rot[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", 0.f);

                        MWWorld::CellStore* playerCell = player.getCell();
                        if (playerCell != nullptr && playerCell->isExterior())
                        {
                            worldspace = playerCell->getCell()->getWorldSpace();
                            store = &mWorld->getWorldModel().getExterior(
                                ESM::positionToExteriorCellLocation(pos.pos[0], pos.pos[1], worldspace));
                        }
                        else
                            store = playerCell;
                    }
                    else
                    {
                        pos.pos[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", 0.f);
                        pos.pos[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", 0.f);
                        pos.pos[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", 0.f);
                        pos.rot[0] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", 0.f);
                        pos.rot[1] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", 0.f);
                        pos.rot[2] = readProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", 0.f);
                        const auto& activeCells = mWorld->getWorldScene().getActiveCells();
                        if (!activeCells.empty())
                            store = *activeCells.begin();
                    }

                    if (store == nullptr)
                        throw std::runtime_error("no active cell available for proof placement");

                    ESM::RefId placementRefId = targetRefId;
                    if (placementRefId.is<ESM::StringRefId>())
                    {
                        const std::string_view editorId = placementRefId.getRefIdString();
                        ESM::RefId resolvedId = findEsm4EditorId<ESM4::Npc>(mWorld->getStore(), editorId);
                        if (resolvedId.empty())
                            resolvedId = findEsm4EditorId<ESM4::Creature>(mWorld->getStore(), editorId);
                        if (!resolvedId.empty())
                        {
                            placementRefId = resolvedId;
                            Log(Debug::Info) << "FNV/ESM4 proof: resolved string base target \"" << editorId
                                             << "\" to form id " << resolvedId.toDebugString();
                        }
                    }

                    if (const char* placementFormId = std::getenv("OPENMW_PROOF_PLACE_ACTOR_FORM_ID"))
                    {
                        if (std::optional<ESM::FormId> formId = parseProofFormId(placementFormId))
                        {
                            placementRefId = ESM::RefId::formIdRefId(*formId);
                            Log(Debug::Info) << "FNV/ESM4 proof: using inventory form id " << placementFormId
                                             << " for missing actor target \"" << value << "\" base="
                                             << targetRefId.toDebugString();
                        }
                        else
                            Log(Debug::Warning) << "FNV/ESM4 proof: ignored invalid inventory form id \""
                                                << placementFormId << "\" for missing actor target \"" << value << "\"";
                    }

                    MWWorld::ManualRef ref(mWorld->getStore(), placementRefId);
                    ref.getPtr().mRef->mData.mPhysicsPostponed = !ref.getPtr().getClass().isActor();
                    ref.getPtr().getCellRef().setPosition(pos);
                    found = mWorld->placeObject(ref.getPtr(), store, pos);
                    found.getClass().adjustPosition(found, true);
                    Log(Debug::Info) << "FNV/ESM4 proof: placed missing actor target \"" << value
                                     << "\" base=" << placementRefId.toDebugString() << " requestedBase="
                                     << targetRefId.toDebugString() << " pos=(" << pos.pos[0] << "," << pos.pos[1]
                                     << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1] << ","
                                     << pos.rot[2] << ") ptr=" << found.toString();
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to place missing actor target \"" << value
                                        << "\": " << e.what();
                }
            }
        }
        return found;
    };

    const auto failFNVSidecar = [&](FNVSidecar::ErrorCode code, std::string message) {
        if (!proofSidecarEnabled || proofSidecarPhase == FNVSidecarOpenMwPhase::Failed)
            return;
        proofSidecarClient.setError(code, message);
        proofSidecarPhase = FNVSidecarOpenMwPhase::Failed;
        Log(Debug::Error) << "FNV sidecar OpenMW: fail-closed code=" << static_cast<std::uint32_t>(code)
                          << " message=\"" << message << "\" frame=" << frameNumber;
    };

    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();
    std::function<void(int)> setPlayableSessionFrontPortraitCamera;

    worldViewerTrace(frameNumber, "frame.begin");
    mEnvironment.setFrameDuration(frametime);
    worldViewerTrace(frameNumber, "frame.duration-set");

    try
    {
        // update input
        worldViewerTrace(frameNumber, "input.begin");
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }
        worldViewerTrace(frameNumber, "input.end");

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            worldViewerTrace(frameNumber, "sound.begin");
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            static bool loggedHiddenVrWindow = false;
            if (!mWindowManager->isWindowVisible() && VR::getVR() && !loggedHiddenVrWindow)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: VR mirror window hidden; keeping world simulation running";
                loggedHiddenVrWindow = true;
            }

            const bool backgroundPlayableSession = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_BACKGROUND");
            if (!mWindowManager->isWindowVisible() && !VR::getVR() && !backgroundPlayableSession)
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            static bool backgroundPlayableSessionLogged = false;
            if (!mWindowManager->isWindowVisible() && !VR::getVR() && backgroundPlayableSession
                && !backgroundPlayableSessionLogged)
            {
                backgroundPlayableSessionLogged = true;
                Log(Debug::Info) << "Playable session: minimized/hidden window detected; keeping simulation and native "
                                    "capture active without foreground input";
            }

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }
        worldViewerTrace(frameNumber, "sound.end");

        {
            worldViewerTrace(frameNumber, "lua-sync.begin");
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }
        worldViewerTrace(frameNumber, "lua-sync.end");

        // update game state
        worldViewerTrace(frameNumber, "state.begin");
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }
        worldViewerTrace(frameNumber, "state.end");

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            worldViewerTrace(frameNumber, "script.begin");
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }
        worldViewerTrace(frameNumber, "script.end");

        int requestedTimeSequenceIndex = -1;
        for (std::size_t i = 0; i < worldViewerTimeSequence.size(); ++i)
        {
            if (frameNumber >= static_cast<unsigned>(worldViewerTimeSequence[i].mFrame))
                requestedTimeSequenceIndex = static_cast<int>(i);
        }
        if (requestedTimeSequenceIndex >= 0 && requestedTimeSequenceIndex != worldViewerTimeSequenceIndex
            && mWorld != nullptr && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            const WorldViewerTimeKeyframe& keyframe
                = worldViewerTimeSequence[static_cast<std::size_t>(requestedTimeSequenceIndex)];
            mWorld->setGlobalFloat(MWWorld::Globals::sGameHour, keyframe.mHour);
            mWorld->advanceTime(0.0, false);
            worldViewerTimeSequenceIndex = requestedTimeSequenceIndex;
            Log(Debug::Info) << "World viewer sky sweep: applied time slot=" << requestedTimeSequenceIndex
                             << " frame=" << frameNumber << " hour=" << keyframe.mHour;
        }

        const bool playableSessionRequested = proofEnvEnabled("OPENMW_PLAYABLE_SESSION");
        const int playableSessionSettleFrames
            = std::max(1, readProofInt("OPENMW_PLAYABLE_SESSION_SETTLE_FRAMES", 120));
        const bool playableSessionReady = playableSessionRequested && mWorld != nullptr
            && mStateManager->getState() == MWBase::StateManager::State_Running && !paused
            && proofWorldReadyFrames >= playableSessionSettleFrames;
        const auto setPlayableSessionCamera = [&](MWRender::Camera::Mode mode) {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            MWRender::Camera* camera = mWorld->getCamera();
            if (player.isEmpty() || camera == nullptr)
                return;

            const float distance = mode == MWRender::Camera::Mode::FirstPerson
                ? 0.f
                : readProofFloat("OPENMW_PLAYABLE_SESSION_CAMERA_DISTANCE", 192.f);
            camera->attachTo(player);
            camera->setMode(mode, true);
            camera->setPreferredCameraDistance(distance);
            camera->processViewChange();
            camera->update(0.f, false);
            camera->instantTransition();
            camera->updateCamera();
        };
        setPlayableSessionFrontPortraitCamera = [&](int orbitIndex) {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            MWRender::Camera* camera = mWorld->getCamera();
            if (player.isEmpty() || camera == nullptr)
                return;

            const bool closeup = orbitIndex >= 4;
            MWWorld::Ptr focalActor = player;
            if (closeup && orbitIndex == 5 && !playableSessionActor.isEmpty())
                focalActor = playableSessionActor;

            const auto resolveRenderedFaceCenter = [](const MWWorld::Ptr& actor, osg::Vec3f& center) {
                if (actor.isEmpty() || actor.getRefData().getBaseNode() == nullptr)
                    return false;
                FalloutProofFaceBoundsVisitor visitor;
                actor.getRefData().getBaseNode()->accept(visitor);
                if (visitor.getHeadMatched() == 0)
                    return false;
                const osg::Vec3d head = visitor.getHeadCenter();
                if (!std::isfinite(head.x()) || !std::isfinite(head.y()) || !std::isfinite(head.z()))
                    return false;
                center.set(static_cast<float>(head.x()), static_cast<float>(head.y()), static_cast<float>(head.z()));
                return true;
            };

            osg::Vec3f focal;
            osg::Vec3f focalFace;
            const bool focalFaceResolved = resolveRenderedFaceCenter(focalActor, focalFace);
            std::string_view focalSource = "reference-height";
            if (closeup && focalFaceResolved)
            {
                focal = focalFace;
                focalSource = "rendered-face";
            }
            else if (!closeup && proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_CENTER_ACTOR_GROUP")
                && !playableSessionActor.isEmpty())
            {
                osg::Vec3f playerFace;
                osg::Vec3f actorFace;
                const bool playerFaceResolved = resolveRenderedFaceCenter(player, playerFace);
                const bool actorFaceResolved = resolveRenderedFaceCenter(playableSessionActor, actorFace);
                if (playerFaceResolved && actorFaceResolved)
                {
                    focal = (playerFace + actorFace) * 0.5f;
                    focalSource = "rendered-face-group";
                }
                else
                {
                    const osg::Vec3f playerPosition = player.getRefData().getPosition().asVec3();
                    const osg::Vec3f actorPosition = playableSessionActor.getRefData().getPosition().asVec3();
                    focal = (playerPosition + actorPosition) * 0.5f
                        + osg::Vec3f(0.f, 0.f,
                            readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_FOCAL_HEIGHT", 112.f));
                    focalSource = "reference-height-group";
                }
            }
            else
            {
                const char* closeupFocalHeightEnv = orbitIndex == 4
                    ? "OPENMW_PLAYABLE_SESSION_PLAYER_CLOSEUP_FOCAL_HEIGHT"
                    : "OPENMW_PLAYABLE_SESSION_ACTOR_CLOSEUP_FOCAL_HEIGHT";
                const float focalHeight = closeup
                    ? readProofFloat(closeupFocalHeightEnv,
                        readProofFloat("OPENMW_PLAYABLE_SESSION_CLOSEUP_FOCAL_HEIGHT", 100.f))
                    : readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_FOCAL_HEIGHT", 112.f);
                focal = focalActor.getRefData().getPosition().asVec3() + osg::Vec3f(0.f, 0.f, focalHeight);
            }
            const float defaultDistance
                = readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_DISTANCE", 112.f);
            osg::Vec3f offset(
                readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_OFFSET_X", 0.f),
                readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_OFFSET_Y", defaultDistance),
                readProofFloat("OPENMW_PLAYABLE_SESSION_PORTRAIT_OFFSET_Z", 0.f));
            if (closeup)
            {
                const float frontSign = offset.y() < 0.f ? -1.f : 1.f;
                const char* closeupDistanceEnv = orbitIndex == 4
                    ? "OPENMW_PLAYABLE_SESSION_PLAYER_CLOSEUP_DISTANCE"
                    : "OPENMW_PLAYABLE_SESSION_ACTOR_CLOSEUP_DISTANCE";
                const char* closeupOffsetZEnv = orbitIndex == 4
                    ? "OPENMW_PLAYABLE_SESSION_PLAYER_CLOSEUP_OFFSET_Z"
                    : "OPENMW_PLAYABLE_SESSION_ACTOR_CLOSEUP_OFFSET_Z";
                const float closeupDistance = readProofFloat(closeupDistanceEnv,
                    readProofFloat("OPENMW_PLAYABLE_SESSION_CLOSEUP_DISTANCE", 90.f));
                const float facing = focalActor.getRefData().getPosition().rot[2];
                // Actor transforms rotate their local +Y axis around -Z. Put the
                // proof camera on that authored forward axis so a close-up is a
                // real face check instead of an accidental rear/side view.
                offset.set(-frontSign * closeupDistance * std::sin(facing),
                    -frontSign * closeupDistance * std::cos(facing),
                    readProofFloat(closeupOffsetZEnv,
                        readProofFloat("OPENMW_PLAYABLE_SESSION_CLOSEUP_OFFSET_Z", 4.f)));
                const char* closeupYawOffsetEnv = orbitIndex == 4
                    ? "OPENMW_PLAYABLE_SESSION_PLAYER_CLOSEUP_YAW_OFFSET_DEGREES"
                    : "OPENMW_PLAYABLE_SESSION_ACTOR_CLOSEUP_YAW_OFFSET_DEGREES";
                const float yawOffset = osg::DegreesToRadians(readProofFloat(closeupYawOffsetEnv, 0.f));
                if (std::abs(yawOffset) > 0.0001f)
                {
                    const float originalX = offset.x();
                    const float originalY = offset.y();
                    offset.x() = originalX * std::cos(yawOffset) - originalY * std::sin(yawOffset);
                    offset.y() = originalX * std::sin(yawOffset) + originalY * std::cos(yawOffset);
                }
            }
            else if (orbitIndex > 0)
            {
                const float planarDistance = std::max(defaultDistance,
                    std::sqrt(offset.x() * offset.x() + offset.y() * offset.y()));
                const float frontSign = offset.y() < 0.f ? -1.f : 1.f;
                if (orbitIndex == 1)
                    offset.x() = -offset.x();
                else if (orbitIndex == 2)
                    offset.set(0.f, frontSign * planarDistance, offset.z());
                else
                    offset.set(0.f, -frontSign * planarDistance, offset.z());
            }
            osg::Vec3f cameraPosition = focal + offset;
            if (closeup && proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_RENDER_RAYCAST"))
            {
                const char* target = orbitIndex == 4 ? "level-one player"
                                                     : std::getenv("OPENMW_PLAYABLE_SESSION_ACTOR");
                selectProofActorCameraByOrbitRays(*mWorld, focalActor, target, focal, cameraPosition);
            }
            if (!closeup && proofEnvEnabled("OPENMW_PLAYABLE_SESSION_PORTRAIT_RAYCAST"))
            {
                // The imported worlds frequently place foliage, signs, shields, or architecture between a
                // mathematically valid proof camera and its subjects. Reuse the rendering/ground ray sampler
                // used by source-driven world-viewer starts so native evidence selects a clear authored angle.
                const osg::Vec3d resolved = resolveWorldViewerOrbitCamera(
                    *mWorld, osg::Vec3d(cameraPosition), osg::Vec3d(focal));
                cameraPosition.set(static_cast<float>(resolved.x()), static_cast<float>(resolved.y()),
                    static_cast<float>(resolved.z()));
            }
            osg::Vec3f direction = focal - cameraPosition;
            if (direction.length2() <= 0.0001f)
                direction.set(0.f, -1.f, 0.f);
            direction.normalize();
            const float horizontal = std::sqrt(
                direction.x() * direction.x() + direction.y() * direction.y());
            const float yaw = std::atan2(-direction.x(), direction.y());
            const float pitch = std::atan2(direction.z(), horizontal);

            camera->attachTo(player);
            camera->setMode(MWRender::Camera::Mode::Static, true);
            camera->setStaticPosition(cameraPosition);
            camera->setYaw(yaw, true);
            camera->setPitch(pitch, true);
            camera->processViewChange();
            camera->update(0.f, false);
            camera->instantTransition();
            camera->updateCamera();
            float focalScreenX = std::numeric_limits<float>::quiet_NaN();
            float focalScreenY = std::numeric_limits<float>::quiet_NaN();
            if (const osg::Camera* renderCamera = mViewer != nullptr ? mViewer->getCamera() : nullptr)
            {
                if (const osg::Viewport* viewport = renderCamera->getViewport();
                    viewport != nullptr && viewport->width() > 0.0 && viewport->height() > 0.0)
                {
                    const osg::Vec3d window = osg::Vec3d(focal) * renderCamera->getViewMatrix()
                        * renderCamera->getProjectionMatrix() * viewport->computeWindowMatrix();
                    focalScreenX = static_cast<float>((window.x() - viewport->x()) / viewport->width());
                    focalScreenY = static_cast<float>((window.y() - viewport->y()) / viewport->height());
                }
            }
            Log(Debug::Info) << "Playable session: framed native portrait camera orbitIndex=" << orbitIndex
                             << " mode=static pos=("
                             << cameraPosition.x() << "," << cameraPosition.y() << "," << cameraPosition.z()
                             << ") focal=(" << focal.x() << "," << focal.y() << "," << focal.z()
                             << ") yaw=" << camera->getYaw() << " pitch=" << camera->getPitch()
                             << " focalActorRotZ=" << focalActor.getRefData().getPosition().rot[2]
                             << " focalSource=" << focalSource << " faceResolved=" << focalFaceResolved
                             << " focalScreen=(" << focalScreenX << "," << focalScreenY << ")";
        };
        const auto setPlayableSessionMovement
            = [&](const MWWorld::Ptr& player, float side, float forward, bool run) {
                  MWMechanics::Movement& movement = player.getClass().getMovementSettings(player);
                  movement.mPosition[0] = side;
                  movement.mPosition[1] = forward;
                  movement.mPosition[2] = 0.f;
                  player.getClass().getCreatureStats(player).setMovementFlag(
                      MWMechanics::CreatureStats::Flag_Run, run);
                  player.getClass().getCreatureStats(player).setMovementFlag(
                      MWMechanics::CreatureStats::Flag_Sneak, false);

                  if (MWBase::LuaManager::ActorControls* controls = mLuaManager->getActorControls(player))
                  {
                      controls->mDisableAI = false;
                      controls->mMovement = forward;
                      controls->mSideMovement = side;
                      controls->mJump = false;
                      controls->mRun = run;
                      controls->mSneak = false;
                      controls->mChanged = true;
                  }
              };
        const auto resolvePlayableSessionActor = [&]() {
            const char* target = std::getenv("OPENMW_PLAYABLE_SESSION_ACTOR");
            if (target != nullptr && *target != '\0')
                return resolveProofActor(target);

            MWWorld::Ptr player = mWorld->getPlayerPtr();
            MWWorld::Ptr nearest;
            float nearestDistance2 = std::numeric_limits<float>::max();
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (ptr.isEmpty() || ptr == player || !ptr.getClass().isActor())
                        return true;
                    const float distance2 = (ptr.getRefData().getPosition().asVec3()
                                                - player.getRefData().getPosition().asVec3())
                                                .length2();
                    if (distance2 < nearestDistance2)
                    {
                        nearestDistance2 = distance2;
                        nearest = ptr;
                    }
                    return true;
                });
            }
            return nearest;
        };
        const auto applyPlayableSessionSafeActorState = [&]() {
            if (playableSessionActor.isEmpty()
                || !proofEnvEnabled("OPENMW_PLAYABLE_SESSION_NEUTRALIZE_ACTOR"))
                return;

            MWMechanics::CreatureStats& actorStats
                = playableSessionActor.getClass().getCreatureStats(playableSessionActor);
            actorStats.setAiSetting(MWMechanics::AiSetting::Fight, 0);
            if (actorStats.getAiSequence().isInCombat())
            {
                actorStats.getAiSequence().stopCombat();
                actorStats.setAttackingOrSpell(false);
                ++playableSessionActorCombatSuppressions;
                Log(Debug::Info) << "Playable session: suppressed false safe-start combat package actor="
                                 << playableSessionActor.toString() << " count="
                                 << playableSessionActorCombatSuppressions;
            }
        };

        if (playableSessionReady && !playableSessionStarted)
        {
            playableSessionStarted = true;
            playableSessionOrbitScreenshotIndex = 0;
            playableSessionOrbitNextFrame = 0;
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            if (proofEnvEnabled("OPENMW_PLAYABLE_SESSION_FORCE_LEVEL_ONE"))
            {
                player.getClass().getCreatureStats(player).setLevel(1);
                if (player.getClass().isNpc())
                    player.getClass().getNpcStats(player).setLevelProgress(0);
            }

            playableSessionPlayerStart = player.getRefData().getPosition().asVec3();
            playableSessionCameraSwitchPosition = playableSessionPlayerStart;
            playableSessionActor = resolvePlayableSessionActor();
            logProofActorRenderBounds(player, "Player", "playable-session-start");
            if (!playableSessionActor.isEmpty())
            {
                const char* actorTarget = std::getenv("OPENMW_PLAYABLE_SESSION_ACTOR");
                logProofActorRenderBounds(playableSessionActor,
                    actorTarget != nullptr ? actorTarget : "nearest", "playable-session-start");
                playableSessionActorStart = playableSessionActor.getRefData().getPosition().asVec3();
                playableSessionActorStartDistance
                    = (playableSessionActorStart - playableSessionPlayerStart).length();
                if (proofEnvEnabled("OPENMW_PLAYABLE_SESSION_NEUTRALIZE_ACTOR"))
                {
                    applyPlayableSessionSafeActorState();
                    Log(Debug::Info) << "Playable session: enabled safe-start hostility guard without freezing actor="
                                     << playableSessionActor.toString();
                }
            }

            setPlayableSessionCamera(MWRender::Camera::Mode::ThirdPerson);
            playableSessionStartScreenshotPending
                = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_CAPTURE_SCREENSHOTS");
            const MWRender::Camera* camera = mWorld->getCamera();
            osg::Vec3f cameraPosition;
            if (camera != nullptr)
            {
                const osg::Vec3d position = camera->getPosition();
                cameraPosition.set(
                    static_cast<float>(position.x()), static_cast<float>(position.y()), static_cast<float>(position.z()));
            }
            const float cameraDistance = camera != nullptr
                ? (cameraPosition - playableSessionPlayerStart).length()
                : std::numeric_limits<float>::quiet_NaN();
            const int level = player.getClass().getCreatureStats(player).getLevel();
            const char* sessionId = std::getenv("OPENMW_PLAYABLE_SESSION_ID");
            Log(Debug::Info) << "Playable session telemetry: phase=start id=\""
                             << (sessionId != nullptr ? sessionId : "default") << "\" frame=" << frameNumber
                             << " level=" << level << " playerPos=(" << playableSessionPlayerStart.x() << ","
                             << playableSessionPlayerStart.y() << "," << playableSessionPlayerStart.z()
                             << ") cameraMode=" << (camera != nullptr ? static_cast<int>(camera->getMode()) : -1)
                             << " cameraDistance=" << cameraDistance << " actorResolved="
                             << (!playableSessionActor.isEmpty() ? 1 : 0) << " actor=\""
                             << (!playableSessionActor.isEmpty() ? playableSessionActor.toString() : std::string())
                             << "\" actorDistance=" << playableSessionActorStartDistance;
        }

        if (playableSessionStarted && !playableSessionFinished && !playableSessionEndTelemetryPending)
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const float duration
                = std::max(0.25f, readProofFloat("OPENMW_PLAYABLE_SESSION_DURATION_SECONDS", 4.f));
            const bool validateCameras = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_VALIDATE_CAMERAS");
            if (validateCameras && !playableSessionCameraSwitched && playableSessionElapsed >= duration * 0.5f)
            {
                playableSessionCameraSwitched = true;
                playableSessionCameraSwitchPosition = player.getRefData().getPosition().asVec3();
                setPlayableSessionCamera(MWRender::Camera::Mode::FirstPerson);
                if (const MWRender::Camera* camera = mWorld->getCamera())
                {
                    const osg::Vec3f cameraPosition = camera->getPosition();
                    playableSessionFirstPersonCameraDistance
                        = (cameraPosition - playableSessionCameraSwitchPosition).length();
                }
                playableSessionMidpointScreenshotPending
                    = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_CAPTURE_SCREENSHOTS");
                Log(Debug::Info) << "Playable session telemetry: phase=camera-switch frame=" << frameNumber
                                 << " mode=first-person playerPos=(" << playableSessionCameraSwitchPosition.x()
                                 << "," << playableSessionCameraSwitchPosition.y() << ","
                                 << playableSessionCameraSwitchPosition.z() << ") cameraDistance="
                                 << playableSessionFirstPersonCameraDistance;
            }

            if (playableSessionElapsed < duration)
            {
                setPlayableSessionMovement(player,
                    readProofFloat("OPENMW_PLAYABLE_SESSION_STRAFE", 0.f),
                    readProofFloat("OPENMW_PLAYABLE_SESSION_FORWARD", 1.f),
                    proofEnvEnabled("OPENMW_PLAYABLE_SESSION_RUN"));
                playableSessionElapsed += std::clamp(frametime, 0.f, 0.1f);
            }
            else
            {
                setPlayableSessionMovement(player, 0.f, 0.f, false);
                if (validateCameras)
                {
                    if (proofEnvEnabled("OPENMW_PLAYABLE_SESSION_FRONT_PORTRAIT"))
                        setPlayableSessionFrontPortraitCamera(0);
                    else
                        setPlayableSessionCamera(MWRender::Camera::Mode::ThirdPerson);
                }
                playableSessionEndTelemetryPending = true;
            }
        }

        if (!playableSessionActorAiStabilized
            && proofEnvEnabled("OPENMW_PLAYABLE_SESSION_STABILIZE_ACTOR_AI")
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            playableSessionActorAiStabilized = true;
            const bool wasActive = mMechanicsManager->isAIActive();
            if (wasActive)
                mMechanicsManager->toggleAI();
            Log(Debug::Info) << "Playable session: stabilized nearby actors by disabling autonomous AI"
                             << " previousActive=" << wasActive
                             << " currentActive=" << mMechanicsManager->isAIActive();
        }

        // update mechanics
        {
            worldViewerTrace(frameNumber, "mechanics.begin");
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }
        worldViewerTrace(frameNumber, "mechanics.end");

        applyPlayableSessionSafeActorState();

        // update physics
        {
            worldViewerTrace(frameNumber, "physics.begin");
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }
        worldViewerTrace(frameNumber, "physics.end");

        // update world
        {
            worldViewerTrace(frameNumber, "world-update.begin");
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }
        worldViewerTrace(frameNumber, "world-update.end");

        if (proofEnvEnabled("OPENMW_FNV_RETAIL_POSE_REPLAY") && mWorld != nullptr
            && mStateManager->getState() != MWBase::StateManager::State_NoGame)
        {
            const char* snapshotPathEnv = std::getenv("OPENMW_FNV_RETAIL_POSE_SNAPSHOT");
            const char* targetRefEnv = std::getenv("OPENMW_FNV_RETAIL_POSE_TARGET_REF");
            static std::string loadedSnapshotPath;
            static std::string failedSnapshotPath;
            static std::shared_ptr<FalloutRetailPoseSnapshot> snapshot;
            static bool actorMissingLogged = false;
            static bool rootApplyLogged = false;

            const std::string snapshotPath = snapshotPathEnv != nullptr ? snapshotPathEnv : "";
            if (!snapshotPath.empty() && snapshotPath != loadedSnapshotPath && snapshotPath != failedSnapshotPath)
            {
                try
                {
                    snapshot = loadFalloutRetailPoseSnapshot(snapshotPath);
                    loadedSnapshotPath = snapshotPath;
                    failedSnapshotPath.clear();
                    actorMissingLogged = false;
                    rootApplyLogged = false;
                    Log(Debug::Info) << "FNV/ESM4 retail pose snapshot loaded: path=\"" << snapshotPath
                                     << "\" nodes=" << snapshot->mNodes.size() << " rootBits="
                                     << formatProofFloatBits(snapshot->mRootBits);
                }
                catch (const std::exception& e)
                {
                    snapshot.reset();
                    failedSnapshotPath = snapshotPath;
                    Log(Debug::Error) << "FNV/ESM4 retail pose snapshot load failed: path=\"" << snapshotPath
                                      << "\" error=\"" << e.what() << "\"";
                }
            }

            if (snapshot != nullptr && targetRefEnv != nullptr && *targetRefEnv != '\0')
            {
                MWWorld::Ptr actor = resolveProofActor(targetRefEnv);
                if (actor.isEmpty() || actor.getRefData().getBaseNode() == nullptr)
                {
                    if (!actorMissingLogged)
                    {
                        actorMissingLogged = true;
                        Log(Debug::Error) << "FNV/ESM4 retail pose replay target unresolved: target=\""
                                          << targetRefEnv << "\"";
                    }
                }
                else
                {
                    actorMissingLogged = false;
                    osg::Node* actorRoot = actor.getRefData().getBaseNode();
                    std::string attachedSnapshotPath;
                    if (!actorRoot->getUserValue("fnvRetailPoseSnapshotPath", attachedSnapshotPath)
                        || attachedSnapshotPath != snapshot->mPath)
                    {
                        const int applyFrame = readProofInt("OPENMW_FNV_RETAIL_POSE_APPLY_FRAME", 0);
                        const int auditFrame = readProofInt("OPENMW_FNV_RETAIL_POSE_AUDIT_FRAME", -1);
                        actorRoot->addUpdateCallback(
                            new FalloutRetailPoseReplayCallback(snapshot, applyFrame, auditFrame));
                        actorRoot->setUserValue("fnvRetailPoseSnapshotPath", snapshot->mPath);
                        Log(Debug::Info) << "FNV/ESM4 retail pose replay attached: target=\"" << targetRefEnv
                                         << "\" applyFrame=" << applyFrame << " auditFrame=" << auditFrame;
                    }

                    const int applyFrame = readProofInt("OPENMW_FNV_RETAIL_POSE_APPLY_FRAME", 0);
                    if (frameNumber >= static_cast<unsigned int>(std::max(0, applyFrame)))
                    {
                        const osg::Vec3f expectedPosition(proofFloatFromBits(snapshot->mRootBits[0]),
                            proofFloatFromBits(snapshot->mRootBits[1]), proofFloatFromBits(snapshot->mRootBits[2]));
                        const osg::Vec3f expectedRotation(proofFloatFromBits(snapshot->mRootBits[3]),
                            proofFloatFromBits(snapshot->mRootBits[4]), proofFloatFromBits(snapshot->mRootBits[5]));
                        const ESM::Position before = actor.getRefData().getPosition();
                        std::array<std::uint32_t, 6> beforeBits = {
                            std::bit_cast<std::uint32_t>(before.pos[0]), std::bit_cast<std::uint32_t>(before.pos[1]),
                            std::bit_cast<std::uint32_t>(before.pos[2]), std::bit_cast<std::uint32_t>(before.rot[0]),
                            std::bit_cast<std::uint32_t>(before.rot[1]), std::bit_cast<std::uint32_t>(before.rot[2])
                        };
                        if (beforeBits[0] != snapshot->mRootBits[0] || beforeBits[1] != snapshot->mRootBits[1]
                            || beforeBits[2] != snapshot->mRootBits[2])
                        {
                            actor = mWorld->moveObject(actor, expectedPosition);
                        }
                        if (beforeBits[3] != snapshot->mRootBits[3] || beforeBits[4] != snapshot->mRootBits[4]
                            || beforeBits[5] != snapshot->mRootBits[5])
                        {
                            mWorld->rotateObject(actor, expectedRotation);
                        }

                        const ESM::Position& after = actor.getRefData().getPosition();
                        const std::array<std::uint32_t, 6> afterBits = {
                            std::bit_cast<std::uint32_t>(after.pos[0]), std::bit_cast<std::uint32_t>(after.pos[1]),
                            std::bit_cast<std::uint32_t>(after.pos[2]), std::bit_cast<std::uint32_t>(after.rot[0]),
                            std::bit_cast<std::uint32_t>(after.rot[1]), std::bit_cast<std::uint32_t>(after.rot[2])
                        };
                        const int auditFrame = readProofInt("OPENMW_FNV_RETAIL_POSE_AUDIT_FRAME", -1);
                        if (!rootApplyLogged || static_cast<int>(frameNumber) == auditFrame)
                        {
                            rootApplyLogged = true;
                            const bool pass = afterBits == snapshot->mRootBits;
                            Log(pass ? Debug::Info : Debug::Error)
                                << "FNV/ESM4 retail root replay: frame=" << frameNumber << " target=\""
                                << targetRefEnv << "\" beforeBits=" << formatProofFloatBits(beforeBits)
                                << " expectedBits=" << formatProofFloatBits(snapshot->mRootBits)
                                << " afterBits=" << formatProofFloatBits(afterBits)
                                << " status=" << (pass ? "pass" : "fail");
                        }
                    }
                }
            }
        }

        // update GUI
        {
            worldViewerTrace(frameNumber, "gui.begin");
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
        worldViewerTrace(frameNumber, "gui.end");
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
        worldViewerTrace(frameNumber, "frame.exception-caught");
    }

    worldViewerTrace(frameNumber, "stats.begin");
    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", mUnrefQueue->getSize());

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", mWorkQueue->getNumItems());
        stats->setAttribute(frameNumber, "WorkThread", mWorkQueue->getNumActiveThreads());

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);
    }
    worldViewerTrace(frameNumber, "stats.end");

    worldViewerTrace(frameNumber, "stereo.begin");
    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);
    worldViewerTrace(frameNumber, "stereo.end");

    worldViewerTrace(frameNumber, "osg-event.begin");
    mViewer->eventTraversal();
    worldViewerTrace(frameNumber, "osg-event.end");
    worldViewerTrace(frameNumber, "osg-update.begin");
    if (viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_SKIP_OSG_UPDATE_TRAVERSAL"))
    {
        Log(Debug::Info) << "World viewer trace: frame=" << frameNumber
                         << " phase=\"osg-update.skipped\" reason=\"proof static scene\"";
        std::fflush(stdout);
        std::fflush(stderr);
    }
    else
    {
        auditWorldViewerOsgUpdateCallbacks(mViewer->getSceneData(), frameNumber);
        mViewer->updateTraversal();
    }
    worldViewerTrace(frameNumber, "osg-update.end");

    // update GUI by world data
    {
        worldViewerTrace(frameNumber, "window-world-sync.begin");
        ScopedProfile<UserStatsType::WindowManager> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateWindowManager();
    }
    worldViewerTrace(frameNumber, "window-world-sync.end");

    if (VR::getVR())
    {
        if (mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            auto playerPtr = MWMechanics::getPlayer();
            auto playerAnim = MWBase::Environment::get().getWorld()->getAnimation(playerPtr);
            if (playerAnim)
                static_cast<MWVR::VRAnimation*>(playerAnim)->updateSpace();
        }
        if (VR::getShouldRecenterZ() || VR::getShouldRecenterXY())
        {
            MWBase::Environment::get().getLuaManager()->vrRecentered(
                VR::getShouldRecenterZ(), VR::getShouldRecenterXY());
            VR::setShouldRecenterXY(false);
            VR::setShouldRecenterZ(false);
        }
        mLuaManager->onVRFrame();
        VR::Session::instance().updateSpaces();
    }

    // Stock OpenMW releases Lua here because the remaining stock work is render-only.  The
    // background proof paths below also perform controlled scene/camera mutations, so defer
    // their Lua update until immediately before rendering.  Lua then overlaps only the cull
    // traversal, matching the worker's threading contract.
    const bool deferProofLuaWorker = proofEnvEnabled("OPENMW_FNV_INTERACTION_AUDIT")
        || proofEnvEnabled("OPENMW_AUTHORED_INTERACTION_AUDIT")
        || proofEnvEnabled("OPENMW_PLAYABLE_SESSION_BACKGROUND")
        || proofEnvEnabled("OPENMW_PROOF_DELAY_STARTUP_SCRIPT");
    if (!deferProofLuaWorker)
    {
        worldViewerTrace(frameNumber, "lua-worker-allow.begin");
        mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);
        worldViewerTrace(frameNumber, "lua-worker-allow.end");
    }

    if (proofEnvEnabled("OPENMW_PROOF_HIDE_FIRST_PERSON"))
    {
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::Mask_FirstPerson;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (!proofFirstPersonHidden)
        {
            proofFirstPersonHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden first-person render mask for clean proof capture";
        }
    }

    if (proofEnvEnabled("OPENMW_PROOF_HIDE_PLAYER_VISUAL"))
    {
        static bool proofPlayerMaskHidden = false;
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::Mask_Player;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (mWorld != nullptr)
        {
            const MWWorld::Ptr player = mWorld->getPlayerPtr();
            if (!player.isEmpty() && player.getRefData().getBaseNode() != nullptr)
                player.getRefData().getBaseNode()->setNodeMask(0);
        }
        if (!proofPlayerMaskHidden)
        {
            proofPlayerMaskHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden player render mask and base node for clean proof capture";
        }
    }

    if (proofEnvEnabled("OPENMW_PROOF_HIDE_WORLD_VISUAL"))
    {
        static bool proofWorldMaskHidden = false;
        const uint32_t worldMask = MWRender::Mask_Static | MWRender::Mask_Object | MWRender::Mask_Terrain
            | MWRender::Mask_Groundcover | MWRender::Mask_Water | MWRender::Mask_SimpleWater
            | MWRender::Mask_Sky | MWRender::Mask_Sun | MWRender::Mask_WeatherParticles;
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~worldMask;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (!proofWorldMaskHidden)
        {
            proofWorldMaskHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden world render masks for actor proof capture";
        }
    }

    if (!proofGuiHidden && std::getenv("OPENMW_PROOF_HIDE_GUI") != nullptr)
    {
        mWindowManager->setHudVisibility(false);
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~MWRender::VisMask::Mask_GUI;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        proofGuiHidden = true;
        Log(Debug::Info) << "FNV/ESM4 proof: hidden GUI/HUD for clean proof capture";
    }
    if (!proofSkyDisabled && std::getenv("OPENMW_PROOF_DISABLE_SKY") != nullptr)
    {
        bool skyEnabled = mWorld->toggleSky();
        if (skyEnabled)
            skyEnabled = mWorld->toggleSky();
        proofSkyDisabled = true;
        Log(Debug::Info) << "FNV/ESM4 proof: disabled sky for clean proof capture finalEnabled=" << skyEnabled;
    }

    const bool proofRunning = mStateManager->getState() == MWBase::StateManager::State_Running;
    const bool proofForceClearLoadingGui = std::getenv("OPENMW_PROOF_FORCE_CLEAR_LOADING_GUI") != nullptr;
    if (!proofLoadingGuiForceCleared && proofForceClearLoadingGui && proofRunning)
    {
        proofLoadingGuiForceCleared = true;
        Log(Debug::Info)
            << "FNV/ESM4 proof: logically cleared loading GUI for world-viewer proof without mutating Lua UI modes";
    }
    const bool proofLoadingGuiRaw = mWindowManager->containsMode(MWGui::GM_Loading)
        || mWindowManager->containsMode(MWGui::GM_LoadingWallpaper)
        || mWindowManager->containsMode(MWGui::GM_MainMenu);
    const bool proofLoadingGui = proofLoadingGuiRaw && !(proofForceClearLoadingGui && proofLoadingGuiForceCleared);
    const bool proofWorldReady = proofRunning && !proofLoadingGui;
    if (proofWorldReady)
        ++proofWorldReadyFrames;
    else
        proofWorldReadyFrames = 0;

    if (proofWorldReady && mWorld != nullptr && std::getenv("OPENMW_FNV_RETAIL_PROJECTION_REPLAY") != nullptr)
    {
        const osg::Matrixf retailProjection = makeFalloutRetailProjectionMatrix();
        mWorld->getRenderingManager()->overrideProjectionMatrix(retailProjection,
            proofFloatFromBits(FalloutRetailVerticalFovBits), proofFloatFromBits(FalloutRetailNearClipBits),
            proofFloatFromBits(FalloutRetailFarClipBits));
        if (!proofRetailProjectionApplied)
        {
            proofRetailProjectionApplied = true;
            Log(Debug::Info) << "FNV/ESM4 retail projection replay: applied source=retail-d3d9-draw-sequences-938-939"
                             << " viewport=2048x1280 baseHorizontalFov=75"
                             << " verticalFovBits="
                             << formatProofFloatBits(FalloutRetailVerticalFovBitArray)
                             << " nearFarBits=" << formatProofFloatBits(FalloutRetailNearFarBits)
                             << " retailD3DBits=" << formatProofFloatBits(FalloutRetailD3DProjectionBits)
                             << " expectedOpenGLBits=" << formatProofFloatBits(FalloutRetailOpenGLProjectionBits);
        }
    }

    if (proofEnvEnabled("OPENMW_PROOF_HIDE_WORLD_OCCLUDERS"))
    {
        static bool proofWorldOccludersHidden = false;
        const uint32_t occluderMask
            = MWRender::Mask_Static | MWRender::Mask_Object | MWRender::Mask_Groundcover;
        const uint32_t mask = mViewer->getCamera()->getCullMask() & ~occluderMask;
        mViewer->getCamera()->setCullMask(mask);
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
        if (!proofWorldOccludersHidden)
        {
            proofWorldOccludersHidden = true;
            Log(Debug::Info) << "FNV/ESM4 proof: hidden static/object/groundcover occluders while preserving terrain, sky, lighting, and actors";
        }
    }

    const int proofRetailProjectionAuditFrame
        = readProofInt("OPENMW_FNV_RETAIL_PROJECTION_AUDIT_FRAME", 420);
    if (!proofRetailProjectionAudited && proofRetailProjectionApplied
        && static_cast<int>(frameNumber) >= proofRetailProjectionAuditFrame)
    {
        proofRetailProjectionAudited = true;
        const std::array<std::uint32_t, 16> actualBits
            = getProofMatrixBits(mViewer->getCamera()->getProjectionMatrix());
        const osg::Viewport* viewport = mViewer->getCamera()->getViewport();
        const int viewportWidth = viewport != nullptr ? static_cast<int>(viewport->width()) : 0;
        const int viewportHeight = viewport != nullptr ? static_cast<int>(viewport->height()) : 0;
        const auto* renderingManager = mWorld->getRenderingManager();
        const std::uint32_t actualFovBits
            = std::bit_cast<std::uint32_t>(renderingManager->getFieldOfView());
        const std::uint32_t actualNearBits
            = std::bit_cast<std::uint32_t>(renderingManager->getNearClipDistance());
        const std::uint32_t actualFarBits
            = std::bit_cast<std::uint32_t>(renderingManager->getViewDistance());
        const std::array<std::uint32_t, 1> actualFovBitArray = { actualFovBits };
        const std::array<std::uint32_t, 2> actualNearFarBits = { actualNearBits, actualFarBits };
        const bool passed = viewportWidth == 2048 && viewportHeight == 1280
            && actualBits == FalloutRetailOpenGLProjectionBits && actualFovBits == FalloutRetailVerticalFovBits
            && actualNearBits == FalloutRetailNearClipBits && actualFarBits == FalloutRetailFarClipBits;
        Log(passed ? Debug::Info : Debug::Error)
            << "FNV/ESM4 retail projection audit: frame=" << frameNumber
            << " viewport=" << viewportWidth << "x" << viewportHeight
            << " fovBits=" << formatProofFloatBits(actualFovBitArray)
            << " nearFarBits=" << formatProofFloatBits(actualNearFarBits)
            << " expectedOpenGLBits=" << formatProofFloatBits(FalloutRetailOpenGLProjectionBits)
            << " actualOpenGLBits=" << formatProofFloatBits(actualBits)
            << " status=" << (passed ? "pass" : "fail");
    }

    worldViewerTrace(frameNumber, "proof-world-ready-evaluated");
    if (proofWorldReady && mWorld != nullptr)
    {
        worldViewerTrace(frameNumber, "dry-start.begin");
        enforceWorldViewerDryStart(*mWorld);
        worldViewerTrace(frameNumber, "dry-start.end");
    }

    const bool proofActorStaticCameraOwnsView = proofActorCameraAligned
        && std::getenv("OPENMW_PROOF_ALIGN_PLAYER_TO_ACTOR") != nullptr
        && std::getenv("OPENMW_PROOF_ACTOR_VIEW_STATIC_CAMERA") != nullptr;
    if (proofWorldReady && mWorld != nullptr && !VR::getVR() && !proofActorStaticCameraOwnsView)
    {
        worldViewerTrace(frameNumber, "static-camera.begin");
        enforceWorldViewerStaticCamera(*mWorld, frameNumber);
        worldViewerTrace(frameNumber, "static-camera.end");
    }
    else if (proofActorStaticCameraOwnsView)
    {
        static bool proofActorStaticCameraSuppressedSurveyLogged = false;
        if (!proofActorStaticCameraSuppressedSurveyLogged)
        {
            proofActorStaticCameraSuppressedSurveyLogged = true;
            Log(Debug::Info)
                << "FNV/ESM4 proof: suppressing static survey camera after actor camera alignment frame="
                << frameNumber;
        }
    }

    if (viewerTelemetryEnabled("OPENMW_WORLD_VIEWER_TELEMETRY") && proofRunning && mWorld != nullptr)
    {
        worldViewerTrace(frameNumber, "periodic-telemetry.begin");
        const int telemetryInterval = readProofInt("OPENMW_WORLD_VIEWER_TELEMETRY_INTERVAL", 30);
        const bool telemetryDue = !worldViewerTelemetryLogged || telemetryInterval <= 0
            || frameNumber >= worldViewerTelemetryLastFrame + static_cast<unsigned int>(telemetryInterval);
        if (telemetryDue)
        {
            logWorldViewerTelemetry(*mWorld, *mViewer, frameNumber, static_cast<int>(mStateManager->getState()),
                proofLoadingGui, proofWorldReady, proofWorldReadyFrames);
            worldViewerTelemetryLastFrame = frameNumber;
            worldViewerTelemetryLogged = true;
        }
        worldViewerTrace(frameNumber, "periodic-telemetry.end");
    }

    if (playableSessionEndTelemetryPending && mWorld != nullptr)
    {
        MWWorld::Ptr player = mWorld->getPlayerPtr();
        const osg::Vec3f endPosition = player.getRefData().getPosition().asVec3();
        const auto horizontalDistance = [](const osg::Vec3f& lhs, const osg::Vec3f& rhs) {
            const float x = lhs.x() - rhs.x();
            const float y = lhs.y() - rhs.y();
            return std::sqrt(x * x + y * y);
        };
        const float totalDistance = horizontalDistance(endPosition, playableSessionPlayerStart);
        const float thirdPersonDistance = playableSessionCameraSwitched
            ? horizontalDistance(playableSessionCameraSwitchPosition, playableSessionPlayerStart)
            : totalDistance;
        const float firstPersonDistance = playableSessionCameraSwitched
            ? horizontalDistance(endPosition, playableSessionCameraSwitchPosition)
            : 0.f;
        const float verticalDrift = std::abs(endPosition.z() - playableSessionPlayerStart.z());
        const float averageSpeed = playableSessionElapsed > 0.f ? totalDistance / playableSessionElapsed : 0.f;

        float actorDrift = std::numeric_limits<float>::quiet_NaN();
        float actorEndDistance = std::numeric_limits<float>::quiet_NaN();
        bool actorInCombat = false;
        if (!playableSessionActor.isEmpty())
        {
            const osg::Vec3f actorEnd = playableSessionActor.getRefData().getPosition().asVec3();
            actorDrift = (actorEnd - playableSessionActorStart).length();
            actorEndDistance = (actorEnd - endPosition).length();
            actorInCombat = playableSessionActor.getClass()
                                .getCreatureStats(playableSessionActor)
                                .getAiSequence()
                                .isInCombat();
        }

        const MWRender::Camera* camera = mWorld->getCamera();
        osg::Vec3f cameraEndPosition;
        if (camera != nullptr)
        {
            const osg::Vec3d position = camera->getPosition();
            cameraEndPosition.set(
                static_cast<float>(position.x()), static_cast<float>(position.y()), static_cast<float>(position.z()));
        }
        const float cameraEndDistance = camera != nullptr
            ? (cameraEndPosition - endPosition).length()
            : std::numeric_limits<float>::quiet_NaN();
        const int level = player.getClass().getCreatureStats(player).getLevel();
        const bool validateCameras = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_VALIDATE_CAMERAS");
        const bool requireActor = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_REQUIRE_ACTOR");
        const float minimumDistance
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MIN_DISTANCE", 64.f));
        const float minimumSegmentDistance
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MIN_CAMERA_SEGMENT_DISTANCE", 24.f));
        const float minimumSpeed
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MIN_SPEED", 20.f));
        const float maximumSpeed
            = std::max(minimumSpeed, readProofFloat("OPENMW_PLAYABLE_SESSION_MAX_SPEED", 600.f));
        const float maximumVerticalDrift
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MAX_VERTICAL_DRIFT", 512.f));
        const float maximumFirstPersonCameraDistance = std::max(
            0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MAX_FIRST_PERSON_CAMERA_DISTANCE", 512.f));
        const float maximumActorDrift
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MAX_ACTOR_DRIFT", 256.f));
        const float maximumActorDistance
            = std::max(0.f, readProofFloat("OPENMW_PLAYABLE_SESSION_MAX_ACTOR_DISTANCE", 3072.f));
        const bool levelPass = level == 1;
        const bool movementPass = totalDistance >= minimumDistance && averageSpeed >= minimumSpeed
            && averageSpeed <= maximumSpeed && verticalDrift <= maximumVerticalDrift;
        const bool cameraPass = !validateCameras
            || (playableSessionCameraSwitched && thirdPersonDistance >= minimumSegmentDistance
                && firstPersonDistance >= minimumSegmentDistance
                && std::isfinite(playableSessionFirstPersonCameraDistance)
                && playableSessionFirstPersonCameraDistance <= maximumFirstPersonCameraDistance);
        const bool actorResolvedPass = !requireActor || !playableSessionActor.isEmpty();
        const bool actorStabilityPass = !requireActor
            || (!playableSessionActor.isEmpty() && std::isfinite(actorDrift)
                && actorDrift <= maximumActorDrift && actorEndDistance <= maximumActorDistance && !actorInCombat);
        const bool sessionPass
            = levelPass && movementPass && cameraPass && actorResolvedPass && actorStabilityPass;

        const char* sessionId = std::getenv("OPENMW_PLAYABLE_SESSION_ID");
        Log(Debug::Info) << "Playable session telemetry: phase=end id=\""
                         << (sessionId != nullptr ? sessionId : "default") << "\" frame=" << frameNumber
                         << " elapsed=" << playableSessionElapsed << " level=" << level << " playerStart=("
                         << playableSessionPlayerStart.x() << "," << playableSessionPlayerStart.y() << ","
                         << playableSessionPlayerStart.z() << ") playerEnd=(" << endPosition.x() << ","
                         << endPosition.y() << "," << endPosition.z() << ") distance=" << totalDistance
                         << " averageSpeed=" << averageSpeed << " verticalDrift=" << verticalDrift
                         << " thirdPersonDistance=" << thirdPersonDistance << " firstPersonDistance="
                         << firstPersonDistance << " firstPersonCameraDistance="
                         << playableSessionFirstPersonCameraDistance << " cameraEndMode="
                         << (camera != nullptr ? static_cast<int>(camera->getMode()) : -1)
                         << " cameraEndDistance=" << cameraEndDistance << " actorResolved="
                         << (!playableSessionActor.isEmpty() ? 1 : 0) << " actorStartDistance="
                         << playableSessionActorStartDistance << " actorEndDistance=" << actorEndDistance
                         << " actorDrift=" << actorDrift << " actorInCombat=" << (actorInCombat ? 1 : 0)
                         << " actorCombatSuppressions=" << playableSessionActorCombatSuppressions
                         << " levelPass=" << (levelPass ? 1 : 0) << " movementPass="
                         << (movementPass ? 1 : 0) << " cameraPass=" << (cameraPass ? 1 : 0)
                         << " actorResolvedPass=" << (actorResolvedPass ? 1 : 0) << " actorStabilityPass="
                         << (actorStabilityPass ? 1 : 0) << " result=" << (sessionPass ? "pass" : "fail");

        playableSessionEndTelemetryPending = false;
        playableSessionFinished = true;
        playableSessionEndScreenshotPending
            = proofEnvEnabled("OPENMW_PLAYABLE_SESSION_CAPTURE_SCREENSHOTS");
        if (!playableSessionEndScreenshotPending)
            playableSessionExitFrame = frameNumber + 2;
    }

    if (!proofDelayedStartupScriptExecuted && std::getenv("OPENMW_PROOF_DELAY_STARTUP_SCRIPT") != nullptr
        && !mStartupScript.empty() && proofWorldReady)
    {
        // The console script can change cells and rebuild the active-object lists.  At this
        // point in the frame the Lua worker has already been released, so mutating the scene
        // concurrently with nearby.* list iteration races the worker (and can recurse through
        // sol/Lua error handling until the worker stack overflows).  Finish this frame's Lua
        // update before executing the one-shot world mutation.
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
        Log(Debug::Info) << "FNV/ESM4 proof: executing delayed startup script after world load";
        mWindowManager->executeInConsole(mStartupScript);
        proofDelayedStartupScriptExecuted = true;
    }

    const bool proofFNVBootstrapProfile = std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr;
    const bool proofFNVBootstrapOutside = std::getenv("OPENMW_FNV_BOOTSTRAP_DOC_SENT") != nullptr;
    if (!proofFNVBootstrapApplied && proofRunning && (proofFNVBootstrapProfile || proofFNVBootstrapOutside))
    {
        try
        {
            applyFNVLevelOneCourierBootstrap(
                *mWorld, *mJournal, proofFNVBootstrapOutside, proofFNVBootstrapProfile);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "FNV/ESM4 proof: level-1 Courier bootstrap failed: " << e.what();
        }
        proofFNVBootstrapApplied = true;
    }

    const char* worldViewerStartCameraMode = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_MODE");
    const bool worldViewerStaticStartCamera
        = worldViewerStartCameraMode != nullptr && std::string(worldViewerStartCameraMode) == "static";
    if (!worldViewerNonStaticStartCameraSettled && proofWorldReady && proofWorldReadyFrames >= 2 && !VR::getVR()
        && worldViewerNonStaticStartCameraRequested() && mWorld != nullptr && !proofActorStaticCameraOwnsView)
    {
        worldViewerNonStaticStartCameraSettled = settleWorldViewerNonStaticStartCamera(*mWorld);
    }

    const bool worldViewerExplicitNonStaticStartCamera = worldViewerNonStaticStartCameraRequested();
    if (!fnvFlatStartupCameraSettled && proofWorldReady && proofWorldReadyFrames >= 2 && !VR::getVR()
        && hasFalloutNvContent(mContentFiles) && !worldViewerStaticStartCamera
        && !worldViewerExplicitNonStaticStartCamera && !proofActorStaticCameraOwnsView
        && std::getenv("OPENMW_FNV_PROOF_RESET_CAMERA") == nullptr)
    {
        settleFNVFlatStartupCamera(*mWorld);
        fnvFlatStartupCameraSettled = true;
    }

    if (!proofFNVCameraResetApplied && proofWorldReady && std::getenv("OPENMW_FNV_PROOF_RESET_CAMERA") != nullptr)
    {
        resetFNVProofCamera(*mWorld);
        proofFNVCameraResetApplied = true;
    }

    int requestedCameraAngleSequenceIndex = -1;
    for (std::size_t i = 0; i < worldViewerCameraAngleSequence.size(); ++i)
    {
        if (frameNumber >= static_cast<unsigned>(worldViewerCameraAngleSequence[i].mFrame))
            requestedCameraAngleSequenceIndex = static_cast<int>(i);
    }
    if (requestedCameraAngleSequenceIndex >= 0 && proofWorldReady && mWorld != nullptr && !VR::getVR())
    {
        if (MWRender::Camera* camera = mWorld->getCamera())
        {
            const WorldViewerCameraAngleKeyframe& keyframe
                = worldViewerCameraAngleSequence[static_cast<std::size_t>(requestedCameraAngleSequenceIndex)];
            camera->setPitch(keyframe.mPitch, true);
            camera->setYaw(keyframe.mYaw, true);
            camera->setRoll(0.f);
            camera->updateCamera();
            if (requestedCameraAngleSequenceIndex != worldViewerCameraAngleSequenceIndex)
            {
                worldViewerCameraAngleSequenceIndex = requestedCameraAngleSequenceIndex;
                Log(Debug::Info) << "World viewer sky sweep: applied camera slot="
                                 << requestedCameraAngleSequenceIndex << " frame=" << frameNumber
                                 << " pitch=" << keyframe.mPitch << " yaw=" << keyframe.mYaw;
            }
        }
    }

    if (!proofInventoryOpened && proofInventoryFrame >= 0 && frameNumber >= static_cast<unsigned>(proofInventoryFrame)
        && proofRunning)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: opening native inventory screen at frame " << frameNumber;
        mWindowManager->pushGuiMode(MWGui::GM_Inventory);
        proofInventoryOpened = true;
    }

    while (proofInventoryPaneFrameIndex < proofInventoryPaneFrames.size()
        && frameNumber >= static_cast<unsigned>(proofInventoryPaneFrames[proofInventoryPaneFrameIndex]) && proofRunning)
    {
        const int paneIndex = proofInventoryPaneFrameIndex < proofInventoryPaneIndices.size()
            ? proofInventoryPaneIndices[proofInventoryPaneFrameIndex]
            : 1;
        if (!mWindowManager->containsMode(MWGui::GM_Inventory))
            mWindowManager->pushGuiMode(MWGui::GM_Inventory);
        Log(Debug::Info) << "FNV/ESM4 proof: raising native inventory pane index=" << paneIndex << " at frame "
                         << frameNumber;
        mWindowManager->setActiveControllerWindow(MWGui::GM_Inventory, paneIndex);
        ++proofInventoryPaneFrameIndex;
    }

    if (!proofQuickSaveQueued && proofQuickSaveFrame >= 0 && frameNumber >= static_cast<unsigned>(proofQuickSaveFrame)
        && proofRunning)
    {
        const char* quickSaveName = std::getenv("OPENMW_PROOF_QUICKSAVE_NAME");
        const std::string name
            = quickSaveName != nullptr && *quickSaveName != '\0' ? quickSaveName : "FNV Proof Save";
        Log(Debug::Info) << "FNV/ESM4 proof: requesting quicksave \"" << name << "\" at frame " << frameNumber;
        mStateManager->quickSave(name);
        proofQuickSaveQueued = true;
    }

    if (!proofGodModeEnabled && proofRunning && std::getenv("OPENMW_PROOF_GOD_MODE") != nullptr && mWorld != nullptr)
    {
        if (!mWorld->getGodModeState())
            mWorld->toggleGodMode();
        proofGodModeEnabled = true;
        Log(Debug::Info) << "FNV/ESM4 proof: enabled god mode for deterministic proof capture";
    }

    if (!proofActorBaseRosterExpanded && proofWorldReady && mWorld != nullptr
        && !proofActorBatchTargets.empty() && !proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_ALL_LOADED"))
    {
        if (proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_AUTO_FRAMES"))
        {
            const int configuredFirst = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_FIRST_FRAME");
            const int configuredStep = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_FRAMES_PER_ACTOR");
            const int poseFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_POSE_FRAMES");
            const int poseFrames = poseFramesEnv >= 1 ? poseFramesEnv : 12;
            const int poseWindow = static_cast<int>(proofActorPoseGroups.size() + 2) * poseFrames;
            const int firstFrame = configuredFirst >= 0
                ? configuredFirst
                : std::max<int>(static_cast<int>(frameNumber) + 60, std::max(0, proofSayFrame) + poseWindow + 60);
            const int framesPerActor = configuredStep >= 1 ? configuredStep : std::max(90, poseWindow + 60);
            proofScreenshotFrames.clear();
            proofScreenshotFrames.reserve(proofActorBatchTargets.size());
            for (std::size_t index = 0; index < proofActorBatchTargets.size(); ++index)
            {
                const std::uint64_t requested = static_cast<std::uint64_t>(firstFrame)
                    + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(framesPerActor);
                proofScreenshotFrames.push_back(static_cast<int>(
                    std::min<std::uint64_t>(requested, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))));
            }
        }
        proofActorBaseRosterExpanded = true;
        Log(Debug::Info) << "FNV/ESM4 actor batch: explicit roster ready selected="
                         << proofActorBatchTargets.size() << " screenshotFrames=" << proofScreenshotFrames.size();
    }

    if (!proofActorBaseRosterExpanded && proofWorldReady && mWorld != nullptr
        && proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_ALL_LOADED"))
    {
        struct LoadedActorBase
        {
            ESM::FormId mId;
            std::string mType;
            std::string mEditorId;
            std::string mName;
            std::string mVisualSignature;
            std::string mSelectedWeapon;
            std::uint32_t mFlags = 0;
            int mRepresentativeScore = 0;
            std::size_t mRepresentativeOfCount = 1;
        };

        const auto npcTemplateChain = [](const ESM4::Npc& base) {
            std::vector<const ESM4::Npc*> records;
            const ESM4::Npc* current = &base;
            for (int depth = 0; current != nullptr && depth < 16; ++depth)
            {
                if (std::find(records.begin(), records.end(), current) != records.end())
                    break;
                records.push_back(current);
                if (current->mBaseTemplate.isZeroOrUnset())
                    break;
                const ESM4::Npc* next = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledNpc, ESM4::Npc>(
                    ESM::RefId(current->mBaseTemplate), 1);
                if (next == nullptr || next == current)
                    break;
                current = next;
            }
            return records;
        };
        const auto chooseNpcTemplate = [](const std::vector<const ESM4::Npc*>& records, std::uint16_t flag) {
            for (const ESM4::Npc* record : records)
            {
                if (record == nullptr)
                    continue;
                if (record->mIsTES4)
                    return record;
                if (record->mIsFONV && (record->mBaseConfig.fo3.templateFlags & flag) == 0)
                    return record;
                if (record->mIsFO4 && (record->mBaseConfig.fo4.templateFlags & flag) == 0)
                    return record;
                if (!record->mIsFONV && !record->mIsFO4
                    && (record->mBaseConfig.tes5.templateFlags & flag) == 0)
                    return record;
            }
            return static_cast<const ESM4::Npc*>(nullptr);
        };
        const auto makeNpcVisualSignature = [&](const ESM4::Npc& npc, std::string& selectedWeapon) {
            const std::vector<const ESM4::Npc*> records = npcTemplateChain(npc);
            const ESM4::Npc* traits = chooseNpcTemplate(records, ESM4::Npc::Template_UseTraits);
            const ESM4::Npc* model = chooseNpcTemplate(records, ESM4::Npc::Template_UseModel);
            const ESM4::Npc* inventory = chooseNpcTemplate(records, ESM4::Npc::Template_UseInventory);
            if (traits == nullptr)
                traits = &npc;
            if (model == nullptr)
                model = traits;

            const bool female = traits->mIsFONV
                ? (traits->mBaseConfig.fo3.flags & ESM4::Npc::FO3_Female) != 0
                : (traits->mBaseConfig.tes5.flags & ESM4::Npc::TES5_Female) != 0;
            const ESM4::Armor* dominantArmor = nullptr;
            const ESM4::Clothing* dominantClothing = nullptr;
            const ESM4::Weapon* mainWeapon = nullptr;
            std::function<void(ESM::FormId, int)> addVisualItem;
            addVisualItem = [&](ESM::FormId itemId, int depth) {
                if (itemId.isZeroOrUnset() || depth > 16)
                    return;
                if (const ESM4::LevelledItem* levelled
                    = mWorld->getStore().get<ESM4::LevelledItem>().search(ESM::RefId(itemId));
                    levelled != nullptr && levelled->useAll())
                {
                    for (const ESM4::LVLO& entry : levelled->mLvlObject)
                    {
                        if (entry.level <= 1 && entry.item != 0)
                            addVisualItem(ESM::FormId::fromUint32(entry.item), depth + 1);
                    }
                    return;
                }

                if (const ESM4::Armor* armor
                    = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledItem, ESM4::Armor>(ESM::RefId(itemId), 1))
                {
                    if ((armor->mArmorFlags & ESM4::Armor::FO3_UpperBody) != 0)
                    {
                        dominantArmor = armor;
                        dominantClothing = nullptr;
                    }
                    return;
                }
                if (const ESM4::Weapon* weapon
                    = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledItem, ESM4::Weapon>(ESM::RefId(itemId), 1))
                {
                    if (!weapon->mModel.empty()
                        && (mainWeapon == nullptr || weapon->mData.damage > mainWeapon->mData.damage))
                        mainWeapon = weapon;
                    return;
                }
                if (const ESM4::Clothing* clothing
                    = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledItem, ESM4::Clothing>(ESM::RefId(itemId), 1))
                {
                    if ((clothing->mClothingFlags & ESM4::Armor::FO3_UpperBody) != 0)
                    {
                        dominantClothing = clothing;
                        dominantArmor = nullptr;
                    }
                }
            };

            if (inventory != nullptr)
            {
                for (const ESM4::InventoryItem& item : inventory->mInventory)
                    addVisualItem(ESM::FormId::fromUint32(item.item), 0);
                if (const ESM4::Outfit* outfit
                    = mWorld->getStore().get<ESM4::Outfit>().search(ESM::RefId(inventory->mDefaultOutfit)))
                {
                    for (ESM::FormId itemId : outfit->mInventory)
                        addVisualItem(itemId, 0);
                }
            }
            selectedWeapon = mainWeapon != nullptr ? mainWeapon->mEditorId : std::string();

            const auto normalizedPath = [](std::string_view value) {
                std::string result(value);
                std::replace(result.begin(), result.end(), '\\', '/');
                Misc::StringUtils::lowerCaseInPlace(result);
                return result;
            };
            std::ostringstream signature;
            signature << "NPC_";
            if (dominantArmor != nullptr)
            {
                std::string bodyModel = normalizedPath(MWClass::ESM4Npc::chooseEquipmentModel(dominantArmor, female));
                if (bodyModel.empty())
                    bodyModel = Misc::StringUtils::lowerCase(dominantArmor->mEditorId);
                signature << "|body=armor:" << bodyModel << "|powerArmor="
                          << ((dominantArmor->mGeneralFlags & ESM4::Armor::FO3_PowerArmor) != 0);
            }
            else if (dominantClothing != nullptr)
            {
                std::string bodyModel
                    = normalizedPath(MWClass::ESM4Npc::chooseEquipmentModel(dominantClothing, female));
                if (bodyModel.empty())
                    bodyModel = Misc::StringUtils::lowerCase(dominantClothing->mEditorId);
                signature << "|body=clothing:" << bodyModel << "|powerArmor=0";
            }
            else
                signature << "|body=naked|race=" << ESM::RefId(traits->mRace).toDebugString()
                          << "|female=" << female << "|powerArmor=0";
            return signature.str();
        };
        const auto creatureTemplateChain = [](const ESM4::Creature& base) {
            std::vector<const ESM4::Creature*> records;
            const ESM4::Creature* current = &base;
            for (int depth = 0; current != nullptr && depth < 16; ++depth)
            {
                if (std::find(records.begin(), records.end(), current) != records.end())
                    break;
                records.push_back(current);
                if (current->mBaseTemplate.isZeroOrUnset())
                    break;
                const ESM4::Creature* next
                    = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledCreature, ESM4::Creature>(
                        ESM::RefId(current->mBaseTemplate), 1);
                if (next == nullptr || next == current)
                    break;
                current = next;
            }
            return records;
        };
        const auto makeCreatureVisualSignature = [&](const ESM4::Creature& creature) {
            const std::vector<const ESM4::Creature*> records = creatureTemplateChain(creature);
            const ESM4::CreatureVisualTemplate visual = ESM4::resolveCreatureVisualTemplate(records);
            const auto normalizedPath = [](std::string_view value) {
                std::string result(value);
                std::replace(result.begin(), result.end(), '\\', '/');
                Misc::StringUtils::lowerCaseInPlace(result);
                return result;
            };
            std::vector<std::string> meshes;
            if (visual.mModel != nullptr && !visual.mModel->mModel.empty())
                meshes.push_back(normalizedPath(visual.mModel->mModel));
            if (visual.mNif != nullptr)
            {
                for (const std::string& nif : visual.mNif->mNif)
                    meshes.push_back(normalizedPath(nif));
            }
            if (meshes.empty())
                return std::string();
            if (visual.mBodyParts != nullptr)
            {
                for (ESM::FormId bodyPartId : visual.mBodyParts->mBodyParts)
                {
                    if (const ESM4::BodyPartData* bodyPart
                        = mWorld->getStore().get<ESM4::BodyPartData>().search(ESM::RefId(bodyPartId));
                        bodyPart != nullptr && !bodyPart->mModel.empty())
                        meshes.push_back(normalizedPath(bodyPart->mModel));
                }
            }
            std::sort(meshes.begin(), meshes.end());
            meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
            std::ostringstream signature;
            signature << "CREA";
            for (const std::string& mesh : meshes)
                signature << "|mesh=" << mesh;
            return signature.str();
        };
        const auto representativeScore = [](std::string_view editorId, std::string_view name, std::uint32_t flags,
                                             bool armed, bool unique) {
            std::string label(editorId);
            label.push_back(' ');
            label.append(name);
            Misc::StringUtils::lowerCaseInPlace(label);
            int score = 0;
            static constexpr std::array<std::string_view, 9> templateMarkers{
                "dead", "corpse", "template", "test", "audio", "preset", "dummy", "leveled", "lvl"
            };
            for (std::string_view marker : templateMarkers)
            {
                if (label.find(marker) != std::string::npos)
                    score -= 10000;
            }
            score += armed ? 200 : 0;
            score += !name.empty() ? 100 : 0;
            score += !editorId.empty() ? 25 : 0;
            score += (flags & ESM::FLAG_Persistent) != 0 ? 50 : 0;
            score += unique ? 50 : 0;
            return score;
        };

        std::vector<LoadedActorBase> loadedActorBases;
        const auto appendNpc = [&](const ESM4::Npc& npc) {
            if (npc.mId.isZeroOrUnset() || (npc.mFlags & ESM::FLAG_Deleted) != 0)
                return;
            if (proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_EXCLUDE_RAW_PLAYER_BASE")
                && (npc.mId.toUint32() == 0x00000007u || npc.mEditorId == "Player"))
                return;
            std::string selectedWeapon;
            const std::string signature = makeNpcVisualSignature(npc, selectedWeapon);
            LoadedActorBase actor{
                npc.mId, "NPC_", npc.mEditorId, npc.mFullName, signature, selectedWeapon, npc.mFlags
            };
            actor.mRepresentativeScore = representativeScore(npc.mEditorId, npc.mFullName, npc.mFlags,
                !selectedWeapon.empty(), (npc.mBaseConfig.fo3.flags & 0x00000020u) != 0);
            loadedActorBases.push_back(std::move(actor));
        };
        const auto appendCreature = [&](const ESM4::Creature& creature) {
            if (creature.mId.isZeroOrUnset() || (creature.mFlags & ESM::FLAG_Deleted) != 0)
                return;
            const std::string signature = makeCreatureVisualSignature(creature);
            if (signature.empty())
                return;
            LoadedActorBase actor{
                creature.mId, "CREA", creature.mEditorId, creature.mFullName, signature, {}, creature.mFlags
            };
            actor.mRepresentativeScore
                = representativeScore(creature.mEditorId, creature.mFullName, creature.mFlags, false, false);
            loadedActorBases.push_back(std::move(actor));
        };
        for (const ESM4::Npc& npc : mWorld->getStore().get<ESM4::Npc>())
            appendNpc(npc);
        for (const ESM4::Creature& creature : mWorld->getStore().get<ESM4::Creature>())
            appendCreature(creature);
        std::stable_sort(loadedActorBases.begin(), loadedActorBases.end(), [](const auto& left, const auto& right) {
            if (left.mType != right.mType)
                return left.mType == "NPC_";
            return left.mId.toUint32() < right.mId.toUint32();
        });

        const std::size_t totalAvailable = loadedActorBases.size();
        const std::size_t totalNpcs = static_cast<std::size_t>(std::count_if(
            loadedActorBases.begin(), loadedActorBases.end(), [](const LoadedActorBase& actor) {
                return actor.mType == "NPC_";
            }));
        const std::size_t totalCreatures = totalAvailable - totalNpcs;
        const bool representativeVisualTypes
            = proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_REPRESENTATIVE_VISUAL_TYPES");
        std::vector<LoadedActorBase> selectionPool;
        if (representativeVisualTypes)
        {
            std::unordered_map<std::string, std::size_t> signatureIndices;
            selectionPool.reserve(loadedActorBases.size());
            for (const LoadedActorBase& actor : loadedActorBases)
            {
                const auto [it, inserted]
                    = signatureIndices.emplace(actor.mVisualSignature, selectionPool.size());
                if (inserted)
                    selectionPool.push_back(actor);
                else
                {
                    LoadedActorBase& current = selectionPool[it->second];
                    const std::size_t groupCount = current.mRepresentativeOfCount + 1;
                    if (actor.mRepresentativeScore > current.mRepresentativeScore
                        || (actor.mRepresentativeScore == current.mRepresentativeScore
                            && actor.mId.toUint32() < current.mId.toUint32()))
                        current = actor;
                    current.mRepresentativeOfCount = groupCount;
                }
            }
        }
        else
            selectionPool = loadedActorBases;
        const std::size_t distinctVisualTypes = selectionPool.size();
        const int configuredOffset = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_OFFSET");
        const int configuredLimit = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_LIMIT");
        const std::size_t offset = std::min<std::size_t>(
            configuredOffset >= 0 ? static_cast<std::size_t>(configuredOffset) : 0, selectionPool.size());
        const std::size_t remaining = selectionPool.size() - offset;
        const std::size_t selectedCount = configuredLimit > 0
            ? std::min<std::size_t>(static_cast<std::size_t>(configuredLimit), remaining)
            : remaining;

        std::vector<LoadedActorBase> selected;
        selected.reserve(selectedCount);
        selected.insert(selected.end(), selectionPool.begin() + offset, selectionPool.begin() + offset + selectedCount);
        proofActorBatchTargets.clear();
        proofActorBatchTargets.reserve(selected.size());
        for (const LoadedActorBase& actor : selected)
            proofActorBatchTargets.push_back(ESM::RefId(actor.mId).toDebugString());

        if (proofEnvEnabled("OPENMW_PROOF_ACTOR_BATCH_AUTO_FRAMES"))
        {
            const int configuredFirst = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_FIRST_FRAME");
            const int configuredStep = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_FRAMES_PER_ACTOR");
            const int poseFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_POSE_FRAMES");
            const int poseFrames = poseFramesEnv >= 1 ? poseFramesEnv : 12;
            const int poseWindow = static_cast<int>(proofActorPoseGroups.size() + 2) * poseFrames;
            const int firstFrame = configuredFirst >= 0
                ? configuredFirst
                : std::max<int>(static_cast<int>(frameNumber) + 60, std::max(0, proofSayFrame) + poseWindow + 60);
            const int framesPerActor = configuredStep >= 1 ? configuredStep : std::max(90, poseWindow + 60);
            proofScreenshotFrames.clear();
            proofScreenshotFrames.reserve(selected.size());
            for (std::size_t index = 0; index < selected.size(); ++index)
            {
                const std::uint64_t requested = static_cast<std::uint64_t>(firstFrame)
                    + static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(framesPerActor);
                proofScreenshotFrames.push_back(static_cast<int>(
                    std::min<std::uint64_t>(requested, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))));
            }
        }

        const auto writeJsonString = [](std::ostream& stream, std::string_view value) {
            stream << '"';
            for (unsigned char ch : value)
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
        };
        if (const char* rosterPath = std::getenv("OPENMW_PROOF_ACTOR_ROSTER_JSON"))
        {
            if (*rosterPath != '\0')
            {
                std::ofstream roster(rosterPath, std::ios::trunc);
                if (!roster)
                    Log(Debug::Error) << "FNV/ESM4 actor batch: failed to open roster path " << rosterPath;
                else
                {
                    roster << "{\n  \"schema\": \"nikami-fnv-loaded-actor-roster/v1\",\n"
                           << "  \"totalAvailable\": " << totalAvailable << ",\n"
                           << "  \"totalNpcs\": " << totalNpcs << ",\n"
                           << "  \"totalCreatures\": " << totalCreatures << ",\n"
                           << "  \"representativeVisualTypes\": "
                           << (representativeVisualTypes ? "true" : "false") << ",\n"
                           << "  \"distinctVisualTypes\": " << distinctVisualTypes << ",\n"
                           << "  \"offset\": " << offset << ",\n"
                           << "  \"selectedCount\": " << selected.size() << ",\n"
                           << "  \"actors\": [\n";
                    for (std::size_t index = 0; index < selected.size(); ++index)
                    {
                        const LoadedActorBase& actor = selected[index];
                        roster << "    {\"index\": " << index << ", \"type\": ";
                        writeJsonString(roster, actor.mType);
                        roster << ", \"form\": ";
                        writeJsonString(roster, ESM::RefId(actor.mId).toDebugString());
                        roster << ", \"editorId\": ";
                        writeJsonString(roster, actor.mEditorId);
                        roster << ", \"name\": ";
                         writeJsonString(roster, actor.mName);
                         roster << ", \"visualSignature\": ";
                         writeJsonString(roster, actor.mVisualSignature);
                         roster << ", \"selectedWeapon\": ";
                         writeJsonString(roster, actor.mSelectedWeapon);
                         roster << ", \"representativeOfCount\": " << actor.mRepresentativeOfCount
                                << ", \"representativeScore\": " << actor.mRepresentativeScore
                                << ", \"flags\": " << actor.mFlags << "}";
                        if (index + 1 != selected.size())
                            roster << ',';
                        roster << '\n';
                    }
                    roster << "  ]\n}\n";
                    Log(Debug::Info) << "FNV/ESM4 actor batch: wrote deterministic loaded roster path="
                                     << rosterPath << " count=" << selected.size();
                }
            }
        }

        for (std::size_t index = 0; index < selected.size(); ++index)
        {
            const LoadedActorBase& actor = selected[index];
            Log(Debug::Info) << "FNV/ESM4 actor roster: index=" << index << " type=" << actor.mType
                             << " form=" << ESM::RefId(actor.mId).toDebugString() << " editor=\""
                             << actor.mEditorId << "\" name=\"" << actor.mName << "\" selectedWeapon=\""
                             << actor.mSelectedWeapon << "\" representativeOfCount=" << actor.mRepresentativeOfCount
                             << " representativeScore=" << actor.mRepresentativeScore << " flags=0x" << std::hex
                             << actor.mFlags << std::dec;
        }
        Log(Debug::Info) << "FNV/ESM4 actor batch: loaded roster ready totalAvailable=" << totalAvailable
                          << " npcs=" << totalNpcs << " creatures=" << totalCreatures
                          << " representativeVisualTypes=" << representativeVisualTypes
                          << " distinctVisualTypes=" << distinctVisualTypes << " offset=" << offset
                          << " selected=" << selected.size()
                         << " screenshotFrames=" << proofScreenshotFrames.size();
        proofActorBaseRosterExpanded = true;
    }

    if (proofSidecarEnabled && proofSidecarPhase != FNVSidecarOpenMwPhase::Failed)
    {
        if (!proofSidecarSnapshot.mValid)
            failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault, "retail-payload-crc-or-length-invalid");
        else if ((proofSidecarSnapshot.mFlags & FNVSidecar::ErrorFlag) != 0
            || proofSidecarSnapshot.mState == FNVSidecar::State::Error)
        {
            proofSidecarPhase = FNVSidecarOpenMwPhase::Failed;
            if (!proofSidecarPeerErrorLogged)
            {
                proofSidecarPeerErrorLogged = true;
                Log(Debug::Error) << "FNV sidecar OpenMW: peer error code="
                                  << static_cast<std::uint32_t>(proofSidecarSnapshot.mErrorCode)
                                  << " message=\"" << proofSidecarSnapshot.mErrorMessage << "\"";
            }
        }
        else if (proofActorBaseRosterExpanded && proofSidecarSnapshot.mGeneration > proofSidecarGeneration)
        {
            std::string parseError;
            const std::optional<FNVSidecar::RetailAction> action
                = FNVSidecar::parseRetailAction(proofSidecarSnapshot, parseError);
            if (!action && (proofSidecarSnapshot.mFlags & FNVSidecar::RetailReadyFlag) != 0)
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, parseError);
            else if (!action)
            {
                // Retail increments generation before replacing its payload. A
                // frame may observe that intentional publication window; the
                // retail-ready flag is the fail-closed validation boundary.
            }
            else if (proofSidecarSnapshot.mGeneration != proofSidecarGeneration + 1)
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "noncontiguous-generation");
            else if (action->mActorIndex >= proofActorBatchTargets.size())
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "actor-index-out-of-openmw-roster");
            else if (proofSidecarSnapshot.mActionCount != proofActorPoseGroups.size()
                || action->mActionIndex >= proofActorPoseGroups.size())
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "action-count-or-index-mismatch");
            else
            {
                const std::string& expectedActionId = proofSidecarActionIds.empty()
                    ? proofActorPoseGroups[action->mActionIndex]
                    : proofSidecarActionIds[action->mActionIndex];
                const bool actionIdListValid = proofSidecarActionIds.empty()
                    || proofSidecarActionIds.size() == proofActorPoseGroups.size();
                const bool sequenceValid = proofSidecarGeneration == 0
                    ? action->mActorIndex == 0 && action->mActionIndex == 0
                    : ((action->mActorIndex == proofSidecarPreviousActor
                           && action->mActionIndex == proofSidecarPreviousAction + 1)
                        || (action->mActorIndex == proofSidecarPreviousActor + 1
                            && action->mActionIndex == 0));
                const std::optional<ESM::FormId> localBase
                    = parseProofFormId(proofActorBatchTargets[action->mActorIndex]);
                const bool baseMatches = localBase.has_value()
                    && (localBase->toUint32() & 0x00FFFFFFu)
                        == (action->mRetailBaseForm & 0x00FFFFFFu);
                if (!actionIdListValid)
                    failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "openmw-action-id-count-mismatch");
                else if (action->mActionId != expectedActionId)
                    failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "retail-openmw-action-id-mismatch");
                else if (!sequenceValid)
                    failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "actor-action-sequence-mismatch");
                else if (!baseMatches)
                    failFNVSidecar(FNVSidecar::ErrorCode::ActorUnavailable, "retail-openmw-base-form-mismatch");
                else
                {
                    proofSidecarAction = action;
                    proofSidecarGeneration = action->mGeneration;
                    proofSidecarPreviousActor = action->mActorIndex;
                    proofSidecarPreviousAction = action->mActionIndex;
                    proofSidecarActionStartFrame = -1;
                    proofSidecarCaptureStateStartFrame = -1;
                    proofSidecarActionPlayed = false;
                    proofSidecarScreenshotBaseline = {};
                    proofSidecarScreenshotCandidate = {};
                    proofSidecarScreenshotStableFrames = 0;
                    proofSidecarPhase = FNVSidecarOpenMwPhase::Staging;
                    proofScreenshotFrameIndex = action->mActorIndex;
                    Log(Debug::Info) << "FNV sidecar OpenMW: consumed retail action generation="
                                     << action->mGeneration << " actorIndex=" << action->mActorIndex
                                     << " actionIndex=" << action->mActionIndex << " actionId=\""
                                     << action->mActionId << "\" group=\""
                                     << proofActorPoseGroups[action->mActionIndex] << "\" requestedFrames="
                                     << action->mRequestedFrames << " requestedWeaponForm="
                                     << action->mRequestedWeaponForm;
                }
            }
        }
        else if (proofSidecarGeneration != 0 && proofSidecarSnapshot.mGeneration == proofSidecarGeneration
            && (proofSidecarSnapshot.mActorIndex != proofSidecarPreviousActor
                || proofSidecarSnapshot.mActionIndex != proofSidecarPreviousAction))
            failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault, "stable-generation-identity-changed");

        if (proofSidecarPhase != FNVSidecarOpenMwPhase::Failed
            && proofSidecarSnapshot.mDeadlineTickMs != 0 && proofSidecarGeneration != 0
            && proofSidecarSnapshot.mGeneration == proofSidecarGeneration
            && FNVSidecar::monotonicTickMilliseconds() > proofSidecarSnapshot.mDeadlineTickMs
            && (proofSidecarSnapshot.mFlags & FNVSidecar::CaptureAckFlag) == 0)
        {
            const FNVSidecar::ErrorCode code = proofSidecarPhase == FNVSidecarOpenMwPhase::Capturing
                ? FNVSidecar::ErrorCode::ScreenshotTimeout
                : (proofSidecarPhase == FNVSidecarOpenMwPhase::WaitingAdvance
                        ? FNVSidecar::ErrorCode::CaptureAckTimeout
                        : FNVSidecar::ErrorCode::OpenMwReadyTimeout);
            failFNVSidecar(code, "shared-deadline-expired");
        }

        if (!proofSidecarCompletionPublished
            && (proofSidecarSnapshot.mFlags & FNVSidecar::RetailCompleteFlag) != 0
            && proofSidecarPhase == FNVSidecarOpenMwPhase::WaitingAdvance)
        {
            if (proofSidecarClient.markComplete(frameNumber))
            {
                proofSidecarCompletionPublished = true;
                proofSidecarPhase = FNVSidecarOpenMwPhase::Complete;
                proofActorBatchCompletionLogged = true;
                proofActorBatchCompleteFrame = static_cast<int>(frameNumber);
                Log(Debug::Info) << "FNV sidecar OpenMW: sequence complete generation="
                                 << proofSidecarGeneration << " frame=" << frameNumber;
            }
            else
                failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault, "openmw-complete-publish-failed");
        }
    }

    const bool proofRequiresActorForScreenshot = std::getenv("OPENMW_PROOF_REQUIRE_ACTOR_FOR_SCREENSHOT") != nullptr;
    const bool proofSidecarActorRequested = !proofSidecarEnabled
        || (proofSidecarAction.has_value()
            && proofSidecarPhase != FNVSidecarOpenMwPhase::WaitingRetail
            && proofSidecarPhase != FNVSidecarOpenMwPhase::Failed
            && proofSidecarPhase != FNVSidecarOpenMwPhase::Complete);
    const bool proofActorBatchActive = !proofActorBatchTargets.empty()
        && proofScreenshotFrameIndex < proofActorBatchTargets.size() && proofSidecarActorRequested;
    // Native capture is asynchronous. Do not change the staged scene immediately after
    // arming it; advance only when the next target enters its warmup window.
    const int proofActorBatchWarmupFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_BATCH_WARMUP_FRAMES");
    const int proofActorBatchWarmupFrames
        = proofActorBatchWarmupFramesEnv >= 0 ? proofActorBatchWarmupFramesEnv : 30;
    const bool proofActorBatchTransitionReady = proofSidecarEnabled
        || proofActorBatchIndex == static_cast<std::size_t>(-1)
        || proofScreenshotFrameIndex == 0 || proofScreenshotFrameIndex >= proofScreenshotFrames.size()
        || static_cast<int>(frameNumber) + proofActorBatchWarmupFrames
            >= proofScreenshotFrames[proofScreenshotFrameIndex];
    if (proofActorBatchActive && proofActorBatchIndex != proofScreenshotFrameIndex
        && proofActorBatchTransitionReady)
    {
        if (!proofActorBatchPrevious.isEmpty() && mWorld != nullptr)
        {
            try
            {
                mWorld->disable(proofActorBatchPrevious);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof batch: failed to retire previous actor: " << e.what();
            }
        }
        proofActorBatchPrevious = MWWorld::Ptr();
        proofActorBatchIndex = proofScreenshotFrameIndex;
        proofActorBatchSelectedFrame = static_cast<int>(frameNumber);
        proofActorPoseBatchIndex = proofActorBatchIndex;
        proofActorPoseIndex = 0;
        proofActorPoseNextFrame = -1;
        proofActorActivePoseGroups = proofActorPoseAllAvailable
            ? std::vector<std::string>()
            : proofActorPoseGroups;
        proofActorPoseCycleComplete = !proofActorPoseSweepEnabled;
        proofActorPoseInventoryLogged = false;
        proofActorPosePlayed = 0;
        proofActorPoseSkipped = 0;
        proofSayQueued = false;
        proofActorCameraAligned = false;
        proofActorCameraAlignedFrame = -1;
        proofActorCameraAlignedScreenshotIndex = static_cast<std::size_t>(-1);
        proofActorAlignedScreenshotQueued = false;
        proofActorNeutralizedForCamera = false;
        proofActorStagedForCamera = false;
        proofActorSnappedToRenderGround = false;
        proofActorRenderGroundFreshBoundsLatched = false;
        proofActorStagedFrame = -1;
        proofActorSnappedFrame = -1;
        proofPinnedStagedActor = MWWorld::Ptr();
        proofPinnedStagedActorLastLogFrame = -1000000;
        proofActorScreenshotWaitLogged = false;
        proofActorScreenshotLastResolveFrame = -1;
        proofPinnedPlayerToActorView = false;
        Log(Debug::Info) << "FNV/ESM4 proof batch: selected actor index=" << proofActorBatchIndex
                         << " target=\"" << proofActorBatchTargets[proofActorBatchIndex] << "\"";
    }
    const char* proofSayActor = proofActorBatchActive
        ? proofActorBatchTargets[std::min(proofActorBatchIndex, proofActorBatchTargets.size() - 1)].c_str()
        : std::getenv("OPENMW_PROOF_SAY_ACTOR");
    const int proofActorResolveRetryFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_RESOLVE_RETRY_FRAMES");
    const int proofActorResolveRetryFrames = proofActorResolveRetryFramesEnv >= 0 ? proofActorResolveRetryFramesEnv : 30;
    const bool proofOrbitBurstPending = proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && !proofActorViewOrbitDegrees.empty()
        && proofActorCameraAlignedScreenshotIndex != proofScreenshotFrameIndex;
    const bool proofActorScreenshotNeedsResolveRaw = proofRequiresActorForScreenshot
        && (!proofActorCameraAligned || proofOrbitBurstPending)
        && proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofActorScreenshotNeedsResolve = proofActorScreenshotNeedsResolveRaw
        && (proofActorScreenshotLastResolveFrame < 0
            || frameNumber
                >= static_cast<unsigned>(proofActorScreenshotLastResolveFrame + proofActorResolveRetryFrames));
    const bool proofOrbitBurstAlignReached = proofOrbitBurstPending
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex])
        && (!proofRequiresActorForScreenshot || proofActorScreenshotNeedsResolve);
    const bool proofEarlyActorAlignmentPending = proofActorBatchActive
        && proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_EARLY_ALIGN") && !proofActorCameraAligned;
    const bool proofActorCameraAlignmentWindowOpen = proofSidecarEnabled || proofEarlyActorAlignmentPending || !proofActorBatchActive
        || proofScreenshotFrameIndex >= proofScreenshotFrames.size()
        || frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    if (proofEnvEnabled("OPENMW_PROOF_PIN_STAGED_ACTOR") && !proofPinnedStagedActor.isEmpty()
        && mWorld != nullptr)
    {
        try
        {
            const ESM::Position& current = proofPinnedStagedActor.getRefData().getPosition();
            const osg::Vec3f currentPosition(current.pos[0], current.pos[1], current.pos[2]);
            const osg::Vec3f currentRotation(current.rot[0], current.rot[1], current.rot[2]);
            const float positionDrift = (currentPosition - proofPinnedStagedActorPosition).length();
            const float rotationDrift = (currentRotation - proofPinnedStagedActorRotation).length();
            if (positionDrift > 0.001f)
            {
                proofPinnedStagedActor = mWorld->moveObject(proofPinnedStagedActor,
                    proofPinnedStagedActor.getCell(), proofPinnedStagedActorPosition, true, true);
            }
            if (rotationDrift > 0.00001f)
                mWorld->rotateObject(proofPinnedStagedActor, proofPinnedStagedActorRotation);
            if ((positionDrift > 0.001f || rotationDrift > 0.00001f)
                && static_cast<int>(frameNumber) - proofPinnedStagedActorLastLogFrame >= 30)
            {
                proofPinnedStagedActorLastLogFrame = static_cast<int>(frameNumber);
                Log(Debug::Info) << "FNV/ESM4 proof: restored pinned staged actor transform target=\""
                                 << (proofSayActor != nullptr ? proofSayActor : "") << "\" frame=" << frameNumber
                                 << " positionDrift=" << positionDrift << " rotationDrift=" << rotationDrift
                                 << " pos=(" << proofPinnedStagedActorPosition.x() << ","
                                 << proofPinnedStagedActorPosition.y() << "," << proofPinnedStagedActorPosition.z()
                                 << ") rot=(" << proofPinnedStagedActorRotation.x() << ","
                                 << proofPinnedStagedActorRotation.y() << ","
                                 << proofPinnedStagedActorRotation.z() << ")";
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: failed to restore pinned staged actor transform: "
                                << e.what();
        }
    }
    if ((!proofSayQueued || proofOrbitBurstAlignReached || proofActorScreenshotNeedsResolve
            || proofEarlyActorAlignmentPending)
        && (proofSidecarEnabled
            || (proofSayFrame >= 0 && frameNumber >= static_cast<unsigned>(proofSayFrame))))
    {
        const char* proofSayFile = std::getenv("OPENMW_PROOF_SAY_FILE");
        const char* proofSayTopic = std::getenv("OPENMW_PROOF_SAY_TOPIC");
        MWWorld::Ptr proofActor;
        if (proofSayActor != nullptr && *proofSayActor != '\0')
        {
            proofActor = resolveProofActor(proofSayActor);
            if (proofActorBatchActive && !proofActor.isEmpty() && !proofActor.getRefData().isEnabled()
                && mWorld != nullptr)
                mWorld->enable(proofActor);
            if (proofActorScreenshotNeedsResolve)
                proofActorScreenshotLastResolveFrame = static_cast<int>(frameNumber);
            Log(Debug::Info) << "FNV/ESM4 proof: resolved proof say actor \"" << proofSayActor
                             << "\" -> " << proofActor.toString();
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_SUPPRESS_ACTOR_AI") != nullptr
                && !proofActorNeutralizedForCamera)
            {
                try
                {
                    MWMechanics::CreatureStats& proofActorStats = proofActor.getClass().getCreatureStats(proofActor);
                    // A Fallout package can leave a scripted furniture animation running after the actor is moved.
                    // The capture copy must start from a deterministic neutral state; ordinary world actors keep
                    // their authored packages and animations because this path is proof-only.
                    proofActorStats.getAiSequence().clear();
                    proofActorStats.setAttackingOrSpell(false);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Fight, 0);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Flee, 0);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Alarm, 0);
                    proofActorStats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, false);
                    proofActorStats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceRun, false);
                    MWMechanics::Movement& proofMovement = proofActor.getClass().getMovementSettings(proofActor);
                    proofMovement.mPosition[0] = 0.f;
                    proofMovement.mPosition[1] = 0.f;
                    proofMovement.mPosition[2] = 0.f;
                    proofMovement.mRotation[0] = 0.f;
                    proofMovement.mRotation[1] = 0.f;
                    proofMovement.mRotation[2] = 0.f;
                    if (proofActor.getType() == ESM4::Npc::sRecordId)
                    {
                        MWClass::ESM4Npc::setFurnitureState(
                            proofActor, MWClass::FalloutFurnitureState::None);
                        MWClass::ESM4Npc::setFurniturePlacement(
                            proofActor, MWClass::FalloutFurniturePlacement {});

                        // Canonical actor proofs must stage the weapon authored for
                        // this exact NPC, not merely attach it in the renderer while
                        // leaving the mechanics state holstered. Conversely, an NPC
                        // with no authored equipped weapon must stay unarmed.
                        const ESM4::Weapon* equippedWeapon = MWClass::ESM4Npc::getEquippedWeapon(proofActor);
                        proofActorStats.setDrawState(equippedWeapon != nullptr
                                ? MWMechanics::DrawState::Weapon
                                : MWMechanics::DrawState::Nothing);
                    }
                    if (mMechanicsManager != nullptr)
                    {
                        mMechanicsManager->clearAnimationQueue(proofActor, true);
                        mMechanicsManager->forceStateUpdate(proofActor);
                    }
                    if (std::getenv("OPENMW_PROOF_DISABLE_ACTOR_COLLISION") != nullptr && mWorld != nullptr)
                        mWorld->setActorCollisionMode(proofActor, false, false);
                    Log(Debug::Info) << "FNV/ESM4 proof: suppressed proof actor AI and reset staged animation target=\""
                                     << proofSayActor << "\" ptr=" << proofActor.toString();
                    if (proofActor.getType() == ESM4::Npc::sRecordId)
                    {
                        const ESM4::Npc* npc = proofActor.get<ESM4::Npc>()->mBase;
                        const ESM4::Weapon* equippedWeapon = MWClass::ESM4Npc::getEquippedWeapon(proofActor);
                        const MWMechanics::DrawState terminalDrawState = proofActorStats.getDrawState();
                        Log(Debug::Info) << "FNV/ESM4 proof: phase=canonical-actor-terminal-identity"
                                         << " target=\"" << proofSayActor << "\""
                                         << " ref=" << proofActor.getCellRef().getRefNum().toString("FormId:")
                                         << " base=" << proofActor.getCellRef().getRefId().toDebugString()
                                         << " npc=\"" << (npc != nullptr ? npc->mEditorId : std::string()) << "\""
                                         << " npcForm=" << (npc != nullptr ? ESM::RefId(npc->mId) : ESM::RefId())
                                         << " equippedWeapon=" << (equippedWeapon != nullptr)
                                         << " weapon=\""
                                         << (equippedWeapon != nullptr ? equippedWeapon->mEditorId : std::string())
                                         << "\" weaponForm="
                                         << (equippedWeapon != nullptr ? ESM::RefId(equippedWeapon->mId) : ESM::RefId())
                                         << " terminalDrawState="
                                         << (terminalDrawState == MWMechanics::DrawState::Weapon
                                                 ? "Weapon"
                                                 : (terminalDrawState == MWMechanics::DrawState::Spell ? "Spell"
                                                                                                      : "Nothing"));
                    }
                    proofActorNeutralizedForCamera = true;
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to suppress proof actor AI target=\""
                                        << proofSayActor << "\": " << e.what();
                }
            }
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_STAGE_ACTOR") != nullptr
                && !proofActorStagedForCamera && mWorld != nullptr)
            {
                try
                {
                    proofActorStagedForCamera = stageProofActorForCamera(*mWorld, proofActor, proofSayActor);
                    if (proofActorStagedForCamera)
                    {
                        proofActorStagedFrame = static_cast<int>(frameNumber);
                        if (proofEnvEnabled("OPENMW_PROOF_PIN_STAGED_ACTOR"))
                        {
                            proofPinnedStagedActor = proofActor;
                            const ESM::Position& staged = proofActor.getRefData().getPosition();
                            proofPinnedStagedActorPosition.set(staged.pos[0], staged.pos[1], staged.pos[2]);
                            proofPinnedStagedActorRotation.set(staged.rot[0], staged.rot[1], staged.rot[2]);
                            Log(Debug::Info) << "FNV/ESM4 proof: pinned staged actor transform target=\""
                                             << proofSayActor << "\" pos=(" << staged.pos[0] << ","
                                             << staged.pos[1] << "," << staged.pos[2] << ") rot=("
                                             << staged.rot[0] << "," << staged.rot[1] << "," << staged.rot[2]
                                             << ") refScale=" << proofActor.getCellRef().getScale();
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to stage actor target=\""
                                        << proofSayActor << "\": " << e.what();
                    proofActorStagedForCamera = true;
                }
            }
            const bool proofActorStageRenderUpdateReady = !proofActorStagedForCamera
                || proofActorStagedFrame < 0 || static_cast<int>(frameNumber) > proofActorStagedFrame;
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_SNAP_ACTOR_TO_RENDER_GROUND") != nullptr
                && !proofActorSnappedToRenderGround && mWorld != nullptr && proofActorStageRenderUpdateReady)
            {
                try
                {
                    proofActorSnappedToRenderGround = snapProofActorToRenderGround(*mWorld, proofActor,
                        proofSayActor, proofActorRenderGroundFreshBoundsLatched);
                    if (proofActorSnappedToRenderGround)
                        proofActorSnappedFrame = static_cast<int>(frameNumber);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: render-ground snap failed target=\""
                                        << proofSayActor << "\": " << e.what();
                    proofActorSnappedToRenderGround = true;
                    proofActorSnappedFrame = static_cast<int>(frameNumber);
                }
            }
            else if (!proofActor.isEmpty()
                && std::getenv("OPENMW_PROOF_SNAP_ACTOR_TO_RENDER_GROUND") != nullptr
                && !proofActorSnappedToRenderGround && !proofActorStageRenderUpdateReady)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: render-ground snap waiting for post-stage render update target=\""
                                 << proofSayActor << "\" stagedFrame=" << proofActorStagedFrame
                                 << " frame=" << frameNumber;
            }
            if (!proofActor.isEmpty())
                logProofActorRenderBounds(proofActor, proofSayActor, "post-stage-snap");
            if (proofActorBatchActive && !proofActor.isEmpty())
                proofActorBatchPrevious = proofActor;

            const bool proofNeutralActorPreviewRequested = proofEnvEnabled("OPENMW_PROOF_NEUTRAL_ACTOR_PREVIEW")
                || proofEnvEnabled("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW");
            if (!proofActor.isEmpty() && proofNeutralActorPreviewRequested && !proofNeutralActorPreviewAttempted)
            {
                proofNeutralActorPreviewAttempted = true;
                try
                {
                    osg::Group* sceneRoot = mViewer != nullptr && mViewer->getSceneData() != nullptr
                        ? mViewer->getSceneData()->asGroup()
                        : nullptr;
                    if (sceneRoot == nullptr)
                        throw std::runtime_error("missing viewer scene root");

                    proofNeutralActorPreviewRoot = new osg::Group;
                    proofNeutralActorPreviewRoot->setName("FNV Neutral Actor Preview Root");
                    proofNeutralActorPreviewRoot->setNodeMask(MWRender::Mask_RenderToTexture);
                    sceneRoot->addChild(proofNeutralActorPreviewRoot);

                    const std::string profile = [] {
                        const char* value = std::getenv("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE");
                        return value != nullptr && value[0] != '\0' ? std::string(value) : std::string("full-body");
                    }();
                    const float zoom = readProofFloat("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_ZOOM", 1.f);
                    proofNeutralActorPreviews.clear();
                    proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                        proofNeutralActorPreviewRoot, mResourceSystem.get(), proofActor,
                        MWRender::FalloutActorPreview::ViewMode::Front, zoom, profile));
                    proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                        proofNeutralActorPreviewRoot, mResourceSystem.get(), proofActor,
                        MWRender::FalloutActorPreview::ViewMode::Left, zoom, profile));
                    proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                        proofNeutralActorPreviewRoot, mResourceSystem.get(), proofActor,
                        MWRender::FalloutActorPreview::ViewMode::Top, zoom, profile));
                    for (const std::unique_ptr<MWRender::FalloutActorPreview>& preview : proofNeutralActorPreviews)
                    {
                        preview->rebuild();
                        preview->redraw();
                    }
                    proofNeutralActorPreviewComposite = createFalloutNeutralActorPreviewComposite(proofNeutralActorPreviews);
                    sceneRoot->addChild(proofNeutralActorPreviewComposite);
                    proofNeutralActorPreviewReady = true;
                    Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview assembled target=\""
                                     << proofSayActor << "\" ptr=" << proofActor.toString()
                                     << " panes=" << proofNeutralActorPreviews.size()
                                     << " profile=" << profile
                                     << " zoom=" << zoom
                                     << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: neutral actor preview failed target=\""
                                        << proofSayActor << "\" error=\"" << e.what()
                                        << "\" runtime=known-blocked gate=runtime-neutral-actor-preview";
                }
            }

            if (proofNeutralActorPreviewRequested && proofNeutralActorPreviewReady
                && !proofNeutralActorPreviewIsolationApplied)
            {
                if (mWindowManager)
                    mWindowManager->setHudVisibility(false);
                if (mWorld != nullptr)
                {
                    if (MWRender::RenderingManager* rendering = mWorld->getRenderingManager())
                    {
                        rendering->setWaterEnabled(false);
                        rendering->setSkyEnabled(false);
                        const MWWorld::Ptr player = mWorld->getPlayerPtr();
                        if (!player.isEmpty() && player.isInCell() && player.getCell() != nullptr
                            && player.getCell()->getCell() != nullptr)
                            rendering->enableTerrain(false, player.getCell()->getCell()->getWorldSpace());
                    }
                }
                if (mViewer && mViewer->getCamera())
                {
                    mViewer->getCamera()->setClearColor(osg::Vec4(0.22f, 0.23f, 0.24f, 1.f));
                    mViewer->getCamera()->setCullMask(MWRender::Mask_RenderToTexture);
                    mViewer->getCamera()->setCullMaskLeft(MWRender::Mask_RenderToTexture);
                    mViewer->getCamera()->setCullMaskRight(MWRender::Mask_RenderToTexture);
                }
                proofActorCameraAligned = true;
                proofActorCameraAlignedFrame = static_cast<int>(frameNumber);
                proofActorCameraAlignedScreenshotIndex = proofScreenshotFrameIndex;
                proofNeutralActorPreviewIsolationApplied = true;
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview isolation target=\""
                                 << proofSayActor
                                 << "\" cullMask=Mask_RenderToTexture hud=hidden sky=hidden water=hidden"
                                 << " terrain=hidden runtime=runtime-supported gate=runtime-neutral-actor-preview";
            }

            const bool proofActorRenderGroundReady
                = std::getenv("OPENMW_PROOF_SNAP_ACTOR_TO_RENDER_GROUND") == nullptr
                || (proofActorSnappedToRenderGround && proofActorSnappedFrame >= 0
                    && static_cast<int>(frameNumber) > proofActorSnappedFrame);
            if (!proofActor.isEmpty() && !proofNeutralActorPreviewReady && proofActorCameraAlignmentWindowOpen
                && !proofActorRenderGroundReady
                && std::getenv("OPENMW_PROOF_ALIGN_PLAYER_TO_ACTOR") != nullptr)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: actor camera waiting for coherent render-ground state target=\""
                                 << proofSayActor << "\" snapped=" << proofActorSnappedToRenderGround
                                 << " snappedFrame=" << proofActorSnappedFrame << " frame=" << frameNumber;
            }
            if (!proofActor.isEmpty() && !proofNeutralActorPreviewReady && proofActorCameraAlignmentWindowOpen
                && proofActorRenderGroundReady
                && std::getenv("OPENMW_PROOF_ALIGN_PLAYER_TO_ACTOR") != nullptr)
            {
                const ESM::Position& actorPos = proofActor.getRefData().getPosition();
                float offsetX = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_X", 0.f);
                float offsetY = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y", -220.f);
                const float offsetZ = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z", 20.f);
                const float targetZ = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_TARGET_Z", 120.f);
                const float cameraDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE", 0.f);
                const float cameraPitch = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_PITCH", 0.f);
                const bool staticDialogueCamera = std::getenv("OPENMW_PROOF_ACTOR_VIEW_STATIC_CAMERA") != nullptr;
                const bool replayRetailAbsoluteCamera
                    = proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_REPLAY_RETAIL_ABSOLUTE_CAMERA");
                const bool requireActorForScreenshot = std::getenv("OPENMW_PROOF_REQUIRE_ACTOR_FOR_SCREENSHOT") != nullptr;
                bool useFaceAxisCamera = false;
                osg::Vec2f faceAxis(0.f, 0.f);
                osg::Vec3d actorAim(actorPos.pos[0], actorPos.pos[1], actorPos.pos[2] + targetZ);
                double cameraZ = actorPos.pos[2] + offsetZ;
                const bool useRenderBounds = std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_RENDER_BOUNDS") != nullptr;
                const bool useFaceBounds = std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_BOUNDS") != nullptr;
                const bool fullBodyView = std::getenv("OPENMW_PROOF_ACTOR_VIEW_FULL_BODY") != nullptr;
                bool actorViewBoundsAccepted = false;
                bool actorViewScaleAccepted = true;
                osg::BoundingBox actorViewRenderBounds;
                int actorViewVisibilityBlockers = -1;
                const auto isProofBoundsNearActor = [&](const osg::BoundingBox& bounds, const char* label) {
                    const double dx = bounds.center().x() - actorPos.pos[0];
                    const double dy = bounds.center().y() - actorPos.pos[1];
                    const double dz = bounds.center().z() - actorPos.pos[2];
                    const double centerDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const osg::Vec3d boundsSize(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(),
                        bounds.zMax() - bounds.zMin());
                    const double boundsDiagonal = boundsSize.length();
                    const float maxCenterDistance
                        = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_MAX_BOUNDS_CENTER_DISTANCE", 1000.f);
                    const float maxCenterDiagonals
                        = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_MAX_BOUNDS_CENTER_DIAGONALS", 2.f);
                    const float minRelativeAllowance
                        = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_MIN_RELATIVE_BOUNDS_ALLOWANCE", 64.f);
                    const double relativeAllowance
                        = std::max(static_cast<double>(minRelativeAllowance), boundsDiagonal * maxCenterDiagonals);
                    const double allowedCenterDistance
                        = std::min(static_cast<double>(maxCenterDistance), relativeAllowance);
                    if (centerDistance <= allowedCenterDistance)
                        return true;
                    Log(Debug::Warning) << "FNV/ESM4 proof: actor " << label
                                        << " bounds rejected as stale/far target=\"" << proofSayActor
                                        << "\" center=(" << bounds.center().x() << "," << bounds.center().y()
                                        << "," << bounds.center().z() << ") actorPos=(" << actorPos.pos[0] << ","
                                        << actorPos.pos[1] << "," << actorPos.pos[2] << ") distance="
                                        << centerDistance << " boundsSize=(" << boundsSize.x() << ","
                                        << boundsSize.y() << "," << boundsSize.z() << ") boundsDiagonal="
                                        << boundsDiagonal << " maxCenterDiagonals=" << maxCenterDiagonals
                                        << " absoluteMax=" << maxCenterDistance << " allowed="
                                        << allowedCenterDistance;
                    return false;
                };
                if (useRenderBounds && proofActor.getRefData().getBaseNode() != nullptr)
                {
                    osg::ComputeBoundsVisitor boundsVisitor;
                    boundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
                    proofActor.getRefData().getBaseNode()->accept(boundsVisitor);
                    const osg::BoundingBox bounds = boundsVisitor.getBoundingBox();
                    if (bounds.valid() && isProofBoundsNearActor(bounds, "render"))
                    {
                        const float focusPercent = readProofFloat(
                            "OPENMW_PROOF_ACTOR_VIEW_BOUNDS_FOCUS", fullBodyView ? 0.5f : 0.72f);
                        const double focusZ = bounds.zMin() + (bounds.zMax() - bounds.zMin()) * focusPercent;
                        actorAim = osg::Vec3d(bounds.center().x(), bounds.center().y(), focusZ);
                        cameraZ = focusZ + (offsetZ - targetZ);
                        actorViewBoundsAccepted = true;
                        actorViewRenderBounds = bounds;
                        const float refScale = proofActor.getCellRef().getScale();
                        osg::Vec3f expectedRenderRootScale(refScale, refScale, refScale);
                        proofActor.getClass().adjustScale(proofActor, expectedRenderRootScale, true);
                        const osg::Vec3f renderRootScale
                            = proofActor.getRefData().getBaseNode()->getScale();
                        const float scaleTolerance
                            = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_SCALE_TOLERANCE", 0.001f);
                        actorViewScaleAccepted
                            = std::abs(renderRootScale.x() - expectedRenderRootScale.x()) <= scaleTolerance
                            && std::abs(renderRootScale.y() - expectedRenderRootScale.y()) <= scaleTolerance
                            && std::abs(renderRootScale.z() - expectedRenderRootScale.z()) <= scaleTolerance;
                        Log(Debug::Info) << "FNV/ESM4 proof: actor render bounds target=\"" << proofSayActor
                                         << "\" min=(" << bounds.xMin() << "," << bounds.yMin() << ","
                                         << bounds.zMin() << ") max=(" << bounds.xMax() << "," << bounds.yMax()
                                         << "," << bounds.zMax() << ") focus=(" << actorAim.x() << ","
                                         << actorAim.y() << "," << actorAim.z() << ") size=("
                                         << bounds.xMax() - bounds.xMin() << "," << bounds.yMax() - bounds.yMin()
                                         << "," << bounds.zMax() - bounds.zMin() << ") cameraZ=" << cameraZ;
                        Log(actorViewScaleAccepted ? Debug::Info : Debug::Error)
                            << "FNV/ESM4 proof: actor assembled scale gate target=\"" << proofSayActor
                            << "\" refScale=" << refScale << " expectedRenderRootScale=("
                            << expectedRenderRootScale.x() << "," << expectedRenderRootScale.y() << ","
                            << expectedRenderRootScale.z() << ") renderRootScale=(" << renderRootScale.x() << ","
                            << renderRootScale.y() << "," << renderRootScale.z() << ") tolerance=" << scaleTolerance
                            << " accepted=" << actorViewScaleAccepted;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor render bounds invalid target=\""
                                            << proofSayActor << "\"";
                }
                if (useFaceBounds && proofActor.getRefData().getBaseNode() != nullptr)
                {
                    const osg::Vec3d renderBoundsAim = actorAim;
                    const double renderBoundsCameraZ = cameraZ;
                    const bool renderBoundsAccepted = actorViewBoundsAccepted;
                    FalloutProofFaceBoundsVisitor faceBoundsVisitor;
                    proofActor.getRefData().getBaseNode()->accept(faceBoundsVisitor);
                    const osg::BoundingBox bounds = faceBoundsVisitor.getBounds();
                    if (bounds.valid() && isProofBoundsNearActor(bounds, "face"))
                    {
                        actorAim = osg::Vec3d(bounds.center().x(), bounds.center().y(), bounds.center().z());
                        actorViewBoundsAccepted = true;
                        const osg::Vec3d headCenter = faceBoundsVisitor.getHeadCenter();
                        const osg::Vec3d featureCenter = faceBoundsVisitor.getFeatureCenter();
                        const osg::Vec3d faceVector = featureCenter - headCenter;
                        const osg::Vec3d headForward = faceBoundsVisitor.getHeadForward();
                        if (faceBoundsVisitor.getHeadBoneMatched() > 0)
                        {
                            const float forwardFocus
                                = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_HEAD_FORWARD_FOCUS", 4.f);
                            actorAim = headCenter + headForward * forwardFocus;
                        }
                        else if (faceBoundsVisitor.getHeadMatched() > 0
                            && faceBoundsVisitor.getFeatureMatched() > 0)
                        {
                            const float featureFocus
                                = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FEATURE_FOCUS", 0.3f);
                            actorAim = headCenter + faceVector * featureFocus;
                        }
                        else if (faceBoundsVisitor.getFeatureMatched() > 0)
                            actorAim = featureCenter;
                        else if (faceBoundsVisitor.getHeadMatched() > 0)
                            actorAim = headCenter;
                        cameraZ = actorAim.z() + (offsetZ - targetZ);
                        const double faceVectorPlanar
                            = std::sqrt(faceVector.x() * faceVector.x() + faceVector.y() * faceVector.y());
                        const double headForwardPlanar
                            = std::sqrt(headForward.x() * headForward.x() + headForward.y() * headForward.y());
                        if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_AXIS") != nullptr
                            && faceBoundsVisitor.getHeadBoneMatched() > 0 && headForwardPlanar > 1e-3)
                        {
                            faceAxis.set(static_cast<float>(headForward.x() / headForwardPlanar),
                                static_cast<float>(headForward.y() / headForwardPlanar));
                            useFaceAxisCamera = true;
                        }
                        else if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_AXIS") != nullptr
                            && faceBoundsVisitor.getHeadMatched() > 0 && faceBoundsVisitor.getFeatureMatched() > 0
                            && faceVectorPlanar > 1e-3)
                        {
                            faceAxis.set(static_cast<float>(faceVector.x() / faceVectorPlanar),
                                static_cast<float>(faceVector.y() / faceVectorPlanar));
                            useFaceAxisCamera = true;
                        }
                        Log(Debug::Info) << "FNV/ESM4 proof: actor face bounds target=\"" << proofSayActor
                                         << "\" matched=" << faceBoundsVisitor.getMatched() << " min=("
                                         << bounds.xMin() << "," << bounds.yMin() << "," << bounds.zMin()
                                         << ") max=(" << bounds.xMax() << "," << bounds.yMax() << ","
                                         << bounds.zMax() << ") focus=(" << actorAim.x() << "," << actorAim.y()
                                         << "," << actorAim.z() << ") cameraZ=" << cameraZ
                                         << " headMatched=" << faceBoundsVisitor.getHeadMatched()
                                         << " headBoneMatched=" << faceBoundsVisitor.getHeadBoneMatched()
                                         << " featureMatched=" << faceBoundsVisitor.getFeatureMatched()
                                         << " headCenter=(" << headCenter.x() << "," << headCenter.y() << ","
                                         << headCenter.z() << ") featureCenter=(" << featureCenter.x() << ","
                                         << featureCenter.y() << "," << featureCenter.z() << ") headForward=("
                                         << headForward.x() << "," << headForward.y() << "," << headForward.z()
                                         << ") faceAxis=("
                                         << faceAxis.x() << "," << faceAxis.y()
                                         << ") useFaceAxisCamera=" << useFaceAxisCamera;
                        if (fullBodyView && renderBoundsAccepted)
                        {
                            actorAim = renderBoundsAim;
                            cameraZ = renderBoundsCameraZ;
                            actorViewBoundsAccepted = true;
                        }
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor face bounds invalid target=\""
                                            << proofSayActor << "\" matched="
                                            << faceBoundsVisitor.getMatched();
                }
                if (!useFaceAxisCamera
                    && std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_HEAD_POSE_AXIS") != nullptr)
                {
                    osg::Vec3d headCenter;
                    osg::Vec3d headForward;
                    if (resolveFalloutProofHeadPose(proofActor, headCenter, headForward))
                    {
                        const double headForwardPlanar
                            = std::sqrt(headForward.x() * headForward.x() + headForward.y() * headForward.y());
                        bool credibleHeadPose = true;
                        // A humanoid head must sit in the upper half of the assembled
                        // bounds.  Creature rigs are not required to follow that
                        // convention: robots commonly expose a useful authored view
                        // axis on a screen/wheel anchor below the visual midpoint.
                        // Rejecting that anchor made the camera fall back to actor yaw
                        // and photographed the back of otherwise valid creature rigs.
                        if (fullBodyView && actorViewRenderBounds.valid()
                            && proofActor.getType() == ESM4::Npc::sRecordId)
                        {
                            const double boundsHeight
                                = actorViewRenderBounds.zMax() - actorViewRenderBounds.zMin();
                            const double minimumHeadFraction = readProofFloat(
                                "OPENMW_PROOF_ACTOR_VIEW_MIN_HEAD_HEIGHT_FRACTION", 0.5f);
                            const double minimumHeadZ
                                = actorViewRenderBounds.zMin() + boundsHeight * minimumHeadFraction;
                            credibleHeadPose = headCenter.z() >= minimumHeadZ;
                            if (!credibleHeadPose)
                            {
                                Log(Debug::Warning) << "FNV/ESM4 proof: rejected implausible head-pose camera axis target=\""
                                                    << proofSayActor << "\" headZ=" << headCenter.z()
                                                    << " minimumHeadZ=" << minimumHeadZ
                                                    << " boundsZ=(" << actorViewRenderBounds.zMin() << ","
                                                    << actorViewRenderBounds.zMax() << ")";
                            }
                        }
                        if (credibleHeadPose && headForwardPlanar > 1e-3)
                        {
                            const float forwardFocus
                                = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_HEAD_FORWARD_FOCUS", 4.f);
                            if (!fullBodyView || !actorViewBoundsAccepted)
                            {
                                actorAim = headCenter + headForward * forwardFocus;
                                cameraZ = actorAim.z() + (offsetZ - targetZ);
                            }
                            faceAxis.set(static_cast<float>(headForward.x() / headForwardPlanar),
                                static_cast<float>(headForward.y() / headForwardPlanar));
                            useFaceAxisCamera = true;
                            if (!fullBodyView || actorViewRenderBounds.valid())
                                actorViewBoundsAccepted = true;
                            Log(Debug::Info) << "FNV/ESM4 proof: actor head-pose camera axis target=\""
                                             << proofSayActor << "\" headCenter=(" << headCenter.x() << ","
                                             << headCenter.y() << "," << headCenter.z() << ") headForward=("
                                             << headForward.x() << "," << headForward.y() << ","
                                             << headForward.z() << ") faceAxis=(" << faceAxis.x() << ","
                                             << faceAxis.y() << ")";
                        }
                    }
                }
                if (fullBodyView && actorViewRenderBounds.valid())
                {
                    // Retail does not put the eye on the visual centerline of a low creature.  Keep the aim
                    // tied to the assembled box, but give the camera a small ground-relative elevation so the
                    // road/terrain cannot erase the lower half of the actor.  Tall NPCs and robots naturally
                    // retain their bounds-center eye because their center already exceeds this floor.
                    const double groundZ = actorPos.pos[2];
                    const double minimumAimZ = groundZ
                        + readProofFloat("OPENMW_PROOF_ACTOR_VIEW_LOW_RIG_AIM_ABOVE_GROUND", 16.f);
                    const double minimumCameraZ = groundZ
                        + readProofFloat("OPENMW_PROOF_ACTOR_VIEW_LOW_RIG_CAMERA_ABOVE_GROUND", 48.f);
                    actorAim.z() = std::max<double>(actorViewRenderBounds.center().z(), minimumAimZ);
                    cameraZ = std::max(actorAim.z(), minimumCameraZ);
                    Log(Debug::Info) << "FNV/ESM4 proof: actor full-body ground-relative vertical fit target=\""
                                     << proofSayActor << "\" groundZ=" << groundZ
                                     << " boundsCenterZ=" << actorViewRenderBounds.center().z()
                                     << " minimumAimZ=" << minimumAimZ << " minimumCameraZ=" << minimumCameraZ
                                     << " actorAimZ=" << actorAim.z() << " cameraZ=" << cameraZ;
                }
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_ACTOR_FACING") != nullptr)
                {
                    float frontDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FRONT_DISTANCE", 0.f);
                    if (!proofActorViewFrontDistances.empty())
                    {
                        const std::size_t distanceIndex = std::min(
                            proofScreenshotFrameIndex, proofActorViewFrontDistances.size() - 1);
                        frontDistance = proofActorViewFrontDistances[distanceIndex];
                    }
                    if (fullBodyView && actorViewRenderBounds.valid())
                    {
                        const float boundsHeight = actorViewRenderBounds.zMax() - actorViewRenderBounds.zMin();
                        const float boundsDistanceScale
                            = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FULL_BODY_DISTANCE_SCALE", 1.35f);
                        frontDistance = std::max(frontDistance, boundsHeight * boundsDistanceScale);

                        // Height alone cannot fit broad or long creatures.  A bark scorpion is only about
                        // 156 units tall but its animated box is roughly 376x353, so the old 220-unit camera
                        // landed inside that box.  Fit the sphere enclosing every bounds corner against the
                        // active projection's tighter FOV (including the requested screen margin).  This is
                        // orientation independent and therefore remains safe while the semantic front axis is
                        // changing from one creature skeleton convention to another.
                        const double radiusX = std::max(
                            std::abs(actorViewRenderBounds.xMin() - actorAim.x()),
                            std::abs(actorViewRenderBounds.xMax() - actorAim.x()));
                        const double radiusY = std::max(
                            std::abs(actorViewRenderBounds.yMin() - actorAim.y()),
                            std::abs(actorViewRenderBounds.yMax() - actorAim.y()));
                        const double radiusZ = std::max(
                            std::abs(actorViewRenderBounds.zMin() - actorAim.z()),
                            std::abs(actorViewRenderBounds.zMax() - actorAim.z()));
                        const double boundsRadius
                            = std::sqrt(radiusX * radiusX + radiusY * radiusY + radiusZ * radiusZ);
                        const float fullBodyMargin
                            = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FULL_BODY_MARGIN", 0.03f);
                        const double ndcLimit = std::clamp(1.0 - 2.0 * static_cast<double>(fullBodyMargin), 0.1, 1.0);
                        double effectiveTangent = 0.0;
                        if (mViewer != nullptr && mViewer->getCamera() != nullptr)
                        {
                            const osg::Matrixd projection = mViewer->getCamera()->getProjectionMatrix();
                            const double projectionX = std::abs(projection(0, 0));
                            const double projectionY = std::abs(projection(1, 1));
                            if (projectionX > 1e-6 && projectionY > 1e-6)
                                effectiveTangent = std::min(1.0 / projectionX, 1.0 / projectionY) * ndcLimit;
                        }
                        const double fitFactor = effectiveTangent > 1e-6
                            ? std::sqrt(1.0 + 1.0 / (effectiveTangent * effectiveTangent))
                            : 2.2;
                        const double fitPadding
                            = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FULL_BODY_FIT_PADDING", 1.02f);
                        const float fittedDistance
                            = static_cast<float>(boundsRadius * fitFactor * fitPadding);
                        const float requestedDistance = frontDistance;
                        const bool creatureAutoFit
                            = proofActor.getType() == ESM::REC_CREA4
                            && proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_CREATURE_AUTO_FIT");
                        frontDistance = creatureAutoFit
                            ? fittedDistance
                            : std::max(frontDistance, fittedDistance);
                        Log(Debug::Info) << "FNV/ESM4 proof: actor full-body distance fit target=\""
                                         << proofSayActor << "\" boundsRadius=" << boundsRadius
                                         << " effectiveTangent=" << effectiveTangent
                                         << " fitFactor=" << fitFactor << " fittedDistance=" << fittedDistance
                                         << " requestedDistance=" << requestedDistance
                                         << " creatureAutoFit=" << creatureAutoFit
                                         << " selectedDistance=" << frontDistance;
                    }
                    if (frontDistance <= 0.f)
                    {
                        frontDistance = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                        if (frontDistance <= 0.f)
                            frontDistance = 220.f;
                    }
                    const float actorYaw = actorPos.rot[2];
                    // Fallout creature skeletons that do not expose a usable
                    // face/head axis are authored facing the opposite planar
                    // direction from the humanoid actor convention.  Prefer
                    // geometry-derived face axes; only invert the fallback.
                    const bool falloutCreatureFacingFallback
                        = !useFaceAxisCamera && proofActor.getType() == ESM::REC_CREA4;
                    const float frontSign
                        = (std::getenv("OPENMW_PROOF_ACTOR_VIEW_FALLOUT_FRONT") != nullptr
                              || falloutCreatureFacingFallback)
                        ? -1.f
                        : 1.f;
                    if (useFaceAxisCamera)
                    {
                        offsetX = faceAxis.x() * frontDistance;
                        offsetY = faceAxis.y() * frontDistance;
                    }
                    else
                    {
                        offsetX = std::sin(actorYaw) * frontDistance * frontSign;
                        offsetY = std::cos(actorYaw) * frontDistance * frontSign;
                    }
                    float orbitDegrees = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_ORBIT_DEGREES", 0.f);
                    if (!proofActorViewOrbitDegrees.empty())
                    {
                        const std::size_t orbitIndex = std::min(
                            proofScreenshotFrameIndex, proofActorViewOrbitDegrees.size() - 1);
                        orbitDegrees = proofActorViewOrbitDegrees[orbitIndex];
                    }
                    if (std::abs(orbitDegrees) > 1e-3f)
                    {
                        const float orbitRadians = orbitDegrees * static_cast<float>(osg::PI) / 180.f;
                        const float cosOrbit = std::cos(orbitRadians);
                        const float sinOrbit = std::sin(orbitRadians);
                        const float rotatedX = offsetX * cosOrbit - offsetY * sinOrbit;
                        const float rotatedY = offsetX * sinOrbit + offsetY * cosOrbit;
                        offsetX = rotatedX;
                        offsetY = rotatedY;
                    }
                    const float openMwForwardX = std::sin(actorYaw);
                    const float openMwForwardY = std::cos(actorYaw);
                    const float falloutForwardX = -openMwForwardX;
                    const float falloutForwardY = -openMwForwardY;
                    const float offsetLength = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                    const float cameraDirX = offsetLength > 0.f ? offsetX / offsetLength : 0.f;
                    const float cameraDirY = offsetLength > 0.f ? offsetY / offsetLength : 0.f;
                    const float openMwFrontDot = cameraDirX * openMwForwardX + cameraDirY * openMwForwardY;
                    const float falloutFrontDot = cameraDirX * falloutForwardX + cameraDirY * falloutForwardY;
                    Log(Debug::Info) << "FNV/ESM4 proof: actor-facing camera offset target=\"" << proofSayActor
                                     << "\" actorYaw=" << actorYaw << " frontDistance=" << frontDistance
                                     << " frontSign=" << frontSign
                                     << " useFaceAxisCamera=" << useFaceAxisCamera
                                     << " orbitDegrees=" << orbitDegrees
                                     << " offset=(" << offsetX << "," << offsetY << "," << offsetZ
                                     << ") cameraDir=(" << cameraDirX << "," << cameraDirY
                                     << ") openMwFrontDot=" << openMwFrontDot
                                     << " falloutFrontDot=" << falloutFrontDot;
                }
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                osg::Vec3f targetPos(
                    static_cast<float>(actorAim.x() + offsetX), static_cast<float>(actorAim.y() + offsetY),
                    static_cast<float>(cameraZ));
                if (replayRetailAbsoluteCamera)
                {
                    targetPos.set(readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RETAIL_CAMERA_X", targetPos.x()),
                        readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RETAIL_CAMERA_Y", targetPos.y()),
                        readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RETAIL_CAMERA_Z", targetPos.z()));
                    cameraZ = targetPos.z();
                    Log(Debug::Info) << "FNV/ESM4 proof: replaying retail absolute actor camera target=\""
                                     << proofSayActor << "\" pos=(" << targetPos.x() << "," << targetPos.y()
                                     << "," << targetPos.z() << ") aim=(" << actorAim.x() << "," << actorAim.y()
                                     << "," << actorAim.z() << ")";
                }
                if (!replayRetailAbsoluteCamera
                    && std::getenv("OPENMW_PROOF_ACTOR_VIEW_RAYCAST_BACKOFF") != nullptr)
                {
                    const MWPhysics::RayCastingInterface* rayCasting = mWorld->getRayCasting();
                    if (rayCasting != nullptr)
                    {
                        const osg::Vec3f focus(
                            static_cast<float>(actorAim.x()), static_cast<float>(actorAim.y()),
                            static_cast<float>(actorAim.z()));
                        const osg::Vec3f ray = targetPos - focus;
                        const float rayLength = ray.length();
                        if (rayLength > 1e-3f)
                        {
                            const osg::Vec3f rayDirection = ray / rayLength;
                            const int mask = MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                                | MWPhysics::CollisionType_Door;
                            const MWPhysics::RayCastingResult rayResult
                                = rayCasting->castRay(focus, targetPos, { proofActor }, {}, mask);
                            if (rayResult.mHit)
                            {
                                const float clearance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_RAYCAST_CLEARANCE", 24.f);
                                const osg::Vec3f adjusted = rayResult.mHitPos - rayDirection * clearance;
                                const float adjustedDistance = (adjusted - focus).length();
                                if (adjustedDistance > 32.f)
                                {
                                    Log(Debug::Info)
                                        << "FNV/ESM4 proof: actor orbit camera raycast adjusted target=\""
                                        << proofSayActor << "\" hit=(" << rayResult.mHitPos.x() << ","
                                        << rayResult.mHitPos.y() << "," << rayResult.mHitPos.z() << ") from=("
                                        << targetPos.x() << "," << targetPos.y() << "," << targetPos.z()
                                        << ") to=(" << adjusted.x() << "," << adjusted.y() << ","
                                        << adjusted.z() << ")";
                                    targetPos = adjusted;
                                    cameraZ = targetPos.z();
                                }
                                else
                                {
                                    Log(Debug::Warning)
                                        << "FNV/ESM4 proof: actor orbit camera raycast hit too close target=\""
                                        << proofSayActor << "\" hit=(" << rayResult.mHitPos.x() << ","
                                        << rayResult.mHitPos.y() << "," << rayResult.mHitPos.z() << ")";
                                }
                            }
                            else
                            {
                                Log(Debug::Info) << "FNV/ESM4 proof: actor orbit camera raycast clear target=\""
                                                 << proofSayActor << "\"";
                            }
                        }
                    }
                }
                if (!replayRetailAbsoluteCamera)
                {
                    if (selectProofActorCameraByOrbitRays(*mWorld, proofActor, proofSayActor, actorAim, targetPos))
                        cameraZ = targetPos.z();
                    if (adjustProofActorCameraByRenderRay(*mWorld, proofActor, proofSayActor, actorAim, targetPos))
                        cameraZ = targetPos.z();
                }
                if (fullBodyView && actorViewRenderBounds.valid())
                {
                    actorViewVisibilityBlockers = raiseProofActorCameraForClearVisibility(
                        *mWorld, proofActor, proofSayActor, actorViewRenderBounds, targetPos);
                    cameraZ = targetPos.z();
                }
                osg::Vec3f playerTargetPos(targetPos);
                const bool pinPlayerToActorView = std::getenv("OPENMW_PROOF_PIN_PLAYER_TO_ACTOR_VIEW") != nullptr;
                if (!staticDialogueCamera || pinPlayerToActorView)
                {
                    if (!staticDialogueCamera)
                    {
                        const float playerEyeZ = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_PLAYER_EYE_Z", 124.f);
                        playerTargetPos.z() -= playerEyeZ;
                    }
                    player = mWorld->moveObject(player, playerTargetPos, true, true);
                }
                const ESM::Position& playerPos = player.getRefData().getPosition();
                const float yawToActor = static_cast<float>(
                    std::atan2(actorAim.x() - targetPos.x(), actorAim.y() - targetPos.y()));
                const float cameraYawToActor = -yawToActor;
                const osg::Vec3f playerTargetRotation(0.f, 0.f, -yawToActor);
                if (!staticDialogueCamera || pinPlayerToActorView)
                    mWorld->rotateObject(player, playerTargetRotation);
                if (MWRender::Camera* camera = mWorld->getCamera())
                {
                    osg::Vec3d proofCameraPos;
                    if (staticDialogueCamera)
                    {
                        const osg::Vec3d cameraTarget(actorAim);
                        const osg::Vec3d cameraPos(targetPos.x(), targetPos.y(), targetPos.z());
                        proofCameraPos = cameraPos;
                        const float dx = static_cast<float>(cameraTarget.x() - cameraPos.x());
                        const float dy = static_cast<float>(cameraTarget.y() - cameraPos.y());
                        const float dz = static_cast<float>(cameraTarget.z() - cameraPos.z());
                        const float horizontal = std::sqrt(dx * dx + dy * dy);
                        camera->setMode(MWRender::Camera::Mode::Static, true);
                        camera->setStaticPosition(cameraPos);
                        camera->setPitch(static_cast<float>(std::atan2(dz, horizontal)), true);
                        camera->setYaw(-static_cast<float>(std::atan2(dx, dy)), true);
                        camera->setRoll(0.f);
                        camera->updateCamera();
                    }
                    else
                    {
                        camera->attachTo(player);
                        camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
                        camera->setPreferredCameraDistance(cameraDistance);
                        camera->update(0.f, false);
                        const osg::Vec3d initialCameraPos = camera->getPosition();
                        const float dx = static_cast<float>(actorAim.x() - initialCameraPos.x());
                        const float dy = static_cast<float>(actorAim.y() - initialCameraPos.y());
                        const float dz = static_cast<float>(actorAim.z() - initialCameraPos.z());
                        const float horizontal = std::sqrt(dx * dx + dy * dy);
                        camera->setPitch(static_cast<float>(std::atan2(dz, horizontal)) + cameraPitch, true);
                        camera->setYaw(cameraYawToActor, true);
                        camera->setRoll(0.f);
                        camera->updateCamera();
                        proofCameraPos = camera->getPosition();
                    }
                    const osg::Vec3d cameraPos = camera->getPosition();
                    const osg::Vec3d cameraToAim = actorAim - cameraPos;
                    const double cameraToAimDistance = cameraToAim.length();
                    const double cameraPlanarDistance = std::sqrt(
                        cameraToAim.x() * cameraToAim.x() + cameraToAim.y() * cameraToAim.y());
                    const osg::Vec3d requestedDelta(
                        cameraPos.x() - targetPos.x(), cameraPos.y() - targetPos.y(), cameraPos.z() - targetPos.z());
                    const double requestedDeltaDistance = requestedDelta.length();
                    Log(Debug::Info) << "FNV/ESM4 proof: aligned player camera to actor target=\""
                                     << proofSayActor << "\" playerPos=(" << playerPos.pos[0] << ","
                                     << playerPos.pos[1] << "," << playerPos.pos[2] << ") actorPos=("
                                     << actorPos.pos[0] << "," << actorPos.pos[1] << "," << actorPos.pos[2]
                                     << ") actorRot=(" << actorPos.rot[0] << "," << actorPos.rot[1] << ","
                                     << actorPos.rot[2] << ") actorAim=(" << actorAim.x() << "," << actorAim.y()
                                     << "," << actorAim.z() << ") yawToActor=" << yawToActor
                                     << " cameraYawToActor=" << cameraYawToActor << " requestedCameraPos=("
                                     << targetPos.x() << "," << targetPos.y() << "," << targetPos.z()
                                     << ") cameraPos=(" << cameraPos.x() << ","
                                     << cameraPos.y() << "," << cameraPos.z() << ") cameraDelta=("
                                     << requestedDelta.x() << "," << requestedDelta.y() << ","
                                     << requestedDelta.z() << ") cameraDeltaDistance=" << requestedDeltaDistance
                                     << " aimDelta=(" << cameraToAim.x() << ","
                                     << cameraToAim.y() << "," << cameraToAim.z() << ") aimDistance="
                                     << cameraToAimDistance << " aimPlanarDistance=" << cameraPlanarDistance
                                     << " cameraDistance="
                                     << cameraDistance << " cameraPitch=" << camera->getPitch()
                                     << " cameraYaw=" << camera->getYaw() << " staticDialogueCamera="
                                     << staticDialogueCamera;
                    bool actorFocusInFrame = true;
                    if (requireActorForScreenshot)
                    {
                        actorFocusInFrame = false;
                        const osg::Matrix viewProj = camera->getViewMatrix() * camera->getProjectionMatrix();
                        const osg::Vec4d clipPoint = osg::Vec4d(actorAim, 1.0) * viewProj;
                        if (clipPoint.w() > 0.0)
                        {
                            const double screenPointX = (clipPoint.x() / clipPoint.w() + 1.0) * 0.5;
                            const double screenPointY = (clipPoint.y() / clipPoint.w() - 1.0) * -0.5;
                            actorFocusInFrame = screenPointX >= 0.02 && screenPointX <= 0.98 && screenPointY >= 0.02
                                && screenPointY <= 0.98;
                            if ((useRenderBounds || useFaceBounds) && !actorViewBoundsAccepted)
                                actorFocusInFrame = false;
                            if (!actorViewScaleAccepted)
                                actorFocusInFrame = false;
                            if (fullBodyView && !actorViewRenderBounds.valid())
                            {
                                actorFocusInFrame = false;
                                Log(Debug::Warning)
                                    << "FNV/ESM4 proof: actor full-body screen gate rejected missing current render bounds target=\""
                                    << proofSayActor << "\"";
                            }
                            if (fullBodyView
                                && proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_VISIBILITY_RAYCAST")
                                && actorViewVisibilityBlockers != 0)
                            {
                                actorFocusInFrame = false;
                                Log(Debug::Warning)
                                    << "FNV/ESM4 proof: actor full-body visibility gate rejected blocked camera target=\""
                                    << proofSayActor << "\" blockers=" << actorViewVisibilityBlockers;
                            }
                            if (fullBodyView && actorViewRenderBounds.valid())
                            {
                                double minScreenX = 1.0;
                                double maxScreenX = 0.0;
                                double minScreenY = 1.0;
                                double maxScreenY = 0.0;
                                bool allBoundsCornersInFront = true;
                                for (int corner = 0; corner < 8; ++corner)
                                {
                                    const osg::Vec3d point(
                                        (corner & 1) != 0 ? actorViewRenderBounds.xMax() : actorViewRenderBounds.xMin(),
                                        (corner & 2) != 0 ? actorViewRenderBounds.yMax() : actorViewRenderBounds.yMin(),
                                        (corner & 4) != 0 ? actorViewRenderBounds.zMax() : actorViewRenderBounds.zMin());
                                    const osg::Vec4d clip = osg::Vec4d(point, 1.0) * viewProj;
                                    if (clip.w() <= 0.0)
                                    {
                                        allBoundsCornersInFront = false;
                                        break;
                                    }
                                    const double x = (clip.x() / clip.w() + 1.0) * 0.5;
                                    const double y = (clip.y() / clip.w() - 1.0) * -0.5;
                                    minScreenX = std::min(minScreenX, x);
                                    maxScreenX = std::max(maxScreenX, x);
                                    minScreenY = std::min(minScreenY, y);
                                    maxScreenY = std::max(maxScreenY, y);
                                }
                                const float fullBodyMargin
                                    = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FULL_BODY_MARGIN", 0.03f);
                                const double projectedWidth = maxScreenX - minScreenX;
                                const double projectedHeight = maxScreenY - minScreenY;
                                const double projectedArea = projectedWidth * projectedHeight;
                                const double visibleWidth
                                    = std::max(0.0, std::min(maxScreenX, 1.0) - std::max(minScreenX, 0.0));
                                const double visibleHeight
                                    = std::max(0.0, std::min(maxScreenY, 1.0) - std::max(minScreenY, 0.0));
                                const double visibleArea = visibleWidth * visibleHeight;
                                const float minimumProjectedWidth = readProofFloat(
                                    "OPENMW_PROOF_ACTOR_VIEW_MIN_FULL_BODY_SCREEN_WIDTH", 0.f);
                                const float minimumProjectedHeight = readProofFloat(
                                    "OPENMW_PROOF_ACTOR_VIEW_MIN_FULL_BODY_SCREEN_HEIGHT", 0.f);
                                const float minimumProjectedArea = readProofFloat(
                                    "OPENMW_PROOF_ACTOR_VIEW_MIN_FULL_BODY_SCREEN_AREA", 0.f);
                                const bool diagnosticFullBodyInFrame = allBoundsCornersInFront
                                    && minScreenX >= fullBodyMargin && maxScreenX <= 1.0 - fullBodyMargin
                                    && minScreenY >= fullBodyMargin && maxScreenY <= 1.0 - fullBodyMargin
                                    && projectedWidth >= minimumProjectedWidth
                                    && projectedHeight >= minimumProjectedHeight
                                    && projectedArea >= minimumProjectedArea;
                                // The canonical parity camera is an oracle transform, not a framing suggestion.
                                // Retail intentionally crops some scaled creature boxes (notably the 2.5x raven),
                                // so moving that camera to satisfy diagnostic containment destroys 1:1 pairing.
                                // In replay mode retain the hard current-bounds/scale/visibility gates and require
                                // a meaningful viewport intersection; dynamic proof cameras still require every
                                // bounds corner inside the requested margin.
                                const bool retailReplayBoundsVisible = allBoundsCornersInFront
                                    && visibleWidth >= minimumProjectedWidth
                                    && visibleHeight >= minimumProjectedHeight
                                    && visibleArea >= minimumProjectedArea;
                                const bool fullBodyInFrame = replayRetailAbsoluteCamera
                                    ? retailReplayBoundsVisible
                                    : diagnosticFullBodyInFrame;
                                actorFocusInFrame = actorFocusInFrame && fullBodyInFrame;
                                Log(Debug::Info) << "FNV/ESM4 proof: actor full-body screen gate target=\""
                                                 << proofSayActor << "\" screenMin=(" << minScreenX << ","
                                                 << minScreenY << ") screenMax=(" << maxScreenX << ","
                                                 << maxScreenY << ") margin=" << fullBodyMargin
                                                 << " projectedSize=(" << projectedWidth << ","
                                                 << projectedHeight << ") projectedArea=" << projectedArea
                                                 << " visibleSize=(" << visibleWidth << "," << visibleHeight
                                                 << ") visibleArea=" << visibleArea
                                                 << " minimumProjectedSize=(" << minimumProjectedWidth << ","
                                                 << minimumProjectedHeight << ") minimumProjectedArea="
                                                 << minimumProjectedArea
                                                 << " policy="
                                                 << (replayRetailAbsoluteCamera ? "retail-viewport-intersection"
                                                                               : "diagnostic-full-containment")
                                                 << " allCornersInFront=" << allBoundsCornersInFront
                                                 << " inFrame=" << fullBodyInFrame;
                            }
                            if (proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_REQUIRE_HUMAN_POSE"))
                            {
                                std::string actorModel;
                                try
                                {
                                    actorModel = Misc::StringUtils::lowerCase(
                                        std::string(proofActor.getClass().getModel(proofActor)));
                                }
                                catch (const std::exception&)
                                {
                                }
                                std::replace(actorModel.begin(), actorModel.end(), '\\', '/');
                                const bool humanSkeleton
                                    = actorModel.find("characters/_male/skeleton.nif") != std::string::npos;
                                if (humanSkeleton)
                                {
                                    const FalloutProofPortraitPose pose
                                        = resolveFalloutProofPortraitPose(proofActor);
                                    const bool allBonesResolved = pose.mHeadResolved && pose.mPelvisResolved
                                        && pose.mLeftHandResolved && pose.mRightHandResolved
                                        && pose.mLeftFootResolved && pose.mRightFootResolved;
                                    const double footZ = allBonesResolved
                                        ? (pose.mLeftFootCenter.z() + pose.mRightFootCenter.z()) * 0.5
                                        : 0.0;
                                    const float minHeadAbovePelvis = readProofFloat(
                                        "OPENMW_PROOF_ACTOR_VIEW_MIN_HEAD_ABOVE_PELVIS", 20.f);
                                    const float minPelvisAboveFeet = readProofFloat(
                                        "OPENMW_PROOF_ACTOR_VIEW_MIN_PELVIS_ABOVE_FEET", 20.f);
                                    const float maxFootDeltaZ = readProofFloat(
                                        "OPENMW_PROOF_ACTOR_VIEW_MAX_FOOT_DELTA_Z", 30.f);
                                    const bool upright = allBonesResolved
                                        && pose.mHeadCenter.z() - pose.mPelvisCenter.z() >= minHeadAbovePelvis
                                        && pose.mPelvisCenter.z() - footZ >= minPelvisAboveFeet
                                        && std::abs(pose.mLeftFootCenter.z() - pose.mRightFootCenter.z())
                                            <= maxFootDeltaZ;
                                    const float poseMargin = readProofFloat(
                                        "OPENMW_PROOF_ACTOR_VIEW_POSE_MARGIN", 0.02f);
                                    bool poseInFrame = allBonesResolved;
                                    const auto projectPosePoint = [&](const osg::Vec3d& point) {
                                        const osg::Vec4d clip = osg::Vec4d(point, 1.0) * viewProj;
                                        if (clip.w() <= 0.0)
                                            return false;
                                        const double x = (clip.x() / clip.w() + 1.0) * 0.5;
                                        const double y = (clip.y() / clip.w() - 1.0) * -0.5;
                                        return x >= poseMargin && x <= 1.0 - poseMargin
                                            && y >= poseMargin && y <= 1.0 - poseMargin;
                                    };
                                    if (poseInFrame)
                                    {
                                        poseInFrame = projectPosePoint(pose.mHeadCenter)
                                            && projectPosePoint(pose.mPelvisCenter)
                                            && projectPosePoint(pose.mLeftHandCenter)
                                            && projectPosePoint(pose.mRightHandCenter)
                                            && projectPosePoint(pose.mLeftFootCenter)
                                            && projectPosePoint(pose.mRightFootCenter);
                                    }
                                    actorFocusInFrame
                                        = actorFocusInFrame && allBonesResolved && upright && poseInFrame;
                                    Log(Debug::Info) << "FNV/ESM4 proof: actor human-pose gate target=\""
                                                     << proofSayActor << "\" model=\"" << actorModel
                                                     << "\" resolved=(head=" << pose.mHeadResolved
                                                     << ",pelvis=" << pose.mPelvisResolved
                                                     << ",leftHand=" << pose.mLeftHandResolved
                                                     << ",rightHand=" << pose.mRightHandResolved
                                                     << ",leftFoot=" << pose.mLeftFootResolved
                                                     << ",rightFoot=" << pose.mRightFootResolved
                                                     << ") z=(head=" << pose.mHeadCenter.z()
                                                     << ",pelvis=" << pose.mPelvisCenter.z()
                                                     << ",leftFoot=" << pose.mLeftFootCenter.z()
                                                     << ",rightFoot=" << pose.mRightFootCenter.z()
                                                     << ") upright=" << upright << " inFrame=" << poseInFrame
                                                     << " accepted=" << actorFocusInFrame;
                                }
                            }
                            const float maxCameraDrift
                                = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_MAX_CAMERA_DRIFT", 4.f);
                            if (requestedDeltaDistance > maxCameraDrift)
                                actorFocusInFrame = false;
                            Log(Debug::Info) << "FNV/ESM4 proof: actor focus screen target=\"" << proofSayActor
                                             << "\" clip=(" << clipPoint.x() << "," << clipPoint.y() << ","
                                             << clipPoint.z() << "," << clipPoint.w() << ") screen=("
                                             << screenPointX << "," << screenPointY << ") boundsAccepted="
                                             << actorViewBoundsAccepted << " cameraDeltaDistance="
                                             << requestedDeltaDistance << " inFrame=" << actorFocusInFrame;
                        }
                        else
                        {
                            actorFocusInFrame = false;
                            Log(Debug::Warning)
                                << "FNV/ESM4 proof: actor focus projection sign was behind camera target=\""
                                << proofSayActor << "\" w=" << clipPoint.w() << "; rejecting actor alignment";
                        }
                    }
                    if (actorFocusInFrame)
                    {
                        proofActorCameraAligned = true;
                        proofActorCameraAlignedFrame = static_cast<int>(frameNumber);
                        proofActorCameraAlignedScreenshotIndex = proofScreenshotFrameIndex;
                        if (pinPlayerToActorView)
                        {
                            proofPinnedPlayerToActorView = true;
                            proofPinnedPlayerPosition = playerTargetPos;
                            proofPinnedPlayerRotation = playerTargetRotation;
                            proofPinnedPlayerFirstFrame = static_cast<int>(frameNumber);
                            proofPinnedPlayerLastLogFrame = static_cast<int>(frameNumber) - 1000000;
                            Log(Debug::Info) << "FNV/ESM4 proof: pinned player to actor view target=\""
                                             << proofSayActor << "\" pos=(" << proofPinnedPlayerPosition.x() << ","
                                             << proofPinnedPlayerPosition.y() << "," << proofPinnedPlayerPosition.z()
                                             << ") rot=(" << proofPinnedPlayerRotation.x() << ","
                                             << proofPinnedPlayerRotation.y() << "," << proofPinnedPlayerRotation.z()
                                             << ") staticDialogueCamera=" << staticDialogueCamera;
                        }
                    }
                    else
                    {
                        proofActorCameraAligned = false;
                        proofActorCameraAlignedFrame = -1;
                        proofActorCameraAlignedScreenshotIndex = static_cast<std::size_t>(-1);
                        if (pinPlayerToActorView)
                            proofPinnedPlayerToActorView = false;
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor camera alignment rejected target=\""
                                            << proofSayActor << "\" frame=" << frameNumber;
                    }

                    if (std::getenv("OPENMW_PROOF_LOG_NEARBY_REFS") != nullptr)
                    {
                        const float radius = readProofFloat("OPENMW_PROOF_NEARBY_REF_RADIUS", 450.f);
                        const float radiusSq = radius * radius;
                        int nearbyLogged = 0;
                        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
                        {
                            if (cellstore == nullptr)
                                continue;
                            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                                if (ptr.isEmpty())
                                    return true;
                                const ESM::Position& pos = ptr.getRefData().getPosition();
                                const float dx = pos.pos[0] - static_cast<float>(proofCameraPos.x());
                                const float dy = pos.pos[1] - static_cast<float>(proofCameraPos.y());
                                const float dz = pos.pos[2] - static_cast<float>(proofCameraPos.z());
                                if (dx * dx + dy * dy + dz * dz > radiusSq)
                                    return true;

                                const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
                                const ESM::RefId baseId = ptr.getCellRef().getRefId();
                                std::string name;
                                try
                                {
                                    name = std::string(ptr.getClass().getName(ptr));
                                }
                                catch (const std::exception&)
                                {
                                }
                                VFS::Path::Normalized model;
                                try
                                {
                                    model = ptr.getClass().getCorrectedModel(ptr);
                                }
                                catch (const std::exception& e)
                                {
                                    Log(Debug::Warning) << "FNV/ESM4 proof: nearby ref model lookup failed for "
                                                        << ptr.toString() << ": " << e.what();
                                }
                                unsigned int nodeMask = 0;
                                osg::Vec3d scenePos;
                                if (ptr.getRefData().getBaseNode())
                                {
                                    nodeMask = ptr.getRefData().getBaseNode()->getNodeMask();
                                    scenePos = ptr.getRefData().getBaseNode()->getPosition();
                                }
                                Log(Debug::Info) << "FNV/ESM4 proof: nearby ref ref="
                                                 << refNum.toString("FormId:") << " base="
                                                 << baseId.toDebugString() << " type=" << ptr.getType()
                                                 << " actor=" << ptr.getClass().isActor() << " name=\"" << name
                                                 << "\" model=" << model.value() << " pos=(" << pos.pos[0]
                                                 << "," << pos.pos[1] << "," << pos.pos[2] << ") scenePos=("
                                                 << scenePos.x() << "," << scenePos.y() << "," << scenePos.z()
                                                 << ") nodeMask=0x" << std::hex << nodeMask << std::dec
                                                 << " ptr=" << ptr.toString();
                                ++nearbyLogged;
                                return nearbyLogged < 120;
                            });
                            if (nearbyLogged >= 120)
                                break;
                        }
                        Log(Debug::Info) << "FNV/ESM4 proof: nearby ref dump complete count=" << nearbyLogged
                                         << " radius=" << radius;
                    }
                }
            }
        }

        if (!proofSayQueued && !proofActor.isEmpty() && proofEnvEnabled("OPENMW_PROOF_START_DIALOGUE")
            && mWindowManager != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: opening real dialogue GUI for " << proofActor.toString()
                             << " at frame " << frameNumber;
            mWindowManager->pushGuiMode(MWGui::GM_Dialogue, proofActor);
        }
        else if (!proofSayQueued && !proofActor.isEmpty() && proofSayTopic != nullptr && *proofSayTopic != '\0'
            && mDialogueManager != nullptr)
        {
            const bool said = mDialogueManager->say(proofActor, ESM::RefId::stringRefId(proofSayTopic));
            Log(Debug::Info) << "FNV/ESM4 proof: actor dialogue say topic \"" << proofSayTopic << "\" result="
                             << said << " at frame " << frameNumber;
        }
        else if (!proofSayQueued && !proofActor.isEmpty() && proofSayFile != nullptr && *proofSayFile != '\0'
            && mSoundManager != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: playing actor proof voice \"" << proofSayFile << "\" for "
                             << proofActor.toString() << " at frame " << frameNumber;
            mSoundManager->say(proofActor, VFS::Path::Normalized(proofSayFile));
        }
        else if (!proofSayQueued && proofSayFile != nullptr && *proofSayFile != '\0'
            && mSoundManager != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: playing proof voice \"" << proofSayFile << "\" at frame "
                             << frameNumber;
            mSoundManager->say(VFS::Path::Normalized(proofSayFile));
        }
        else if (!proofSayQueued && proofSayFile != nullptr && *proofSayFile != '\0')
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: skipped proof voice \"" << proofSayFile
                                << "\" because sound manager is unavailable at frame " << frameNumber;
        }
        proofSayQueued = true;
    }

    if (proofSidecarEnabled && proofSidecarAction.has_value()
        && proofSidecarPhase != FNVSidecarOpenMwPhase::Failed
        && proofSidecarPhase != FNVSidecarOpenMwPhase::Complete)
    {
        const FNVSidecar::RetailAction& action = *proofSidecarAction;
        const std::string& requestedGroup = proofActorPoseGroups[action.mActionIndex];
        const auto localWeapon = [&]() -> const ESM4::Weapon* {
            if (proofActorBatchPrevious.isEmpty()
                || proofActorBatchPrevious.getType() != ESM4::Npc::sRecordId)
                return nullptr;
            return MWClass::ESM4Npc::getEquippedWeapon(proofActorBatchPrevious);
        };
        const auto exactWeapon = [&]() {
            const ESM4::Weapon* weapon = localWeapon();
            if (action.mRequestedWeaponForm == 0)
                return weapon == nullptr;
            return weapon != nullptr
                && (weapon->mId.toUint32() & 0x00FFFFFFu)
                    == (action.mRequestedWeaponForm & 0x00FFFFFFu);
        };
        const auto applyWeaponDrawState = [&](bool weaponDrawn) {
            if (proofActorBatchPrevious.isEmpty()
                || proofActorBatchPrevious.getType() != ESM4::Npc::sRecordId)
                return;
            proofActorBatchPrevious.getClass().getCreatureStats(proofActorBatchPrevious)
                .setDrawState(weaponDrawn && action.mRequestedWeaponForm != 0
                        ? MWMechanics::DrawState::Weapon : MWMechanics::DrawState::Nothing);
        };
        const auto buildOpenMwTelemetry = [&](const FNVSidecarScreenshot* screenshot) {
            std::ostringstream out;
            out << std::setprecision(9)
                << "{\"schema\":\"nikami-fnv-sidecar-openmw/v1\",\"sequenceId\":";
            writeProofJsonString(out, action.mSequenceId);
            out << ",\"key\":{\"sequenceId\":";
            writeProofJsonString(out, action.mSequenceId);
            out << ",\"actorIndex\":" << action.mActorIndex << ",\"actionIndex\":"
                << action.mActionIndex << "},\"generation\":" << action.mGeneration
                << ",\"frame\":" << frameNumber << ",\"actor\":{";
            if (!proofActorBatchPrevious.isEmpty())
            {
                const ESM::Position& position = proofActorBatchPrevious.getRefData().getPosition();
                out << "\"ref\":";
                writeProofJsonString(out,
                    proofActorBatchPrevious.getCellRef().getRefNum().toString("FormId:"));
                out << ",\"base\":";
                writeProofJsonString(out,
                    proofActorBatchPrevious.getCellRef().getRefId().toDebugString());
                out << ",\"position\":[" << position.pos[0] << ',' << position.pos[1] << ','
                    << position.pos[2] << "],\"rotation\":[" << position.rot[0] << ',' << position.rot[1]
                    << ',' << position.rot[2] << ']';
            }
            out << "},\"action\":{\"id\":";
            writeProofJsonString(out, action.mActionId);
            out << ",\"openMwGroup\":";
            writeProofJsonString(out, requestedGroup);
            out << ",\"requestedFrames\":" << action.mRequestedFrames << ",\"elapsedFrames\":"
                << (proofSidecarActionStartFrame >= 0
                        ? static_cast<int>(frameNumber) - proofSidecarActionStartFrame : 0)
                << ",\"accepted\":" << (proofSidecarActionPlayed ? "true" : "false") << '}';

            const bool openMwWeaponDrawn = !proofActorBatchPrevious.isEmpty()
                && proofActorBatchPrevious.getType() == ESM4::Npc::sRecordId
                && proofActorBatchPrevious.getClass().getCreatureStats(proofActorBatchPrevious).getDrawState()
                    == MWMechanics::DrawState::Weapon;
            out << ",\"animation\":{\"retailWeaponOut\":" << (action.mWeaponDrawn ? "true" : "false")
                << ",\"weaponOut\":" << (openMwWeaponDrawn ? "true" : "false") << '}';

            const ESM4::Weapon* weapon = localWeapon();
            out << ",\"weaponPolicy\":{\"retailRequestedForm\":" << action.mRequestedWeaponForm
                << ",\"openMwEquippedForm\":" << (weapon != nullptr ? weapon->mId.toUint32() : 0)
                << ",\"openMwEditorId\":";
            writeProofJsonString(out,
                weapon != nullptr ? std::string_view(weapon->mEditorId) : std::string_view{});
            out << ",\"exactBySourceForm\":" << (exactWeapon() ? "true" : "false") << '}';

            out << ",\"equipment\":{";
            if (!proofActorBatchPrevious.isEmpty()
                && proofActorBatchPrevious.getType() == ESM4::Npc::sRecordId)
            {
                const std::vector<const ESM4::Armor*>& armor
                    = MWClass::ESM4Npc::getEquippedArmor(proofActorBatchPrevious);
                const std::vector<const ESM4::Clothing*>& clothing
                    = MWClass::ESM4Npc::getEquippedClothing(proofActorBatchPrevious);
                out << "\"armorForms\":[";
                for (std::size_t index = 0; index < armor.size(); ++index)
                {
                    if (index != 0)
                        out << ',';
                    out << (armor[index] != nullptr ? armor[index]->mId.toUint32() : 0);
                }
                out << "],\"clothingForms\":[";
                for (std::size_t index = 0; index < clothing.size(); ++index)
                {
                    if (index != 0)
                        out << ',';
                    out << (clothing[index] != nullptr ? clothing[index]->mId.toUint32() : 0);
                }
                out << ']';
                if (const ESM4::Npc* npc = proofActorBatchPrevious.get<ESM4::Npc>()->mBase)
                {
                    out << ",\"npc\":{\"form\":" << npc->mId.toUint32() << ",\"race\":"
                        << npc->mRace.toUint32() << ",\"hair\":" << npc->mHair.toUint32()
                        << ",\"eyes\":" << npc->mEyes.toUint32() << ",\"hairColor\":["
                        << static_cast<unsigned int>(npc->mHairColour.red) << ','
                        << static_cast<unsigned int>(npc->mHairColour.green) << ','
                        << static_cast<unsigned int>(npc->mHairColour.blue) << ','
                        << static_cast<unsigned int>(npc->mHairColour.custom) << "],\"headParts\":[";
                    for (std::size_t index = 0; index < npc->mHeadParts.size(); ++index)
                    {
                        if (index != 0)
                            out << ',';
                        out << npc->mHeadParts[index].toUint32();
                    }
                    out << "]}";
                }
            }
            out << '}';

            osg::Camera* camera = mViewer != nullptr ? mViewer->getCamera() : nullptr;
            out << ",\"camera\":{\"available\":" << (camera != nullptr ? "true" : "false");
            if (camera != nullptr)
            {
                const auto writeMatrix = [&](std::string_view name, const osg::Matrixd& matrix) {
                    out << ",\"" << name << "\":[";
                    for (std::size_t row = 0; row < 4; ++row)
                    {
                        for (std::size_t column = 0; column < 4; ++column)
                        {
                            if (row != 0 || column != 0)
                                out << ',';
                            out << matrix(row, column);
                        }
                    }
                    out << ']';
                };
                writeMatrix("view", camera->getViewMatrix());
                writeMatrix("projection", camera->getProjectionMatrix());
            }
            out << '}';
            out << ",\"screenshot\":{\"exists\":"
                << (screenshot != nullptr && screenshot->mValid ? "true" : "false");
            if (screenshot != nullptr && screenshot->mValid)
            {
                out << ",\"path\":";
                writeProofJsonString(out, screenshot->mPath.generic_string());
                out << ",\"bytes\":" << screenshot->mSize;
            }
            out << "}}";
            return out.str();
        };

        if (proofSidecarPhase == FNVSidecarOpenMwPhase::Staging
            && proofActorBatchIndex == action.mActorIndex && !proofActorBatchPrevious.isEmpty()
            && proofActorStagedForCamera && proofActorCameraAligned && mWorld != nullptr
            && mMechanicsManager != nullptr)
        {
            MWRender::Animation* animation = mWorld->getAnimation(proofActorBatchPrevious);
            if (!exactWeapon())
                failFNVSidecar(FNVSidecar::ErrorCode::WeaponPolicyFailed,
                    "openmw-retail-exact-weapon-mismatch");
            else if (animation == nullptr || !animation->hasAnimation(requestedGroup))
                failFNVSidecar(FNVSidecar::ErrorCode::ActionRejected,
                    "openmw-animation-group-unavailable-" + requestedGroup);
            else
            {
                applyWeaponDrawState(action.mWeaponDrawn);
                mMechanicsManager->clearAnimationQueue(proofActorBatchPrevious, true);
                proofSidecarActionPlayed = mMechanicsManager->playAnimationGroup(
                    proofActorBatchPrevious, requestedGroup, 1, 1, true);
                if (!proofSidecarActionPlayed)
                    failFNVSidecar(FNVSidecar::ErrorCode::ActionRejected,
                        "openmw-animation-group-rejected-" + requestedGroup);
                else
                {
                    proofSidecarActionStartFrame = static_cast<int>(frameNumber);
                    proofSidecarPhase = FNVSidecarOpenMwPhase::Settling;
                    Log(Debug::Info) << "FNV sidecar OpenMW: action staged generation="
                                     << action.mGeneration << " actorIndex=" << action.mActorIndex
                                     << " actionIndex=" << action.mActionIndex << " group=\""
                                     << requestedGroup << "\" frame=" << frameNumber;
                }
            }
        }

        if (proofSidecarPhase == FNVSidecarOpenMwPhase::Settling
            && proofSidecarActionStartFrame >= 0
            && static_cast<std::uint64_t>(static_cast<int>(frameNumber) - proofSidecarActionStartFrame)
                >= action.mRequestedFrames
            && (proofSidecarSnapshot.mFlags & FNVSidecar::RetailReadyFlag) != 0)
        {
            const std::string telemetry = buildOpenMwTelemetry(nullptr);
            if (proofSidecarClient.publishReady(proofSidecarSnapshot, frameNumber, telemetry))
            {
                proofSidecarPhase = FNVSidecarOpenMwPhase::Ready;
                Log(Debug::Info) << "FNV sidecar OpenMW: both-ready publication generation="
                                 << action.mGeneration << " actorIndex=" << action.mActorIndex
                                 << " actionIndex=" << action.mActionIndex << " frame=" << frameNumber;
            }
            else
                failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault, "openmw-ready-publish-failed");
        }

        if (proofSidecarPhase == FNVSidecarOpenMwPhase::Ready
            && (proofSidecarSnapshot.mFlags & FNVSidecar::RetailCapturedFlag) != 0)
        {
            std::string parseError;
            const std::optional<FNVSidecar::RetailAction> capturedAction
                = FNVSidecar::parseRetailAction(proofSidecarSnapshot, parseError);
            if (!capturedAction)
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan,
                    "retail-captured-state-invalid-" + parseError);
            else if (capturedAction->mGeneration != action.mGeneration
                || capturedAction->mActorIndex != action.mActorIndex
                || capturedAction->mActionIndex != action.mActionIndex
                || capturedAction->mRetailBaseForm != action.mRetailBaseForm
                || capturedAction->mActionId != action.mActionId
                || capturedAction->mRequestedFrames != action.mRequestedFrames
                || capturedAction->mRequestedWeaponForm != action.mRequestedWeaponForm)
                failFNVSidecar(FNVSidecar::ErrorCode::InvalidPlan, "retail-captured-state-identity-changed");
            else
            {
                proofSidecarAction->mWeaponDrawn = capturedAction->mWeaponDrawn;
                applyWeaponDrawState(capturedAction->mWeaponDrawn);
                proofSidecarCaptureStateStartFrame = static_cast<int>(frameNumber);
                proofSidecarPhase = FNVSidecarOpenMwPhase::SettlingCaptureState;
                Log(Debug::Info) << "FNV sidecar OpenMW: consumed retail captured state generation="
                                 << action.mGeneration << " weaponOut=" << capturedAction->mWeaponDrawn
                                 << " frame=" << frameNumber;
            }
        }

        constexpr int captureStateSettleFrames = 2;
        if (proofSidecarPhase == FNVSidecarOpenMwPhase::SettlingCaptureState
            && proofSidecarCaptureStateStartFrame >= 0
            && static_cast<int>(frameNumber) - proofSidecarCaptureStateStartFrame >= captureStateSettleFrames)
        {
            if (mScreenCaptureHandler == nullptr)
                failFNVSidecar(FNVSidecar::ErrorCode::ScreenshotTimeout, "screen-capture-handler-unavailable");
            else
            {
                proofSidecarScreenshotBaseline = newestSidecarScreenshot(mCfgMgr.getScreenshotPath());
                proofSidecarScreenshotCandidate = {};
                proofSidecarScreenshotStableFrames = 0;
                mScreenCaptureHandler->setFramesToCapture(1);
                mScreenCaptureHandler->captureNextFrame(*mViewer);
                proofSidecarPhase = FNVSidecarOpenMwPhase::Capturing;
                Log(Debug::Info) << "FNV sidecar OpenMW: capture issued generation="
                                 << action.mGeneration << " actorIndex=" << action.mActorIndex
                                 << " actionIndex=" << action.mActionIndex << " frame=" << frameNumber;
            }
        }

        if (proofSidecarPhase == FNVSidecarOpenMwPhase::Capturing)
        {
            const FNVSidecarScreenshot candidate = newestSidecarScreenshot(mCfgMgr.getScreenshotPath());
            if (isNewSidecarScreenshot(proofSidecarScreenshotBaseline, candidate))
            {
                if (proofSidecarScreenshotCandidate.mValid
                    && proofSidecarScreenshotCandidate.mPath == candidate.mPath
                    && proofSidecarScreenshotCandidate.mSize == candidate.mSize)
                    ++proofSidecarScreenshotStableFrames;
                else
                {
                    proofSidecarScreenshotCandidate = candidate;
                    proofSidecarScreenshotStableFrames = 1;
                }
            }
            if (proofSidecarScreenshotStableFrames >= 2)
            {
                const std::string telemetry = buildOpenMwTelemetry(&proofSidecarScreenshotCandidate);
                if (proofSidecarClient.markCaptured(action.mGeneration, action.mActorIndex,
                        action.mActionIndex, frameNumber, telemetry))
                {
                    proofSidecarPhase = FNVSidecarOpenMwPhase::WaitingAdvance;
                    Log(Debug::Info) << "FNV sidecar OpenMW: capture file proven generation="
                                     << action.mGeneration << " actorIndex=" << action.mActorIndex
                                     << " actionIndex=" << action.mActionIndex << " path=\""
                                     << proofSidecarScreenshotCandidate.mPath.string() << "\" bytes="
                                     << proofSidecarScreenshotCandidate.mSize << " frame=" << frameNumber;
                }
                else
                    failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault,
                        "openmw-captured-publish-failed");
            }
        }

        if (proofSidecarPhase == FNVSidecarOpenMwPhase::WaitingAdvance
            && proofSidecarSnapshot.mGeneration == action.mGeneration
            && (proofSidecarSnapshot.mFlags & FNVSidecar::RetailCapturedFlag) != 0
            && (proofSidecarSnapshot.mFlags & FNVSidecar::OpenMwCapturedFlag) != 0
            && (proofSidecarSnapshot.mFlags & FNVSidecar::CaptureAckFlag) == 0)
        {
            if (!proofSidecarClient.acknowledgeCapture(
                    action.mGeneration, action.mActorIndex, action.mActionIndex))
                failFNVSidecar(FNVSidecar::ErrorCode::SharedMemoryFault, "capture-ack-publish-failed");
        }
    }

    if (!proofSidecarEnabled && proofActorBatchActive && proofActorPoseBatchIndex == proofActorBatchIndex
        && proofActorPoseSweepEnabled && !proofActorBatchPrevious.isEmpty() && proofActorStagedForCamera
        && proofActorCameraAligned && mWorld != nullptr && mMechanicsManager != nullptr)
    {
        MWRender::Animation* animation = mWorld->getAnimation(proofActorBatchPrevious);
        if (!proofActorPoseInventoryLogged && animation != nullptr)
        {
            const std::vector<std::string> availableGroups = animation->getAnimationGroups();
            proofActorActivePoseGroups = proofActorPoseAllAvailable ? availableGroups : proofActorPoseGroups;
            std::ostringstream inventory;
            for (std::size_t index = 0; index < availableGroups.size(); ++index)
            {
                if (index != 0)
                    inventory << ',';
                inventory << availableGroups[index];
            }
            Log(Debug::Info) << "FNV/ESM4 actor pose inventory: actorIndex=" << proofActorBatchIndex
                             << " target=\"" << proofActorBatchTargets[proofActorBatchIndex]
                             << "\" availableCount=" << availableGroups.size() << " groups=[" << inventory.str()
                             << "] sweep=" << (proofActorPoseAllAvailable ? "all-available" : "exact-requested")
                             << " sweepCount=" << proofActorActivePoseGroups.size();
            if (proofActorBatchPrevious.getType() == ESM4::Npc::sRecordId)
            {
                const ESM4::Weapon* mainWeapon = MWClass::ESM4Npc::getEquippedWeapon(proofActorBatchPrevious);
                const MWMechanics::DrawState drawState
                    = proofActorBatchPrevious.getClass().getCreatureStats(proofActorBatchPrevious).getDrawState();
                Log(Debug::Info) << "FNV/ESM4 actor authored weapon state: actorIndex=" << proofActorBatchIndex
                                 << " target=\"" << proofActorBatchTargets[proofActorBatchIndex]
                                 << "\" selectedCount=" << (mainWeapon != nullptr ? 1 : 0) << " editor=\""
                                 << (mainWeapon != nullptr ? mainWeapon->mEditorId : std::string()) << "\" drawState="
                                 << (drawState == MWMechanics::DrawState::Weapon
                                         ? "weapon"
                                         : (drawState == MWMechanics::DrawState::Spell ? "spell" : "nothing"));
            }
            proofActorPoseInventoryLogged = true;
            proofActorPoseCycleComplete = proofActorActivePoseGroups.empty();
            const int startDelayEnv = getProofFrame("OPENMW_PROOF_ACTOR_POSE_START_DELAY_FRAMES");
            const int startDelay = startDelayEnv >= 0 ? startDelayEnv : 12;
            proofActorPoseNextFrame = static_cast<int>(frameNumber) + startDelay;
        }

        if (proofActorPoseInventoryLogged && !proofActorPoseCycleComplete && proofActorPoseNextFrame >= 0
            && frameNumber >= static_cast<unsigned int>(proofActorPoseNextFrame))
        {
            const int poseFramesEnv = getProofFrame("OPENMW_PROOF_ACTOR_POSE_FRAMES");
            const int poseFrames = poseFramesEnv >= 1 ? poseFramesEnv : 12;
            if (proofActorPoseIndex < proofActorActivePoseGroups.size())
            {
                const std::string& group = proofActorActivePoseGroups[proofActorPoseIndex];
                const bool available = animation != nullptr && animation->hasAnimation(group);
                const bool played = available
                    && mMechanicsManager->playAnimationGroup(proofActorBatchPrevious, group, 1, 1, true);
                if (played)
                    ++proofActorPosePlayed;
                else
                    ++proofActorPoseSkipped;
                Log(played ? Debug::Info : Debug::Warning)
                     << "FNV/ESM4 actor pose gate: actorIndex=" << proofActorBatchIndex << " target=\""
                     << proofActorBatchTargets[proofActorBatchIndex] << "\" poseIndex=" << proofActorPoseIndex
                     << " group=\"" << group << "\" resolvedGroup=\"" << group
                     << "\" available=" << available << " played=" << played
                     << " exact=1 status=" << (played ? "pass" : "fail");
                ++proofActorPoseIndex;
                proofActorPoseNextFrame = static_cast<int>(frameNumber) + poseFrames;
            }
            else
            {
                proofActorPoseCycleComplete = true;
                Log(Debug::Info) << "FNV/ESM4 actor pose cycle: actorIndex=" << proofActorBatchIndex
                                 << " target=\"" << proofActorBatchTargets[proofActorBatchIndex]
                                 << "\" requested=" << proofActorActivePoseGroups.size() << " played="
                                 << proofActorPosePlayed << " skipped=" << proofActorPoseSkipped
                                 << " selectedFrame=" << proofActorBatchSelectedFrame << " completeFrame="
                                 << frameNumber << " status=complete";
            }
        }
    }

    const char* proofDialogueTopic = std::getenv("OPENMW_PROOF_DIALOGUE_TOPIC");
    const int proofDialogueTopicDelay = std::max(1, getProofFrame("OPENMW_PROOF_DIALOGUE_TOPIC_DELAY"));
    if (!proofDialogueTopicQueued && proofSayQueued && proofDialogueTopic != nullptr && *proofDialogueTopic != '\0'
        && proofSayFrame >= 0 && frameNumber >= static_cast<unsigned>(proofSayFrame + proofDialogueTopicDelay)
        && mDialogueManager != nullptr)
    {
        struct ProofDialogueCallback final : MWBase::DialogueManager::ResponseCallback
        {
            void addResponse(std::string_view title, std::string_view text) override
            {
                Log(Debug::Info) << "FNV/ESM4 proof: dialogue response title=\"" << title << "\" text=\"" << text
                                 << "\"";
            }
        } callback;
        Log(Debug::Info) << "FNV/ESM4 proof: selecting dialogue topic \"" << proofDialogueTopic << "\" at frame "
                         << frameNumber;
        mDialogueManager->keywordSelected(proofDialogueTopic, &callback);
        proofDialogueTopicQueued = true;
    }

    if (proofPinnedPlayerToActorView && proofRunning && mWorld != nullptr
        && std::getenv("OPENMW_PROOF_PIN_PLAYER_TO_ACTOR_VIEW") != nullptr)
    {
        try
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const osg::Vec3f before = player.getRefData().getPosition().asVec3();
            player = mWorld->moveObject(player, proofPinnedPlayerPosition, true, true);
            mWorld->rotateObject(player, proofPinnedPlayerRotation);
            if (static_cast<int>(frameNumber) - proofPinnedPlayerLastLogFrame >= 30)
            {
                proofPinnedPlayerLastLogFrame = static_cast<int>(frameNumber);
                Log(Debug::Info) << "FNV/ESM4 proof: refreshed pinned player actor view frame=" << frameNumber
                                 << " firstFrame=" << proofPinnedPlayerFirstFrame
                                 << " before=(" << before.x() << "," << before.y() << "," << before.z() << ")"
                                 << " pos=(" << proofPinnedPlayerPosition.x() << "," << proofPinnedPlayerPosition.y()
                                 << "," << proofPinnedPlayerPosition.z() << ") rot=("
                                 << proofPinnedPlayerRotation.x() << "," << proofPinnedPlayerRotation.y() << ","
                                 << proofPinnedPlayerRotation.z() << ")";
            }
        }
        catch (const std::exception& e)
        {
            proofPinnedPlayerToActorView = false;
            Log(Debug::Warning) << "FNV/ESM4 proof: failed to refresh pinned player actor view: " << e.what();
        }
    }

    const auto executeProofTimedScript = [&](int targetFrame, const char* pathEnv, bool& executed) {
        if (executed || targetFrame < 0 || frameNumber < static_cast<unsigned>(targetFrame) || !proofRunning
            || !proofWorldReady)
            return;
        const char* scriptPath = std::getenv(pathEnv);
        if (scriptPath == nullptr || *scriptPath == '\0')
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: timed script env " << pathEnv
                                << " is empty at frame " << frameNumber;
            executed = true;
            return;
        }
        Log(Debug::Info) << "FNV/ESM4 proof: executing timed script " << pathEnv << "=\""
                         << scriptPath << "\" at frame " << frameNumber;
        mWindowManager->executeInConsole(std::filesystem::path(scriptPath));
        executed = true;
    };
    executeProofTimedScript(proofTimedScript1Frame, "OPENMW_PROOF_TIMED_SCRIPT_1", proofTimedScript1Executed);
    executeProofTimedScript(proofTimedScript2Frame, "OPENMW_PROOF_TIMED_SCRIPT_2", proofTimedScript2Executed);

    const bool fnvInteractionRequested = proofEnvEnabled("OPENMW_FNV_INTERACTION_AUDIT");
    const bool fnvInteractionDoorOnly = proofEnvEnabled("OPENMW_FNV_INTERACTION_DOOR_ONLY");
    if (fnvInteractionRequested && fnvInteractionPhase >= 0 && proofRunning && mWorld != nullptr
        && mWindowManager != nullptr)
    {
        // The native audit deliberately activates actors and crosses cell doors after the Lua
        // worker is normally released for the frame.  Serialize only this opt-in audit path so
        // those scene mutations cannot invalidate the worker's shared nearby-object lists.
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
        const auto interactionGateSummary = [&]() {
            std::ostringstream stream;
            stream << "actor=" << (fnvInteractionActorPass ? 1 : 0)
                   << " dialogue=" << (fnvInteractionDialoguePass ? 1 : 0)
                   << " quest=" << (fnvInteractionQuestPass ? 1 : 0)
                   << " doorIn=" << (fnvInteractionDoorInPass ? 1 : 0)
                   << " interiorActors=" << (fnvInteractionInteriorActorsPass ? 1 : 0)
                   << " radio=" << (fnvInteractionRadioPass ? 1 : 0)
                   << " doorOut=" << (fnvInteractionDoorOutPass ? 1 : 0);
            return stream.str();
        };
        const auto finishInteraction = [&](bool pass, std::string_view reason) {
            Log(pass ? Debug::Info : Debug::Error)
                << "FNV interaction audit: result=" << (pass ? "pass" : "fail")
                << " reason=\"" << reason << "\" " << interactionGateSummary()
                << " frame=" << frameNumber;
            fnvInteractionPhase = -1;
            mStateManager->requestQuit();
        };
        const auto advanceInteraction = [&](int phase) {
            fnvInteractionPhase = phase;
            fnvInteractionPhaseFrame = frameNumber;
            fnvInteractionPhaseStartTime = frameStart;
            Log(Debug::Info) << "FNV interaction audit: phase=" << phase << " frame=" << frameNumber;
        };
        const auto queueInteractionCapture = [&](std::string_view label) {
            if (mScreenCaptureHandler == nullptr)
                return;
            Log(Debug::Info) << "FNV interaction audit: queuing native capture label=" << label
                             << " frame=" << frameNumber;
            mScreenCaptureHandler->setFramesToCapture(1);
            mScreenCaptureHandler->captureNextFrame(*mViewer);
        };
        const auto findActiveRef = [&](std::uint32_t rawFormId) {
            const ESM::FormId formId = ESM::FormId::fromUint32(rawFormId);
            MWWorld::Ptr found;
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (!ptr.isEmpty() && ptr.getCellRef().getRefNum() == formId)
                    {
                        found = ptr;
                        return false;
                    }
                    return true;
                });
                if (!found.isEmpty())
                    break;
            }
            return found;
        };
        const auto countActiveRef = [&](std::uint32_t rawFormId) {
            const ESM::FormId formId = ESM::FormId::fromUint32(rawFormId);
            int count = 0;
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (!ptr.isEmpty() && ptr.getCellRef().getRefNum() == formId)
                        ++count;
                    return true;
                });
            }
            return count;
        };
        const auto activateInteractionTarget = [&](const MWWorld::Ptr& target, std::string_view label) {
            if (target.isEmpty())
            {
                Log(Debug::Error) << "FNV interaction audit: activation target missing label=" << label;
                return false;
            }
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            std::unique_ptr<MWWorld::Action> action = target.getClass().activate(target, player);
            const bool actionable = action != nullptr && !action->isNullAction();
            Log(actionable ? Debug::Info : Debug::Error)
                << "FNV interaction audit: activate label=" << label << " target=" << target.toString()
                << " type=" << target.getTypeDescription() << " actionable=" << (actionable ? 1 : 0);
            if (actionable)
                action->execute(player);
            return actionable;
        };
        const auto playerCellId = [&]() {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            if (player.isEmpty() || player.getCell() == nullptr || player.getCell()->getCell() == nullptr)
                return ESM::RefId();
            return player.getCell()->getCell()->getId();
        };
        const auto aimInteractionCameraAt = [&](const MWWorld::Ptr& target, std::string_view label,
                                                    float aimOffsetZ) {
            MWRender::Camera* camera = mWorld->getCamera();
            if (camera == nullptr || target.isEmpty())
                return false;

            const osg::Vec3d cameraPosition = camera->getPosition();
            osg::Vec3d aimPosition = target.getRefData().getPosition().asVec3();
            aimPosition.z() += aimOffsetZ;
            const osg::Vec3d delta = aimPosition - cameraPosition;
            const double horizontal = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            if (horizontal < 1.0)
                return false;

            const float pitch = static_cast<float>(std::atan2(delta.z(), horizontal));
            const float yaw = -static_cast<float>(std::atan2(delta.x(), delta.y()));
            camera->setPitch(pitch, true);
            camera->setYaw(yaw, true);
            camera->setRoll(0.f);
            camera->instantTransition();
            camera->updateCamera();
            Log(Debug::Info) << "FNV interaction audit: aim label=" << label
                             << " cameraPos=(" << cameraPosition.x() << "," << cameraPosition.y() << ","
                             << cameraPosition.z() << ") targetPos=(" << aimPosition.x() << ","
                             << aimPosition.y() << "," << aimPosition.z() << ") pitch=" << pitch
                             << " yaw=" << yaw;
            return true;
        };
        const unsigned int phaseFrames = frameNumber - fnvInteractionPhaseFrame;
        const double phaseSeconds = fnvInteractionPhaseStartTime != 0
            ? timer->delta_s(fnvInteractionPhaseStartTime, frameStart)
            : 0.0;
        const double phaseTimeoutSeconds
            = std::max(5.f, readProofFloat("OPENMW_FNV_INTERACTION_PHASE_TIMEOUT_SECONDS", 45.f));

        if (fnvInteractionPhase == 0 && proofWorldReady
            && proofWorldReadyFrames
                >= std::max(1, readProofInt("OPENMW_FNV_INTERACTION_SETTLE_FRAMES", 900)))
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const int level = player.getClass().getCreatureStats(player).getLevel();
            fnvInteractionActor = findActiveRef(0x1104c80);
            if (fnvInteractionActor.isEmpty())
            {
                finishInteraction(false, "Easy Pete authored reference is not active outside Goodsprings");
            }
            else
            {
                const osg::Vec3f actorPosition = fnvInteractionActor.getRefData().getPosition().asVec3();
                const osg::Vec3f retailChair(-67966.9297f, 3447.8076f, 8387.3105f);
                const float chairDistance = (actorPosition - retailChair).length();
                const bool inCombat = fnvInteractionActor.getClass()
                                          .getCreatureStats(fnvInteractionActor)
                                          .getAiSequence()
                                          .isInCombat();
                const int activeCopies = countActiveRef(0x1104c80);
                fnvInteractionActorSettledPosition = actorPosition;
                fnvInteractionActorPass
                    = level == 1 && activeCopies == 1 && chairDistance <= 192.f && !inCombat;
                Log(fnvInteractionActorPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: actor settle level=" << level << " refCopies=" << activeCopies
                    << " pos=(" << actorPosition.x() << "," << actorPosition.y() << "," << actorPosition.z()
                    << ") retailChairDistance=" << chairDistance << " inCombat=" << (inCombat ? 1 : 0)
                    << " result=" << (fnvInteractionActorPass ? "pass" : "fail");
                aimInteractionCameraAt(fnvInteractionActor, "outside-easy-pete-settled",
                    readProofFloat("OPENMW_FNV_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueInteractionCapture("outside-easy-pete-settled");
                advanceInteraction(fnvInteractionDoorOnly ? 3 : 8);
            }
        }
        else if (fnvInteractionPhase == 8 && phaseFrames >= 8)
        {
            if (!activateInteractionTarget(fnvInteractionActor, "easy-pete-dialogue"))
                finishInteraction(false, "Easy Pete activation returned a null action");
            else
                advanceInteraction(1);
        }
        else if (fnvInteractionPhase == 1)
        {
            const bool dialogueOpen = mWindowManager->containsMode(MWGui::GM_Dialogue);
            const bool voiceActive = mSoundManager != nullptr && mSoundManager->sayActive(fnvInteractionActor);
            fnvInteractionGreetingAudioSeen = fnvInteractionGreetingAudioSeen || voiceActive;
            if (dialogueOpen && fnvInteractionGreetingAudioSeen && !voiceActive && phaseFrames >= 5)
            {
                struct InteractionDialogueCallback final : MWBase::DialogueManager::ResponseCallback
                {
                    bool mHadText = false;
                    void addResponse(std::string_view title, std::string_view text) override
                    {
                        mHadText = mHadText || !text.empty();
                        Log(Debug::Info) << "FNV interaction audit: dialogue response title=\"" << title
                                         << "\" text=\"" << text << "\"";
                    }
                } callback;
                constexpr std::string_view topic = "Why are you called Easy Pete?";
                mDialogueManager->keywordSelected(topic, &callback);
                fnvInteractionDialoguePass = dialogueOpen && callback.mHadText;
                queueInteractionCapture("easy-pete-authored-topic");
                advanceInteraction(2);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishInteraction(false, "Easy Pete greeting GUI or authored voice did not complete");
        }
        else if (fnvInteractionPhase == 2)
        {
            const bool voiceActive = mSoundManager != nullptr && mSoundManager->sayActive(fnvInteractionActor);
            fnvInteractionTopicAudioSeen = fnvInteractionTopicAudioSeen || voiceActive;
            if (fnvInteractionTopicAudioSeen && !voiceActive && phaseFrames >= 5)
            {
                fnvInteractionDialoguePass = fnvInteractionDialoguePass && fnvInteractionGreetingAudioSeen
                    && fnvInteractionTopicAudioSeen;
                mDialogueManager->goodbyeSelected();
                mWindowManager->removeGuiMode(MWGui::GM_Dialogue);
                const bool stageSet = mWorld->getESM4QuestRuntime().setStage("VCG02", 5);
                const MWWorld::ESM4QuestState* quest = mWorld->getESM4QuestRuntime().search("VCG02");
                fnvInteractionQuestPass = stageSet && quest != nullptr && quest->mCurrentStage == 5
                    && quest->mStageDone.contains(5) && quest->mStageDone.at(5);
                Log(fnvInteractionQuestPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: quest VCG02 stage=5 state="
                    << (quest != nullptr ? static_cast<int>(quest->mCurrentStage) : -1)
                    << " notificationQueued=" << (fnvInteractionQuestPass ? 1 : 0)
                    << " result=" << (fnvInteractionQuestPass ? "pass" : "fail");
                advanceInteraction(3);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishInteraction(false, "Easy Pete authored topic voice did not complete");
        }
        else if (fnvInteractionPhase == 3)
        {
            if (!fnvInteractionDoorOnly && !fnvInteractionQuestCaptureQueued && phaseFrames >= 10)
            {
                fnvInteractionQuestCaptureQueued = true;
                queueInteractionCapture("quest-notification");
            }
            if (phaseFrames >= 90)
            {
                MWWorld::Ptr door = findActiveRef(0x110636f);
                const ESM::RefId expectedInterior(ESM::FormId::fromUint32(0x1106185));
                const bool authoredPair = !door.isEmpty() && door.getClass().isDoor()
                    && door.getCellRef().getTeleport() && door.getCellRef().getDestCell() == expectedInterior;
                fnvInteractionDoorInPass
                    = authoredPair && activateInteractionTarget(door, "prospector-saloon-front-door");
                Log(fnvInteractionDoorInPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: exterior door sourceRef=FormId:0x110636f expectedCell="
                    << expectedInterior.toDebugString() << " authoredPair=" << (authoredPair ? 1 : 0)
                    << " result=" << (fnvInteractionDoorInPass ? "pass" : "fail");
                if (!fnvInteractionDoorInPass)
                    finishInteraction(false, "Prospector Saloon exterior XTEL action failed");
                else
                    advanceInteraction(4);
            }
        }
        else if (fnvInteractionPhase == 4)
        {
            const ESM::RefId expectedInterior(ESM::FormId::fromUint32(0x1106185));
            if (playerCellId() == expectedInterior && phaseFrames >= 180)
            {
                int actorCount = 0;
                for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
                {
                    if (cellstore == nullptr)
                        continue;
                    cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                        if (!ptr.isEmpty() && ptr.getClass().isActor())
                            ++actorCount;
                        return true;
                    });
                }
                const bool sunny = !findActiveRef(0x1104e85).isEmpty();
                const bool trudy = !findActiveRef(0x1104c6d).isEmpty();
                const bool cheyenne = !findActiveRef(0x110588e).isEmpty();
                fnvInteractionInteriorActorsPass = actorCount >= 2 && (sunny || trudy) && cheyenne;
                Log(fnvInteractionInteriorActorsPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: saloon interior cell=" << playerCellId().toDebugString()
                    << " actors=" << actorCount << " Sunny=" << (sunny ? 1 : 0)
                    << " Trudy=" << (trudy ? 1 : 0) << " Cheyenne=" << (cheyenne ? 1 : 0)
                    << " result=" << (fnvInteractionInteriorActorsPass ? "pass" : "fail");
                MWWorld::Ptr interiorProofActor = findActiveRef(0x110588e);
                if (interiorProofActor.isEmpty())
                    interiorProofActor = !findActiveRef(0x1104e85).isEmpty() ? findActiveRef(0x1104e85)
                                                                          : findActiveRef(0x1104c6d);
                aimInteractionCameraAt(interiorProofActor, "saloon-interior-actors",
                    readProofFloat("OPENMW_FNV_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueInteractionCapture("saloon-interior-actors");

                fnvInteractionRadio = findActiveRef(0x1109087);
                fnvInteractionRadioSound = {};
                if (!fnvInteractionRadio.isEmpty() && fnvInteractionRadio.getType() == ESM4::Activator::sRecordId)
                {
                    const ESM4::Activator& activator = *fnvInteractionRadio.get<ESM4::Activator>()->mBase;
                    ESM::FormId broadcast = activator.mLoopingSound;
                    if (broadcast.isZeroOrUnset())
                        broadcast = activator.mRadioTemplate;
                    if (broadcast.isZeroOrUnset() && !activator.mRadioStation.isZeroOrUnset())
                    {
                        const ESM4::TalkingActivator* station = mWorld->getStore()
                            .get<ESM4::TalkingActivator>()
                            .search(ESM::RefId(activator.mRadioStation));
                        if (station != nullptr)
                            broadcast = !station->mLoopSound.isZeroOrUnset() ? station->mLoopSound
                                                                            : station->mRadioTemplate;
                    }
                    fnvInteractionRadioSound = ESM::RefId(broadcast);
                }
                const bool radioAction = activateInteractionTarget(fnvInteractionRadio, "goodsprings-radio");
                const bool radioPlaying = radioAction && mUseSound && mSoundManager != nullptr
                    && !fnvInteractionRadioSound.empty()
                    && mSoundManager->getSoundPlaying(fnvInteractionRadio, fnvInteractionRadioSound);
                fnvInteractionRadioPass = radioPlaying;
                Log(fnvInteractionRadioPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: radio sourceRef=FormId:0x1109087 sound="
                    << fnvInteractionRadioSound.toDebugString() << " soundEnabled=" << (mUseSound ? 1 : 0)
                    << " playing=" << (radioPlaying ? 1 : 0)
                    << " result=" << (fnvInteractionRadioPass ? "pass" : "fail");
                advanceInteraction(5);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishInteraction(false, "Prospector Saloon interior did not become active");
        }
        else if (fnvInteractionPhase == 5)
        {
            const bool radioStillPlaying = mSoundManager != nullptr && !fnvInteractionRadio.isEmpty()
                && !fnvInteractionRadioSound.empty()
                && mSoundManager->getSoundPlaying(fnvInteractionRadio, fnvInteractionRadioSound);
            fnvInteractionRadioPass = fnvInteractionRadioPass && radioStillPlaying;
            if (!fnvInteractionRadioCaptureQueued && phaseFrames >= 45)
            {
                fnvInteractionRadioCaptureQueued = true;
                aimInteractionCameraAt(fnvInteractionRadio, "goodsprings-radio-on",
                    readProofFloat("OPENMW_FNV_INTERACTION_RADIO_AIM_Z", 24.f));
                queueInteractionCapture("goodsprings-radio-on");
            }
            if (fnvInteractionRadioCaptureQueued && phaseFrames >= 53)
            {
                MWWorld::Ptr exitDoor = findActiveRef(0x110618e);
                const bool authoredExit = !exitDoor.isEmpty() && exitDoor.getClass().isDoor()
                    && exitDoor.getCellRef().getTeleport();
                fnvInteractionDoorOutPass
                    = authoredExit && activateInteractionTarget(exitDoor, "prospector-saloon-exit-door");
                Log(fnvInteractionDoorOutPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: interior exit sourceRef=FormId:0x110618e authoredPair="
                    << (authoredExit ? 1 : 0) << " radioStillPlaying=" << (radioStillPlaying ? 1 : 0)
                    << " result=" << (fnvInteractionDoorOutPass ? "pass" : "fail");
                if (!fnvInteractionDoorOutPass)
                    finishInteraction(false, "Prospector Saloon interior XTEL action failed");
                else
                    advanceInteraction(6);
            }
        }
        else if (fnvInteractionPhase == 6)
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const bool outside = !player.isEmpty() && player.getCell() != nullptr && player.getCell()->isExterior();
            if (outside && phaseFrames >= 180)
            {
                MWWorld::Ptr peteAfterReturn = findActiveRef(0x1104c80);
                const int activeCopies = countActiveRef(0x1104c80);
                float actorDrift = std::numeric_limits<float>::infinity();
                bool actorInCombat = true;
                if (!peteAfterReturn.isEmpty())
                {
                    actorDrift = (peteAfterReturn.getRefData().getPosition().asVec3()
                                     - fnvInteractionActorSettledPosition)
                                     .length();
                    actorInCombat = peteAfterReturn.getClass()
                                        .getCreatureStats(peteAfterReturn)
                                        .getAiSequence()
                                        .isInCombat();
                }
                fnvInteractionActorPass = fnvInteractionActorPass && activeCopies == 1
                    && actorDrift <= 256.f && !actorInCombat;
                Log(fnvInteractionActorPass ? Debug::Info : Debug::Error)
                    << "FNV interaction audit: exterior return cell=" << playerCellId().toDebugString()
                    << " EasyPeteCopies=" << activeCopies << " actorDrift=" << actorDrift
                    << " actorInCombat=" << (actorInCombat ? 1 : 0)
                    << " weather=" << mWorld->getCurrentWeatherScriptId()
                    << " weatherTransition=" << mWorld->getWeatherTransition()
                    << " result=" << (fnvInteractionActorPass ? "pass" : "fail");
                aimInteractionCameraAt(peteAfterReturn, "outside-return-weather-actor",
                    readProofFloat("OPENMW_FNV_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueInteractionCapture("outside-return-weather-actor");
                advanceInteraction(7);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishInteraction(false, "Prospector Saloon exit did not return to the exterior");
        }
        else if (fnvInteractionPhase == 7 && phaseFrames >= 8)
        {
            const bool pass = fnvInteractionActorPass && fnvInteractionDoorInPass
                && fnvInteractionInteriorActorsPass && fnvInteractionRadioPass && fnvInteractionDoorOutPass
                && (fnvInteractionDoorOnly || (fnvInteractionDialoguePass && fnvInteractionQuestPass));
            finishInteraction(pass,
                pass ? (fnvInteractionDoorOnly ? "complete authored door circuit"
                                               : "complete authored interaction circuit")
                     : "one or more authored interaction gates failed");
        }
    }

    const bool authoredInteractionRequested = proofEnvEnabled("OPENMW_AUTHORED_INTERACTION_AUDIT");
    if (authoredInteractionRequested && authoredInteractionPhase >= 0 && proofRunning && mWorld != nullptr
        && mWindowManager != nullptr)
    {
        // This audit uses only normal activation actions, but it deliberately crosses cells and
        // repositions the player between authored interaction points. Keep those mutations on the
        // main thread and outside the Lua worker's nearby-object traversal window.
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
        const auto envText = [](const char* name) -> std::string_view {
            const char* value = std::getenv(name);
            return value != nullptr ? std::string_view(value) : std::string_view();
        };
        const auto formIdFromEnv = [&](const char* name) {
            const std::string_view value = envText(name);
            const std::optional<ESM::FormId> parsed = parseProofFormId(value);
            return parsed.value_or(ESM::FormId());
        };
        const std::string_view auditLabel = [&]() {
            const std::string_view configured = envText("OPENMW_AUTHORED_INTERACTION_LABEL");
            return configured.empty() ? std::string_view("unnamed") : configured;
        }();
        const ESM::FormId actorFormId = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_ACTOR_REF");
        const ESM::FormId doorInFormId = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_DOOR_IN_REF");
        const ESM::FormId interiorCellFormId
            = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_INTERIOR_CELL");
        const ESM::FormId interiorActorFormId
            = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_INTERIOR_ACTOR_REF");
        const ESM::FormId radioFormId = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_RADIO_REF");
        const ESM::FormId doorOutFormId = formIdFromEnv("OPENMW_AUTHORED_INTERACTION_DOOR_OUT_REF");

        const auto findActiveRef = [&](ESM::FormId formId) {
            MWWorld::Ptr found;
            if (formId.isZeroOrUnset())
                return found;
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (!ptr.isEmpty() && ptr.getCellRef().getRefNum() == formId)
                    {
                        found = ptr;
                        return false;
                    }
                    return true;
                });
                if (!found.isEmpty())
                    break;
            }
            return found;
        };
        const auto countActiveRef = [&](ESM::FormId formId) {
            int count = 0;
            if (formId.isZeroOrUnset())
                return count;
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (!ptr.isEmpty() && ptr.getCellRef().getRefNum() == formId)
                        ++count;
                    return true;
                });
            }
            return count;
        };
        const auto playerCellId = [&]() {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            if (player.isEmpty() || player.getCell() == nullptr || player.getCell()->getCell() == nullptr)
                return ESM::RefId();
            return player.getCell()->getCell()->getId();
        };
        const auto queueCapture = [&](std::string_view label) {
            if (mScreenCaptureHandler == nullptr)
                return;
            Log(Debug::Info) << "Authored interaction audit: label=" << auditLabel
                             << " queueCapture=" << label << " frame=" << frameNumber;
            mScreenCaptureHandler->setFramesToCapture(1);
            mScreenCaptureHandler->captureNextFrame(*mViewer);
        };
        const auto activateTarget = [&](const MWWorld::Ptr& target, std::string_view label) {
            if (target.isEmpty())
                return false;
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            std::unique_ptr<MWWorld::Action> action = target.getClass().activate(target, player);
            const bool actionable = action != nullptr && !action->isNullAction();
            Log(actionable ? Debug::Info : Debug::Error)
                << "Authored interaction audit: label=" << auditLabel << " activate=" << label
                << " target=" << target.toString() << " type=" << target.getTypeDescription()
                << " actionable=" << (actionable ? 1 : 0);
            if (actionable)
                action->execute(player);
            return actionable;
        };
        const auto aimCameraAt = [&](const MWWorld::Ptr& target, std::string_view label, float offsetZ) {
            MWRender::Camera* camera = mWorld->getCamera();
            if (camera == nullptr || target.isEmpty())
                return false;
            const osg::Vec3d cameraPosition = camera->getPosition();
            osg::Vec3d aimPosition = target.getRefData().getPosition().asVec3();
            aimPosition.z() += offsetZ;
            const osg::Vec3d delta = aimPosition - cameraPosition;
            const double horizontal = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            if (horizontal < 1.0)
                return false;
            camera->setPitch(static_cast<float>(std::atan2(delta.z(), horizontal)), true);
            camera->setYaw(-static_cast<float>(std::atan2(delta.x(), delta.y())), true);
            camera->setRoll(0.f);
            camera->instantTransition();
            camera->updateCamera();
            Log(Debug::Info) << "Authored interaction audit: label=" << auditLabel << " aim=" << label
                             << " cameraPos=(" << cameraPosition.x() << "," << cameraPosition.y() << ","
                             << cameraPosition.z() << ") targetPos=(" << aimPosition.x() << ","
                             << aimPosition.y() << "," << aimPosition.z() << ")";
            return true;
        };
        const auto gateSummary = [&]() {
            std::ostringstream stream;
            stream << "actor=" << (authoredInteractionActorPass ? 1 : 0)
                   << " dialogue=" << (authoredInteractionDialoguePass ? 1 : 0)
                   << " doorIn=" << (authoredInteractionDoorInPass ? 1 : 0)
                   << " interiorActors=" << (authoredInteractionInteriorActorsPass ? 1 : 0)
                   << " radio=" << (authoredInteractionRadioPass ? 1 : 0)
                   << " doorOut=" << (authoredInteractionDoorOutPass ? 1 : 0);
            return stream.str();
        };
        const auto finishAudit = [&](bool pass, std::string_view reason) {
            Log(pass ? Debug::Info : Debug::Error)
                << "Authored interaction audit: label=" << auditLabel
                << " result=" << (pass ? "pass" : "fail") << " reason=\"" << reason << "\" "
                << gateSummary() << " frame=" << frameNumber;
            authoredInteractionPhase = -1;
            mStateManager->requestQuit();
        };
        const auto advanceAudit = [&](int phase) {
            authoredInteractionPhase = phase;
            authoredInteractionPhaseFrame = frameNumber;
            authoredInteractionPhaseStartTime = frameStart;
            Log(Debug::Info) << "Authored interaction audit: label=" << auditLabel
                             << " phase=" << phase << " frame=" << frameNumber;
        };
        const unsigned int phaseFrames = frameNumber - authoredInteractionPhaseFrame;
        const double phaseSeconds = authoredInteractionPhaseStartTime != 0
            ? timer->delta_s(authoredInteractionPhaseStartTime, frameStart)
            : 0.0;
        const double phaseTimeoutSeconds = std::max(
            5.f, readProofFloat("OPENMW_AUTHORED_INTERACTION_PHASE_TIMEOUT_SECONDS", 45.f));

        if (authoredInteractionPhase == 0 && proofWorldReady
            && proofWorldReadyFrames
                >= std::max(1, readProofInt("OPENMW_AUTHORED_INTERACTION_SETTLE_FRAMES", 240)))
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            player.getClass().getCreatureStats(player).setLevel(1);
            if (player.getClass().isNpc())
                player.getClass().getNpcStats(player).setLevelProgress(0);
            authoredInteractionActor = findActiveRef(actorFormId);
            if (authoredInteractionActor.isEmpty())
                finishAudit(false, "authored exterior actor is not active");
            else
            {
                const osg::Vec3f playerPosition = player.getRefData().getPosition().asVec3();
                const osg::Vec3f actorPosition
                    = authoredInteractionActor.getRefData().getPosition().asVec3();
                const float actorDistance = (actorPosition - playerPosition).length();
                const bool actorInCombat = authoredInteractionActor.getClass()
                                               .getCreatureStats(authoredInteractionActor)
                                               .getAiSequence()
                                               .isInCombat();
                const int actorCopies = countActiveRef(actorFormId);
                authoredInteractionActorSettledPosition = actorPosition;
                authoredInteractionActorPass = actorCopies == 1 && !actorInCombat
                    && actorDistance <= readProofFloat("OPENMW_AUTHORED_INTERACTION_MAX_ACTOR_DISTANCE", 1024.f);
                Log(authoredInteractionActorPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel << " actorSettle level="
                    << player.getClass().getCreatureStats(player).getLevel() << " copies=" << actorCopies
                    << " distance=" << actorDistance << " inCombat=" << (actorInCombat ? 1 : 0)
                    << " pos=(" << actorPosition.x() << "," << actorPosition.y() << ","
                    << actorPosition.z() << ") result="
                    << (authoredInteractionActorPass ? "pass" : "fail");
                if (MWRender::Camera* camera = mWorld->getCamera())
                {
                    camera->attachTo(player);
                    camera->setMode(MWRender::Camera::Mode::FirstPerson, true);
                    camera->setPreferredCameraDistance(0.f);
                    camera->processViewChange();
                    camera->update(0.f, false);
                    camera->instantTransition();
                    camera->updateCamera();
                }
                aimCameraAt(authoredInteractionActor, "exterior-actor",
                    readProofFloat("OPENMW_AUTHORED_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueCapture("exterior-actor");
                advanceAudit(1);
            }
        }
        else if (authoredInteractionPhase == 1 && phaseFrames >= 8)
        {
            if (!activateTarget(authoredInteractionActor, "exterior-actor-dialogue"))
                finishAudit(false, "exterior actor activation returned a null action");
            else
                advanceAudit(2);
        }
        else if (authoredInteractionPhase == 2)
        {
            const bool dialogueOpen = mWindowManager->containsMode(MWGui::GM_Dialogue);
            const bool voiceActive
                = mSoundManager != nullptr && mSoundManager->sayActive(authoredInteractionActor);
            authoredInteractionGreetingAudioSeen
                = authoredInteractionGreetingAudioSeen || voiceActive;
            if (dialogueOpen && authoredInteractionGreetingAudioSeen && !voiceActive && phaseFrames >= 5)
            {
                struct AuthoredInteractionDialogueCallback final
                    : MWBase::DialogueManager::ResponseCallback
                {
                    bool mHadText = false;
                    void addResponse(std::string_view title, std::string_view text) override
                    {
                        mHadText = mHadText || !text.empty();
                        Log(Debug::Info) << "Authored interaction audit: dialogueResponse title=\""
                                         << title << "\" text=\"" << text << "\"";
                    }
                } callback;
                const std::string_view topic = envText("OPENMW_AUTHORED_INTERACTION_DIALOGUE_TOPIC");
                if (!topic.empty())
                    mDialogueManager->keywordSelected(topic, &callback);
                authoredInteractionDialoguePass = mUseSound && dialogueOpen
                    && authoredInteractionGreetingAudioSeen && (topic.empty() || callback.mHadText);
                queueCapture("authored-dialogue-topic");
                if (topic.empty())
                {
                    mDialogueManager->goodbyeSelected();
                    mWindowManager->removeGuiMode(MWGui::GM_Dialogue);
                    authoredInteractionTopicAudioSeen = true;
                }
                advanceAudit(3);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishAudit(false, "authored greeting GUI or voice did not complete");
        }
        else if (authoredInteractionPhase == 3)
        {
            const std::string_view topic = envText("OPENMW_AUTHORED_INTERACTION_DIALOGUE_TOPIC");
            const bool voiceActive
                = mSoundManager != nullptr && mSoundManager->sayActive(authoredInteractionActor);
            authoredInteractionTopicAudioSeen = authoredInteractionTopicAudioSeen || voiceActive;
            if ((topic.empty() || (authoredInteractionTopicAudioSeen && !voiceActive)) && phaseFrames >= 5)
            {
                authoredInteractionDialoguePass = authoredInteractionDialoguePass
                    && authoredInteractionTopicAudioSeen;
                if (!topic.empty())
                {
                    mDialogueManager->goodbyeSelected();
                    mWindowManager->removeGuiMode(MWGui::GM_Dialogue);
                }
                const std::string_view preDoorScript
                    = envText("OPENMW_AUTHORED_INTERACTION_PRE_DOOR_SCRIPT");
                if (!preDoorScript.empty())
                {
                    Log(Debug::Info) << "Authored interaction audit: label=" << auditLabel
                                     << " executePreDoorScript=\"" << preDoorScript << "\"";
                    mWindowManager->executeInConsole(std::filesystem::path(preDoorScript));
                }
                advanceAudit(4);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishAudit(false, "authored dialogue topic voice did not complete");
        }
        else if (authoredInteractionPhase == 4)
        {
            const unsigned int settleFrames = static_cast<unsigned int>(std::max(
                8, readProofInt("OPENMW_AUTHORED_INTERACTION_PRE_DOOR_SETTLE_FRAMES", 180)));
            if (phaseFrames >= settleFrames)
            {
                MWWorld::Ptr door = findActiveRef(doorInFormId);
                const ESM::RefId expectedInterior(interiorCellFormId);
                const bool authoredPair = !door.isEmpty() && door.getClass().isDoor()
                    && door.getCellRef().getTeleport()
                    && (interiorCellFormId.isZeroOrUnset()
                        || door.getCellRef().getDestCell() == expectedInterior);
                if (!door.isEmpty())
                    aimCameraAt(door, "exterior-door", 48.f);
                authoredInteractionDoorInPass = authoredPair && activateTarget(door, "exterior-door");
                Log(authoredInteractionDoorInPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel
                    << " exteriorDoor=" << doorInFormId.toUint32()
                    << " expectedInterior=" << expectedInterior.toDebugString()
                    << " authoredPair=" << (authoredPair ? 1 : 0)
                    << " result=" << (authoredInteractionDoorInPass ? "pass" : "fail");
                if (!authoredInteractionDoorInPass)
                    finishAudit(false, "authored exterior door action failed");
                else
                    advanceAudit(5);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishAudit(false, "authored exterior door did not become active");
        }
        else if (authoredInteractionPhase == 5)
        {
            const bool expectedCellActive = interiorCellFormId.isZeroOrUnset()
                ? !mWorld->getPlayerPtr().getCell()->isExterior()
                : playerCellId() == ESM::RefId(interiorCellFormId);
            if (expectedCellActive && phaseFrames >= 180)
            {
                int actorCount = 0;
                for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
                {
                    if (cellstore == nullptr)
                        continue;
                    cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                        if (!ptr.isEmpty() && ptr.getClass().isActor())
                            ++actorCount;
                        return true;
                    });
                }
                MWWorld::Ptr interiorActor = findActiveRef(interiorActorFormId);
                const bool requiredActorPresent
                    = interiorActorFormId.isZeroOrUnset() || !interiorActor.isEmpty();
                authoredInteractionInteriorActorsPass = actorCount >= 1 && requiredActorPresent;
                Log(authoredInteractionInteriorActorsPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel
                    << " interiorCell=" << playerCellId().toDebugString() << " actors=" << actorCount
                    << " requiredActor=" << interiorActorFormId.toUint32()
                    << " requiredActorPresent=" << (requiredActorPresent ? 1 : 0)
                    << " result=" << (authoredInteractionInteriorActorsPass ? "pass" : "fail");
                if (!interiorActor.isEmpty())
                    aimCameraAt(interiorActor, "interior-actor",
                        readProofFloat("OPENMW_AUTHORED_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueCapture("interior-actors");

                authoredInteractionRadioPass = radioFormId.isZeroOrUnset();
                if (!radioFormId.isZeroOrUnset())
                {
                    authoredInteractionRadio = findActiveRef(radioFormId);
                    authoredInteractionRadioSound = {};
                    if (!authoredInteractionRadio.isEmpty()
                        && authoredInteractionRadio.getType() == ESM4::Activator::sRecordId)
                    {
                        const ESM4::Activator& activator
                            = *authoredInteractionRadio.get<ESM4::Activator>()->mBase;
                        ESM::FormId broadcast = activator.mLoopingSound;
                        if (broadcast.isZeroOrUnset())
                            broadcast = activator.mRadioTemplate;
                        if (broadcast.isZeroOrUnset() && !activator.mRadioStation.isZeroOrUnset())
                        {
                            const ESM4::TalkingActivator* station = mWorld->getStore()
                                .get<ESM4::TalkingActivator>()
                                .search(ESM::RefId(activator.mRadioStation));
                            if (station != nullptr)
                                broadcast = !station->mLoopSound.isZeroOrUnset()
                                    ? station->mLoopSound
                                    : station->mRadioTemplate;
                        }
                        authoredInteractionRadioSound = ESM::RefId(broadcast);
                    }
                    const bool radioAction
                        = activateTarget(authoredInteractionRadio, "interior-radio");
                    const bool radioPlaying = mSoundManager != nullptr
                        && ((!authoredInteractionRadioSound.empty()
                                && mSoundManager->getSoundPlaying(
                                    authoredInteractionRadio, authoredInteractionRadioSound))
                            || mSoundManager->sayActive(authoredInteractionRadio));
                    authoredInteractionRadioPass = radioAction && mUseSound && radioPlaying;
                }
                Log(authoredInteractionRadioPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel
                    << " radio=" << radioFormId.toUint32()
                    << " sound=" << authoredInteractionRadioSound.toDebugString()
                    << " soundEnabled=" << (mUseSound ? 1 : 0)
                    << " result=" << (authoredInteractionRadioPass ? "pass" : "fail");
                advanceAudit(6);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishAudit(false, "authored interior did not become active");
        }
        else if (authoredInteractionPhase == 6)
        {
            if (!radioFormId.isZeroOrUnset())
            {
                const bool radioStillPlaying = mSoundManager != nullptr
                    && !authoredInteractionRadio.isEmpty()
                    && ((!authoredInteractionRadioSound.empty()
                            && mSoundManager->getSoundPlaying(
                                authoredInteractionRadio, authoredInteractionRadioSound))
                        || mSoundManager->sayActive(authoredInteractionRadio));
                authoredInteractionRadioPass = authoredInteractionRadioPass && radioStillPlaying;
                if (!authoredInteractionRadioCaptureQueued && phaseFrames >= 45)
                {
                    authoredInteractionRadioCaptureQueued = true;
                    aimCameraAt(authoredInteractionRadio, "interior-radio",
                        readProofFloat("OPENMW_AUTHORED_INTERACTION_RADIO_AIM_Z", 24.f));
                    queueCapture("interior-radio-on");
                }
            }
            else
                authoredInteractionRadioCaptureQueued = true;

            if (authoredInteractionRadioCaptureQueued && phaseFrames >= 53)
            {
                MWWorld::Ptr exitDoor = findActiveRef(doorOutFormId);
                const bool authoredExit = !exitDoor.isEmpty() && exitDoor.getClass().isDoor()
                    && exitDoor.getCellRef().getTeleport();
                authoredInteractionDoorOutPass
                    = authoredExit && activateTarget(exitDoor, "interior-exit-door");
                Log(authoredInteractionDoorOutPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel
                    << " interiorExit=" << doorOutFormId.toUint32()
                    << " authoredPair=" << (authoredExit ? 1 : 0)
                    << " result=" << (authoredInteractionDoorOutPass ? "pass" : "fail");
                if (!authoredInteractionDoorOutPass)
                    finishAudit(false, "authored interior exit action failed");
                else
                    advanceAudit(7);
            }
        }
        else if (authoredInteractionPhase == 7)
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const bool outside = !player.isEmpty() && player.getCell() != nullptr
                && player.getCell()->isExterior();
            if (outside && phaseFrames >= 180)
            {
                MWWorld::Ptr actorAfterReturn = findActiveRef(actorFormId);
                const bool requireActorOnReturn
                    = proofEnvEnabled("OPENMW_AUTHORED_INTERACTION_REQUIRE_ACTOR_ON_RETURN");
                const int actorCopies = countActiveRef(actorFormId);
                float actorDrift = std::numeric_limits<float>::infinity();
                bool actorInCombat = false;
                if (!actorAfterReturn.isEmpty())
                {
                    actorDrift = (actorAfterReturn.getRefData().getPosition().asVec3()
                                     - authoredInteractionActorSettledPosition)
                                     .length();
                    actorInCombat = actorAfterReturn.getClass()
                                        .getCreatureStats(actorAfterReturn)
                                        .getAiSequence()
                                        .isInCombat();
                }
                const bool actorReturnPass = requireActorOnReturn
                    ? (!actorAfterReturn.isEmpty() && actorCopies == 1 && !actorInCombat
                        && actorDrift
                            <= readProofFloat("OPENMW_AUTHORED_INTERACTION_MAX_ACTOR_DRIFT", 512.f))
                    : (actorAfterReturn.isEmpty() || (actorCopies == 1 && !actorInCombat));
                authoredInteractionActorPass = authoredInteractionActorPass && actorReturnPass;
                Log(authoredInteractionActorPass ? Debug::Info : Debug::Error)
                    << "Authored interaction audit: label=" << auditLabel << " exteriorReturnCell="
                    << playerCellId().toDebugString() << " actorCopies=" << actorCopies
                    << " actorDrift=" << actorDrift << " actorInCombat=" << (actorInCombat ? 1 : 0)
                    << " weather=" << mWorld->getCurrentWeatherScriptId()
                    << " weatherTransition=" << mWorld->getWeatherTransition()
                    << " result=" << (authoredInteractionActorPass ? "pass" : "fail");
                if (!actorAfterReturn.isEmpty())
                    aimCameraAt(actorAfterReturn, "exterior-return-actor",
                        readProofFloat("OPENMW_AUTHORED_INTERACTION_ACTOR_AIM_Z", 96.f));
                queueCapture("exterior-return");
                advanceAudit(8);
            }
            else if (phaseSeconds > phaseTimeoutSeconds)
                finishAudit(false, "authored exit did not return to an exterior");
        }
        else if (authoredInteractionPhase == 8 && phaseFrames >= 8)
        {
            const bool pass = authoredInteractionActorPass && authoredInteractionDialoguePass
                && authoredInteractionDoorInPass && authoredInteractionInteriorActorsPass
                && authoredInteractionRadioPass && authoredInteractionDoorOutPass;
            finishAudit(pass, pass ? "complete authored interaction circuit"
                                   : "one or more authored interaction gates failed");
        }
    }

    const bool proofScreenshotFrameReached = !proofSidecarEnabled
        && proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofScreenshotReadyFramesReached
        = !proofSidecarEnabled && !proofScreenshotReadyQueued && proofScreenshotReadyFrames >= 0
        && proofWorldReadyFrames >= proofScreenshotReadyFrames;
    bool worldViewerCameraReadyForScreenshot = true;
    if (!worldViewerStaticStartCamera && std::getenv("OPENMW_WORLD_VIEWER_REQUIRE_CAMERA_SETTLED") != nullptr
        && std::getenv("OPENMW_WORLD_VIEWER_START_POS_X") != nullptr && mWorld != nullptr)
    {
        const MWWorld::Ptr player = mWorld->getPlayerPtr();
        const osg::Vec3f cameraPos = mWorld->getCamera()->getPosition();
        const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
        const float cameraDistanceToPlayer = (cameraPos - playerPos).length();
        worldViewerCameraReadyForScreenshot = cameraDistanceToPlayer < 4096.f;
        if (!worldViewerCameraReadyForScreenshot && !worldViewerCameraWaitLogged)
        {
            Log(Debug::Info) << "World viewer proof: waiting for camera settle before screenshot frame=" << frameNumber
                             << " distanceToPlayer=" << cameraDistanceToPlayer << " cameraPos=(" << cameraPos.x()
                             << "," << cameraPos.y() << "," << cameraPos.z() << ") playerPos=(" << playerPos.x()
                             << "," << playerPos.y() << "," << playerPos.z() << ")";
            worldViewerCameraWaitLogged = true;
        }
    }
    const int proofActorAlignedScreenshotDelay = getProofFrame("OPENMW_PROOF_ACTOR_ALIGNED_SCREENSHOT_DELAY");
    const int proofActorAlignedScreenshotMinFrame = getProofFrame("OPENMW_PROOF_ACTOR_ALIGNED_SCREENSHOT_MIN_FRAME");
    const bool proofActorAlignedScreenshotReached = !proofSidecarEnabled
        && !proofActorAlignedScreenshotQueued && proofActorCameraAligned
        && proofActorAlignedScreenshotDelay >= 0 && proofActorCameraAlignedFrame >= 0
        && frameNumber >= static_cast<unsigned>(proofActorCameraAlignedFrame + proofActorAlignedScreenshotDelay)
        && (proofActorAlignedScreenshotMinFrame < 0
            || frameNumber >= static_cast<unsigned>(proofActorAlignedScreenshotMinFrame));
    const bool proofPortraitClearRequired
        = std::getenv("OPENMW_WORLD_VIEWER_REQUIRE_PORTRAIT_CLEAR") != nullptr;
    const bool proofPortraitCapturePending
        = proofScreenshotFrameReached || proofScreenshotReadyFramesReached || proofActorAlignedScreenshotReached;
    bool proofActorVisualReady = true;
    const bool proofActorPoseReadyForScreenshot = proofSidecarEnabled || !proofActorBatchActive || !proofActorPoseSweepEnabled
        || (proofActorPoseBatchIndex == proofActorBatchIndex && proofActorPoseCycleComplete);
    unsigned int proofActorVisibleDrawables = 0;
    unsigned int proofActorVisibleRigs = 0;
    unsigned int proofActorResolvedRigs = 0;
    unsigned int proofActorCullReadyRigs = 0;
    unsigned int proofActorMaximumCullTraversal = 0;
    unsigned int proofActorRootParents = 0;
    unsigned int proofActorRootMask = 0;
    if (proofRequiresActorForScreenshot && proofActorCameraAligned && proofPortraitCapturePending)
    {
        MWWorld::Ptr proofVisualActor;
        if (proofSayActor != nullptr && *proofSayActor != '\0')
            proofVisualActor = resolveProofActor(proofSayActor);
        osg::Node* proofVisualRoot
            = proofVisualActor.isEmpty() ? nullptr : proofVisualActor.getRefData().getBaseNode();
        if (proofVisualRoot != nullptr)
        {
            proofActorRootParents = proofVisualRoot->getNumParents();
            proofActorRootMask = proofVisualRoot->getNodeMask();
            FalloutProofDrawableCullVisitor visitor(
                static_cast<unsigned int>(std::max(0, proofActorCameraAlignedFrame)));
            proofVisualRoot->accept(visitor);
            proofActorVisibleDrawables = visitor.mVisibleDrawables;
            proofActorVisibleRigs = visitor.mVisibleRigs;
            proofActorResolvedRigs = visitor.mResolvedRigs;
            proofActorCullReadyRigs = visitor.mCullReadyRigs;
            proofActorMaximumCullTraversal = visitor.mMaximumCullTraversal;
        }
        proofActorVisualReady = !proofVisualActor.isEmpty() && proofVisualActor.getRefData().isEnabled()
            && proofVisualRoot != nullptr && proofActorRootParents > 0 && proofActorRootMask != 0
            && proofActorVisibleDrawables > 0
            && (proofActorVisibleRigs == 0
                || (proofActorResolvedRigs > 0 && proofActorCullReadyRigs > 0));
    }
    bool proofPortraitClear = !proofPortraitClearRequired;
    std::string proofPortraitRejectReason;
    FalloutProofPortraitPose proofPortraitPose;
    float proofPortraitHeadX = -1.f;
    float proofPortraitHeadY = -1.f;
    float proofPortraitLeftHandOffsetZ = std::numeric_limits<float>::quiet_NaN();
    float proofPortraitRightHandOffsetZ = std::numeric_limits<float>::quiet_NaN();
    float proofPortraitHeadMotion = std::numeric_limits<float>::quiet_NaN();
    float proofPortraitForwardDot = std::numeric_limits<float>::quiet_NaN();
    if (proofPortraitClearRequired && proofPortraitCapturePending && mWorld != nullptr && mViewer != nullptr)
    {
        const char* actorRefText = std::getenv("OPENMW_WORLD_VIEWER_START_CAMERA_FOLLOW_REF");
        MWWorld::Ptr portraitActor;
        if (actorRefText != nullptr && *actorRefText != '\0')
        {
            const ESM::RefId actorRef = makeProofRefId(actorRefText);
            portraitActor = mWorld->searchPtr(actorRef, true, false);
            if (portraitActor.isEmpty())
            {
                if (const ESM::FormId* formId = actorRef.getIf<ESM::FormId>())
                    portraitActor = MWBase::Environment::get().getWorldModel()->getPtr(*formId);
            }
        }
        proofPortraitPose = resolveFalloutProofPortraitPose(portraitActor);

        bool frameClear = true;
        if (!proofPortraitPose.mHeadResolved)
        {
            frameClear = false;
            proofPortraitRejectReason = "head-unresolved";
        }

        const osg::Camera* renderCamera = mViewer->getCamera();
        const osg::Viewport* viewport = renderCamera != nullptr ? renderCamera->getViewport() : nullptr;
        if (frameClear && viewport != nullptr && viewport->width() > 0.0 && viewport->height() > 0.0)
        {
            const osg::Vec3d window = proofPortraitPose.mHeadCenter * renderCamera->getViewMatrix()
                * renderCamera->getProjectionMatrix() * viewport->computeWindowMatrix();
            proofPortraitHeadX = static_cast<float>((window.x() - viewport->x()) / viewport->width());
            proofPortraitHeadY = static_cast<float>((window.y() - viewport->y()) / viewport->height());
            const float minHeadX = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MIN_HEAD_X", 0.30f);
            const float maxHeadX = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MAX_HEAD_X", 0.70f);
            const float minHeadY = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MIN_HEAD_Y", 0.38f);
            const float maxHeadY = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MAX_HEAD_Y", 0.72f);
            if (proofPortraitHeadX < minHeadX || proofPortraitHeadX > maxHeadX
                || proofPortraitHeadY < minHeadY || proofPortraitHeadY > maxHeadY)
            {
                frameClear = false;
                proofPortraitRejectReason = "head-outside-portrait-safe-area";
            }
        }
        else if (frameClear)
        {
            frameClear = false;
            proofPortraitRejectReason = "viewport-unresolved";
        }

        const float maximumHandOffsetZ
            = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MAX_HAND_OFFSET_Z", -18.f);
        if (proofPortraitPose.mLeftHandResolved)
        {
            proofPortraitLeftHandOffsetZ
                = static_cast<float>(proofPortraitPose.mLeftHandCenter.z() - proofPortraitPose.mHeadCenter.z());
            if (proofPortraitLeftHandOffsetZ > maximumHandOffsetZ)
            {
                frameClear = false;
                proofPortraitRejectReason = "left-hand-near-face";
            }
        }
        else
        {
            frameClear = false;
            proofPortraitRejectReason = "left-hand-unresolved";
        }
        if (proofPortraitPose.mRightHandResolved)
        {
            proofPortraitRightHandOffsetZ
                = static_cast<float>(proofPortraitPose.mRightHandCenter.z() - proofPortraitPose.mHeadCenter.z());
            if (proofPortraitRightHandOffsetZ > maximumHandOffsetZ)
            {
                frameClear = false;
                proofPortraitRejectReason = "right-hand-near-face";
            }
        }
        else
        {
            frameClear = false;
            proofPortraitRejectReason = "right-hand-unresolved";
        }

        if (proofPortraitPose.mHeadResolved && proofPortraitPreviousHeadResolved)
        {
            proofPortraitHeadMotion
                = static_cast<float>((proofPortraitPose.mHeadCenter - proofPortraitPreviousHead).length());
            proofPortraitForwardDot
                = static_cast<float>(proofPortraitPose.mHeadForward * proofPortraitPreviousForward);
            const float maximumHeadMotion
                = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MAX_HEAD_MOTION", 1.5f);
            const float minimumForwardDot
                = readProofFloat("OPENMW_WORLD_VIEWER_PORTRAIT_MIN_FORWARD_DOT", 0.995f);
            if (proofPortraitHeadMotion > maximumHeadMotion || proofPortraitForwardDot < minimumForwardDot)
            {
                frameClear = false;
                proofPortraitRejectReason = "head-pose-unstable";
            }
        }
        else if (proofPortraitPose.mHeadResolved)
        {
            frameClear = false;
            proofPortraitRejectReason = "head-stability-warmup";
        }

        if (proofPortraitPose.mHeadResolved)
        {
            proofPortraitPreviousHead = proofPortraitPose.mHeadCenter;
            proofPortraitPreviousForward = proofPortraitPose.mHeadForward;
            proofPortraitPreviousHeadResolved = true;
        }
        else
            proofPortraitPreviousHeadResolved = false;

        proofPortraitClearFrames = frameClear ? proofPortraitClearFrames + 1 : 0;
        const int requiredClearFrames
            = std::max(1, readProofInt("OPENMW_WORLD_VIEWER_PORTRAIT_CLEAR_FRAMES", 8));
        proofPortraitClear = frameClear && proofPortraitClearFrames >= requiredClearFrames;
        if (frameClear && !proofPortraitClear)
            proofPortraitRejectReason = "clear-frame-settling";
        if (!proofPortraitClear && (static_cast<int>(frameNumber) - proofPortraitLastRejectLogFrame >= 30
            || proofPortraitClearFrames == requiredClearFrames - 1))
        {
            proofPortraitLastRejectLogFrame = static_cast<int>(frameNumber);
            Log(Debug::Info) << "World viewer portrait acceptance: frame=" << frameNumber
                             << " screenshotIndex=" << proofScreenshotFrameIndex << " headNormalized=("
                             << proofPortraitHeadX << "," << proofPortraitHeadY << ") handOffsetZ=("
                             << proofPortraitLeftHandOffsetZ << "," << proofPortraitRightHandOffsetZ
                             << ") headMotion=" << proofPortraitHeadMotion << " forwardDot="
                             << proofPortraitForwardDot << " clearFrames=" << proofPortraitClearFrames << "/"
                             << requiredClearFrames << " status=reject reason=" << proofPortraitRejectReason;
        }
    }
    if ((proofScreenshotFrameReached || proofScreenshotReadyFramesReached || proofActorAlignedScreenshotReached)
        && mScreenCaptureHandler != nullptr)
    {
        if (!proofWorldReady && !proofScreenshotWaitLogged)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: waiting to capture screenshot until world is ready state="
                             << static_cast<int>(mStateManager->getState()) << " loadingGui=" << proofLoadingGui
                             << " frame=" << frameNumber;
            proofScreenshotWaitLogged = true;
        }
        if (!proofWorldReady)
        {
            worldViewerTrace(frameNumber, "screenshot-wait-render.begin");
            mViewer->renderingTraversals();
            worldViewerTrace(frameNumber, "screenshot-wait-render.end");
            worldViewerTrace(frameNumber, "screenshot-wait-lua-finish.begin");
            mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
            worldViewerTrace(frameNumber, "screenshot-wait-lua-finish.end");
            return true;
        }
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
        if (!proofActorPoseReadyForScreenshot)
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
        if (!proofPortraitClear)
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
        | (backgroundPlayableSession ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (!backgroundPlayableSession && windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (!backgroundPlayableSession && windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    if (backgroundPlayableSession)
    {
        Log(Debug::Info) << "Playable session: creating a hidden flat OpenGL window for background native capture";
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

    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine lua permanent storage begin";
    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine lua init begin";
    mLuaManager->init();
    Log(Debug::Verbose) << "FNV/ESM4 diag: prepareEngine lua init complete";

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

