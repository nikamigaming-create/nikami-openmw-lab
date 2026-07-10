#include "engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>
#include <vector>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/Image>
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
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadmisc.hpp>
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
#include "mwworld/cellstore.hpp"
#include "mwworld/containerstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/esmstore.hpp"
#include "mwworld/manualref.hpp"
#include "mwworld/worldimp.hpp"
#include "mwworld/worldmodel.hpp"

#include "mwphysics/collisiontype.hpp"
#include "mwphysics/raycasting.hpp"

#include "mwrender/characterpreview.hpp"
#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"
#include "mwclass/esm4npc.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"
#include "mwmechanics/actorutil.hpp"
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

    bool snapProofActorToRenderGround(MWWorld::World& world, MWWorld::Ptr& actor, const char* target)
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

        const float visualBottom = bounds.valid() ? bounds.zMin() : current.pos[2];
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

    bool selectProofActorCameraByOrbitRays(MWWorld::World& world, const MWWorld::Ptr& actor, const char* target,
        const osg::Vec3f& focus, osg::Vec3f& targetPos)
    {
        if (!proofEnvEnabled("OPENMW_PROOF_ACTOR_VIEW_ORBIT_RAYCAST"))
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
        return lowerName.find("fnv part ") != std::string_view::npos
            && (lowerName.find("characters/head/head") != std::string_view::npos
                || lowerName.find("actors/human/characterassets/male/malehead") != std::string_view::npos
                || lowerName.find("actors/human/characterassets/female/femalehead") != std::string_view::npos);
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
            if (lowerName == "bip01 head" || lowerName == "bip01 head_nub")
            {
                const osg::Matrixd localToWorld = osg::computeLocalToWorld(getNodePath());
                mHeadCenter += localToWorld.getTrans();
                ++mHeadMatched;
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
        unsigned int getHeadMatched() const { return mHeadMatched; }
        unsigned int getFeatureMatched() const { return mFeatureMatched; }
        osg::Vec3d getHeadCenter() const
        {
            return mHeadMatched > 0 ? mHeadCenter / static_cast<double>(mHeadMatched) : osg::Vec3d();
        }
        osg::Vec3d getFeatureCenter() const
        {
            return mFeatureMatched > 0 ? mFeatureCenter / static_cast<double>(mFeatureMatched) : osg::Vec3d();
        }

    private:
        osg::BoundingBox mBounds;
        osg::Vec3d mHeadCenter;
        osg::Vec3d mFeatureCenter;
        unsigned int mMatched = 0;
        unsigned int mHeadMatched = 0;
        unsigned int mFeatureMatched = 0;
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
            inventory.add(id, count, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter inventory added " << editorId << " x" << count << " id="
                             << id.toDebugString();
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
            inventory.add(refId, count, false);
            Log(Debug::Info) << "FNV/ESM4 proof: starter proof inventory added " << id << " x" << count;
            return true;
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 proof: starter proof inventory failed " << id << ": " << e.what();
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
            { ESM::Attribute::Strength, 6.f },
            { ESM::Attribute::Intelligence, 6.f },
            { ESM::Attribute::Willpower, 5.f },
            { ESM::Attribute::Agility, 6.f },
            { ESM::Attribute::Speed, 5.f },
            { ESM::Attribute::Endurance, 6.f },
            { ESM::Attribute::Personality, 5.f },
            { ESM::Attribute::Luck, 6.f },
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

        MWWorld::ContainerStore& inventory = player.getClass().getContainerStore(player);
        const MWWorld::ESMStore& store = world.getStore();
        int added = 0;
        added += addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNV9mmPistol", 1) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Weapon>(inventory, store, "WeapNVVarmintRifle", 1) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Ammunition>(inventory, store, "Ammo9mm", 60) ? 1 : 0;
        added += addFNVEditorItem<ESM4::Potion>(inventory, store, "Stimpak", 5) ? 1 : 0;
        added += addFNVEditorItem<ESM4::MiscItem>(inventory, store, "BobbyPin", 5) ? 1 : 0;
        added += addFNVEditorItem<ESM4::MiscItem>(inventory, store, "Caps001", 75) ? 1 : 0;
        int proofAdded = 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_PISTOL", 1) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_VARMINT_RIFLE", 1) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_9MM_AMMO", 60) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_STIMPAK", 5) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_BOBBY_PIN", 5) ? 1 : 0;
        proofAdded += addProofInventoryItem(inventory, "FNV_PROOF_CAPS", 75) ? 1 : 0;

        const ESM::RefId proofOutfitId = findEsm4EditorId<ESM4::Armor>(store, "OutfitRepublican02");
        if (!proofOutfitId.empty())
        {
            if (const ESM4::Armor* proofOutfit = store.get<ESM4::Armor>().search(proofOutfitId))
            {
                try
                {
                    const bool equipped = MWClass::ESM4Npc::addEquippedArmor(player, proofOutfit);
                    Log(Debug::Info) << "FNV/ESM4 proof: level-1 Courier visual outfit "
                                     << proofOutfit->mEditorId << " form=" << proofOutfitId.toDebugString()
                                     << " model=" << MWClass::ESM4Npc::chooseEquipmentModel(
                                                        proofOutfit, MWClass::ESM4Npc::isFemale(player))
                                     << " added=" << equipped;
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: level-1 Courier visual outfit "
                                        << proofOutfit->mEditorId << " skipped: " << e.what();
                }
            }
        }
        else
            Log(Debug::Warning) << "FNV/ESM4 proof: level-1 Courier visual outfit OutfitRepublican02 not found";

        Log(Debug::Info) << "FNV/ESM4 proof: level-1 Courier profile applied level=" << stats.getLevel()
                         << " attributes=8 skills=" << ESM::Skill::Length << " starterItemKinds=" << added
                         << " proofStarterItemKinds=" << proofAdded;
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
        Log(Debug::Info) << "FNV/ESM4 diag: settled flat startup camera via zoom-cycle equivalent"
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
    static const std::vector<int> proofScreenshotFrames = getProofFrames("OPENMW_PROOF_SCREENSHOT_FRAME");
    static const std::vector<float> proofActorViewOrbitDegrees
        = getProofFloats("OPENMW_PROOF_ACTOR_VIEW_ORBIT_DEGREES");
    static const int proofScreenshotReadyFrames = getProofFrame("OPENMW_PROOF_SCREENSHOT_READY_FRAMES");
    static const int proofInventoryFrame = getProofFrame("OPENMW_PROOF_INVENTORY_FRAME");
    static const std::vector<int> proofInventoryPaneFrames = getProofFrames("OPENMW_PROOF_INVENTORY_PANE_FRAME");
    static const std::vector<int> proofInventoryPaneIndices = getProofFrames("OPENMW_PROOF_INVENTORY_PANE_INDEX");
    static const int proofQuickSaveFrame = getProofFrame("OPENMW_PROOF_QUICKSAVE_FRAME");
    static const int proofSayFrame = getProofFrame("OPENMW_PROOF_SAY_FRAME");
    static const int proofTimedScript1Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_1_FRAME");
    static const int proofTimedScript2Frame = getProofFrame("OPENMW_PROOF_TIMED_SCRIPT_2_FRAME");
    static std::size_t proofScreenshotFrameIndex = 0;
    static std::size_t proofInventoryPaneFrameIndex = 0;
    static bool proofScreenshotReadyQueued = false;
    static bool proofInventoryOpened = false;
    static bool proofQuickSaveQueued = false;
    static bool proofSayQueued = false;
    static bool proofTimedScript1Executed = false;
    static bool proofTimedScript2Executed = false;
    static bool proofActorCameraAligned = false;
    static int proofActorCameraAlignedFrame = -1;
    static std::size_t proofActorCameraAlignedScreenshotIndex = static_cast<std::size_t>(-1);
    static bool proofActorAlignedScreenshotQueued = false;
    static bool proofActorStagedForCamera = false;
    static bool proofActorSnappedToRenderGround = false;
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
    static bool worldViewerNonStaticStartCameraSettled = false;
    static bool fnvFlatStartupCameraSettled = false;
    static bool proofScreenshotWaitLogged = false;
    static int proofWorldReadyFrames = 0;
    static bool worldViewerTelemetryLogged = false;
    static unsigned int worldViewerTelemetryLastFrame = 0;
    static bool worldViewerCameraWaitLogged = false;
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
        MWWorld::Ptr actor = mWorld->searchPtr(makeProofRefId(value), false, false);
        if (!actor.isEmpty())
            return actor;

        const std::string_view target(value);
        const ESM::RefId targetRefId = makeProofRefId(value);
        const std::optional<ESM::FormId> targetFormId = parseProofFormId(target);
        const std::string targetCompact = compactProofToken(target);
        const bool logActors = std::getenv("OPENMW_PROOF_LOG_ACTORS") != nullptr;
        int scanned = 0;
        int actors = 0;
        MWWorld::Ptr found;

        for (MWWorld::CellStore* cellstore : mWorld->getWorldScene().getActiveCells())
        {
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

    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

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
                Log(Debug::Info) << "FNV/ESM4 diag: VR mirror window hidden; keeping world simulation running";
                loggedHiddenVrWindow = true;
            }

            if (!mWindowManager->isWindowVisible() && !VR::getVR())
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

    // if there is a separate Lua thread, it starts the update now
    worldViewerTrace(frameNumber, "lua-worker-allow.begin");
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);
    worldViewerTrace(frameNumber, "lua-worker-allow.end");

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

    if (!proofDelayedStartupScriptExecuted && std::getenv("OPENMW_PROOF_DELAY_STARTUP_SCRIPT") != nullptr
        && !mStartupScript.empty() && proofWorldReady)
    {
        Log(Debug::Info) << "FNV/ESM4 proof: executing delayed startup script after world load";
        mWindowManager->executeInConsole(mStartupScript);
        proofDelayedStartupScriptExecuted = true;
    }

    const bool proofFNVBootstrapProfile = hasFalloutNvContent(mContentFiles)
        || std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr;
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
        && worldViewerNonStaticStartCameraRequested() && mWorld != nullptr)
    {
        worldViewerNonStaticStartCameraSettled = settleWorldViewerNonStaticStartCamera(*mWorld);
    }

    const bool worldViewerExplicitNonStaticStartCamera = worldViewerNonStaticStartCameraRequested();
    if (!fnvFlatStartupCameraSettled && proofWorldReady && proofWorldReadyFrames >= 2 && !VR::getVR()
        && hasFalloutNvContent(mContentFiles) && !worldViewerStaticStartCamera
        && !worldViewerExplicitNonStaticStartCamera)
    {
        settleFNVFlatStartupCamera(*mWorld);
        fnvFlatStartupCameraSettled = true;
    }

    if (!proofFNVCameraResetApplied && proofWorldReady && std::getenv("OPENMW_FNV_PROOF_RESET_CAMERA") != nullptr)
    {
        resetFNVProofCamera(*mWorld);
        proofFNVCameraResetApplied = true;
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

    const bool proofRequiresActorForScreenshot = std::getenv("OPENMW_PROOF_REQUIRE_ACTOR_FOR_SCREENSHOT") != nullptr;
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
    if ((!proofSayQueued || proofOrbitBurstAlignReached || proofActorScreenshotNeedsResolve) && proofSayFrame >= 0
        && frameNumber >= static_cast<unsigned>(proofSayFrame))
    {
        const char* proofSayFile = std::getenv("OPENMW_PROOF_SAY_FILE");
        const char* proofSayActor = std::getenv("OPENMW_PROOF_SAY_ACTOR");
        const char* proofSayTopic = std::getenv("OPENMW_PROOF_SAY_TOPIC");
        MWWorld::Ptr proofActor;
        if (proofSayActor != nullptr && *proofSayActor != '\0')
        {
            proofActor = resolveProofActor(proofSayActor);
            if (proofActorScreenshotNeedsResolve)
                proofActorScreenshotLastResolveFrame = static_cast<int>(frameNumber);
            Log(Debug::Info) << "FNV/ESM4 proof: resolved proof say actor \"" << proofSayActor
                             << "\" -> " << proofActor.toString();
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_SUPPRESS_ACTOR_AI") != nullptr)
            {
                try
                {
                    MWMechanics::CreatureStats& proofActorStats = proofActor.getClass().getCreatureStats(proofActor);
                    proofActorStats.getAiSequence().stopCombat();
                    proofActorStats.setAttackingOrSpell(false);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Fight, 0);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Flee, 0);
                    proofActorStats.setAiSetting(MWMechanics::AiSetting::Alarm, 0);
                    proofActorStats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, false);
                    proofActorStats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceRun, false);
                    if (std::getenv("OPENMW_PROOF_DISABLE_ACTOR_COLLISION") != nullptr && mWorld != nullptr)
                        mWorld->setActorCollisionMode(proofActor, false, false);
                    Log(Debug::Info) << "FNV/ESM4 proof: suppressed proof actor AI target=\"" << proofSayActor
                                     << "\" ptr=" << proofActor.toString();
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
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: failed to stage actor target=\""
                                        << proofSayActor << "\": " << e.what();
                    proofActorStagedForCamera = true;
                }
            }
            if (!proofActor.isEmpty() && std::getenv("OPENMW_PROOF_SNAP_ACTOR_TO_RENDER_GROUND") != nullptr
                && !proofActorSnappedToRenderGround && mWorld != nullptr)
            {
                try
                {
                    proofActorSnappedToRenderGround
                        = snapProofActorToRenderGround(*mWorld, proofActor, proofSayActor);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 proof: render-ground snap failed target=\""
                                        << proofSayActor << "\": " << e.what();
                    proofActorSnappedToRenderGround = true;
                }
            }
            if (!proofActor.isEmpty())
                logProofActorRenderBounds(proofActor, proofSayActor, "post-stage-snap");

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

            if (!proofActor.isEmpty() && !proofNeutralActorPreviewReady
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
                const bool requireActorForScreenshot = std::getenv("OPENMW_PROOF_REQUIRE_ACTOR_FOR_SCREENSHOT") != nullptr;
                bool useFaceAxisCamera = false;
                osg::Vec2f faceAxis(0.f, 0.f);
                osg::Vec3d actorAim(actorPos.pos[0], actorPos.pos[1], actorPos.pos[2] + targetZ);
                double cameraZ = actorPos.pos[2] + offsetZ;
                const bool useRenderBounds = std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_RENDER_BOUNDS") != nullptr;
                const bool useFaceBounds = std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_BOUNDS") != nullptr;
                bool actorViewBoundsAccepted = false;
                const auto isProofBoundsNearActor = [&](const osg::BoundingBox& bounds, const char* label) {
                    const double dx = bounds.center().x() - actorPos.pos[0];
                    const double dy = bounds.center().y() - actorPos.pos[1];
                    const double dz = bounds.center().z() - actorPos.pos[2];
                    const double centerDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float maxCenterDistance
                        = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_MAX_BOUNDS_CENTER_DISTANCE", 1000.f);
                    if (centerDistance <= maxCenterDistance)
                        return true;
                    Log(Debug::Warning) << "FNV/ESM4 proof: actor " << label
                                        << " bounds rejected as stale/far target=\"" << proofSayActor
                                        << "\" center=(" << bounds.center().x() << "," << bounds.center().y()
                                        << "," << bounds.center().z() << ") actorPos=(" << actorPos.pos[0] << ","
                                        << actorPos.pos[1] << "," << actorPos.pos[2] << ") distance="
                                        << centerDistance << " max=" << maxCenterDistance;
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
                        const float focusPercent = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_BOUNDS_FOCUS", 0.72f);
                        const double focusZ = bounds.zMin() + (bounds.zMax() - bounds.zMin()) * focusPercent;
                        actorAim = osg::Vec3d(bounds.center().x(), bounds.center().y(), focusZ);
                        cameraZ = focusZ + (offsetZ - targetZ);
                        actorViewBoundsAccepted = true;
                        Log(Debug::Info) << "FNV/ESM4 proof: actor render bounds target=\"" << proofSayActor
                                         << "\" min=(" << bounds.xMin() << "," << bounds.yMin() << ","
                                         << bounds.zMin() << ") max=(" << bounds.xMax() << "," << bounds.yMax()
                                         << "," << bounds.zMax() << ") focus=(" << actorAim.x() << ","
                                         << actorAim.y() << "," << actorAim.z() << ") size=("
                                         << bounds.xMax() - bounds.xMin() << "," << bounds.yMax() - bounds.yMin()
                                         << "," << bounds.zMax() - bounds.zMin() << ") cameraZ=" << cameraZ;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor render bounds invalid target=\""
                                            << proofSayActor << "\"";
                }
                if (useFaceBounds && proofActor.getRefData().getBaseNode() != nullptr)
                {
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
                        if (faceBoundsVisitor.getHeadMatched() > 0 && faceBoundsVisitor.getFeatureMatched() > 0)
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
                        if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_FACE_AXIS") != nullptr
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
                                         << " featureMatched=" << faceBoundsVisitor.getFeatureMatched()
                                         << " headCenter=(" << headCenter.x() << "," << headCenter.y() << ","
                                         << headCenter.z() << ") featureCenter=(" << featureCenter.x() << ","
                                         << featureCenter.y() << "," << featureCenter.z() << ") faceAxis=("
                                         << faceAxis.x() << "," << faceAxis.y()
                                         << ") useFaceAxisCamera=" << useFaceAxisCamera;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 proof: actor face bounds invalid target=\""
                                            << proofSayActor << "\" matched="
                                            << faceBoundsVisitor.getMatched();
                }
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_USE_ACTOR_FACING") != nullptr)
                {
                    float frontDistance = readProofFloat("OPENMW_PROOF_ACTOR_VIEW_FRONT_DISTANCE", 0.f);
                    if (frontDistance <= 0.f)
                    {
                        frontDistance = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                        if (frontDistance <= 0.f)
                            frontDistance = 220.f;
                    }
                    const float actorYaw = actorPos.rot[2];
                    const float frontSign = std::getenv("OPENMW_PROOF_ACTOR_VIEW_FALLOUT_FRONT") != nullptr ? -1.f : 1.f;
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
                if (std::getenv("OPENMW_PROOF_ACTOR_VIEW_RAYCAST_BACKOFF") != nullptr)
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
                if (selectProofActorCameraByOrbitRays(*mWorld, proofActor, proofSayActor, actorAim, targetPos))
                    cameraZ = targetPos.z();
                if (adjustProofActorCameraByRenderRay(*mWorld, proofActor, proofSayActor, actorAim, targetPos))
                    cameraZ = targetPos.z();
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

        if (!proofSayQueued && !proofActor.isEmpty() && proofSayTopic != nullptr && *proofSayTopic != '\0'
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

    const bool proofScreenshotFrameReached = proofScreenshotFrameIndex < proofScreenshotFrames.size()
        && frameNumber >= static_cast<unsigned>(proofScreenshotFrames[proofScreenshotFrameIndex]);
    const bool proofScreenshotReadyFramesReached
        = !proofScreenshotReadyQueued && proofScreenshotReadyFrames >= 0 && proofWorldReadyFrames >= proofScreenshotReadyFrames;
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
    const bool proofActorAlignedScreenshotReached = !proofActorAlignedScreenshotQueued && proofActorCameraAligned
        && proofActorAlignedScreenshotDelay >= 0 && proofActorCameraAlignedFrame >= 0
        && frameNumber >= static_cast<unsigned>(proofActorCameraAlignedFrame + proofActorAlignedScreenshotDelay)
        && (proofActorAlignedScreenshotMinFrame < 0
            || frameNumber >= static_cast<unsigned>(proofActorAlignedScreenshotMinFrame));
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
        worldViewerTrace(frameNumber, "screenshot-queue.end");
    }

    worldViewerTrace(frameNumber, "rendering-traversals.begin");
    mViewer->renderingTraversals();
    worldViewerTrace(frameNumber, "rendering-traversals.end");

    worldViewerTrace(frameNumber, "lua-worker-finish.begin");
    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
    worldViewerTrace(frameNumber, "lua-worker-finish.end");

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
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine data load complete";
    listener->loadingOff();

    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine world init begin";
    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue, std::move(camera));
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine world init complete";
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine world scene registered";
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine setupPlayer begin";
    mWorld->setupPlayer();
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine setupPlayer complete";
    mWorld->setRandomSeed(mRandomSeed);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine random seed set";

    // ## VR_PATCH BEGIN
    if (VR::getVR())
    {
        configureVRScene();
    }
    // ## VR_PATCH END

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine gmst loader begin";
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
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine gmst loader ready";

    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine window store begin";
    mWindowManager->setStore(mWorld->getStore());
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine window initUI begin";
    mWindowManager->initUI();
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine window initUI complete";

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine translation load begin";
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine translation load complete";

    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine compiler extensions begin";
    Compiler::registerExtensions(mExtensions);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine compiler extensions complete";

    // Create script system
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine script system begin";
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine script system ready";

    // Create game mechanics system
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine mechanics begin";
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine mechanics ready";

    // Create dialog system
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine dialogue begin";
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine dialogue ready";

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
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua init begin";
    mLuaManager->init();
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua init complete";

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
    Log(Debug::Info) << "FNV/ESM4 diag: prepareEngine lua worker ready";
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

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
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

