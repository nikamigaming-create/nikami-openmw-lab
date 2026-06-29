#include "engine.hpp"
#include "mwrender/assetcapture.hpp"

#include <cerrno>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <future>
#include <filesystem>
#include <fstream>
#include <istream>
#include <limits>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/Texture2D>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <MyGUI_LogManager.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/format.hpp>
#include <components/misc/strings/lower.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadavif.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#ifdef OPENMW_ENABLE_VR
#include "mwmechanics/actorutil.hpp"
#include "mwvr/vrinputmanager.hpp"
#include "mwvr/vrgui.hpp"
#include "mwvr/vranimation.hpp"
#include <components/misc/callbackmanager.hpp>
#include <components/vr/session.hpp>
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
#include <components/xr/instance.hpp>
#include <components/xr/interactionprofiles.hpp>
#include <components/xr/session.hpp>
#endif

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/cellstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/globals.hpp"
#include "mwworld/inventorystore.hpp"
#include "mwworld/livecellref.hpp"
#include "mwworld/manualref.hpp"
#include "mwworld/scene.hpp"
#include "mwworld/worldimp.hpp"

#include "mwphysics/raycasting.hpp"

#include "mwrender/vismask.hpp"
#include "mwrender/camera.hpp"
#include "mwrender/characterpreview.hpp"
#include "mwrender/renderingmanager.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/creaturestats.hpp"
#include "mwmechanics/mechanicsmanagerimp.hpp"
#include "mwmechanics/npcstats.hpp"

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

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    void logFnvDlodSettingsProbe(const VFS::Manager& vfs)
    {
        if (std::getenv("OPENMW_FNV_DLODSETTINGS_DIAG") == nullptr)
            return;

        std::size_t count = 0;
        std::size_t totalBytes = 0;
        for (const VFS::Path::Normalized& path : vfs.getRecursiveDirectoryIterator("lodsettings/"))
        {
            if (!Misc::StringUtils::ciEndsWith(path.value(), ".dlodsettings"))
                continue;

            try
            {
                Files::IStreamPtr stream = vfs.get(path);
                stream->ignore(std::numeric_limits<std::streamsize>::max());
                const std::streamoff bytes = stream->gcount();
                if (bytes < 0)
                    throw std::runtime_error("negative byte count");

                ++count;
                totalBytes += static_cast<std::size_t>(bytes);
                Log(Debug::Info) << "FNV/ESM4 proof: DLOD settings loaded path=" << path.value()
                                 << " archive=\"" << vfs.getArchive(path) << "\" bytes=" << bytes;
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: DLOD settings load failed path=" << path.value()
                                    << " error=" << e.what();
            }
        }

        Log(Debug::Info) << "FNV/ESM4 proof: DLOD settings summary count=" << count << " totalBytes=" << totalBytes
                         << " pagingBinding=loaded-pending-runtime";
    }

    void logFnvPsaDeathPoseProbe(const VFS::Manager& vfs)
    {
        if (std::getenv("OPENMW_FNV_PSA_DEATHPOSE_DIAG") == nullptr)
            return;

        std::size_t count = 0;
        std::size_t totalBytes = 0;
        for (const VFS::Path::Normalized& path : vfs.getRecursiveDirectoryIterator("meshes/"))
        {
            if (!Misc::StringUtils::ciEndsWith(path.value(), ".psa"))
                continue;
            if (path.value().find("deathpose") == std::string::npos)
                continue;

            try
            {
                Files::IStreamPtr stream = vfs.get(path);
                stream->ignore(std::numeric_limits<std::streamsize>::max());
                const std::streamoff bytes = stream->gcount();
                if (bytes < 0)
                    throw std::runtime_error("negative byte count");

                ++count;
                totalBytes += static_cast<std::size_t>(bytes);
                Log(Debug::Info) << "FNV/ESM4 proof: PSA death-pose loaded path=" << path.value()
                                 << " archive=\"" << vfs.getArchive(path) << "\" bytes=" << bytes;
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: PSA death-pose load failed path=" << path.value()
                                    << " error=" << e.what();
            }
        }

        Log(Debug::Info) << "FNV/ESM4 proof: PSA death-pose summary count=" << count << " totalBytes=" << totalBytes
                         << " playbackBinding=loaded-pending-runtime";
    }

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

#ifdef OPENMW_ENABLE_VR
    class InitializeVrOperation : public osg::GraphicsOperation
    {
    public:
        InitializeVrOperation(OMW::Engine* engine)
            : GraphicsOperation("InitializeVrOperation", false)
            , mEngine(engine)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override { mEngine->configureVRGraphics(graphicsContext); }

    private:
        OMW::Engine* mEngine;
    };
#endif

    std::vector<int> getProofFrames(const char* name)
    {
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

            int frame = -1;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), frame);
            if (result.ec == std::errc() && result.ptr == token.data() + token.size() && frame >= 0)
                frames.push_back(frame);

            if (comma == std::string_view::npos)
                break;
            value.remove_prefix(comma + 1);
        }

        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        return frames;
    }

    float getProofFloat(const char* name, float fallback)
    {
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return fallback;

        char* end = nullptr;
        const float value = std::strtof(env, &end);
        if (end == env || *end != '\0')
            return fallback;

        return value;
    }

    int getProofInt(const char* name, int fallback)
    {
        const char* env = std::getenv(name);
        if (env == nullptr || *env == '\0')
            return fallback;

        char* end = nullptr;
        const long value = std::strtol(env, &end, 10);
        if (end == env || *end != '\0')
            return fallback;

        return static_cast<int>(value);
    }

    std::string escapeProofRuntimeJsonRegex(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size() * 2);
        for (char c : value)
        {
            switch (c)
            {
                case '\\':
                case '.':
                case '^':
                case '$':
                case '|':
                case '(':
                case ')':
                case '[':
                case ']':
                case '{':
                case '}':
                case '*':
                case '+':
                case '?':
                    escaped.push_back('\\');
                    break;
                default:
                    break;
            }
            escaped.push_back(c);
        }
        return escaped;
    }

    std::string unescapeProofRuntimeJsonString(std::string value)
    {
        std::string result;
        result.reserve(value.size());
        bool escaped = false;
        for (char c : value)
        {
            if (escaped)
            {
                switch (c)
                {
                    case '"':
                    case '\\':
                    case '/':
                        result.push_back(c);
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    default:
                        result.push_back(c);
                        break;
                }
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            result.push_back(c);
        }
        return result;
    }

    bool readProofRuntimeCommandString(const std::string& content, std::string_view key, std::string& value)
    {
        try
        {
            const std::regex pattern("\"" + escapeProofRuntimeJsonRegex(key)
                + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"");
            std::smatch match;
            if (!std::regex_search(content, match, pattern))
                return false;
            value = unescapeProofRuntimeJsonString(match[1].str());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool readProofRuntimeCommandFile(const char* path, std::string& content)
    {
        if (path == nullptr || path[0] == '\0')
            return false;
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return false;
        std::ostringstream stream;
        stream << input.rdbuf();
        content = stream.str();
        return true;
    }

    std::string getProofRuntimeActorTarget(std::string fallback)
    {
        const char* path = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
        if (path == nullptr || path[0] == '\0')
            return fallback;

        std::string content;
        if (!readProofRuntimeCommandFile(path, content))
            return fallback;

        std::string target;
        for (std::string_view key : { std::string_view("actorTarget"), std::string_view("runtimeTarget"),
                 std::string_view("target") })
        {
            if (readProofRuntimeCommandString(content, key, target) && !target.empty())
                return target;
        }
        return fallback;
    }

    std::string getProofRuntimeCommandString(std::initializer_list<std::string_view> keys, std::string fallback = {})
    {
        const char* path = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
        if (path == nullptr || path[0] == '\0')
            return fallback;

        std::string content;
        if (!readProofRuntimeCommandFile(path, content))
            return fallback;

        std::string value;
        for (std::string_view key : keys)
        {
            if (readProofRuntimeCommandString(content, key, value) && !value.empty())
                return value;
        }
        return fallback;
    }

    float getProofRuntimeFloat(std::initializer_list<std::string_view> keys, float fallback)
    {
        const char* path = std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
        if (path == nullptr || path[0] == '\0')
            return fallback;

        std::string content;
        if (!readProofRuntimeCommandFile(path, content))
            return fallback;

        std::string valueText;
        for (std::string_view key : keys)
        {
            if (!readProofRuntimeCommandString(content, key, valueText) || valueText.empty())
                continue;
            try
            {
                size_t consumed = 0;
                const float value = std::stof(valueText, &consumed);
                if (consumed == valueText.size() && std::isfinite(value))
                    return value;
            }
            catch (...)
            {
            }
        }
        return fallback;
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
            osg::ref_ptr<osg::Texture2D> texture = previews[i]->getTexture();
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

        Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview composite uses upright RTT V coordinates"
                         << " runtime=runtime-supported gate=runtime-neutral-actor-preview-uv";
        return camera.release();
    }

    std::string falloutProofFormToken(std::string_view value)
    {
        while (!value.empty()
            && (std::isspace(static_cast<unsigned char>(value.front())) || value.front() == '"' || value.front() == '\''))
            value.remove_prefix(1);
        while (!value.empty()
            && (std::isspace(static_cast<unsigned char>(value.back())) || value.back() == '"' || value.back() == '\''))
            value.remove_suffix(1);

        std::string text(value);
        Misc::StringUtils::lowerCaseInPlace(text);
        constexpr std::string_view prefix = "formid:";
        if (text.rfind(prefix, 0) == 0)
            text.erase(0, prefix.size());
        if (text.rfind("0x", 0) == 0)
            text.erase(0, 2);
        if (text.empty())
            return {};
        if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
            return {};
        if (text.size() > 6)
            text.erase(0, text.size() - 6);
        while (text.size() < 6)
            text.insert(text.begin(), '0');
        return text;
    }

    bool falloutProofFormTargetMatches(std::string_view candidate, std::string_view target)
    {
        const std::string candidateToken = falloutProofFormToken(candidate);
        return !candidateToken.empty() && candidateToken == falloutProofFormToken(target);
    }

    ESM::RefId falloutProofTargetRefId(std::string_view target)
    {
        try
        {
            return ESM::RefId::deserializeText(target);
        }
        catch (...)
        {
            return ESM::RefId::stringRefId(target);
        }
    }

    std::optional<ESM::FormId> falloutProofTargetFormId(std::string_view target)
    {
        const ESM::RefId refId = falloutProofTargetRefId(target);
        if (const ESM::FormId* formId = refId.getIf<ESM::FormId>())
            return *formId;
        return std::nullopt;
    }

    const ESM4::Npc* findFalloutProofNpcBaseByTarget(
        const MWWorld::ESMStore& store, std::string_view target, int& scanned)
    {
        scanned = 0;
        if (target.empty())
            return nullptr;

        const std::string targetText(target);
        const std::string targetLower = Misc::StringUtils::lowerCase(targetText);
        if (const ESM4::Npc* npc = store.get<ESM4::Npc>().search(falloutProofTargetRefId(target)))
            return npc;

        const auto& npcs = store.get<ESM4::Npc>();
        for (auto it = npcs.begin(); it != npcs.end(); ++it)
        {
            ++scanned;
            const ESM4::Npc& npc = *it;
            if (!npc.mIsFONV)
                continue;

            const std::string editorLower = Misc::StringUtils::lowerCase(npc.mEditorId);
            const std::string fullLower = Misc::StringUtils::lowerCase(npc.mFullName);
            const std::string form = ESM::RefId(npc.mId).toDebugString();
            const std::string formLower = Misc::StringUtils::lowerCase(form);

            if (editorLower == targetLower || fullLower == targetLower || formLower == targetLower
                || falloutProofFormTargetMatches(form, targetText)
                || (!editorLower.empty() && editorLower.find(targetLower) != std::string::npos)
                || (!fullLower.empty() && fullLower.find(targetLower) != std::string::npos)
                || (!editorLower.empty() && !targetLower.empty()
                    && targetLower.find(editorLower) != std::string::npos)
                || (!fullLower.empty() && !targetLower.empty() && targetLower.find(fullLower) != std::string::npos))
                return &npc;
        }

        return nullptr;
    }

    const ESM4::ActorCharacter* findFalloutProofActorRefByTarget(
        const MWWorld::ESMStore& store, std::string_view target, int& scanned)
    {
        scanned = 0;
        if (target.empty())
            return nullptr;

        const std::string targetText(target);
        const std::string targetLower = Misc::StringUtils::lowerCase(targetText);
        const std::optional<ESM::FormId> targetFormId = falloutProofTargetFormId(target);
        if (targetFormId)
        {
            if (const ESM4::ActorCharacter* ref = store.get<ESM4::ActorCharacter>().searchStatic(*targetFormId))
                return ref;
        }

        const auto& actorRefs = store.get<ESM4::ActorCharacter>();
        for (auto it = actorRefs.begin(); it != actorRefs.end(); ++it)
        {
            ++scanned;
            const ESM4::ActorCharacter& ref = *it;
            const std::string editorLower = Misc::StringUtils::lowerCase(ref.mEditorId);
            const std::string fullLower = Misc::StringUtils::lowerCase(ref.mFullName);
            const std::string form = ESM::RefId(ref.mId).toDebugString();
            const std::string formLower = Misc::StringUtils::lowerCase(form);
            const std::string baseForm = ESM::RefId(ref.mBaseObj).toDebugString();
            const std::string baseFormLower = Misc::StringUtils::lowerCase(baseForm);

            if (editorLower == targetLower || fullLower == targetLower || formLower == targetLower
                || baseFormLower == targetLower || falloutProofFormTargetMatches(form, targetText)
                || falloutProofFormTargetMatches(baseForm, targetText)
                || (!editorLower.empty() && editorLower.find(targetLower) != std::string::npos)
                || (!fullLower.empty() && fullLower.find(targetLower) != std::string::npos))
                return &ref;
        }

        return nullptr;
    }

    const ESM4::Creature* findFalloutProofCreatureBaseByTarget(
        const MWWorld::ESMStore& store, std::string_view target, int& scanned)
    {
        scanned = 0;
        if (target.empty())
            return nullptr;

        const std::string targetText(target);
        const std::string targetLower = Misc::StringUtils::lowerCase(targetText);
        if (const ESM4::Creature* creature = store.get<ESM4::Creature>().search(falloutProofTargetRefId(target)))
            return creature;

        const auto& creatures = store.get<ESM4::Creature>();
        for (auto it = creatures.begin(); it != creatures.end(); ++it)
        {
            ++scanned;
            const ESM4::Creature& creature = *it;

            const std::string editorLower = Misc::StringUtils::lowerCase(creature.mEditorId);
            const std::string fullLower = Misc::StringUtils::lowerCase(creature.mFullName);
            const std::string form = ESM::RefId(creature.mId).toDebugString();
            const std::string formLower = Misc::StringUtils::lowerCase(form);

            if (editorLower == targetLower || fullLower == targetLower || formLower == targetLower
                || falloutProofFormTargetMatches(form, targetText)
                || (!editorLower.empty() && editorLower.find(targetLower) != std::string::npos)
                || (!fullLower.empty() && fullLower.find(targetLower) != std::string::npos)
                || (!editorLower.empty() && !targetLower.empty()
                    && targetLower.find(editorLower) != std::string::npos)
                || (!fullLower.empty() && !targetLower.empty() && targetLower.find(fullLower) != std::string::npos))
                return &creature;
        }

        return nullptr;
    }

    const ESM4::ActorCharacter* findFalloutProofCreatureRefByTarget(
        const MWWorld::ESMStore& store, std::string_view target, int& scanned)
    {
        scanned = 0;
        if (target.empty())
            return nullptr;

        const std::string targetText(target);
        const std::string targetLower = Misc::StringUtils::lowerCase(targetText);
        const std::optional<ESM::FormId> targetFormId = falloutProofTargetFormId(target);
        if (targetFormId)
        {
            if (const ESM4::ActorCreature* ref = store.get<ESM4::ActorCreature>().searchStatic(*targetFormId))
                return ref;
        }

        const auto& creatureRefs = store.get<ESM4::ActorCreature>();
        for (auto it = creatureRefs.begin(); it != creatureRefs.end(); ++it)
        {
            ++scanned;
            const ESM4::ActorCharacter& ref = *it;
            const std::string editorLower = Misc::StringUtils::lowerCase(ref.mEditorId);
            const std::string fullLower = Misc::StringUtils::lowerCase(ref.mFullName);
            const std::string form = ESM::RefId(ref.mId).toDebugString();
            const std::string formLower = Misc::StringUtils::lowerCase(form);
            const std::string baseForm = ESM::RefId(ref.mBaseObj).toDebugString();
            const std::string baseFormLower = Misc::StringUtils::lowerCase(baseForm);

            if (editorLower == targetLower || fullLower == targetLower || formLower == targetLower
                || baseFormLower == targetLower || falloutProofFormTargetMatches(form, targetText)
                || falloutProofFormTargetMatches(baseForm, targetText)
                || (!editorLower.empty() && editorLower.find(targetLower) != std::string::npos)
                || (!fullLower.empty() && fullLower.find(targetLower) != std::string::npos))
                return &ref;
        }

        return nullptr;
    }

    ESM::RefId getProofRefId(const char* name, const char* fallback)
    {
        const char* env = std::getenv(name);
        return ESM::RefId::stringRefId(env != nullptr && *env != '\0' ? env : fallback);
    }

    const ESM4::Weapon* findEsm4WeaponByEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& weapons = store.get<ESM4::Weapon>();
        for (auto it = weapons.begin(); it != weapons.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->mEditorId, editorId))
                return &*it;
        }
        return nullptr;
    }

    const ESM4::Ammunition* findEsm4AmmoByEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& ammo = store.get<ESM4::Ammunition>();
        for (auto it = ammo.begin(); it != ammo.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->mEditorId, editorId))
                return &*it;
        }
        return nullptr;
    }

    const ESM4::Perk* findEsm4PerkByEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& perks = store.get<ESM4::Perk>();
        for (auto it = perks.begin(); it != perks.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->mEditorId, editorId))
                return &*it;
        }
        return nullptr;
    }

    const ESM4::ActorValueInfo* findEsm4ActorValueByEditorId(
        const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& actorValues = store.get<ESM4::ActorValueInfo>();
        for (auto it = actorValues.begin(); it != actorValues.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->mEditorId, editorId))
                return &*it;
        }
        return nullptr;
    }

    const ESM4::Quest* findEsm4QuestByEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
    {
        const auto& quests = store.get<ESM4::Quest>();
        for (auto it = quests.begin(); it != quests.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->mEditorId, editorId))
                return &*it;
        }
        return nullptr;
    }

    const ESM4::QuestObjective* findEsm4QuestObjective(
        const ESM4::Quest& quest, int objectiveIndex)
    {
        for (const ESM4::QuestObjective& objective : quest.mObjectives)
        {
            if (objective.mIndex == objectiveIndex)
                return &objective;
        }
        return nullptr;
    }

    struct FalloutQuestTargetResolution
    {
        ESM::RecNameInts mTargetRecordType = static_cast<ESM::RecNameInts>(0);
        ESM::RefId mParentCell;
        ESM::FormId mBaseObject;
        ESM::Position mPosition;
        std::string mEditorId;
        std::string mFullName;
    };

    std::optional<FalloutQuestTargetResolution> resolveFalloutQuestTarget(
        const MWWorld::ESMStore& store, ESM::FormId target)
    {
        if (const ESM4::Reference* ref = store.get<ESM4::Reference>().searchStatic(target))
        {
            return FalloutQuestTargetResolution{
                .mTargetRecordType = ESM::REC_REFR4,
                .mParentCell = ref->mParent,
                .mBaseObject = ref->mBaseObj,
                .mPosition = ref->mPos,
                .mEditorId = ref->mEditorId,
                .mFullName = ref->mFullName,
            };
        }

        if (const ESM4::ActorCharacter* ref = store.get<ESM4::ActorCharacter>().searchStatic(target))
        {
            return FalloutQuestTargetResolution{
                .mTargetRecordType = ESM::REC_ACHR4,
                .mParentCell = ref->mParent,
                .mBaseObject = ref->mBaseObj,
                .mPosition = ref->mPos,
                .mEditorId = ref->mEditorId,
                .mFullName = ref->mFullName,
            };
        }

        if (const ESM4::ActorCharacter* ref = store.get<ESM4::ActorCreature>().searchStatic(target))
        {
            return FalloutQuestTargetResolution{
                .mTargetRecordType = ESM::REC_ACRE4,
                .mParentCell = ref->mParent,
                .mBaseObject = ref->mBaseObj,
                .mPosition = ref->mPos,
                .mEditorId = ref->mEditorId,
                .mFullName = ref->mFullName,
            };
        }

        return std::nullopt;
    }

    bool hasFinitePosition(const ESM::Position& position)
    {
        return std::isfinite(position.pos[0]) && std::isfinite(position.pos[1]) && std::isfinite(position.pos[2])
            && std::isfinite(position.rot[0]) && std::isfinite(position.rot[1]) && std::isfinite(position.rot[2]);
    }

    std::string recNameToProofString(ESM::RecNameInts recName)
    {
        if (recName == static_cast<ESM::RecNameInts>(0))
            return "";
        return std::string(ESM::getRecNameString(recName).toStringView());
    }

    std::string proofHex(std::uint32_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << value;
        return stream.str();
    }

    std::string falloutConditionFunctionName(std::uint32_t function)
    {
        switch (function)
        {
            case ESM4::FUN_GetQuestRunning:
                return "GetQuestRunning";
            case ESM4::FUN_GetStage:
                return "GetStage";
            case ESM4::FUN_GetStageDone:
                return "GetStageDone";
            case ESM4::FUN_GetObjectiveCompleted:
                return "GetObjectiveCompleted";
            case ESM4::FUN_GetObjectiveDisplayed:
                return "GetObjectiveDisplayed";
            case ESM4::FUN_GetQuestCompleted:
                return "GetQuestCompleted";
            default:
                return "Unsupported";
        }
    }

    struct FalloutQuestConditionRef
    {
        const ESM4::Quest* mOwnerQuest = nullptr;
        const ESM4::TargetCondition* mCondition = nullptr;
        std::string mOwnerScope;
        int mStageIndex = -1;
        int mEntryIndex = -1;
        int mObjectiveIndex = -1;
        int mTargetIndex = -1;
        int mConditionIndex = -1;
    };

    void considerFalloutQuestCondition(FalloutQuestConditionRef& found, const ESM4::Quest& ownerQuest,
        const ESM4::TargetCondition& condition, std::string_view ownerScope, int stageIndex, int entryIndex,
        int objectiveIndex, int targetIndex, int conditionIndex, std::uint32_t function)
    {
        if (found.mCondition != nullptr || condition.functionIndex != function)
            return;

        found.mOwnerQuest = &ownerQuest;
        found.mCondition = &condition;
        found.mOwnerScope = std::string(ownerScope);
        found.mStageIndex = stageIndex;
        found.mEntryIndex = entryIndex;
        found.mObjectiveIndex = objectiveIndex;
        found.mTargetIndex = targetIndex;
        found.mConditionIndex = conditionIndex;
    }

    FalloutQuestConditionRef findFalloutQuestConditionByFunction(
        const MWWorld::ESMStore& store, std::uint32_t function)
    {
        FalloutQuestConditionRef found;
        const auto& quests = store.get<ESM4::Quest>();
        for (auto it = quests.begin(); it != quests.end(); ++it)
        {
            const ESM4::Quest& quest = *it;
            for (std::size_t index = 0; index < quest.mTargetConditions.size(); ++index)
            {
                considerFalloutQuestCondition(found, quest, quest.mTargetConditions[index], "QUST-top-level", -1, -1,
                    -1, -1, static_cast<int>(index), function);
                if (found.mCondition != nullptr)
                    return found;
            }
            for (const ESM4::QuestStage& stage : quest.mStages)
            {
                for (std::size_t entryIndex = 0; entryIndex < stage.mEntries.size(); ++entryIndex)
                {
                    const ESM4::QuestStageEntry& entry = stage.mEntries[entryIndex];
                    for (std::size_t conditionIndex = 0; conditionIndex < entry.mConditions.size(); ++conditionIndex)
                    {
                        considerFalloutQuestCondition(found, quest, entry.mConditions[conditionIndex],
                            "QUST-stage-entry", stage.mIndex, static_cast<int>(entryIndex), -1, -1,
                            static_cast<int>(conditionIndex), function);
                        if (found.mCondition != nullptr)
                            return found;
                    }
                }
            }
            for (const ESM4::QuestObjective& objective : quest.mObjectives)
            {
                for (std::size_t targetIndex = 0; targetIndex < objective.mTargets.size(); ++targetIndex)
                {
                    const ESM4::QuestObjectiveTarget& target = objective.mTargets[targetIndex];
                    for (std::size_t conditionIndex = 0; conditionIndex < target.mConditions.size(); ++conditionIndex)
                    {
                        considerFalloutQuestCondition(found, quest, target.mConditions[conditionIndex],
                            "QUST-objective-target", -1, -1, objective.mIndex, static_cast<int>(targetIndex),
                            static_cast<int>(conditionIndex), function);
                        if (found.mCondition != nullptr)
                            return found;
                    }
                }
            }
        }
        return found;
    }

    const ESM4::Quest* resolveFalloutConditionQuestParam(
        const MWWorld::ESMStore& store, std::uint32_t rawFormId)
    {
        const ESM::FormId formId = ESM::FormId::fromUint32(rawFormId);
        if (const ESM4::Quest* quest = store.get<ESM4::Quest>().searchStatic(formId))
            return quest;

        const ESM4::Quest* fallback = nullptr;
        const auto& quests = store.get<ESM4::Quest>();
        for (auto it = quests.begin(); it != quests.end(); ++it)
        {
            if (it->mId.mIndex != formId.mIndex)
                continue;
            if (fallback != nullptr)
                return nullptr;
            fallback = &*it;
        }
        return fallback;
    }

    ESM::RefId falloutQuestRuntimeId(const ESM4::Quest& quest)
    {
        if (!quest.mEditorId.empty())
            return ESM::RefId::stringRefId(quest.mEditorId);
        return ESM::RefId::formIdRefId(quest.mId);
    }

    bool compareFalloutConditionValue(float actual, const ESM4::TargetCondition& condition, bool& supported)
    {
        supported = (condition.condition & ESM4::CTF_UseGlobal) == 0;
        if (!supported)
            return false;

        const float expected = condition.comparison;
        constexpr float epsilon = 0.0001f;
        switch (condition.condition & 0xE0)
        {
            case ESM4::CTF_EqualTo:
                return std::abs(actual - expected) <= epsilon;
            case ESM4::CTF_NotEqualTo:
                return std::abs(actual - expected) > epsilon;
            case ESM4::CTF_GreaterThan:
                return actual > expected;
            case ESM4::CTF_GrThOrEqTo:
                return actual + epsilon >= expected;
            case ESM4::CTF_LessThan:
                return actual < expected;
            case ESM4::CTF_LeThOrEqTo:
                return actual <= expected + epsilon;
            default:
                supported = false;
                return false;
        }
    }

    struct FalloutConditionEvaluation
    {
        bool mFunctionSupported = false;
        bool mParamQuestFound = false;
        bool mComparisonSupported = false;
        bool mResult = false;
        float mValue = 0.f;
        ESM::RefId mParamQuestId;
        std::string mParamQuestEditorId;
    };

    FalloutConditionEvaluation evaluateFalloutQuestCondition(
        const MWWorld::ESMStore& store, MWBase::Journal& journal, const ESM4::TargetCondition& condition)
    {
        FalloutConditionEvaluation result;
        const ESM4::Quest* paramQuest = resolveFalloutConditionQuestParam(store, condition.param1);
        result.mParamQuestFound = paramQuest != nullptr;
        if (paramQuest == nullptr)
            return result;

        result.mParamQuestId = falloutQuestRuntimeId(*paramQuest);
        result.mParamQuestEditorId = paramQuest->mEditorId;

        switch (condition.functionIndex)
        {
            case ESM4::FUN_GetQuestRunning:
                result.mFunctionSupported = true;
                result.mValue = journal.isQuestStarted(result.mParamQuestId)
                    && !journal.getQuestFinished(result.mParamQuestId) ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetStage:
                result.mFunctionSupported = true;
                result.mValue = static_cast<float>(journal.getJournalIndex(result.mParamQuestId));
                break;
            case ESM4::FUN_GetStageDone:
                result.mFunctionSupported = true;
                result.mValue = journal.isQuestStageDone(
                    result.mParamQuestId, static_cast<int>(condition.param2)) ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetObjectiveCompleted:
                result.mFunctionSupported = true;
                result.mValue = journal.getQuestObjectiveCompleted(
                    result.mParamQuestId, static_cast<int>(condition.param2)) ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetObjectiveDisplayed:
                result.mFunctionSupported = true;
                result.mValue = journal.getQuestObjectiveDisplayed(
                    result.mParamQuestId, static_cast<int>(condition.param2)) ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetQuestCompleted:
                result.mFunctionSupported = true;
                result.mValue = journal.getQuestFinished(result.mParamQuestId) ? 1.f : 0.f;
                break;
            default:
                return result;
        }

        result.mResult = compareFalloutConditionValue(result.mValue, condition, result.mComparisonSupported);
        return result;
    }

    int falloutConditionStageValueForResult(const ESM4::TargetCondition& condition, bool expectedResult)
    {
        const int comparison = static_cast<int>(std::lround(condition.comparison));
        int value = comparison;
        switch (condition.condition & 0xE0)
        {
            case ESM4::CTF_EqualTo:
                value = expectedResult ? comparison : comparison + 1;
                break;
            case ESM4::CTF_NotEqualTo:
                value = expectedResult ? comparison + 1 : comparison;
                break;
            case ESM4::CTF_GreaterThan:
                value = expectedResult ? comparison + 1 : comparison;
                break;
            case ESM4::CTF_GrThOrEqTo:
                value = expectedResult ? comparison : comparison - 1;
                break;
            case ESM4::CTF_LessThan:
                value = expectedResult ? comparison - 1 : comparison;
                break;
            case ESM4::CTF_LeThOrEqTo:
                value = expectedResult ? comparison : comparison + 1;
                break;
            default:
                break;
        }
        return std::max(0, value);
    }

    void setFalloutBooleanConditionState(
        MWBase::Journal& journal, std::uint32_t function, const ESM::RefId& questId, int objectiveIndex, bool value)
    {
        switch (function)
        {
            case ESM4::FUN_GetQuestRunning:
                journal.setQuestFinished(questId, !value);
                break;
            case ESM4::FUN_GetQuestCompleted:
                journal.setQuestFinished(questId, value);
                break;
            case ESM4::FUN_GetObjectiveCompleted:
                journal.setQuestObjectiveCompleted(questId, objectiveIndex, value);
                break;
            case ESM4::FUN_GetObjectiveDisplayed:
                journal.setQuestObjectiveDisplayed(questId, objectiveIndex, value);
                break;
            case ESM4::FUN_GetStageDone:
                if (!value)
                    journal.setJournalIndex(questId, objectiveIndex);
                break;
            default:
                break;
        }
    }

    bool setFalloutConditionStateForResult(const MWWorld::ESMStore& store, MWBase::Journal& journal,
        const ESM4::TargetCondition& condition, bool expectedResult, FalloutConditionEvaluation& evaluation)
    {
        const ESM4::Quest* paramQuest = resolveFalloutConditionQuestParam(store, condition.param1);
        if (paramQuest == nullptr)
            return false;
        const ESM::RefId questId = falloutQuestRuntimeId(*paramQuest);
        if (condition.functionIndex == ESM4::FUN_GetStage)
        {
            journal.setJournalIndex(questId, falloutConditionStageValueForResult(condition, expectedResult));
            evaluation = evaluateFalloutQuestCondition(store, journal, condition);
            return evaluation.mFunctionSupported && evaluation.mComparisonSupported
                && evaluation.mResult == expectedResult;
        }

        for (bool value : { false, true })
        {
            setFalloutBooleanConditionState(journal, condition.functionIndex, questId,
                static_cast<int>(condition.param2), value);
            evaluation = evaluateFalloutQuestCondition(store, journal, condition);
            if (evaluation.mFunctionSupported && evaluation.mComparisonSupported
                && evaluation.mResult == expectedResult)
                return true;
        }

        return false;
    }

    bool runFalloutQuestConditionCase(const MWWorld::ESMStore& store, MWBase::Journal& journal,
        std::uint32_t function)
    {
        const FalloutQuestConditionRef conditionRef = findFalloutQuestConditionByFunction(store, function);
        if (conditionRef.mCondition == nullptr || conditionRef.mOwnerQuest == nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 proof: quest condition evaluator FAIL function=" << function
                             << " functionName=" << falloutConditionFunctionName(function)
                             << " reason=missing-harvested-qust-condition";
            return false;
        }

        FalloutConditionEvaluation passEvaluation;
        FalloutConditionEvaluation failEvaluation;
        const bool passStateApplied = setFalloutConditionStateForResult(
            store, journal, *conditionRef.mCondition, true, passEvaluation);
        const bool failStateApplied = setFalloutConditionStateForResult(
            store, journal, *conditionRef.mCondition, false, failEvaluation);
        const bool pass = passStateApplied && failStateApplied && passEvaluation.mResult && !failEvaluation.mResult;

        Log(Debug::Info) << "FNV/ESM4 proof: quest condition evaluator " << (pass ? "PASS" : "FAIL")
                         << " function=" << function
                         << " functionName=" << falloutConditionFunctionName(function)
                         << " ownerQuest=" << conditionRef.mOwnerQuest->mEditorId
                         << " ownerQuestFormId=" << conditionRef.mOwnerQuest->mId.toString("FormId:")
                         << " ownerScope=" << conditionRef.mOwnerScope
                         << " stageIndex=" << conditionRef.mStageIndex
                         << " entryIndex=" << conditionRef.mEntryIndex
                         << " objectiveIndex=" << conditionRef.mObjectiveIndex
                         << " targetIndex=" << conditionRef.mTargetIndex
                         << " conditionIndex=" << conditionRef.mConditionIndex
                         << " conditionFlags=" << proofHex(conditionRef.mCondition->condition)
                         << " comparison=" << conditionRef.mCondition->comparison
                         << " paramQuest=" << passEvaluation.mParamQuestEditorId
                         << " param1=" << ESM::FormId::fromUint32(conditionRef.mCondition->param1).toString("FormId:")
                         << " param2=" << conditionRef.mCondition->param2
                         << " runOn=" << conditionRef.mCondition->runOn
                         << " passStateApplied=" << (passStateApplied ? 1 : 0)
                         << " passValue=" << passEvaluation.mValue
                         << " passResult=" << (passEvaluation.mResult ? 1 : 0)
                         << " failStateApplied=" << (failStateApplied ? 1 : 0)
                         << " failValue=" << failEvaluation.mValue
                         << " failResult=" << (failEvaluation.mResult ? 1 : 0)
                         << " runtimeBoundary=selected-quest-condition-evaluator-runtime-supported"
                         << " fullConditionRuntime=loaded-pending-runtime"
                         << " unsupportedConditionFunctionsRuntime=loaded-pending-runtime";
        return pass;
    }

    void runFalloutQuestConditionProof()
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MWBase::Journal& journal = *MWBase::Environment::get().getJournal();
        const std::array<std::uint32_t, 5> functions = { {
            ESM4::FUN_GetQuestRunning,
            ESM4::FUN_GetStage,
            ESM4::FUN_GetObjectiveCompleted,
            ESM4::FUN_GetObjectiveDisplayed,
            ESM4::FUN_GetQuestCompleted,
        } };

        int passed = 0;
        for (std::uint32_t function : functions)
        {
            if (runFalloutQuestConditionCase(store, journal, function))
                ++passed;
        }

        const int missingStageDoneQuestConditions
            = findFalloutQuestConditionByFunction(store, ESM4::FUN_GetStageDone).mCondition == nullptr ? 1 : 0;
        const bool pass = passed == static_cast<int>(functions.size()) && missingStageDoneQuestConditions == 1;
        Log(Debug::Info) << "FNV/ESM4 proof: quest condition evaluator summary " << (pass ? "PASS" : "FAIL")
                         << " evaluated=" << passed
                         << " expected=" << functions.size()
                         << " missingObservedQuestGetStageDone=" << missingStageDoneQuestConditions
                         << " runtimeBoundary=selected-quest-condition-evaluator-runtime-supported"
                         << " fullConditionRuntime=loaded-pending-runtime"
                         << " unsupportedConditionFunctionsRuntime=loaded-pending-runtime";
    }

    int getRuntimeGameSettingInt(const MWWorld::ESMStore& store, std::string_view id, int fallback, bool& found)
    {
        const ESM::GameSetting* setting = store.get<ESM::GameSetting>().search(id);
        found = setting != nullptr;
        if (setting == nullptr)
            return fallback;
        try
        {
            return setting->mValue.getInteger();
        }
        catch (const std::exception&)
        {
            return fallback;
        }
    }

    void runFalloutProgressionProof(MWWorld::Ptr player)
    {
        if (player.isEmpty() || !player.getClass().isNpc())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: progression runtime BLOCKED reason=no-player-npc";
            return;
        }

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        bool maxLevelFound = false;
        bool traitSlotsFound = false;
        bool skillBaseFound = false;
        bool skillIntervalFound = false;
        bool xpBumpFound = false;
        const int maxLevel = std::max(1, getRuntimeGameSettingInt(store, "iMaxCharacterLevel", 30, maxLevelFound));
        const int traitSlots
            = std::max(0, getRuntimeGameSettingInt(store, "iTraitMenuMaxNumTraits", 2, traitSlotsFound));
        const int skillPointBase
            = std::max(0, getRuntimeGameSettingInt(store, "iLevelUpSkillPointsBase", 0, skillBaseFound));
        const int skillPointInterval
            = std::max(1, getRuntimeGameSettingInt(store, "iLevelUpSkillPointsInterval", 1, skillIntervalFound));
        const int xpBumpBase = std::max(0, getRuntimeGameSettingInt(store, "iXPBumpBase", 0, xpBumpFound));

        MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
        const int beforeLevel = stats.getLevel();
        const int beforeExperience = stats.getFalloutExperience();
        const int pendingPerkPoints = std::max(0, maxLevel - 1);
        const int syntheticExperience = xpBumpBase * maxLevel;
        const int pendingSkillPointBaseline = skillPointBase + (maxLevel / skillPointInterval);

        stats.setLevel(maxLevel);
        stats.setFalloutProgressionState(syntheticExperience, pendingPerkPoints, traitSlots, maxLevel);

        const bool gmstsFound
            = maxLevelFound && traitSlotsFound && skillBaseFound && skillIntervalFound && xpBumpFound;
        const bool pass = gmstsFound && stats.getLevel() == maxLevel && stats.getFalloutMaxLevel() == maxLevel
            && stats.getFalloutExperience() == syntheticExperience
            && stats.getFalloutPendingPerkPoints() == pendingPerkPoints
            && stats.getFalloutPendingTraitPoints() == traitSlots;

        Log(Debug::Info) << "FNV/ESM4 proof: progression runtime " << (pass ? "PASS" : "FAIL")
                         << " maxLevelGmst=" << maxLevel
                         << " traitSlotsGmst=" << traitSlots
                         << " skillPointBaseGmst=" << skillPointBase
                         << " skillPointIntervalGmst=" << skillPointInterval
                         << " xpBumpBaseGmst=" << xpBumpBase
                         << " gmstsFound=" << gmstsFound
                         << " beforeLevel=" << beforeLevel
                         << " afterLevel=" << stats.getLevel()
                         << " beforeExperience=" << beforeExperience
                         << " afterExperience=" << stats.getFalloutExperience()
                         << " pendingPerkPoints=" << stats.getFalloutPendingPerkPoints()
                         << " pendingTraitPoints=" << stats.getFalloutPendingTraitPoints()
                         << " pendingSkillPointBaseline=" << pendingSkillPointBaseline
                         << " saveSubrecords=FEXP,FPPT,FTPT,FMLV"
                         << " runtimeBoundary=player-max-level-progression-state-runtime-supported"
                         << " xpCurveRuntime=loaded-pending-runtime"
                         << " skillPointFormulaRuntime=loaded-pending-runtime"
                         << " perkAwardRuntime=loaded-pending-runtime"
                         << " traitMenuRuntime=loaded-pending-runtime";
    }

    void runFalloutQuestSaveLoadProof(MWState::StateManager& stateManager, MWWorld::Ptr player)
    {
        if (player.isEmpty())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: quest save/load runtime BLOCKED reason=no-player";
            stateManager.requestQuit();
            return;
        }

        const char* modeEnv = std::getenv("OPENMW_FNV_PROOF_QUEST_SAVELOAD");
        const std::string_view mode(modeEnv != nullptr && *modeEnv != '\0' ? modeEnv : "save");
        const bool savePhase = mode == "save";
        const bool loadPhase = mode == "load";
        if (!savePhase && !loadPhase)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: quest save/load runtime BLOCKED reason=bad-mode mode="
                                << mode;
            stateManager.requestQuit();
            return;
        }

        MWBase::Journal& journal = *MWBase::Environment::get().getJournal();
        const ESM::RefId journalQuest = getProofRefId("OPENMW_FNV_PROOF_QUEST_SAVELOAD_STAGE_QUEST", "VMS57");
        const int stageIndex = std::max(0, getProofInt("OPENMW_FNV_PROOF_QUEST_SAVELOAD_STAGE", 10));
        const ESM::RefId objectiveQuest
            = getProofRefId("OPENMW_FNV_PROOF_QUEST_SAVELOAD_OBJECTIVE_QUEST", "VCG01");
        const int objectiveIndex = std::max(0, getProofInt("OPENMW_FNV_PROOF_QUEST_SAVELOAD_OBJECTIVE", 10));

        int entryAdded = -1;
        int fallbackSetIndex = -1;
        if (savePhase)
        {
            entryAdded = 0;
            fallbackSetIndex = 0;
            try
            {
                journal.addEntry(journalQuest, stageIndex, player);
                entryAdded = 1;
            }
            catch (...)
            {
                if (journal.getJournalIndex(journalQuest) < stageIndex)
                {
                    journal.setJournalIndex(journalQuest, stageIndex);
                    fallbackSetIndex = 1;
                }
            }
            journal.setQuestObjectiveDisplayed(objectiveQuest, objectiveIndex, true);
            journal.setQuestObjectiveCompleted(objectiveQuest, objectiveIndex, true);
        }

        const int currentIndex = journal.getJournalIndex(journalQuest);
        const bool displayed = journal.getQuestObjectiveDisplayed(objectiveQuest, objectiveIndex);
        const bool completed = journal.getQuestObjectiveCompleted(objectiveQuest, objectiveIndex);
        const std::size_t journalEntries = journal.getEntries().size();
        const std::size_t questCount = journal.getQuests().size();
        const std::size_t objectiveStateCount = journal.countQuestObjectiveStates();
        const bool pass = currentIndex == stageIndex && displayed && completed
            && (!savePhase || entryAdded == 1);

        Log(Debug::Info) << "FNV/ESM4 proof: quest save/load runtime " << (pass ? "PASS" : "FAIL")
                         << " phase=" << mode
                         << " journalQuest=" << journalQuest.toDebugString()
                         << " stageIndex=" << stageIndex
                         << " currentIndex=" << currentIndex
                         << " entryAdded=" << entryAdded
                         << " fallbackSetIndex=" << fallbackSetIndex
                         << " objectiveQuest=" << objectiveQuest.toDebugString()
                         << " objective=" << objectiveIndex
                         << " displayed=" << (displayed ? 1 : 0)
                         << " completed=" << (completed ? 1 : 0)
                         << " journalEntries=" << journalEntries
                         << " questCount=" << questCount
                         << " objectiveStateCount=" << objectiveStateCount
                         << " saveRecords=QUES,JOUR,QOBJ"
                         << " runtimeBoundary=selected-fnv-quest-stage-and-objective-save-load-runtime-supported"
                         << " resultScriptRuntime=loaded-pending-runtime"
                         << " conditionRuntime=loaded-pending-runtime"
                         << " targetMarkerRuntime=loaded-pending-runtime"
                         << " fullQuestCompletionRuntime=loaded-pending-runtime";

        if (savePhase && pass)
        {
            stateManager.saveGame("fnv-quest-saveload-proof");
            Log(Debug::Info) << "FNV/ESM4 proof: quest save/load runtime saved description=fnv-quest-saveload-proof";
        }

        stateManager.requestQuit();
    }

    void runFalloutQuestTargetProof()
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const char* questEnv = std::getenv("OPENMW_FNV_PROOF_QUEST_TARGET_QUEST");
        const std::string_view questId(questEnv != nullptr && *questEnv != '\0' ? questEnv : "VMS57");
        const int objectiveIndex = std::max(0, getProofInt("OPENMW_FNV_PROOF_QUEST_TARGET_OBJECTIVE", 10));
        const int targetIndex = std::max(0, getProofInt("OPENMW_FNV_PROOF_QUEST_TARGET_INDEX", 0));

        const ESM4::Quest* quest = findEsm4QuestByEditorId(store, questId);
        const ESM4::QuestObjective* objective = quest != nullptr ? findEsm4QuestObjective(*quest, objectiveIndex)
                                                                 : nullptr;
        const int targetCount = objective != nullptr ? static_cast<int>(objective->mTargets.size()) : 0;
        const ESM4::QuestObjectiveTarget* target
            = objective != nullptr && targetIndex < targetCount ? &objective->mTargets[targetIndex] : nullptr;

        std::optional<FalloutQuestTargetResolution> resolution;
        bool cellFound = false;
        int baseRecordType = 0;
        if (target != nullptr && !target->mTarget.isZeroOrUnset())
        {
            resolution = resolveFalloutQuestTarget(store, target->mTarget);
            if (resolution)
            {
                cellFound = store.get<ESM4::Cell>().search(resolution->mParentCell) != nullptr;
                baseRecordType = store.find(ESM::RefId(resolution->mBaseObject));
            }
        }

        const bool positionFinite = resolution && hasFinitePosition(resolution->mPosition);
        const bool pass = quest != nullptr && objective != nullptr && target != nullptr
            && !target->mTarget.isZeroOrUnset() && resolution.has_value() && cellFound && baseRecordType != 0
            && positionFinite;
        const ESM::RecNameInts targetRecordType
            = resolution ? resolution->mTargetRecordType : static_cast<ESM::RecNameInts>(0);
        const std::string targetRecordTypeName = recNameToProofString(targetRecordType);
        const std::string baseRecordTypeName
            = recNameToProofString(static_cast<ESM::RecNameInts>(baseRecordType));

        Log(Debug::Info) << "FNV/ESM4 proof: quest target runtime " << (pass ? "PASS" : "FAIL")
                         << " quest=" << questId
                         << " questFound=" << (quest != nullptr)
                         << " objective=" << objectiveIndex
                         << " objectiveFound=" << (objective != nullptr)
                         << " targetIndex=" << targetIndex
                         << " targetCount=" << targetCount
                         << " targetFormId="
                         << (target != nullptr ? target->mTarget.toString("FormId:") : "FormId:0x0")
                         << " targetFlags=" << (target != nullptr ? static_cast<int>(target->mFlags) : 0)
                         << " targetConditions="
                         << (target != nullptr ? static_cast<int>(target->mConditions.size()) : 0)
                         << " targetResolved=" << resolution.has_value()
                         << " targetRecordType=" << targetRecordTypeName
                         << " targetRecordTypeHex=0x" << std::hex << static_cast<int>(targetRecordType) << std::dec
                         << " targetEditorId=\"" << (resolution ? resolution->mEditorId : "") << "\""
                         << " targetFullName=\"" << (resolution ? resolution->mFullName : "") << "\""
                         << " parentCell="
                         << (resolution ? resolution->mParentCell.toDebugString() : ESM::RefId().toDebugString())
                         << " cellFound=" << cellFound
                         << " baseFormId="
                         << (resolution ? resolution->mBaseObject.toString("FormId:") : "FormId:0x0")
                         << " baseRecordType=" << baseRecordTypeName
                         << " baseRecordTypeHex=0x" << std::hex << baseRecordType << std::dec
                         << " positionFinite=" << positionFinite
                         << " pos=(" << (resolution ? resolution->mPosition.pos[0] : 0.f) << ","
                         << (resolution ? resolution->mPosition.pos[1] : 0.f) << ","
                         << (resolution ? resolution->mPosition.pos[2] : 0.f) << ")"
                         << " rot=(" << (resolution ? resolution->mPosition.rot[0] : 0.f) << ","
                         << (resolution ? resolution->mPosition.rot[1] : 0.f) << ","
                         << (resolution ? resolution->mPosition.rot[2] : 0.f) << ")"
                         << " runtimeBoundary=selected-quest-objective-target-resolution-runtime-supported"
                         << " hudMarkerRuntime=loaded-pending-runtime"
                         << " conditionRuntime=loaded-pending-runtime"
                         << " pathingRuntime=loaded-pending-runtime";
    }

    void runFalloutActorValueProof(MWWorld::Ptr player)
    {
        if (player.isEmpty() || !player.getClass().isNpc())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: actor value runtime BLOCKED reason=no-player-npc";
            return;
        }

        const std::array<std::pair<std::string_view, float>, 7> special = { {
            { "AVStrength", 5.f },
            { "AVPerception", 5.f },
            { "AVEndurance", 5.f },
            { "AVCharisma", 5.f },
            { "AVIntelligence", 5.f },
            { "AVAgility", 5.f },
            { "AVLuck", 5.f },
        } };

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
        std::vector<std::string> missing;
        std::vector<std::string> badRecordTypes;
        std::vector<std::string> missingPlayerValues;
        std::vector<std::string> labels;
        std::size_t markerCount = 0;
        std::size_t iconCount = 0;
        std::size_t descriptionCount = 0;

        const std::size_t beforeCount = stats.getFalloutActorValues().size();
        for (const auto& [editorId, value] : special)
        {
            const ESM4::ActorValueInfo* actorValue = findEsm4ActorValueByEditorId(store, editorId);
            if (actorValue == nullptr)
            {
                missing.emplace_back(editorId);
                continue;
            }

            const ESM::RefId id(actorValue->mId);
            const int recordType = store.find(id);
            if (recordType != ESM::REC_AVIF4)
                badRecordTypes.emplace_back(std::string(editorId));

            stats.setFalloutActorValue(id, value);
            if (!stats.hasFalloutActorValue(id) || stats.getFalloutActorValue(id) != value)
                missingPlayerValues.emplace_back(std::string(editorId));

            markerCount += actorValue->mProgressionMarkers.size();
            if (!actorValue->mIcon.empty())
                ++iconCount;
            if (!actorValue->mDescription.empty())
                ++descriptionCount;
            labels.push_back(actorValue->mEditorId);
        }

        const bool pass = missing.empty() && badRecordTypes.empty() && missingPlayerValues.empty()
            && markerCount >= special.size() && iconCount == special.size() && descriptionCount == special.size();

        std::ostringstream labelLog;
        for (const std::string& label : labels)
        {
            if (labelLog.tellp() > 0)
                labelLog << ",";
            labelLog << label;
        }

        Log(Debug::Info) << "FNV/ESM4 proof: actor value runtime " << (pass ? "PASS" : "FAIL")
                         << " avifRecords=" << store.get<ESM4::ActorValueInfo>().getSize()
                         << " selectedSpecial=" << labels.size()
                         << " labels=" << labelLog.str()
                         << " missing=" << missing.size()
                         << " badRecordTypes=" << badRecordTypes.size()
                         << " playerMissingValues=" << missingPlayerValues.size()
                         << " beforeCount=" << beforeCount
                         << " afterCount=" << stats.getFalloutActorValues().size()
                         << " progressionMarkers=" << markerCount
                         << " icons=" << iconCount
                         << " descriptions=" << descriptionCount
                         << " saveSubrecords=FAVB,FAVF"
                         << " runtimeBoundary=selected-special-actor-value-state-runtime-supported"
                         << " progressionRuntime=loaded-pending-runtime"
                         << " maxLevelRuntime=loaded-pending-runtime"
                         << " perkEffectRuntime=loaded-pending-runtime";
    }

    void runFalloutPlayerPerkProof(MWWorld::Ptr player)
    {
        if (player.isEmpty() || !player.getClass().isNpc())
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: player perk runtime BLOCKED reason=no-player-npc";
            return;
        }

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM4::Perk* builtToDestroy = findEsm4PerkByEditorId(store, "BuiltToDestroy");
        const ESM4::Perk* wildWasteland = findEsm4PerkByEditorId(store, "WildWasteland");
        const int builtRecordType = builtToDestroy != nullptr ? store.find(ESM::RefId(builtToDestroy->mId)) : 0;
        const int wildRecordType = wildWasteland != nullptr ? store.find(ESM::RefId(wildWasteland->mId)) : 0;

        MWMechanics::NpcStats& stats = player.getClass().getNpcStats(player);
        const std::size_t beforeCount = stats.getFalloutPerks().size();
        if (builtToDestroy != nullptr)
            stats.addFalloutPerk(ESM::RefId(builtToDestroy->mId));
        if (wildWasteland != nullptr)
            stats.addFalloutPerk(ESM::RefId(wildWasteland->mId));

        const bool hasBuilt = builtToDestroy != nullptr && stats.hasFalloutPerk(ESM::RefId(builtToDestroy->mId));
        const bool hasWild = wildWasteland != nullptr && stats.hasFalloutPerk(ESM::RefId(wildWasteland->mId));
        const bool pass = builtToDestroy != nullptr && wildWasteland != nullptr && builtRecordType == ESM::REC_PERK4
            && wildRecordType == ESM::REC_PERK4 && hasBuilt && hasWild;

        Log(Debug::Info) << "FNV/ESM4 proof: player perk runtime " << (pass ? "PASS" : "FAIL")
                         << " perks=" << store.get<ESM4::Perk>().getSize()
                         << " builtEdid=" << (builtToDestroy != nullptr ? builtToDestroy->mEditorId : "")
                         << " builtId="
                         << (builtToDestroy != nullptr ? ESM::RefId(builtToDestroy->mId) : ESM::RefId())
                         << " builtRecordType=0x" << std::hex << builtRecordType << std::dec
                         << " builtHas=" << hasBuilt
                         << " builtEffectTypes="
                         << (builtToDestroy != nullptr ? builtToDestroy->mEffectTypes.size() : 0)
                         << " builtEffectData="
                         << (builtToDestroy != nullptr ? builtToDestroy->mEffectData.size() : 0)
                         << " wildEdid=" << (wildWasteland != nullptr ? wildWasteland->mEditorId : "")
                         << " wildId="
                         << (wildWasteland != nullptr ? ESM::RefId(wildWasteland->mId) : ESM::RefId())
                         << " wildRecordType=0x" << std::hex << wildRecordType << std::dec
                         << " wildHas=" << hasWild
                         << " beforeCount=" << beforeCount
                         << " afterCount=" << stats.getFalloutPerks().size()
                         << " saveSubrecord=FPRK"
                         << " runtimeBoundary=player-perk-membership-runtime-supported"
                         << " effectsRuntime=loaded-pending-runtime"
                         << " levelUpSelectionRuntime=loaded-pending-runtime"
                         << " actorValueRuntime=loaded-pending-runtime";
    }

    void runFalloutNonzeroProjectileProof()
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM4::Ammunition* ammo = findEsm4AmmoByEditorId(store, "Ammo40mmGrenadeIncendiary");
        const bool projectileSet = ammo != nullptr && !ammo->mData.mProjectile.isZeroOrUnset();
        const int projectileRecordType = projectileSet ? store.find(ESM::RefId(ammo->mData.mProjectile)) : 0;
        const ESM4::Projectile* projectile = projectileSet
            ? store.get<ESM4::Projectile>().search(ESM::RefId(ammo->mData.mProjectile))
            : nullptr;
        const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();

        VFS::Path::Normalized rawModel;
        VFS::Path::Normalized resolvedModel;
        bool modelExists = false;
        std::string modelArchive;
        if (projectile != nullptr && !projectile->mModel.empty() && vfs != nullptr)
        {
            rawModel = VFS::Path::toNormalized(projectile->mModel);
            resolvedModel = Misc::ResourceHelpers::correctMeshPath(rawModel);
            modelExists = vfs->exists(resolvedModel);
            if (modelExists)
                modelArchive = vfs->getArchive(resolvedModel);
        }

        const bool pass = ammo != nullptr && projectileSet && projectile != nullptr && modelExists
            && Misc::StringUtils::ciEqual(projectile->mEditorId, "40mmGrenadeProjectileInc");
        Log(Debug::Info) << "FNV/ESM4 proof: nonzero projectile binding " << (pass ? "PASS" : "FAIL")
                         << " ammoEdid=" << (ammo != nullptr ? ammo->mEditorId : "")
                         << " ammoId=" << (ammo != nullptr ? ESM::RefId(ammo->mId) : ESM::RefId())
                         << " ammoProjectile="
                         << (ammo != nullptr ? ESM::RefId(ammo->mData.mProjectile) : ESM::RefId())
                         << " ammoProjectileFormId="
                         << (ammo != nullptr ? ammo->mData.mProjectile.toString("FormId:") : "FormId:0x0")
                         << " ammoProjectileSet=" << projectileSet
                         << " projectileRecordType=0x" << std::hex << projectileRecordType << std::dec
                         << " projectileFound=" << (projectile != nullptr)
                         << " projectileEdid=" << (projectile != nullptr ? projectile->mEditorId : "")
                         << " projectileId="
                         << (projectile != nullptr ? ESM::RefId(projectile->mId) : ESM::RefId())
                         << " projectileModelRaw=\"" << rawModel << "\""
                         << " projectileModelResolved=\"" << resolvedModel << "\""
                         << " projectileModelExists=" << modelExists
                         << " projectileModelArchive=\"" << modelArchive << "\""
                         << " projectileDataBytes=" << (projectile != nullptr ? projectile->mData.size() : 0)
                         << " projectileSoundLevel=" << (projectile != nullptr ? projectile->mSoundLevel : 0)
                         << " runtimeBoundary=definition-model-binding-runtime-supported"
                         << " spawnedProjectileRuntime=loaded-pending-runtime";
    }

    void runFalloutReal10mmProof(MWWorld::Ptr player, MWGui::WindowManager& windowManager)
    {
        if (player.isEmpty() || !player.getClass().hasInventoryStore(player))
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: real 10mm equip BLOCKED reason=no-player-inventory";
            return;
        }

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM4::Weapon* weapon = findEsm4WeaponByEditorId(store, "Weap10mmPistol");
        const ESM4::Ammunition* namedAmmo = findEsm4AmmoByEditorId(store, "Ammo10mm");
        const ESM4::Ammunition* weaponAmmo = nullptr;
        int weaponAmmoRecordType = 0;
        if (weapon != nullptr && !weapon->mAmmo.isZeroOrUnset())
        {
            weaponAmmoRecordType = store.find(ESM::RefId(weapon->mAmmo));
            weaponAmmo = store.get<ESM4::Ammunition>().search(ESM::RefId(weapon->mAmmo));
        }
        const ESM4::Ammunition* ammo = weaponAmmo != nullptr ? weaponAmmo : namedAmmo;
        Log(Debug::Info) << "FNV/ESM4 proof: real 10mm store scan weapons=" << store.get<ESM4::Weapon>().getSize()
                         << " ammo=" << store.get<ESM4::Ammunition>().getSize()
                         << " weaponFound=" << (weapon != nullptr)
                         << " weaponAmmoFound=" << (weaponAmmo != nullptr)
                         << " weaponAmmoRecordType=0x" << std::hex << weaponAmmoRecordType << std::dec
                         << " namedAmmoFound=" << (namedAmmo != nullptr)
                         << " ammoFound=" << (ammo != nullptr)
                         << " ammoSource=" << (weaponAmmo != nullptr ? "weaponAmmo" : "editorIdFallback");

        if (weapon == nullptr || ammo == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: real 10mm equip BLOCKED reason=missing-real-record"
                                << " weaponFound=" << (weapon != nullptr)
                                << " weaponAmmoFound=" << (weaponAmmo != nullptr)
                                << " namedAmmoFound=" << (namedAmmo != nullptr)
                                << " ammoFound=" << (ammo != nullptr);
            return;
        }

        if (weaponAmmo == nullptr && !weapon->mAmmo.isZeroOrUnset() && !(weapon->mAmmo == ammo->mId))
        {
            Log(Debug::Info) << "FNV/ESM4 proof: real 10mm ammo reference classified known-blocked"
                             << " reason=weapon-ammo-reference-not-loaded-as-AMMO"
                             << " weaponAmmo=" << ESM::RefId(weapon->mAmmo)
                             << " weaponAmmoRecordType=0x" << std::hex << weaponAmmoRecordType << std::dec
                             << " selectedAmmo=" << ESM::RefId(ammo->mId)
                             << " selectedAmmoEdid=" << ammo->mEditorId;
        }

        if (!weapon->mIcon.empty())
        {
            const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
            const VFS::Path::Normalized rawIcon = VFS::Path::toNormalized(weapon->mIcon);
            const VFS::Path::Normalized resolvedIcon = Misc::ResourceHelpers::correctIconPath(rawIcon, *vfs);
            const VFS::Path::Normalized canonicalIcon(
                "textures/interface/icons/pipboyimages/weapons/weapons_10mm_pistol.dds");
            int tenMillimeterIconMatches = 0;
            for (const VFS::Path::Normalized& path :
                vfs->getRecursiveDirectoryIterator("textures/interface/icons/pipboyimages/weapons"))
            {
                if (path.value().find("10mm") != std::string::npos)
                    ++tenMillimeterIconMatches;
                if (tenMillimeterIconMatches >= 16)
                    break;
            }

            Log(Debug::Info) << "FNV/ESM4 proof: real 10mm icon probe"
                             << " rawIcon=\"" << rawIcon << "\""
                             << " resolvedIcon=\"" << resolvedIcon << "\""
                             << " resolvedExists=" << vfs->exists(resolvedIcon)
                             << " resolvedArchive=\"" << vfs->getArchive(resolvedIcon) << "\""
                             << " canonicalIcon=\"" << canonicalIcon << "\""
                             << " canonicalExists=" << vfs->exists(canonicalIcon)
                             << " canonicalArchive=\"" << vfs->getArchive(canonicalIcon) << "\""
                             << " tenMillimeterIconMatches=" << tenMillimeterIconMatches;
        }

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        try
        {
            MWWorld::ManualRef weaponRef(store, ESM::RefId(weapon->mId), 1);
            MWWorld::ManualRef ammoRef(store, ESM::RefId(ammo->mId), 48);
            MWWorld::ContainerStoreIterator weaponIt = inventory.add(weaponRef.getPtr(), 1, false);
            MWWorld::ContainerStoreIterator ammoIt = inventory.add(ammoRef.getPtr(), 48, false);
            inventory.equip(MWWorld::InventoryStore::Slot_CarriedRight, weaponIt);
            inventory.equip(MWWorld::InventoryStore::Slot_Ammunition, ammoIt);
            player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
            windowManager.setSelectedWeapon(*weaponIt);

            const MWWorld::ContainerStoreIterator right = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            const MWWorld::ContainerStoreIterator ammoSlot = inventory.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
            const bool rightOk = right != inventory.end() && right->getType() == ESM::REC_WEAP4
                && right->getCellRef().getRefId() == ESM::RefId(weapon->mId);
            const bool ammoOk = ammoSlot != inventory.end() && ammoSlot->getType() == ESM::REC_AMMO4
                && ammoSlot->getCellRef().getRefId() == ESM::RefId(ammo->mId);
            const bool ammoProjectileSet = !ammo->mData.mProjectile.isZeroOrUnset();

            Log(Debug::Info) << "FNV/ESM4 proof: real 10mm equip " << (rightOk && ammoOk ? "PASS" : "FAIL")
                             << " weaponEdid=" << weapon->mEditorId
                             << " weaponId=" << ESM::RefId(weapon->mId)
                             << " weaponName=\"" << weapon->mFullName << "\""
                             << " weaponModel=\"" << weapon->mModel << "\""
                             << " weaponAmmo=" << ESM::RefId(weapon->mAmmo)
                             << " damage=" << weapon->mData.damage
                             << " clipSize=" << static_cast<int>(weapon->mData.clipSize)
                             << " rightSlotType=0x" << std::hex << (right != inventory.end() ? right->getType() : 0)
                             << std::dec
                             << " ammoEdid=" << ammo->mEditorId
                             << " ammoId=" << ESM::RefId(ammo->mId)
                             << " ammoName=\"" << ammo->mFullName << "\""
                             << " ammoModel=\"" << ammo->mModel << "\""
                             << " ammoProjectile=" << ESM::RefId(ammo->mData.mProjectile)
                             << " ammoProjectileFormId=" << ammo->mData.mProjectile.toString("FormId:")
                             << " ammoProjectileSet=" << ammoProjectileSet
                             << " ammoDamage=" << ammo->mData.mDamage
                             << " ammoCount=" << (ammoSlot != inventory.end() ? ammoSlot->getCellRef().getCount() : 0)
                             << " ammoSlotType=0x" << std::hex
                             << (ammoSlot != inventory.end() ? ammoSlot->getType() : 0) << std::dec;

            const ESM::Position& pos = player.getRefData().getPosition();
            const float yaw = pos.rot[2];
            const osg::Vec3f muzzle(pos.pos[0], pos.pos[1], pos.pos[2] + 112.f);
            const osg::Vec3f forward(std::sin(yaw), std::cos(yaw), 0.f);
            const osg::Vec3f endpoint = muzzle + forward * 4096.f;
            Log(Debug::Info) << "FNV/ESM4 proof: real 10mm muzzle ray origin=(" << muzzle.x() << "," << muzzle.y()
                             << "," << muzzle.z() << ") forward=(" << forward.x() << "," << forward.y() << ","
                             << forward.z() << ") endpoint=(" << endpoint.x() << "," << endpoint.y() << ","
                             << endpoint.z() << ") drawState=Weapon";

            if (rightOk && ammoOk)
            {
                MWWorld::Ptr ammoPtr = *ammoSlot;
                const osg::Vec3f launchPos = muzzle + forward * 64.f;
                const MWPhysics::RayCastingInterface* rayCasting
                    = MWBase::Environment::get().getWorld()->getRayCasting();
                const int ammoBefore = ammoPtr.getCellRef().getCount();
                MWPhysics::RayCastingResult trace;
                if (rayCasting != nullptr)
                {
                    trace = rayCasting->castRay(launchPos, endpoint, { player }, {},
                        MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                            | MWPhysics::CollisionType_Door | MWPhysics::CollisionType_Actor);
                }

                Log(Debug::Info) << "FNV/ESM4 proof: real 10mm firing trace request"
                                 << " weaponEdid=" << weapon->mEditorId
                                 << " weaponId=" << ESM::RefId(weapon->mId)
                                 << " ammoEdid=" << ammo->mEditorId
                                 << " ammoId=" << ESM::RefId(ammo->mId)
                                 << " ammoProjectile=" << ESM::RefId(ammo->mData.mProjectile)
                                 << " ammoProjectileFormId=" << ammo->mData.mProjectile.toString("FormId:")
                                 << " ammoProjectileSet=" << ammoProjectileSet
                                 << " ammoRecordSpeed=" << ammo->mData.mSpeed
                                 << " traceFrom=(" << launchPos.x() << "," << launchPos.y() << ","
                                 << launchPos.z() << ")"
                                 << " traceTo=(" << endpoint.x() << "," << endpoint.y() << "," << endpoint.z()
                                 << ")"
                                 << " ammoBefore=" << ammoBefore;

                inventory.remove(ammoPtr, 1);

                const MWWorld::ContainerStoreIterator ammoAfterSlot
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                Log(Debug::Info) << "FNV/ESM4 proof: real 10mm firing trace PASS"
                                 << " ammoBefore=" << ammoBefore
                                 << " ammoAfter="
                                 << (ammoAfterSlot != inventory.end() ? ammoAfterSlot->getCellRef().getCount() : 0)
                                 << " raycastAvailable=" << (rayCasting != nullptr)
                                 << " hit=" << trace.mHit
                                 << " hitObj="
                                 << (trace.mHitObject.isEmpty() ? ESM::RefId()
                                                                 : trace.mHitObject.getCellRef().getRefId())
                                 << " hitPos=(" << trace.mHitPos.x() << "," << trace.mHitPos.y() << ","
                                 << trace.mHitPos.z() << ")"
                                 << " note=physical ESM4 projectile visual deferred because Ammo10mm has no bound projectile form and its model is a pickup mesh";
            }
            else
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: real 10mm firing BLOCKED reason=equip-proof-failed"
                                    << " rightOk=" << rightOk << " ammoOk=" << ammoOk;
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: real 10mm equip BLOCKED reason=exception error=" << e.what();
        }
    }

    struct FalloutFloorSampleState
    {
        bool mHaveLastPos = false;
        osg::Vec3f mLastPos;
        int mLogs = 0;
    };

    void logFalloutActorFloorSample(const MWWorld::Ptr& ptr, std::string_view label,
        MWPhysics::RayCastingInterface const& rayCasting, FalloutFloorSampleState& state)
    {
        if (ptr.isEmpty() || !ptr.getClass().isActor())
            return;

        const osg::Vec3f pos = ptr.getRefData().getPosition().asVec3();
        const osg::Vec3f halfExtents = MWBase::Environment::get().getWorld()->getHalfExtents(ptr);
        const bool onGround = MWBase::Environment::get().getWorld()->isOnGround(ptr);
        const float zDelta = state.mHaveLastPos ? pos.z() - state.mLastPos.z() : 0.f;
        state.mLastPos = pos;
        state.mHaveLastPos = true;

        if (state.mLogs >= 80 && onGround && zDelta > -1.f)
            return;

        const int supportMask
            = MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door;
        const osg::Vec3f from = pos + osg::Vec3f(0.f, 0.f, std::max(halfExtents.z() * 2.f, 128.f));
        const osg::Vec3f to = pos - osg::Vec3f(0.f, 0.f, 1024.f);
        const MWPhysics::RayCastingResult ray = rayCasting.castRay(from, to, { ptr }, {}, supportMask);
        const osg::Vec3f wideFrom = pos + osg::Vec3f(0.f, 0.f, 4096.f);
        const osg::Vec3f wideTo = pos - osg::Vec3f(0.f, 0.f, 4096.f);
        const MWPhysics::RayCastingResult wideRay = rayCasting.castRay(wideFrom, wideTo, { ptr }, {},
            MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door);
        const float floorDelta = ray.mHit ? pos.z() - ray.mHitPos.z() : 0.f;
        const float wideFloorDelta = wideRay.mHit ? pos.z() - wideRay.mHitPos.z() : 0.f;

        std::string cellName;
        if (ptr.getCell() && ptr.getCell()->getCell())
            cellName = ptr.getCell()->getCell()->getDescription();

        Log(Debug::Info) << "FNV/ESM4 floor watchdog: " << label
                         << " ref=" << ptr.getCellRef().getRefId()
                         << " type=0x" << std::hex << ptr.getType() << std::dec
                         << " cell=\"" << cellName << "\""
                         << " pos=(" << pos.x() << "," << pos.y() << "," << pos.z() << ")"
                         << " zDelta=" << zDelta
                         << " onGround=" << onGround
                         << " halfExtents=(" << halfExtents.x() << "," << halfExtents.y() << ","
                         << halfExtents.z() << ")"
                         << " rayHit=" << ray.mHit
                         << " floorDelta=" << floorDelta
                         << " hitPos=(" << ray.mHitPos.x() << "," << ray.mHitPos.y() << "," << ray.mHitPos.z()
                         << ") hitObj=" << (ray.mHitObject.isEmpty() ? ESM::RefId() : ray.mHitObject.getCellRef().getRefId())
                         << " wideRayHit=" << wideRay.mHit
                         << " wideFloorDelta=" << wideFloorDelta
                         << " wideHitPos=(" << wideRay.mHitPos.x() << "," << wideRay.mHitPos.y() << ","
                         << wideRay.mHitPos.z() << ") wideHitObj="
                         << (wideRay.mHitObject.isEmpty() ? ESM::RefId() : wideRay.mHitObject.getCellRef().getRefId());
        ++state.mLogs;
    }

    bool hasFalloutNvContent(const std::vector<std::string>& contentFiles)
    {
        for (std::string file : contentFiles)
        {
            std::transform(file.begin(), file.end(), file.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (file == "falloutnv.esm" || file.ends_with("/falloutnv.esm") || file.ends_with("\\falloutnv.esm"))
                return true;
        }
        return false;
    }

    bool fnvFlatCameraIsAttached(const MWWorld::Ptr& player, const MWRender::Camera& camera)
    {
        const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
        const osg::Vec3d cameraPos = camera.getPosition();
        if (!std::isfinite(cameraPos.x()) || !std::isfinite(cameraPos.y()) || !std::isfinite(cameraPos.z()))
            return false;

        if (std::abs(cameraPos.x()) < 1.0 && std::abs(cameraPos.y()) < 1.0 && std::abs(cameraPos.z()) < 1.0)
            return false;

        const double dx = cameraPos.x() - playerPos.x();
        const double dy = cameraPos.y() - playerPos.y();
        const double horizontalDistance2 = dx * dx + dy * dy;
        return horizontalDistance2 < 4096.0 * 4096.0 && cameraPos.z() > playerPos.z() - 512.0
            && cameraPos.z() < playerPos.z() + 2048.0;
    }

    bool settleFNVFlatStartupCamera(
        MWBase::World& world, osg::Camera* viewerCamera, int attempt, unsigned int frameNumber)
    {
        MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return false;

        MWRender::Camera* camera = world.getCamera();
        if (camera == nullptr || viewerCamera == nullptr)
            return false;

        const ESM::Position& pos = player.getRefData().getPosition();
        const float cameraPitch = getProofFloat("OPENMW_FNV_FLAT_CAMERA_PITCH", 0.20f);
        const float cameraYaw = getProofFloat("OPENMW_FNV_FLAT_CAMERA_YAW", -pos.rot[2]);

        if (attempt == 1 || attempt % 30 == 0)
            world.reattachPlayerCamera();

        camera->attachTo(player);
        camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
        camera->setPreferredCameraDistance(getProofFloat("OPENMW_FNV_FLAT_CAMERA_NUDGE_DISTANCE", 128.f));
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
        camera->updateCamera(viewerCamera);

        const osg::Vec3d cameraPos = camera->getPosition();
        const osg::Vec3d trackedPos = camera->getTrackedPosition();
        const bool attached = fnvFlatCameraIsAttached(player, *camera);
        if (!attached && (attempt <= 5 || attempt % 30 == 0))
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: flat startup camera not attached yet frame=" << frameNumber
                                << " attempt=" << attempt << " mode=" << static_cast<int>(camera->getMode())
                                << " playerPos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2]
                                << ") trackedPos=(" << trackedPos.x() << "," << trackedPos.y() << ","
                                << trackedPos.z() << ") cameraPos=(" << cameraPos.x() << "," << cameraPos.y()
                                << "," << cameraPos.z() << ") cameraPitch=" << camera->getPitch()
                                << " cameraYaw=" << camera->getYaw();
            return false;
        }

        if (!attached)
            return false;

        Log(Debug::Info) << "FNV/ESM4 diag: settled flat startup camera via zoom-cycle equivalent frame="
                         << frameNumber << " attempt=" << attempt << " mode=" << static_cast<int>(camera->getMode())
                         << " playerPos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2]
                         << ") playerRotZ=" << pos.rot[2] << " trackedPos=(" << trackedPos.x() << ","
                         << trackedPos.y() << "," << trackedPos.z() << ") cameraPos=(" << cameraPos.x() << ","
                         << cameraPos.y() << "," << cameraPos.z() << ") cameraPitch=" << camera->getPitch()
                         << " cameraYaw=" << camera->getYaw();
        return true;
    }
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
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    static bool proofWalkStarted = false;
    static bool proofWalkCompleted = false;
    static bool proofWalkDropped = false;
    static int proofWalkAirborneFrames = 0;
    static float proofWalkMinZ = std::numeric_limits<float>::infinity();
    const char* proofWalkEnabled = std::getenv("OPENMW_PROOF_WALK_END_X");
    const int proofWalkStartFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_WALK_START_FRAME", 120.f));
    const int proofWalkEndFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_WALK_END_FRAME", 540.f));
    static bool proofPlacementApplied = false;
    const bool proofBaselinePlacementEnabled = std::getenv("OPENMW_FNV_BOOTSTRAP_POS_X") != nullptr;

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
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

        static bool proofHourApplied = false;
        if (!proofHourApplied && std::getenv("OPENMW_FNV_BOOTSTRAP_HOUR") != nullptr
            && mStateManager->getState() != MWBase::StateManager::State_NoGame)
        {
            const float proofHour = getProofFloat("OPENMW_FNV_BOOTSTRAP_HOUR", 12.f);
            mWorld->setGlobalFloat(MWWorld::Globals::sGameHour, proofHour);
            mWorld->advanceTime(0.0, false);
            proofHourApplied = true;
            Log(Debug::Info) << "FNV/ESM4 proof: applied proof gamehour=" << proofHour;
        }

        static bool proofSoundPlayed = false;
        const char* proofSoundFormId = std::getenv("OPENMW_FNV_PROOF_SOUND_FORMID");
        const int proofSoundFrame = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_SOUND_FRAME", 180.f));
        if (!proofSoundPlayed && proofSoundFormId != nullptr && *proofSoundFormId != '\0'
            && frameNumber >= static_cast<unsigned>(proofSoundFrame)
            && mStateManager->getState() != MWBase::StateManager::State_NoGame)
        {
            proofSoundPlayed = true;
            if (!mSoundManager || !mSoundManager->isEnabled())
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: sound playback skipped soundDisabled=1 id="
                                    << proofSoundFormId;
            }
            else
            {
                try
                {
                    const ESM::RefId soundId = ESM::RefId::deserializeText(proofSoundFormId);
                    MWBase::Sound* sound = mSoundManager->playSound(
                        soundId, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Normal);
                    if (sound != nullptr)
                    {
                        Log(Debug::Info) << "FNV/ESM4 proof: sound playback played id=" << proofSoundFormId;
                    }
                    else
                    {
                        Log(Debug::Warning) << "FNV/ESM4 proof: sound playback failed id=" << proofSoundFormId;
                    }
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: sound playback invalid id=" << proofSoundFormId
                                        << " error=" << e.what();
                }
            }
        }

        static bool proofReal10mmApplied = false;
        const char* proofReal10mm = std::getenv("OPENMW_FNV_PROOF_REAL_10MM");
        const bool proofReal10mmEnabled = proofReal10mm != nullptr && *proofReal10mm != '\0';
        const int proofReal10mmFrame = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_REAL_10MM_FRAME", 150.f));
        if (!proofReal10mmApplied && proofReal10mmEnabled
            && (!proofBaselinePlacementEnabled || proofPlacementApplied)
            && frameNumber >= static_cast<unsigned>(proofReal10mmFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofReal10mmApplied = true;
            runFalloutReal10mmProof(mWorld->getPlayerPtr(), *mWindowManager);
        }

        static bool proofNonzeroProjectileApplied = false;
        const char* proofNonzeroProjectile = std::getenv("OPENMW_FNV_PROOF_NONZERO_PROJECTILE");
        const bool proofNonzeroProjectileEnabled = proofNonzeroProjectile != nullptr && *proofNonzeroProjectile != '\0';
        const int proofNonzeroProjectileFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_NONZERO_PROJECTILE_FRAME", 150.f));
        if (!proofNonzeroProjectileApplied && proofNonzeroProjectileEnabled
            && frameNumber >= static_cast<unsigned>(proofNonzeroProjectileFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofNonzeroProjectileApplied = true;
            runFalloutNonzeroProjectileProof();
        }

        static bool proofPlayerPerkApplied = false;
        const char* proofPlayerPerk = std::getenv("OPENMW_FNV_PROOF_PLAYER_PERKS");
        const bool proofPlayerPerkEnabled = proofPlayerPerk != nullptr && *proofPlayerPerk != '\0';
        const int proofPlayerPerkFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_PLAYER_PERKS_FRAME", 150.f));
        if (!proofPlayerPerkApplied && proofPlayerPerkEnabled
            && frameNumber >= static_cast<unsigned>(proofPlayerPerkFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofPlayerPerkApplied = true;
            runFalloutPlayerPerkProof(mWorld->getPlayerPtr());
        }

        static bool proofActorValueApplied = false;
        const char* proofActorValue = std::getenv("OPENMW_FNV_PROOF_ACTOR_VALUES");
        const bool proofActorValueEnabled = proofActorValue != nullptr && *proofActorValue != '\0';
        const int proofActorValueFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_ACTOR_VALUES_FRAME", 150.f));
        if (!proofActorValueApplied && proofActorValueEnabled
            && frameNumber >= static_cast<unsigned>(proofActorValueFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofActorValueApplied = true;
            runFalloutActorValueProof(mWorld->getPlayerPtr());
        }

        static bool proofProgressionApplied = false;
        const char* proofProgression = std::getenv("OPENMW_FNV_PROOF_PROGRESSION");
        const bool proofProgressionEnabled = proofProgression != nullptr && *proofProgression != '\0';
        const int proofProgressionFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_PROGRESSION_FRAME", 150.f));
        if (!proofProgressionApplied && proofProgressionEnabled
            && frameNumber >= static_cast<unsigned>(proofProgressionFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofProgressionApplied = true;
            runFalloutProgressionProof(mWorld->getPlayerPtr());
        }

        static bool proofQuestSaveLoadApplied = false;
        const char* proofQuestSaveLoad = std::getenv("OPENMW_FNV_PROOF_QUEST_SAVELOAD");
        const bool proofQuestSaveLoadEnabled = proofQuestSaveLoad != nullptr && *proofQuestSaveLoad != '\0';
        const int proofQuestSaveLoadFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_QUEST_SAVELOAD_FRAME", 150.f));
        if (!proofQuestSaveLoadApplied && proofQuestSaveLoadEnabled
            && frameNumber >= static_cast<unsigned>(proofQuestSaveLoadFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofQuestSaveLoadApplied = true;
            runFalloutQuestSaveLoadProof(*mStateManager, mWorld->getPlayerPtr());
        }

        static bool proofQuestTargetApplied = false;
        const char* proofQuestTarget = std::getenv("OPENMW_FNV_PROOF_QUEST_TARGETS");
        const bool proofQuestTargetEnabled = proofQuestTarget != nullptr && *proofQuestTarget != '\0';
        const int proofQuestTargetFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_QUEST_TARGET_FRAME", 150.f));
        if (!proofQuestTargetApplied && proofQuestTargetEnabled
            && frameNumber >= static_cast<unsigned>(proofQuestTargetFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofQuestTargetApplied = true;
            runFalloutQuestTargetProof();
        }

        static bool proofQuestConditionApplied = false;
        const char* proofQuestCondition = std::getenv("OPENMW_FNV_PROOF_QUEST_CONDITIONS");
        const bool proofQuestConditionEnabled = proofQuestCondition != nullptr && *proofQuestCondition != '\0';
        const int proofQuestConditionFrame
            = static_cast<int>(getProofFloat("OPENMW_FNV_PROOF_QUEST_CONDITION_FRAME", 150.f));
        if (!proofQuestConditionApplied && proofQuestConditionEnabled
            && frameNumber >= static_cast<unsigned>(proofQuestConditionFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            proofQuestConditionApplied = true;
            runFalloutQuestConditionProof();
        }

        // update mechanics
        {
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

        if (!proofWalkCompleted && proofWalkEnabled != nullptr && *proofWalkEnabled != '\0'
            && frameNumber >= static_cast<unsigned>(proofWalkStartFrame)
            && frameNumber <= static_cast<unsigned>(proofWalkEndFrame)
            && mStateManager->getState() == MWBase::StateManager::State_Running && !paused)
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const osg::Vec3f current = player.getRefData().getPosition().asVec3();
            const osg::Vec3f target(getProofFloat("OPENMW_PROOF_WALK_END_X", current.x()),
                getProofFloat("OPENMW_PROOF_WALK_END_Y", current.y()),
                getProofFloat("OPENMW_PROOF_WALK_END_Z", current.z()));
            const osg::Vec3f delta = target - current;
            const float distance = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            const float reachRadius = getProofFloat("OPENMW_PROOF_WALK_REACH_RADIUS", 96.f);
            if (distance > reachRadius && !proofWalkDropped)
            {
                const float speed = getProofFloat("OPENMW_PROOF_WALK_SPEED", 180.f);
                const float yaw = static_cast<float>(std::atan2(delta.x(), delta.y()));
                mWorld->rotateObject(player, osg::Vec3f(0.f, 0.f, yaw));
                mWorld->queueMovement(player, osg::Vec3f(0.f, speed, 0.f));
            }
            else
                mWorld->queueMovement(player, osg::Vec3f());
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    mViewer->eventTraversal();
    mViewer->updateTraversal();

#ifdef OPENMW_ENABLE_VR
    if (VR::getVR())
    {
        if (mStateManager->getState() == MWBase::StateManager::State_Running)
        {
            auto playerPtr = MWMechanics::getPlayer();
            auto playerAnim = MWBase::Environment::get().getWorld()->getAnimation(playerPtr);
            if (playerAnim != nullptr)
                static_cast<MWVR::VRAnimation*>(playerAnim)->updateSpace();
        }
        VR::Session::instance().updateSpaces();
    }
#endif

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateFocusObject();
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    static bool fnvFlatStartupCameraSettled = false;
    static int fnvFlatStartupCameraAttempts = 0;
    static unsigned int fnvFlatStartupCameraSettledFrame = 0;
    static int fnvWorldReadyFrames = 0;
    const bool fnvLoadingGui = mWindowManager->containsMode(MWGui::GM_Loading)
        || mWindowManager->containsMode(MWGui::GM_LoadingWallpaper)
        || mWindowManager->containsMode(MWGui::GM_MainMenu);
    const bool fnvWorldReady = mStateManager->getState() == MWBase::StateManager::State_Running && !fnvLoadingGui;
    if (fnvWorldReady)
        ++fnvWorldReadyFrames;
    else
        fnvWorldReadyFrames = 0;

    if (!fnvFlatStartupCameraSettled && fnvWorldReadyFrames >= 10 && hasFalloutNvContent(mContentFiles)
        && std::getenv("OPENMW_FNV_DISABLE_FLAT_CAMERA_SETTLE") == nullptr)
    {
        ++fnvFlatStartupCameraAttempts;
        fnvFlatStartupCameraSettled
            = settleFNVFlatStartupCamera(*mWorld, mViewer->getCamera(), fnvFlatStartupCameraAttempts, frameNumber);
        if (fnvFlatStartupCameraSettled && fnvFlatStartupCameraSettledFrame == 0)
            fnvFlatStartupCameraSettledFrame = frameNumber;
        if (!fnvFlatStartupCameraSettled && fnvFlatStartupCameraAttempts >= 180)
        {
            fnvFlatStartupCameraSettled = true;
            fnvFlatStartupCameraSettledFrame = frameNumber;
            Log(Debug::Error) << "FNV/ESM4 diag: flat startup camera did not attach after "
                              << fnvFlatStartupCameraAttempts << " attempts at frame " << frameNumber
                              << "; leaving current camera state for input";
        }
    }
    if (!proofPlacementApplied && proofBaselinePlacementEnabled
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        ESM::Position position;
        position.pos[0] = getProofFloat("OPENMW_FNV_BOOTSTRAP_POS_X", -67450.f);
        position.pos[1] = getProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Y", 2600.f);
        position.pos[2] = getProofFloat("OPENMW_FNV_BOOTSTRAP_POS_Z", 8425.f);
        position.rot[0] = getProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_X", 0.f);
        position.rot[1] = getProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Y", 0.f);
        position.rot[2] = getProofFloat("OPENMW_FNV_BOOTSTRAP_ROT_Z", -1.2f);

        ESM::Position cellProbe = position;
        ESM::RefId cellId;
        std::string resolvedInteriorCell;
        std::string requestedProofCell;
        bool proofCellResolutionBlocked = false;
        if (const char* proofCell = std::getenv("OPENMW_FNV_BOOTSTRAP_CELL");
            proofCell != nullptr && *proofCell != '\0')
        {
            requestedProofCell = proofCell;
            ESM::Position interiorProbe = position;
            const ESM::RefId interiorCellId = mWorld->findInteriorPosition(requestedProofCell, interiorProbe);
            if (!interiorCellId.empty())
            {
                cellId = interiorCellId;
                resolvedInteriorCell = requestedProofCell;
            }
            else
            {
                try
                {
                    cellId = ESM::RefId::deserializeText(proofCell);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to parse proof cell \"" << proofCell
                                        << "\": " << e.what();
                }
            }
        }
        if (cellId.empty())
        {
            if (!requestedProofCell.empty())
                proofCellResolutionBlocked = true;
            else
                cellId = mWorld->findExteriorPosition("Goodsprings", cellProbe);
        }
        else if (resolvedInteriorCell.empty()
            && MWBase::Environment::get().getWorldModel()->findCell(cellId, false) == nullptr)
        {
            proofCellResolutionBlocked = !requestedProofCell.empty();
            if (!proofCellResolutionBlocked)
            {
                cellProbe = position;
                cellId = mWorld->findExteriorPosition("Goodsprings", cellProbe);
            }
        }
        if (proofCellResolutionBlocked)
        {
            proofPlacementApplied = true;
            Log(Debug::Warning) << "FNV/ESM4 proof marker_error: requested bootstrap cell \"" << requestedProofCell
                                << "\" resolved to "
                                << (cellId.empty() ? std::string("empty cell") : ("missing cell " + cellId.toDebugString()))
                                << "; refusing fallback placement";
        }
        else if (!cellId.empty())
        {
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            const bool alreadyInTargetCell = player.isInCell()
                && player.getCell() != nullptr && player.getCell()->getCell()->getId() == cellId;
            if (!alreadyInTargetCell)
            {
                if (!resolvedInteriorCell.empty())
                    mWorld->changeToInteriorCell(resolvedInteriorCell, position, false, true);
                else
                    mWorld->changeToCell(cellId, position, false, true);
            }
            MWWorld::CellStore* placementCell = player.getCell();
            player = mWorld->moveObject(player, placementCell, position.asVec3(), true, true);
            mWorld->rotateObject(player, osg::Vec3f(position.rot[0], position.rot[1], position.rot[2]));

            if (MWRender::Camera* camera = mWorld->getCamera())
            {
                const float cameraDistance = getProofFloat("OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE", 0.f);
                camera->attachTo(player);
                camera->setMode(MWRender::Camera::Mode::FirstPerson, true);
                camera->setPreferredCameraDistance(cameraDistance);
                camera->update(0.f, false);
                camera->setPitch(-position.rot[0], true);
                camera->setYaw(-position.rot[2], true);
                camera->setRoll(0.f);
                camera->updateCamera(mViewer->getCamera());
            }

            proofPlacementApplied = true;
            Log(Debug::Info) << "FNV/ESM4 proof: moved player to proof cell"
                             << (requestedProofCell.empty() ? "" : (" request=\"" + requestedProofCell + "\""))
                             << (resolvedInteriorCell.empty() ? " type=exterior-or-ref" : " type=interior") << " pos=("
                             << position.pos[0] << ","
                             << position.pos[1] << "," << position.pos[2] << ") rot=(" << position.rot[0] << ","
                             << position.rot[1] << "," << position.rot[2] << ") cell=" << cellId.toDebugString();
            if (std::getenv("OPENMW_FNV_TERRAIN_PROBE_POINTS") != nullptr
                && !mWorld->getWorldScene().logFNVNamedTerrainProbePoints())
                Log(Debug::Warning) << "FNV/ESM4 proof: named terrain probe requested but no probe points were logged";
        }
        else
        {
            proofPlacementApplied = true;
            Log(Debug::Warning) << "FNV/ESM4 proof: failed to resolve proof Goodsprings position";
        }
    }

    static bool proofSkyDisabled = false;
    if (!proofSkyDisabled && std::getenv("OPENMW_PROOF_DISABLE_SKY") != nullptr
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        bool skyEnabled = mWorld->toggleSky();
        if (skyEnabled)
            skyEnabled = mWorld->toggleSky();
        proofSkyDisabled = true;
        Log(Debug::Info) << "FNV/ESM4 proof: disabled sky finalEnabled=" << skyEnabled;
    }

    static FalloutFloorSampleState falloutPlayerFloorState;
    static std::map<std::string, FalloutFloorSampleState> falloutActorFloorStates;
    static int falloutFloorFrames = 0;
    if (mStateManager->getState() == MWBase::StateManager::State_Running && mWorld->getRayCasting() != nullptr
        && std::getenv("OPENMW_FNV_FLOOR_WATCHDOG") != nullptr && falloutFloorFrames < 720)
    {
        ++falloutFloorFrames;
        const MWPhysics::RayCastingInterface& rayCasting = *mWorld->getRayCasting();
        MWWorld::Ptr player = mWorld->getPlayerPtr();
        logFalloutActorFloorSample(player, "player", rayCasting, falloutPlayerFloorState);

        if (falloutFloorFrames % 15 == 0 && !player.isEmpty())
        {
            const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
            int actorSamples = 0;
            for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr || actorSamples >= 6)
                    continue;

                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (actorSamples >= 6 || ptr.isEmpty() || ptr == player || !ptr.getClass().isActor())
                        return true;

                    const osg::Vec3f pos = ptr.getRefData().getPosition().asVec3();
                    if ((pos - playerPos).length2() > 2048.f * 2048.f)
                        return true;

                    std::string key = ptr.getCellRef().getRefNum().toString("FormId:");
                    if (key.empty())
                        key = ptr.getCellRef().getRefId().serializeText();
                    const std::string label = "nearby-actor-" + std::to_string(actorSamples + 1);
                    logFalloutActorFloorSample(ptr, label, rayCasting, falloutActorFloorStates[key]);
                    ++actorSamples;
                    return true;
                });
            }
        }
    }

    if (!proofWalkCompleted && proofWalkEnabled != nullptr && *proofWalkEnabled != '\0'
        && frameNumber >= static_cast<unsigned>(proofWalkStartFrame)
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        MWWorld::Ptr player = mWorld->getPlayerPtr();
        const osg::Vec3f current = player.getRefData().getPosition().asVec3();
        const osg::Vec3f target(getProofFloat("OPENMW_PROOF_WALK_END_X", current.x()),
            getProofFloat("OPENMW_PROOF_WALK_END_Y", current.y()),
            getProofFloat("OPENMW_PROOF_WALK_END_Z", current.z()));
        const float speed = getProofFloat("OPENMW_PROOF_WALK_SPEED", 180.f);
        const float reachRadius = getProofFloat("OPENMW_PROOF_WALK_REACH_RADIUS", 96.f);
        const float minAllowedZ = getProofFloat("OPENMW_PROOF_WALK_MIN_Z", current.z() - 256.f);
        const osg::Vec3f delta = target - current;
        const float distance = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
        const bool onGround = mWorld->isOnGround(player);
        proofWalkMinZ = std::min(proofWalkMinZ, current.z());
        if (!onGround)
            ++proofWalkAirborneFrames;
        if (current.z() < minAllowedZ)
            proofWalkDropped = true;

        if (!proofWalkStarted)
        {
            proofWalkStarted = true;
            Log(Debug::Info) << "FNV/ESM4 proof walk: start frame=" << frameNumber << " pos=(" << current.x()
                             << "," << current.y() << "," << current.z() << ") target=(" << target.x() << ","
                             << target.y() << "," << target.z() << ") speed=" << speed
                             << " reachRadius=" << reachRadius << " minAllowedZ=" << minAllowedZ;
        }

        if (distance <= reachRadius || frameNumber > static_cast<unsigned>(proofWalkEndFrame) || proofWalkDropped)
            mWorld->queueMovement(player, osg::Vec3f());

        if ((frameNumber - static_cast<unsigned>(proofWalkStartFrame)) % 60 == 0)
            Log(Debug::Info) << "FNV/ESM4 proof walk: sample frame=" << frameNumber << " pos=(" << current.x()
                             << "," << current.y() << "," << current.z() << ") distance=" << distance
                             << " onGround=" << onGround << " dropped=" << proofWalkDropped
                             << " minZ=" << proofWalkMinZ;

        if (distance <= reachRadius || frameNumber >= static_cast<unsigned>(proofWalkEndFrame) || proofWalkDropped)
        {
            mWorld->queueMovement(player, osg::Vec3f());
            proofWalkCompleted = true;
            Log(Debug::Info) << "FNV/ESM4 proof walk: summary reached=" << (distance <= reachRadius)
                             << " dropped=" << proofWalkDropped << " airborneFrames=" << proofWalkAirborneFrames
                             << " minZ=" << proofWalkMinZ << " finalDistance=" << distance << " finalPos=("
                             << current.x() << "," << current.y() << "," << current.z() << ") target=("
                             << target.x() << "," << target.y() << "," << target.z() << ")";
        }
    }

    static bool proofActorCameraApplied = false;
    static bool proofActorDumpApplied = false;
    const int proofActorDumpFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_DUMP_ACTORS_FRAME", -1.f));
    if (!proofActorDumpApplied && proofActorDumpFrame >= 0 && frameNumber >= static_cast<unsigned>(proofActorDumpFrame)
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        int scanned = 0;
        int actors = 0;
        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
        {
            if (cellstore == nullptr)
                continue;

            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                ++scanned;
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return true;
                ++actors;

                std::string actorName;
                try
                {
                    actorName = std::string(ptr.getClass().getName(ptr));
                }
                catch (const std::exception&)
                {
                }

                const std::string baseId = ptr.getCellRef().getRefId().toDebugString();
                const std::string refId = ptr.getCellRef().getRefNum().toString("FormId:");
                std::string baseEditorId;
                std::string baseFullName;
                std::string baseFormId;
                if (ptr.getType() == ESM::REC_NPC_4)
                {
                    if (const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>())
                    {
                        if (ref->mBase != nullptr)
                        {
                            baseEditorId = ref->mBase->mEditorId;
                            baseFullName = ref->mBase->mFullName;
                            baseFormId = ESM::RefId(ref->mBase->mId).toDebugString();
                        }
                    }
                }
                else if (ptr.getType() == ESM::REC_CREA4)
                {
                    if (const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>())
                    {
                        if (ref->mBase != nullptr)
                        {
                            baseEditorId = ref->mBase->mEditorId;
                            baseFullName = ref->mBase->mFullName;
                        }
                    }
                }

                const ESM::Position& actorPos = ptr.getRefData().getPosition();
                Log(Debug::Info) << "FNV/ESM4 proof actor dump: ref=" << refId << " base=" << baseId
                                 << " type=0x" << std::hex << ptr.getType() << std::dec << " name=\"" << actorName
                                 << "\" baseEditor=\"" << baseEditorId << "\" baseFull=\"" << baseFullName
                                 << "\" pos=(" << actorPos.pos[0] << "," << actorPos.pos[1] << ","
                                 << actorPos.pos[2] << ") rot=(" << actorPos.rot[0] << "," << actorPos.rot[1]
                                 << "," << actorPos.rot[2] << ")";
                return true;
            });
        }
        Log(Debug::Info) << "FNV/ESM4 proof actor dump summary: scanned=" << scanned << " actors=" << actors;
        proofActorDumpApplied = true;
    }

    const char* proofActorTargetEnv = std::getenv("OPENMW_PROOF_ACTOR_TARGET");
    std::string proofActorTargetValue = getProofRuntimeActorTarget(
        proofActorTargetEnv != nullptr && *proofActorTargetEnv != '\0' ? std::string(proofActorTargetEnv) : std::string());
    const char* proofActorTarget = proofActorTargetValue.empty() ? nullptr : proofActorTargetValue.c_str();
    const bool proofStageActor = std::getenv("OPENMW_PROOF_STAGE_ACTOR") != nullptr;
    const bool proofStageActorLock = proofStageActor && std::getenv("OPENMW_PROOF_STAGE_ACTOR_UNLOCK") == nullptr;
    const int proofActorFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_ACTOR_FRAME", 240.f));
    static unsigned int proofActorCameraFirstAppliedFrame = 0;
    static unsigned int proofActorCameraLastAppliedFrame = 0;
    const bool proofNeutralActorPreviewRequested = std::getenv("OPENMW_PROOF_NEUTRAL_ACTOR_PREVIEW") != nullptr;
    static bool proofNeutralActorPreviewAttempted = false;
    static bool proofNeutralActorPreviewReady = false;
    static bool proofNeutralActorPreviewIsolationApplied = false;
    static osg::ref_ptr<osg::Group> proofNeutralActorPreviewRoot;
    static osg::ref_ptr<osg::Camera> proofNeutralActorPreviewComposite;
    static std::vector<std::unique_ptr<MWRender::FalloutActorPreview>> proofNeutralActorPreviews;
    static std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> proofNeutralActorPreviewNpcProxy;
    static std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> proofNeutralActorPreviewCreatureProxy;
    static std::string proofNeutralActorPreviewRuntimeFingerprint;
    auto resetNeutralActorPreview = [&]() {
        proofNeutralActorPreviewAttempted = false;
        proofNeutralActorPreviewReady = false;
        proofNeutralActorPreviewIsolationApplied = false;
        proofNeutralActorPreviews.clear();
        proofNeutralActorPreviewNpcProxy.reset();
        proofNeutralActorPreviewCreatureProxy.reset();
        if (proofNeutralActorPreviewComposite != nullptr && proofNeutralActorPreviewComposite->getNumParents() > 0)
            proofNeutralActorPreviewComposite->getParent(0)->removeChild(proofNeutralActorPreviewComposite);
        proofNeutralActorPreviewComposite = nullptr;
        if (proofNeutralActorPreviewRoot != nullptr && proofNeutralActorPreviewRoot->getNumParents() > 0)
            proofNeutralActorPreviewRoot->getParent(0)->removeChild(proofNeutralActorPreviewRoot);
        proofNeutralActorPreviewRoot = nullptr;
    };
    const std::string proofCurrentNeutralActorPreviewRuntimeFingerprint
        = "target=" + std::string(proofActorTarget != nullptr ? proofActorTarget : "")
        + ";parts=" + getProofRuntimeCommandString({ "actorKitParts", "parts" })
        + ";partModels=" + getProofRuntimeCommandString({ "actorKitPartModels", "partModels" })
        + ";animationSource=" + getProofRuntimeCommandString({ "actorKitAnimationSource", "animationSource" })
        + ";animationGroup=" + getProofRuntimeCommandString({ "actorKitAnimationGroup", "animationGroup" })
        + ";dialogueMode=" + getProofRuntimeCommandString({ "actorKitDialogueMode", "dialogueMode" })
        + ";profile=" + getProofRuntimeCommandString({ "neutralPreviewProfile" });
    if (proofNeutralActorPreviewRuntimeFingerprint.empty())
        proofNeutralActorPreviewRuntimeFingerprint = proofCurrentNeutralActorPreviewRuntimeFingerprint;
    else if (proofNeutralActorPreviewRequested && proofNeutralActorPreviewReady
        && proofNeutralActorPreviewRuntimeFingerprint != proofCurrentNeutralActorPreviewRuntimeFingerprint)
    {
        Log(Debug::Info) << "FNV/ESM4 live runtime: neutral actor preview family changed target=\""
                         << (proofActorTarget != nullptr ? proofActorTarget : "")
                         << "\" oldFingerprint=\"" << proofNeutralActorPreviewRuntimeFingerprint
                         << "\" newFingerprint=\"" << proofCurrentNeutralActorPreviewRuntimeFingerprint
                         << "\" runtime=runtime-supported gate=runtime-live-neutral-preview-family-rebuild";
        resetNeutralActorPreview();
        proofNeutralActorPreviewRuntimeFingerprint = proofCurrentNeutralActorPreviewRuntimeFingerprint;
    }
    static std::string proofActorEffectiveTarget;
    if (proofActorTarget != nullptr && proofActorEffectiveTarget != proofActorTarget)
    {
        Log(Debug::Info) << "FNV/ESM4 live runtime: actor target changed from=\""
                         << proofActorEffectiveTarget << "\" to=\"" << proofActorTarget
                         << "\" file=\""
                         << (std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE") != nullptr
                                 ? std::getenv("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE")
                                 : "")
                         << "\" runtime=runtime-supported gate=runtime-live-actor-target";
        proofActorEffectiveTarget = proofActorTarget;
        proofActorCameraApplied = false;
        proofActorCameraFirstAppliedFrame = 0;
        proofActorCameraLastAppliedFrame = 0;
        resetNeutralActorPreview();
        proofNeutralActorPreviewRuntimeFingerprint = proofCurrentNeutralActorPreviewRuntimeFingerprint;
    }
    const char* proofItemModel = std::getenv("OPENMW_PROOF_ITEM_MODEL");
    const char* proofItemTarget = std::getenv("OPENMW_PROOF_ITEM_TARGET");
    const bool proofItemRequested = proofItemModel != nullptr && *proofItemModel != '\0';
    const int proofItemFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_ITEM_FRAME", 240.f));
    static bool proofItemArmedLogged = false;
    static bool proofItemSpawnAttempted = false;
    static bool proofItemSpawned = false;
    static bool proofItemCameraApplied = false;
    static bool proofItemBoundsValid = false;
    static osg::Vec3f proofItemBoundsCenter(0.f, 0.f, 42.f);
    static osg::Vec3f proofItemBoundsSize(0.f, 0.f, 0.f);
    static float proofItemFrameRadius = 42.f;
    static unsigned int proofItemCameraFirstAppliedFrame = 0;
    static unsigned int proofItemCameraLastAppliedFrame = 0;
    static bool proofItemWorldIsolationApplied = false;
    const bool proofRuntimeWorldExists = mStateManager->getState() != MWBase::StateManager::State_NoGame;
    if (proofItemRequested && !proofItemArmedLogged && proofRuntimeWorldExists)
    {
        proofItemArmedLogged = true;
        Log(Debug::Info) << "FNV/ESM4 proof item model spawn armed: target=\""
                         << (proofItemTarget != nullptr ? proofItemTarget : "") << "\" model=\"" << proofItemModel
                         << "\" requestedFrame=" << proofItemFrame << " currentFrame=" << frameNumber
                         << " state=" << static_cast<int>(mStateManager->getState())
                         << " runtime=loaded-pending-runtime gate=runtime-visual-model-spawn";
    }
    if (proofItemRequested && frameNumber >= static_cast<unsigned>(proofItemFrame)
        && proofRuntimeWorldExists)
    {
        const std::string target = proofItemTarget != nullptr && *proofItemTarget != '\0' ? proofItemTarget : "proof-item";
        const std::string model(proofItemModel);
        const std::string kind = std::getenv("OPENMW_PROOF_ITEM_KIND") != nullptr ? std::getenv("OPENMW_PROOF_ITEM_KIND") : "";
        const std::string recordType
            = std::getenv("OPENMW_PROOF_ITEM_RECORD_TYPE") != nullptr ? std::getenv("OPENMW_PROOF_ITEM_RECORD_TYPE") : "";
        const std::string formId = std::getenv("OPENMW_PROOF_ITEM_FORM_ID") != nullptr ? std::getenv("OPENMW_PROOF_ITEM_FORM_ID") : "";
        const std::string plugin = std::getenv("OPENMW_PROOF_ITEM_PLUGIN") != nullptr ? std::getenv("OPENMW_PROOF_ITEM_PLUGIN") : "";
        const osg::Vec3f itemPos(getProofFloat("OPENMW_PROOF_ITEM_STAGE_X", getProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", 0.f)),
            getProofFloat("OPENMW_PROOF_ITEM_STAGE_Y", getProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", 0.f)),
            getProofFloat("OPENMW_PROOF_ITEM_STAGE_Z", getProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", 0.f)));
        const osg::Vec3f itemRot(getProofFloat("OPENMW_PROOF_ITEM_STAGE_ROT_X", 0.f),
            getProofFloat("OPENMW_PROOF_ITEM_STAGE_ROT_Y", 0.f),
            getProofFloat("OPENMW_PROOF_ITEM_STAGE_ROT_Z", getProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", 0.f)));
        const float itemScale = getProofFloat("OPENMW_PROOF_ITEM_STAGE_SCALE", 1.f);

        if (!proofItemSpawnAttempted)
        {
            proofItemSpawnAttempted = true;
            try
            {
                VFS::Path::Normalized correctedModel
                    = Misc::ResourceHelpers::correctMeshPath(VFS::Path::toNormalized(model));
                if (!mResourceSystem->getVFS()->exists(correctedModel))
                {
                    const VFS::Path::Normalized rawModel(VFS::Path::toNormalized(model));
                    if (mResourceSystem->getVFS()->exists(rawModel))
                        correctedModel = rawModel;
                }

                osg::ref_ptr<const osg::Node> templateNode
                    = mResourceSystem->getSceneManager()->getTemplate(correctedModel);
                osg::ComputeBoundsVisitor computeBoundsVisitor;
                computeBoundsVisitor.setTraversalMask(~(MWRender::Mask_ParticleSystem | MWRender::Mask_Effect));
                const_cast<osg::Node*>(templateNode.get())->accept(computeBoundsVisitor);
                const osg::BoundingBox bounds = computeBoundsVisitor.getBoundingBox();
                proofItemBoundsValid = bounds.valid();
                if (proofItemBoundsValid)
                {
                    proofItemBoundsCenter = bounds.center();
                    proofItemBoundsSize = osg::Vec3f(
                        bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin(), bounds.zMax() - bounds.zMin());
                    proofItemFrameRadius = std::max(
                        { proofItemBoundsSize.x(), proofItemBoundsSize.y(), proofItemBoundsSize.z(), 16.f });
                }

                if (MWRender::RenderingManager* rendering = mWorld->getRenderingManager())
                {
                    rendering->removeEffect("nikami-proof-item");
                    rendering->spawnEffect(correctedModel, {}, itemPos, itemScale, false, false, "nikami-proof-item", true);
                    proofItemSpawned = true;
                }

                Log(Debug::Info) << "FNV/ESM4 proof item model spawn: target=\"" << target << "\" kind=\"" << kind
                                 << "\" recordType=\"" << recordType << "\" formId=\"" << formId << "\" plugin=\""
                                 << plugin << "\" model=\"" << model << "\" correctedModel=\"" << correctedModel
                                 << "\" frame=" << frameNumber << " pos=(" << itemPos.x() << "," << itemPos.y()
                                 << "," << itemPos.z() << ") rot=(" << itemRot.x() << "," << itemRot.y() << ","
                                 << itemRot.z() << ") scale=" << itemScale
                                 << " rotationApplied=false runtime=runtime-supported gate=runtime-visual-model-spawn"
                                 << " inventoryRuntime=loaded-pending-runtime collisionRuntime=loaded-pending-runtime"
                                 << " boundsValid=" << bounds.valid() << " boundsMin=(" << bounds.xMin() << ","
                                  << bounds.yMin() << "," << bounds.zMin() << ") boundsMax=(" << bounds.xMax() << ","
                                  << bounds.yMax() << "," << bounds.zMax() << ") boundsCenter=("
                                  << proofItemBoundsCenter.x() << "," << proofItemBoundsCenter.y() << ","
                                  << proofItemBoundsCenter.z() << ") boundsSize=(" << proofItemBoundsSize.x() << ","
                                  << proofItemBoundsSize.y() << "," << proofItemBoundsSize.z()
                                  << ") frameRadius=" << proofItemFrameRadius
                                  << " radius=" << templateNode->getBound().radius();
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof item model spawn failed: target=\"" << target << "\" model=\""
                                    << model << "\" frame=" << frameNumber << " error=\"" << e.what()
                                    << "\" runtime=known-blocked gate=runtime-visual-model-spawn";
            }
        }

        if (proofItemSpawned)
        {
            if (!proofItemWorldIsolationApplied)
            {
                if (mWindowManager)
                    mWindowManager->setHudVisibility(false);
                if (MWRender::RenderingManager* rendering = mWorld->getRenderingManager())
                {
                    rendering->setWaterEnabled(false);
                    rendering->setSkyEnabled(false);
                    const MWWorld::Ptr player = mWorld->getPlayerPtr();
                    if (!player.isEmpty() && player.isInCell() && player.getCell() != nullptr
                        && player.getCell()->getCell() != nullptr)
                        rendering->enableTerrain(false, player.getCell()->getCell()->getWorldSpace());
                }
                if (mViewer && mViewer->getCamera())
                    mViewer->getCamera()->setClearColor(osg::Vec4(0.22f, 0.23f, 0.24f, 1.f));
                proofItemWorldIsolationApplied = true;
                Log(Debug::Info) << "FNV/ESM4 proof item viewer isolation: hud=hidden sky=hidden water=hidden"
                                 << " terrain=hidden clearColor=(0.22,0.23,0.24,1)"
                                 << " runtime=runtime-supported gate=runtime-neutral-view";
            }

            const float autoOffsetX = proofItemBoundsValid ? std::max(32.f, proofItemFrameRadius * 2.35f) : 84.f;
            const float offsetX = getProofFloat("OPENMW_PROOF_ITEM_VIEW_OFFSET_X",
                getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_X", autoOffsetX));
            const float offsetY = getProofFloat("OPENMW_PROOF_ITEM_VIEW_OFFSET_Y", getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y", 0.f));
            const float autoTargetZ = proofItemBoundsValid ? proofItemBoundsCenter.z() : 42.f;
            const float targetZ = getProofFloat("OPENMW_PROOF_ITEM_VIEW_TARGET_Z",
                getProofFloat("OPENMW_PROOF_ACTOR_VIEW_TARGET_Z", autoTargetZ));
            const float offsetZ = getProofFloat("OPENMW_PROOF_ITEM_VIEW_OFFSET_Z",
                getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z", targetZ));
            const float cameraDistance = getProofFloat("OPENMW_PROOF_ITEM_VIEW_CAMERA_DISTANCE", getProofFloat("OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE", 0.f));
            const osg::Vec3d itemAim(itemPos.x(), itemPos.y(), itemPos.z() + targetZ);
            osg::Vec2f viewOffset(offsetX, offsetY);
            if (std::getenv("OPENMW_PROOF_ITEM_VIEW_LOCAL_OFFSET") != nullptr)
            {
                const float sinYaw = std::sin(itemRot.z());
                const float cosYaw = std::cos(itemRot.z());
                viewOffset = osg::Vec2f(offsetX * sinYaw + offsetY * cosYaw, offsetX * cosYaw - offsetY * sinYaw);
            }
            const osg::Vec3f requestedCameraPos(static_cast<float>(itemAim.x() + viewOffset.x()),
                static_cast<float>(itemAim.y() + viewOffset.y()), itemPos.z() + offsetZ);

            if (MWRender::Camera* camera = mWorld->getCamera())
            {
                const float dx = static_cast<float>(itemAim.x() - requestedCameraPos.x());
                const float dy = static_cast<float>(itemAim.y() - requestedCameraPos.y());
                const float dz = static_cast<float>(itemAim.z() - requestedCameraPos.z());
                const float horizontal = std::sqrt(dx * dx + dy * dy);
                const float yawToItem = static_cast<float>(-std::atan2(dx, dy));
                const float pitchToItem = static_cast<float>(std::atan2(dz, horizontal));

                camera->setMode(MWRender::Camera::Mode::Static, true);
                camera->setStaticPosition(requestedCameraPos);
                camera->setPreferredCameraDistance(cameraDistance);
                camera->setPitch(pitchToItem, true);
                camera->setYaw(yawToItem, true);
                camera->setRoll(0.f);
                camera->updateCamera(mViewer->getCamera());

                const osg::Vec3d finalCameraPos = camera->getPosition();
                if (proofItemCameraFirstAppliedFrame == 0)
                    proofItemCameraFirstAppliedFrame = frameNumber;
                proofItemCameraLastAppliedFrame = frameNumber;
                if (!proofItemCameraApplied || frameNumber % 60 == 0)
                    Log(Debug::Info) << "FNV/ESM4 proof: aligned player camera to item target=\"" << target
                                     << "\" frame=" << frameNumber << " itemPos=(" << itemPos.x() << ","
                                     << itemPos.y() << "," << itemPos.z() << ") itemAim=(" << itemAim.x() << ","
                                     << itemAim.y() << "," << itemAim.z() << ") viewOffset=(" << viewOffset.x()
                                     << "," << viewOffset.y() << ") localOffset="
                                     << (std::getenv("OPENMW_PROOF_ITEM_VIEW_LOCAL_OFFSET") != nullptr)
                                     << " requestedCameraPos=(" << requestedCameraPos.x() << ","
                                     << requestedCameraPos.y() << "," << requestedCameraPos.z() << ") cameraPos=("
                                     << finalCameraPos.x() << "," << finalCameraPos.y() << ","
                                     << finalCameraPos.z() << ") cameraMode=static cameraPitch=" << camera->getPitch()
                                     << " cameraYaw=" << camera->getYaw()
                                     << " runtime=runtime-supported gate=runtime-neutral-view";
                proofItemCameraApplied = true;
            }
        }
    }
    if ((!proofActorCameraApplied || proofStageActorLock) && proofActorTarget != nullptr && *proofActorTarget != '\0'
        && frameNumber >= static_cast<unsigned>(proofActorFrame)
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        MWWorld::Ptr proofActor;
        int scanned = 0;
        int actors = 0;
        const std::string target(proofActorTarget);
        const std::string targetLower = Misc::StringUtils::lowerCase(target);
        static unsigned int lastProofActorLookupFailureLogFrame = 0;
        const bool proofActorLogThisFrame = !proofActorCameraApplied || frameNumber % 60 == 0;

        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
        {
            if (cellstore == nullptr)
                continue;

            cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                ++scanned;
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return true;
                ++actors;

                std::string actorName;
                try
                {
                    actorName = std::string(ptr.getClass().getName(ptr));
                }
                catch (const std::exception&)
                {
                }

                const std::string baseId = ptr.getCellRef().getRefId().toDebugString();
                const std::string refId = ptr.getCellRef().getRefNum().toString("FormId:");
                std::string baseEditorId;
                std::string baseFullName;
                std::string baseFormId;
                if (ptr.getType() == ESM::REC_NPC_4)
                {
                    if (const MWWorld::LiveCellRef<ESM4::Npc>* ref = ptr.get<ESM4::Npc>())
                    {
                        if (ref->mBase != nullptr)
                        {
                            baseEditorId = ref->mBase->mEditorId;
                            baseFullName = ref->mBase->mFullName;
                            baseFormId = ESM::RefId(ref->mBase->mId).toDebugString();
                        }
                    }
                }
                else if (ptr.getType() == ESM::REC_CREA4)
                {
                    if (const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>())
                    {
                        if (ref->mBase != nullptr)
                        {
                            baseEditorId = ref->mBase->mEditorId;
                            baseFullName = ref->mBase->mFullName;
                            baseFormId = ESM::RefId(ref->mBase->mId).toDebugString();
                        }
                    }
                }
                const std::string actorNameLower = Misc::StringUtils::lowerCase(actorName);
                const std::string baseIdLower = Misc::StringUtils::lowerCase(baseId);
                const std::string refIdLower = Misc::StringUtils::lowerCase(refId);
                const std::string baseEditorLower = Misc::StringUtils::lowerCase(baseEditorId);
                const std::string baseFullLower = Misc::StringUtils::lowerCase(baseFullName);
                const std::string baseFormLower = Misc::StringUtils::lowerCase(baseFormId);

                if (baseIdLower == targetLower || refIdLower == targetLower || actorNameLower == targetLower
                    || baseEditorLower == targetLower || baseFullLower == targetLower || baseFormLower == targetLower
                    || falloutProofFormTargetMatches(baseId, target)
                    || falloutProofFormTargetMatches(refId, target)
                    || falloutProofFormTargetMatches(baseFormId, target)
                    || (!actorNameLower.empty() && actorNameLower.find(targetLower) != std::string::npos)
                    || (!baseEditorLower.empty() && baseEditorLower.find(targetLower) != std::string::npos)
                    || (!baseFullLower.empty() && baseFullLower.find(targetLower) != std::string::npos)
                    || (!actorNameLower.empty() && !targetLower.empty()
                        && targetLower.find(actorNameLower) != std::string::npos))
                {
                    proofActor = ptr;
                    if (proofActorLogThisFrame)
                    {
                        const ESM::Position& actorPos = ptr.getRefData().getPosition();
                        Log(Debug::Info) << "FNV/ESM4 proof: active-cell actor match target=\"" << target
                                          << "\" frame=" << frameNumber << " ref=" << refId << " base=" << baseId
                                          << " name=\"" << actorName << "\" baseEditor=\"" << baseEditorId
                                          << "\" baseForm=" << baseFormId << " baseFull=\"" << baseFullName
                                          << "\" pos=(" << actorPos.pos[0] << "," << actorPos.pos[1] << ","
                                          << actorPos.pos[2] << ") rot=(" << actorPos.rot[0] << ","
                                          << actorPos.rot[1] << "," << actorPos.rot[2] << ") scanned=" << scanned
                                          << " actors=" << actors;
                    }
                    return false;
                }

                return true;
            });

            if (!proofActor.isEmpty())
                break;
        }

        auto assembleNeutralActorPreview = [&](const MWWorld::Ptr& previewActor, std::string_view source) {
            if (!proofNeutralActorPreviewRequested || proofNeutralActorPreviewAttempted)
                return false;

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

                proofNeutralActorPreviews.clear();
                proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                    proofNeutralActorPreviewRoot, mResourceSystem.get(), previewActor,
                    MWRender::FalloutActorPreview::ViewMode::Front));
                proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                    proofNeutralActorPreviewRoot, mResourceSystem.get(), previewActor,
                    MWRender::FalloutActorPreview::ViewMode::Left));
                proofNeutralActorPreviews.emplace_back(std::make_unique<MWRender::FalloutActorPreview>(
                    proofNeutralActorPreviewRoot, mResourceSystem.get(), previewActor,
                    MWRender::FalloutActorPreview::ViewMode::Top));
                for (const std::unique_ptr<MWRender::FalloutActorPreview>& preview : proofNeutralActorPreviews)
                {
                    preview->rebuild();
                    preview->redraw();
                }
                proofNeutralActorPreviewComposite = createFalloutNeutralActorPreviewComposite(proofNeutralActorPreviews);
                sceneRoot->addChild(proofNeutralActorPreviewComposite);

                proofNeutralActorPreviewReady = true;
                Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview assembled target=\"" << target
                                 << "\" source=" << source << " panes=" << proofNeutralActorPreviews.size()
                                 << " compositeQuads="
                                 << (proofNeutralActorPreviewComposite != nullptr
                                         ? proofNeutralActorPreviewComposite->getNumChildren()
                                         : 0)
                                 << " runtime=runtime-supported gate=runtime-neutral-actor-preview";
                return true;
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: neutral actor preview failed target=\"" << target
                                    << "\" source=" << source << " error=\"" << e.what()
                                    << "\" runtime=known-blocked gate=runtime-neutral-actor-preview";
                return false;
            }
        };

        auto isolateNeutralActorPreview = [&]() {
            if (!proofNeutralActorPreviewRequested || !proofNeutralActorPreviewReady
                || proofNeutralActorPreviewIsolationApplied)
                return;

            if (mWindowManager)
                mWindowManager->setHudVisibility(false);
            if (MWRender::RenderingManager* rendering = mWorld->getRenderingManager())
            {
                rendering->setWaterEnabled(false);
                rendering->setSkyEnabled(false);
                const MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!player.isEmpty() && player.isInCell() && player.getCell() != nullptr
                    && player.getCell()->getCell() != nullptr)
                    rendering->enableTerrain(false, player.getCell()->getCell()->getWorldSpace());
            }
            if (mViewer && mViewer->getCamera())
            {
                mViewer->getCamera()->setClearColor(osg::Vec4(0.22f, 0.23f, 0.24f, 1.f));
                mViewer->getCamera()->setCullMask(MWRender::Mask_RenderToTexture);
            }
            proofActorCameraApplied = true;
            if (proofActorCameraFirstAppliedFrame == 0)
                proofActorCameraFirstAppliedFrame = frameNumber;
            proofActorCameraLastAppliedFrame = frameNumber;
            proofNeutralActorPreviewIsolationApplied = true;
            Log(Debug::Info) << "FNV/ESM4 proof: neutral actor preview isolation target=\"" << target
                             << "\" cullMask=Mask_RenderToTexture hud=hidden sky=hidden water=hidden"
                             << " terrain=hidden runtime=runtime-supported gate=runtime-neutral-actor-preview";
        };

        if (!proofActor.isEmpty())
        {
            assembleNeutralActorPreview(proofActor, "active-cell-actor");

            isolateNeutralActorPreview();

            if (proofNeutralActorPreviewRequested)
                proofActorCameraLastAppliedFrame = frameNumber;

            if (proofStageActor)
            {
                const ESM::Position& current = proofActor.getRefData().getPosition();
                const osg::Vec3f stagePos(getProofFloat("OPENMW_PROOF_ACTOR_STAGE_X", current.pos[0]),
                    getProofFloat("OPENMW_PROOF_ACTOR_STAGE_Y", current.pos[1]),
                    getProofFloat("OPENMW_PROOF_ACTOR_STAGE_Z", current.pos[2]));
                const osg::Vec3f stageRot(
                    getProofRuntimeFloat({ "actorStageRotX", "stageRotX", "rotX" },
                        getProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_X", current.rot[0])),
                    getProofRuntimeFloat({ "actorStageRotY", "stageRotY", "rotY" },
                        getProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Y", current.rot[1])),
                    getProofRuntimeFloat({ "actorStageRotZ", "stageRotZ", "rotZ", "yawRadians" },
                        getProofFloat("OPENMW_PROOF_ACTOR_STAGE_ROT_Z", current.rot[2])));
                proofActor = mWorld->moveObject(proofActor, stagePos, true, true);
                mWorld->rotateObject(proofActor, stageRot);
                if (proofActorLogThisFrame)
                    Log(Debug::Info) << "FNV/ESM4 proof: staged actor target=\"" << target << "\" frame="
                                     << frameNumber << " pos=(" << stagePos.x() << "," << stagePos.y() << ","
                                     << stagePos.z() << ") rot=(" << stageRot.x() << "," << stageRot.y() << ","
                                     << stageRot.z() << ") lock=" << proofStageActorLock
                                     << " ptr=" << proofActor.toString();
            }

            const ESM::Position& actorPos = proofActor.getRefData().getPosition();
            const float offsetX = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_X", 34.f);
            const float offsetY = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y", 0.f);
            const float offsetZ = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z", 102.f);
            const float targetZ = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_TARGET_Z", 116.f);
            const float playerEyeZ = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_PLAYER_EYE_Z", 124.f);
            const float cameraDistance = getProofFloat("OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE", 0.f);
            const osg::Vec3d actorAim(actorPos.pos[0], actorPos.pos[1], actorPos.pos[2] + targetZ);
            osg::Vec2f viewOffset(offsetX, offsetY);
            if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_LOCAL_OFFSET") != nullptr)
            {
                const float sinYaw = std::sin(actorPos.rot[2]);
                const float cosYaw = std::cos(actorPos.rot[2]);
                viewOffset = osg::Vec2f(offsetX * sinYaw + offsetY * cosYaw, offsetX * cosYaw - offsetY * sinYaw);
            }
            const osg::Vec3f requestedCameraPos(
                static_cast<float>(actorAim.x() + viewOffset.x()),
                static_cast<float>(actorAim.y() + viewOffset.y()),
                static_cast<float>(actorPos.pos[2] + offsetZ));
            MWWorld::Ptr player = mWorld->getPlayerPtr();
            osg::Vec3f playerTargetPos(requestedCameraPos.x(), requestedCameraPos.y(),
                requestedCameraPos.z() - playerEyeZ);
            if (const MWPhysics::RayCastingInterface* rayCasting = mWorld->getRayCasting())
            {
                const osg::Vec3f rayFrom(playerTargetPos.x(), playerTargetPos.y(), requestedCameraPos.z() + 400.f);
                const osg::Vec3f rayTo(playerTargetPos.x(), playerTargetPos.y(), requestedCameraPos.z() - 1200.f);
                const MWPhysics::RayCastingResult support = rayCasting->castRay(
                    rayFrom, rayTo, MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Door);
                const float minPlayerZ = support.mHit ? support.mHitPos.z() + 1.f : playerTargetPos.z();
                if (playerTargetPos.z() < minPlayerZ)
                {
                    if (proofActorLogThisFrame)
                        Log(Debug::Info) << "FNV/ESM4 proof: clamped actor camera player support target=\""
                                         << target << "\" frame=" << frameNumber << " fromZ=" << playerTargetPos.z()
                                         << " toZ=" << minPlayerZ << " hit=" << support.mHit << " hitPos=("
                                         << support.mHitPos.x() << "," << support.mHitPos.y() << ","
                                         << support.mHitPos.z() << ")";
                    playerTargetPos.z() = minPlayerZ;
                }
            }
            player = mWorld->moveObject(player, playerTargetPos, true, true);

            const float yawToActor
                = static_cast<float>(std::atan2(actorAim.x() - requestedCameraPos.x(), actorAim.y() - requestedCameraPos.y()));
            mWorld->rotateObject(player, osg::Vec3f(0.f, 0.f, -yawToActor));

            if (MWRender::Camera* camera = mWorld->getCamera())
            {
                camera->attachTo(player);
                camera->setMode(MWRender::Camera::Mode::ThirdPerson, true);
                camera->setPreferredCameraDistance(cameraDistance);
                camera->update(0.f, false);

                const osg::Vec3d cameraPos = camera->getPosition();
                const float dx = static_cast<float>(actorAim.x() - cameraPos.x());
                const float dy = static_cast<float>(actorAim.y() - cameraPos.y());
                const float dz = static_cast<float>(actorAim.z() - cameraPos.z());
                const float horizontal = std::sqrt(dx * dx + dy * dy);
                camera->setPitch(static_cast<float>(std::atan2(dz, horizontal)), true);
                camera->setYaw(-yawToActor, true);
                camera->setRoll(0.f);
                camera->updateCamera(mViewer->getCamera());

                const osg::Vec3d finalCameraPos = camera->getPosition();
                if (proofActorCameraFirstAppliedFrame == 0)
                    proofActorCameraFirstAppliedFrame = frameNumber;
                proofActorCameraLastAppliedFrame = frameNumber;
                if (proofActorLogThisFrame)
                    Log(Debug::Info) << "FNV/ESM4 proof: aligned player camera to actor target=\"" << target
                                     << "\" frame=" << frameNumber << " playerPos=(" << playerTargetPos.x() << ","
                                     << playerTargetPos.y() << "," << playerTargetPos.z() << ") actorPos=("
                                     << actorPos.pos[0] << ","
                                      << actorPos.pos[1] << "," << actorPos.pos[2] << ") actorAim=("
                                      << actorAim.x() << "," << actorAim.y() << "," << actorAim.z()
                                      << ") viewOffset=(" << viewOffset.x() << "," << viewOffset.y()
                                      << ") localOffset="
                                      << (std::getenv("OPENMW_PROOF_ACTOR_VIEW_LOCAL_OFFSET") != nullptr)
                                      << " requestedCameraPos=(" << requestedCameraPos.x() << ","
                                     << requestedCameraPos.y() << "," << requestedCameraPos.z()
                                     << ") cameraPos=(" << finalCameraPos.x() << "," << finalCameraPos.y()
                                     << "," << finalCameraPos.z() << ") cameraPitch=" << camera->getPitch()
                                     << " cameraYaw=" << camera->getYaw();
            }
        }
        else
        {
            bool baseNpcPreviewHandled = false;
            bool baseCreaturePreviewHandled = false;
            int npcBaseRecordsScanned = 0;
            int creatureBaseRecordsScanned = 0;
            int actorRefRecordsScanned = 0;
            int creatureRefRecordsScanned = 0;
            if (proofNeutralActorPreviewRequested && !proofNeutralActorPreviewAttempted)
            {
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                const ESM4::Npc* npcBase = store != nullptr
                    ? findFalloutProofNpcBaseByTarget(*store, target, npcBaseRecordsScanned)
                    : nullptr;
                const ESM4::ActorCharacter* actorRef = nullptr;
                if (npcBase == nullptr && store != nullptr)
                {
                    actorRef = findFalloutProofActorRefByTarget(*store, target, actorRefRecordsScanned);
                    if (actorRef != nullptr)
                        npcBase = store->get<ESM4::Npc>().search(actorRef->mBaseObj);
                }
                if (npcBase != nullptr)
                {
                    if (actorRef != nullptr)
                    {
                        proofNeutralActorPreviewNpcProxy
                            = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(*actorRef, npcBase);
                    }
                    else
                    {
                        ESM::CellRef proxyRef;
                        proxyRef.blank();
                        proxyRef.mRefID = npcBase->mEditorId.empty() ? ESM::RefId(npcBase->mId)
                                                                     : ESM::RefId::stringRefId(npcBase->mEditorId);
                        proofNeutralActorPreviewNpcProxy
                            = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, npcBase);
                    }
                    MWWorld::Ptr basePreviewActor(proofNeutralActorPreviewNpcProxy.get(), nullptr);
                    Log(Debug::Info) << "FNV/ESM4 proof: base NPC preview match target=\"" << target
                                     << "\" actor=" << npcBase->mEditorId << " form="
                                     << ESM::RefId(npcBase->mId).toDebugString() << " full=\"" << npcBase->mFullName
                                     << "\" scannedActive=" << scanned << " activeActors=" << actors
                                     << " scannedNpcBases=" << npcBaseRecordsScanned
                                     << " scannedActorRefs=" << actorRefRecordsScanned
                                     << " refForm=\""
                                     << (actorRef != nullptr ? ESM::RefId(actorRef->mId).toDebugString() : "")
                                     << "\" refEditor=\""
                                     << (actorRef != nullptr ? actorRef->mEditorId : "")
                                     << "\""
                                     << " runtime=loaded-pending-runtime gate=runtime-neutral-actor-preview";
                    baseNpcPreviewHandled = assembleNeutralActorPreview(basePreviewActor, "base-npc-record");
                    isolateNeutralActorPreview();
                }
                else
                {
                    const ESM4::Creature* creatureBase = store != nullptr
                        ? findFalloutProofCreatureBaseByTarget(*store, target, creatureBaseRecordsScanned)
                        : nullptr;
                    const ESM4::ActorCharacter* creatureRef = nullptr;
                    if (creatureBase == nullptr && store != nullptr)
                    {
                        creatureRef = findFalloutProofCreatureRefByTarget(*store, target, creatureRefRecordsScanned);
                        if (creatureRef != nullptr)
                            creatureBase = store->get<ESM4::Creature>().search(creatureRef->mBaseObj);
                    }
                    if (creatureBase != nullptr)
                    {
                        if (creatureRef != nullptr)
                        {
                            proofNeutralActorPreviewCreatureProxy
                                = std::make_unique<MWWorld::LiveCellRef<ESM4::Creature>>(*creatureRef, creatureBase);
                        }
                        else
                        {
                            ESM::CellRef proxyRef;
                            proxyRef.blank();
                            proxyRef.mRefID = creatureBase->mEditorId.empty()
                                ? ESM::RefId(creatureBase->mId)
                                : ESM::RefId::stringRefId(creatureBase->mEditorId);
                            proofNeutralActorPreviewCreatureProxy
                                = std::make_unique<MWWorld::LiveCellRef<ESM4::Creature>>(proxyRef, creatureBase);
                        }
                        MWWorld::Ptr basePreviewActor(proofNeutralActorPreviewCreatureProxy.get(), nullptr);
                        Log(Debug::Info) << "FNV/ESM4 proof: base creature preview match target=\"" << target
                                         << "\" actor=" << creatureBase->mEditorId << " form="
                                         << ESM::RefId(creatureBase->mId).toDebugString() << " full=\""
                                         << creatureBase->mFullName << "\" scannedActive=" << scanned
                                         << " activeActors=" << actors
                                         << " scannedCreatureBases=" << creatureBaseRecordsScanned
                                         << " scannedCreatureRefs=" << creatureRefRecordsScanned
                                         << " refForm=\""
                                         << (creatureRef != nullptr ? ESM::RefId(creatureRef->mId).toDebugString() : "")
                                         << "\" refEditor=\""
                                         << (creatureRef != nullptr ? creatureRef->mEditorId : "")
                                         << "\""
                                         << " runtime=loaded-pending-runtime gate=runtime-neutral-actor-preview"
                                         << " classification=base-record-visual-preview-only";
                        baseCreaturePreviewHandled
                            = assembleNeutralActorPreview(basePreviewActor, "base-creature-record");
                        isolateNeutralActorPreview();
                    }
                }
            }

            if (lastProofActorLookupFailureLogFrame == 0 || frameNumber - lastProofActorLookupFailureLogFrame >= 60)
            {
                Log(Debug::Warning) << "FNV/ESM4 proof: active-cell actor lookup failed target=\"" << target
                                    << "\" frame=" << frameNumber << " scanned=" << scanned << " actors=" << actors
                                    << " baseNpcPreview="
                                    << (baseNpcPreviewHandled ? "runtime-supported" : "not-applied")
                                    << " scannedNpcBases=" << npcBaseRecordsScanned
                                    << " scannedActorRefs=" << actorRefRecordsScanned
                                    << " baseCreaturePreview="
                                    << (baseCreaturePreviewHandled ? "visual-preview-supported" : "not-applied")
                                    << " scannedCreatureBases=" << creatureBaseRecordsScanned
                                    << " scannedCreatureRefs=" << creatureRefRecordsScanned;
                lastProofActorLookupFailureLogFrame = frameNumber;
            }
        }

        static std::string proofNeutralActorPreviewLiveAuthoringContent;
        static unsigned int proofNeutralActorPreviewLiveAuthoringTick = 0;
        static unsigned int proofNeutralActorPreviewLiveAuthoringLogs = 0;
        if (proofNeutralActorPreviewRequested && proofNeutralActorPreviewReady && !proofNeutralActorPreviews.empty())
        {
            const char* liveAuthoringPath = std::getenv("OPENMW_FNV_LIVE_AUTHORING_FILE");
            if (liveAuthoringPath != nullptr && liveAuthoringPath[0] != '\0'
                && ++proofNeutralActorPreviewLiveAuthoringTick % 3 == 0)
            {
                std::ifstream liveAuthoringInput(liveAuthoringPath, std::ios::binary);
                std::ostringstream liveAuthoringStream;
                liveAuthoringStream << liveAuthoringInput.rdbuf();
                const std::string liveAuthoringContent = liveAuthoringStream.str();
                if (!liveAuthoringContent.empty()
                    && liveAuthoringContent != proofNeutralActorPreviewLiveAuthoringContent)
                {
                    proofNeutralActorPreviewLiveAuthoringContent = liveAuthoringContent;
                    const double previewSimulationTime = static_cast<double>(frameNumber) / 60.0;
                    unsigned int panesRedrawn = 0;
                    for (const std::unique_ptr<MWRender::FalloutActorPreview>& preview : proofNeutralActorPreviews)
                    {
                        if (preview == nullptr)
                            continue;
                        preview->updateLive(previewSimulationTime);
                        ++panesRedrawn;
                    }
                    if (proofNeutralActorPreviewLiveAuthoringLogs < 32)
                    {
                        ++proofNeutralActorPreviewLiveAuthoringLogs;
                        Log(Debug::Info)
                            << "FNV/ESM4 live authoring: neutral actor preview live redraw target=\"" << target
                            << "\" frame=" << frameNumber
                            << " panes=" << panesRedrawn
                            << " file=" << liveAuthoringPath
                            << " contentBytes=" << liveAuthoringContent.size()
                            << " runtime=runtime-supported gate=runtime-live-neutral-preview-redraw";
                    }
                }
            }
        }

        if (!proofActor.isEmpty())
            proofActorCameraApplied = true;
    }

    static const std::vector<int> proofScreenshotFrames = getProofFrames("OPENMW_PROOF_SCREENSHOT_FRAME");
    static bool proofGuiModeApplied = false;
    const char* proofGuiMode = std::getenv("OPENMW_PROOF_GUI_MODE");
    const int proofGuiFrame = static_cast<int>(getProofFloat("OPENMW_PROOF_GUI_FRAME", 240.f));
    if (!proofGuiModeApplied && proofGuiMode != nullptr && *proofGuiMode != '\0'
        && frameNumber >= static_cast<unsigned>(proofGuiFrame)
        && mStateManager->getState() == MWBase::StateManager::State_Running)
    {
        const std::string mode(proofGuiMode);
        std::optional<std::size_t> activeInventoryWindow;
        if (Misc::StringUtils::ciEqual(mode, "map"))
            activeInventoryWindow = 0;
        else if (Misc::StringUtils::ciEqual(mode, "inventory") || Misc::StringUtils::ciEqual(mode, "items"))
            activeInventoryWindow = 1;
        else if (Misc::StringUtils::ciEqual(mode, "magic") || Misc::StringUtils::ciEqual(mode, "data"))
            activeInventoryWindow = 2;
        else if (Misc::StringUtils::ciEqual(mode, "stats") || Misc::StringUtils::ciEqual(mode, "status"))
            activeInventoryWindow = 3;

        if (activeInventoryWindow.has_value())
        {
            mWindowManager->pushGuiMode(MWGui::GM_Inventory);
            mWindowManager->setActiveControllerWindow(MWGui::GM_Inventory, *activeInventoryWindow);
            Log(Debug::Info) << "FNV/ESM4 proof: pushed inventory GUI mode page=\"" << mode
                             << "\" activeWindow=" << *activeInventoryWindow << " at frame " << frameNumber;
        }
        else
            Log(Debug::Warning) << "FNV/ESM4 proof: unsupported proof GUI mode \"" << mode << "\"";
        proofGuiModeApplied = true;
    }

    static std::size_t proofScreenshotFrameIndex = 0;
    static std::string proofLastLiveScreenshotRequestId;
    const bool proofNativeAssetStudioRequested = std::getenv("OPENMW_FNV_ASSET_STUDIO") != nullptr;
    const bool proofScreenshotStateReady = mStateManager->getState() == MWBase::StateManager::State_Running
        || (proofItemRequested && proofItemCameraApplied) || proofNativeAssetStudioRequested;
    const std::string proofLiveScreenshotRequestId = getProofRuntimeCommandString({ "screenshotRequestId" });
    const std::string proofLiveScreenshotLabel = getProofRuntimeCommandString({ "screenshotLabel" });
    bool proofScreenshotCapturedThisFrame = false;
    if (!proofLiveScreenshotRequestId.empty() && proofLiveScreenshotRequestId != proofLastLiveScreenshotRequestId
        && proofScreenshotStateReady && mScreenCaptureHandler != nullptr)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: queuing native screenshot request id=\""
                         << proofLiveScreenshotRequestId << "\" label=\"" << proofLiveScreenshotLabel
                         << "\" frame=" << frameNumber
                         << " flatCameraSettled=" << fnvFlatStartupCameraSettled
                         << " flatCameraSettledFrame=" << fnvFlatStartupCameraSettledFrame
                         << " actorTarget=\"" << (proofActorTarget != nullptr ? proofActorTarget : "") << "\""
                         << " actorCameraApplied=" << proofActorCameraApplied
                         << " actorCameraFirstFrame=" << proofActorCameraFirstAppliedFrame
                         << " actorCameraLastFrame=" << proofActorCameraLastAppliedFrame
                         << " actorStageLock=" << proofStageActorLock
                         << " itemTarget=\"" << (proofItemTarget != nullptr ? proofItemTarget : "") << "\""
                         << " itemModel=\"" << (proofItemModel != nullptr ? proofItemModel : "") << "\""
                         << " itemCameraApplied=" << proofItemCameraApplied
                         << " nativeAssetStudio=" << proofNativeAssetStudioRequested
                         << " runtime=runtime-supported gate=runtime-live-native-screenshot-request";
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
        proofLastLiveScreenshotRequestId = proofLiveScreenshotRequestId;
        proofScreenshotCapturedThisFrame = true;
    }
    if (!proofScreenshotCapturedThisFrame && proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex])
        && proofScreenshotStateReady && mScreenCaptureHandler != nullptr)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: queuing native screenshot at frame " << frameNumber
                         << " index=" << proofScreenshotFrameIndex
                         << " flatCameraSettled=" << fnvFlatStartupCameraSettled
                         << " flatCameraSettledFrame=" << fnvFlatStartupCameraSettledFrame
                         << " actorTarget=\"" << (proofActorTarget != nullptr ? proofActorTarget : "") << "\""
                         << " actorCameraApplied=" << proofActorCameraApplied
                         << " actorCameraFirstFrame=" << proofActorCameraFirstAppliedFrame
                         << " actorCameraLastFrame=" << proofActorCameraLastAppliedFrame
                         << " actorStageLock=" << proofStageActorLock
                         << " itemTarget=\"" << (proofItemTarget != nullptr ? proofItemTarget : "") << "\""
                         << " itemModel=\"" << (proofItemModel != nullptr ? proofItemModel : "") << "\""
                         << " itemCameraApplied=" << proofItemCameraApplied
                         << " itemCameraFirstFrame=" << proofItemCameraFirstAppliedFrame
                         << " itemCameraLastFrame=" << proofItemCameraLastAppliedFrame
                         << " nativeAssetStudio=" << proofNativeAssetStudioRequested;
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
        ++proofScreenshotFrameIndex;
    }

#if defined(OPENMW_ENABLE_VR) && defined(__ANDROID__) && defined(XR_USE_PLATFORM_ANDROID)
    if (VR::getVR())
    {
        unsigned int renderMask = ~0u;
        renderMask &= ~MWRender::VisMask::Mask_GUI;
        renderMask |= MWRender::VisMask::Mask_3DGUI;
        renderMask |= MWRender::VisMask::Mask_3DGUI_NonIntersectable;
        mViewer->getCamera()->setCullMask(renderMask);
        mViewer->getCamera()->setCullMaskLeft(renderMask);
        mViewer->getCamera()->setCullMaskRight(renderMask);

        static unsigned int sLastAndroidVrRenderMask = 0;
        if (sLastAndroidVrRenderMask != renderMask)
        {
            sLastAndroidVrRenderMask = renderMask;
            Log(Debug::Warning) << "Android VR proof: forced draw-time VR cull mask=0x" << std::hex
                                << renderMask << std::dec;
        }
    }
#endif

    mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

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
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
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
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;
#ifdef OPENMW_ENABLE_VR
    mVrViewer = nullptr;
    mCallbackManager = nullptr;
    mVrGUIManager = nullptr;
    mXrSession = nullptr;
    mXrInstance = nullptr;
#endif

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

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
#ifdef __ANDROID__
    static std::unique_ptr<MyGUI::LogManager> sAndroidMyGuiLogManager;
    if (MyGUI::LogManager::getInstancePtr() == nullptr)
        sAndroidMyGuiLogManager = std::make_unique<MyGUI::LogManager>();
#endif

    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

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

#ifdef OPENMW_ENABLE_VR
    if (VR::getVR())
        realizeOperations->add(new InitializeVrOperation(this));
#endif

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
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

#ifdef OPENMW_ENABLE_VR
    mCallbackManager = std::make_unique<Misc::CallbackManager>(mViewer);
#endif

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());
    logFnvDlodSettingsProbe(*mVFS);
    logFnvPsaDeathPoseProbe(*mVFS);

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
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
        Version::getOpenmwVersionDescription(), mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

#ifdef OPENMW_ENABLE_VR
    if (VR::getVR())
        configureVRPreScene(keybinderUser, keybinderUserExists, userGameControllerdb, gameControllerdb);
    else
#endif
        mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
            keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    const bool fnvAssetStudioCleanBoot = std::getenv("OPENMW_FNV_ASSET_STUDIO_CLEAN_BOOT") != nullptr;
    if (!mSkipMenu && !fnvAssetStudioCleanBoot)
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
    listener->loadingOff();

#ifdef OPENMW_ENABLE_VR
    std::unique_ptr<MWRender::Camera> camera;
    if (VR::getVR())
        camera = std::make_unique<MWRender::Camera>(mViewer->getCamera());
#endif

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue
#ifdef OPENMW_ENABLE_VR
        , std::move(camera)
#endif
    );
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    mWindowManager->initUI();
    mLuaManager->initPostLoad();

#ifdef OPENMW_ENABLE_VR
    if (VR::getVR())
        configureVRScene();
#endif

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

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
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

    prepareEngine();

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

    // Start the game
    const bool fnvAssetStudioCleanBoot = std::getenv("OPENMW_FNV_ASSET_STUDIO_CLEAN_BOOT") != nullptr;
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (fnvAssetStudioCleanBoot)
    {
        Log(Debug::Info) << "FNV/ESM4 asset studio clean boot active state=no-game terrain=not-started "
                         << "gate=native-asset-studio-clean-boot runtime=runtime-supported";
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
        mStateManager->newGame(!mNewGame);
    }

    if (mStartupScript.empty())
    {
        const char* fnvProofStartupScript = std::getenv("OPENMW_FNV_PROOF_STARTUP_SCRIPT");
        if (fnvProofStartupScript != nullptr && fnvProofStartupScript[0] != '\0')
            mStartupScript = fnvProofStartupScript;
    }

    bool startupScriptExecuted = false;
    const auto executeStartupScriptWhenRunning = [&] {
        if (startupScriptExecuted || mStartupScript.empty()
            || mStateManager->getState() != MWState::StateManager::State_Running)
            return;

        mWindowManager->executeInConsole(mStartupScript);
        startupScriptExecuted = true;
    };
    executeStartupScriptWhenRunning();

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        executeStartupScriptWhenRunning();

        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        MWRender::AssetCapture::maybeStartFromEnvironment(frameNumber, mViewer.get());
        MWRender::AssetCapture::update();

        if (!frame(frameNumber, static_cast<float>(dt)))
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

#ifdef OPENMW_ENABLE_VR
void OMW::Engine::configureVRGraphics(osg::GraphicsContext* gc)
{
    configureVRInputProfiles();

    mXrInstance = std::make_unique<XR::Instance>(gc, mWindow);
    mXrSession = mXrInstance->createSession();
    if (mXrSession->appShouldShareDepthInfo())
        mSelectDepthFormatOperation->setSupportedFormats(mXrInstance->platform().supportedDepthFormats());
    mSelectColorFormatOperation->setSupportedFormats(mXrInstance->platform().supportedColorFormats());
}

void OMW::Engine::configureVRInputProfiles()
{
    const std::string xrinputuserdefault = (mCfgMgr.getUserConfigPath() / "openxrinteractionprofiles.xml").string();
    const std::string xrinputlocaldefault = (mCfgMgr.getLocalPath() / "openxrinteractionprofiles.xml").string();
    const std::string xrinputglobaldefault = (mCfgMgr.getGlobalPath() / "openxrinteractionprofiles.xml").string();

    std::string xrInteractionProfiles;
    if (std::filesystem::exists(xrinputuserdefault))
        xrInteractionProfiles = xrinputuserdefault;
    else if (std::filesystem::exists(xrinputlocaldefault))
        xrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        xrInteractionProfiles = xrinputglobaldefault;

    std::string defaultXrInteractionProfiles;
    if (std::filesystem::exists(xrinputlocaldefault))
        defaultXrInteractionProfiles = xrinputlocaldefault;
    else if (std::filesystem::exists(xrinputglobaldefault))
        defaultXrInteractionProfiles = xrinputglobaldefault;

    Log(Debug::Verbose) << "xrinteractionprofiles user: " << xrinputuserdefault;
    Log(Debug::Verbose) << "xrinteractionprofiles local: " << xrinputlocaldefault;
    Log(Debug::Verbose) << "xrinteractionprofiles global: " << xrinputglobaldefault;

    XR::loadInteractionProfiles(xrInteractionProfiles, defaultXrInteractionProfiles);
}

void OMW::Engine::configureVRPreScene(const std::filesystem::path& userFile, bool userFileExists,
    const std::filesystem::path& userControllerBindingsFile, const std::filesystem::path& controllerBindingsFile)
{
    VR::setLeftHandedMode(Settings::vr().mLeftHandedMode);

    mVrViewer = std::make_unique<VR::Viewer>(mXrSession, mViewer);
    mCallbackManager->installRenderStages();
    mVrViewer->configureCallbacks();

    auto cullMask = ~(MWRender::VisMask::Mask_UpdateVisitor | MWRender::VisMask::Mask_SimpleWater);
    cullMask &= ~MWRender::VisMask::Mask_GUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI_NonIntersectable;
#if defined(__ANDROID__) && defined(XR_USE_PLATFORM_ANDROID)
    Log(Debug::Warning) << "Android VR proof: main camera cull mask keeps VR GUI, mask=0x" << std::hex << cullMask
                        << std::dec;
#endif
    mViewer->getCamera()->setCullMask(cullMask);
    mViewer->getCamera()->setCullMaskLeft(cullMask);
    mViewer->getCamera()->setCullMaskRight(cullMask);

    mInputManager = std::make_unique<MWVR::VRInputManager>(mWindow, mViewer, mScreenCaptureHandler, userFile,
        userFileExists, userControllerBindingsFile, controllerBindingsFile, mGrab);
    mVrGUIManager = std::make_unique<MWVR::VRGUIManager>(mViewer->getSceneData()->asGroup());

    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(true);
}

void OMW::Engine::configureVRScene()
{
#if defined(__ANDROID__) && defined(XR_USE_PLATFORM_ANDROID)
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(true);
    auto cullMask = ~(MWRender::VisMask::Mask_UpdateVisitor | MWRender::VisMask::Mask_SimpleWater);
    cullMask &= ~MWRender::VisMask::Mask_GUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI;
    cullMask |= MWRender::VisMask::Mask_3DGUI_NonIntersectable;
    mViewer->getCamera()->setCullMask(cullMask);
    mViewer->getCamera()->setCullMaskLeft(cullMask);
    mViewer->getCamera()->setCullMaskRight(cullMask);
    Log(Debug::Warning) << "Android VR proof: configureVRScene reapplied VR GUI cull mask=0x" << std::hex << cullMask
                        << std::dec;
#else
    mStereoManager->setShouldAttachMultiviewFramebufferToMainCamera(false);
#endif
    mCallbackManager->installRenderStages();
    mVrGUIManager->initScene();
}
#endif
