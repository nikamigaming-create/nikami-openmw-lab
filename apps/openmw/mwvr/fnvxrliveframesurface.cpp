#include "fnvxrliveframesurface.hpp"

#include "openxrinput.hpp"

#include "../mwrender/vismask.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/mode.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/worldmodel.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/formid.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/stereo/stereomanager.hpp>
#include <components/stereo/types.hpp>
#include <components/vr/layer.hpp>
#include <components/vr/swapchain.hpp>
#include <components/vr/viewer.hpp>

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/FrameBufferObject>
#include <osg/GLExtensions>
#include <osg/Material>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/PolygonMode>
#include <osg/RenderInfo>
#include <osgUtil/CullVisitor>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace MWVR
{
    namespace
    {
        constexpr std::uint32_t SharedVideoMagic = 0x46585646; // FNVF
        constexpr std::uint32_t SharedStereoMagic = 0x53585646; // FNXS
        constexpr std::uint32_t SharedVideoMaxWidth = 2048;
        constexpr std::uint32_t SharedVideoMaxHeight = 1280;
        constexpr std::size_t SharedVideoHeaderBytes = 28;
        constexpr std::size_t SharedVideoMappingBytes
            = SharedVideoHeaderBytes + SharedVideoMaxWidth * SharedVideoMaxHeight * 4;

        constexpr std::uint32_t DInputSharedMagic = 0x49444e46; // FNDI
        constexpr std::uint32_t DInputSharedVersion = 2;
        constexpr std::uint32_t CameraSharedMagic = 0x43585646; // FNXC
        constexpr std::uint32_t CameraSharedVersion = 1;
        constexpr std::uint32_t RuntimeSharedMagic = 0x53585646; // FNVS
        constexpr std::uint32_t RuntimeSharedVersion = 1;
        constexpr std::uint32_t VrPoseSharedMagic = 0x52505646; // FVPR
        constexpr std::uint32_t VrPoseSharedVersion = 2;
        constexpr std::uint32_t PlayerSharedMagic = 0x50564e46; // FNVP
        constexpr std::uint32_t PlayerSharedVersion = 1;
        constexpr std::uint32_t OpenMwPlayerSharedMagic = 0x4f4d4e46; // FNMO
        constexpr std::uint32_t OpenMwPlayerSharedVersion = 1;
        constexpr std::uint32_t PlayerSharedFlagPlayerNodeValid = 1u << 0;
        constexpr std::uint32_t PlayerSharedFlagCameraValid = 1u << 1;
        constexpr std::uint32_t PlayerSharedFlagCellKnown = 1u << 2;
        constexpr std::uint32_t PlayerSharedFlagThirdPerson = 1u << 3;
        constexpr std::uint32_t PlayerSharedFlagGameplay = 1u << 4;
        constexpr std::uint32_t RuntimePhaseGameplay = 3;
        constexpr std::uint32_t RuntimeBlockingMenuBits = 0x7e;

        struct SharedVideoHeader
        {
            std::uint32_t magic;
            volatile std::int32_t writing;
            volatile std::int32_t sequence;
            std::int32_t width;
            std::int32_t height;
            std::int32_t pitchBytes;
            std::int32_t format;
        };

        struct SharedStereoHeader
        {
            std::uint32_t magic;
            volatile std::int32_t writing;
            volatile std::int32_t sequence;
            std::int32_t width;
            std::int32_t height;
            std::int32_t pitchBytes;
            std::int32_t format;
            std::int32_t separated;
            std::int32_t worldCandidate;
            std::int32_t uiActive;
            std::int32_t poseValid;
            std::int32_t poseSequence;
            float leftEyeRot[4];
            float leftEyePos[3];
            float rightEyeRot[4];
            float rightEyePos[3];
            float leftFov[4];
            float rightFov[4];
        };

        constexpr std::size_t SharedStereoMappingBytes
            = sizeof(SharedStereoHeader) + SharedVideoMaxWidth * SharedVideoMaxHeight * 4 * 2;

        struct SharedDInputState
        {
            std::uint32_t magic;
            std::uint32_t version;
            std::uint32_t frame;
            std::int32_t clientX;
            std::int32_t clientY;
            std::uint32_t pointerActive;
            std::uint32_t mouseClickPacket;
            std::uint32_t keyboardAcceptPacket;
            std::uint32_t reserved[7];
        };

        struct SharedCameraState
        {
            std::uint32_t magic;
            std::uint32_t version;
            volatile std::int32_t sequence;
            std::uint64_t frame;
            std::uint32_t active;
            std::uint32_t thirdPerson;
            float worldRot[9];
            float worldPos[3];
        };

        struct SharedRuntimeState
        {
            std::uint32_t magic;
            std::uint32_t version;
            volatile std::int32_t sequence;
            std::uint64_t frame;
            std::uint32_t menuBits;
            std::uint32_t phase;
            std::uint32_t uiInputAllowed;
            std::uint32_t cameraActive;
            std::uint32_t showroomActive;
            std::uint32_t showroomPhase;
            std::uint32_t showroomSceneIndex;
            std::uint32_t showroomCellFormId;
            std::uint32_t reserved[8];
        };

        struct SharedPlayerState
        {
            std::uint32_t magic;
            std::uint32_t version;
            volatile std::int32_t sequence;
            std::uint32_t flags;
            std::uint64_t frame;
            std::uint32_t currentCellFormId;
            std::uint32_t playerAddress;
            std::uint32_t playerNodeAddress;
            std::uint32_t cameraNodeAddress;
            float playerWorldRot[9];
            float playerWorldPos[3];
            float cameraWorldRot[9];
            float cameraWorldPos[3];
            std::uint32_t reserved[6];
        };

        struct SharedPlayerSnapshot
        {
            std::uint64_t frame = 0;
            std::uint32_t flags = 0;
            std::uint32_t currentCellFormId = 0;
            std::uint32_t playerAddress = 0;
            std::uint32_t playerNodeAddress = 0;
            std::uint32_t cameraNodeAddress = 0;
            float playerWorldRot[9] {};
            float playerWorldPos[3] {};
            float cameraWorldRot[9] {};
            float cameraWorldPos[3] {};
        };

        struct SharedVrPoseState
        {
            std::uint32_t magic;
            std::uint32_t version;
            volatile std::int32_t sequence;
            std::uint64_t frame;
            float hmdRot[4];
            float hmdPos[3];
            float leftRot[4];
            float leftPos[3];
            float rightRot[4];
            float rightPos[3];
            float leftEyeRot[4];
            float leftEyePos[3];
            float rightEyeRot[4];
            float rightEyePos[3];
            float leftFov[4]; // left, right, up, down
            float rightFov[4]; // left, right, up, down
        };

        static_assert(sizeof(SharedPlayerState) == 160, "SharedPlayerState layout changed");
        static_assert(offsetof(SharedPlayerState, sequence) == 8, "SharedPlayerState sequence offset changed");
        static_assert(offsetof(SharedPlayerState, frame) == 16, "SharedPlayerState frame offset changed");
        static_assert(offsetof(SharedPlayerState, currentCellFormId) == 24,
            "SharedPlayerState currentCellFormId offset changed");
        static_assert(
            offsetof(SharedPlayerState, playerWorldRot) == 40, "SharedPlayerState playerWorldRot offset changed");
        static_assert(
            offsetof(SharedPlayerState, playerWorldPos) == 76, "SharedPlayerState playerWorldPos offset changed");
        static_assert(
            offsetof(SharedPlayerState, cameraWorldRot) == 88, "SharedPlayerState cameraWorldRot offset changed");
        static_assert(
            offsetof(SharedPlayerState, cameraWorldPos) == 124, "SharedPlayerState cameraWorldPos offset changed");
        static_assert(sizeof(SharedStereoHeader) == 136, "SharedStereoHeader layout changed");
        static_assert(offsetof(SharedStereoHeader, poseValid) == 40, "SharedStereoHeader poseValid offset changed");
        static_assert(offsetof(SharedStereoHeader, leftEyeRot) == 48, "SharedStereoHeader leftEyeRot offset changed");

        bool envEnabled(const char* name, bool defaultValue)
        {
            const char* value = std::getenv(name);
            if (!value || !*value)
                return defaultValue;
            return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0
                && std::strcmp(value, "FALSE") != 0 && std::strcmp(value, "OFF") != 0;
        }

        float envFloat(const char* name, float defaultValue)
        {
            const char* value = std::getenv(name);
            if (!value || !*value)
                return defaultValue;

            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            return end != value ? parsed : defaultValue;
        }

        bool debugFrameEnabled()
        {
            return envEnabled("OPENMW_FNVXR_RETAIL_SURFACE_DEBUG_FRAME", false);
        }

        void writePose(float rot[4], float pos[3], const Stereo::Pose& pose)
        {
            rot[0] = static_cast<float>(pose.orientation.x());
            rot[1] = static_cast<float>(pose.orientation.y());
            rot[2] = static_cast<float>(pose.orientation.z());
            rot[3] = static_cast<float>(pose.orientation.w());
            const osg::Vec3 meters = pose.position.asMeters();
            pos[0] = static_cast<float>(meters.x());
            pos[1] = static_cast<float>(meters.y());
            pos[2] = static_cast<float>(meters.z());
        }

        void writeFov(float out[4], const Stereo::FieldOfView& fov)
        {
            out[0] = fov.angleLeft;
            out[1] = fov.angleRight;
            out[2] = fov.angleUp;
            out[3] = fov.angleDown;
        }

        bool finiteArray(const float* values, std::size_t count)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!std::isfinite(values[i]))
                    return false;
            }
            return true;
        }

        bool poseArrayLooksUsable(const float rot[4], const float pos[3])
        {
            if (!finiteArray(rot, 4) || !finiteArray(pos, 3))
                return false;

            const float quatLenSq = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
            return quatLenSq >= 0.25f && quatLenSq <= 4.f
                && std::fabs(pos[0]) <= 100.f
                && std::fabs(pos[1]) <= 100.f
                && std::fabs(pos[2]) <= 100.f;
        }

        bool fovArrayLooksUsable(const float fov[4])
        {
            return finiteArray(fov, 4)
                && std::fabs(fov[0]) <= 3.2f
                && std::fabs(fov[1]) <= 3.2f
                && std::fabs(fov[2]) <= 3.2f
                && std::fabs(fov[3]) <= 3.2f;
        }

        Stereo::View readStereoView(const float rot[4], const float pos[3], const float fov[4])
        {
            Stereo::View view;
            view.pose.orientation = osg::Quat(rot[0], rot[1], rot[2], rot[3]);
            view.pose.position = Stereo::Position::fromMeters(pos[0], pos[1], pos[2]);
            view.fov = Stereo::FieldOfView{ fov[0], fov[1], fov[2], fov[3] };
            return view;
        }

        float distanceSquaredN(const float* left, const float* right, int count)
        {
            float result = 0.f;
            for (int i = 0; i < count; ++i)
            {
                const float delta = left[i] - right[i];
                result += delta * delta;
            }
            return result;
        }

        float distanceSquared(const float left[3], const float right[3])
        {
            return distanceSquaredN(left, right, 3);
        }

        float retailPlayerYaw(const SharedPlayerSnapshot& player)
        {
            return -std::atan2(player.playerWorldRot[3], player.playerWorldRot[0]);
        }

        ESM::Position retailPlayerPosition(const SharedPlayerSnapshot& player)
        {
            ESM::Position position;
            position.pos[0] = player.playerWorldPos[0];
            position.pos[1] = player.playerWorldPos[1];
            position.pos[2] = player.playerWorldPos[2];
            position.rot[0] = 0.f;
            position.rot[1] = 0.f;
            position.rot[2] = retailPlayerYaw(player);
            return position;
        }

        ESM::RefId retailCellRefId(std::uint32_t cellFormId)
        {
            return ESM::RefId::formIdRefId(ESM::FormId::fromUint32(cellFormId));
        }

        std::uint32_t cellFormId(const MWWorld::CellStore* cellStore)
        {
            if (!cellStore || !cellStore->getCell())
                return 0;

            const ESM::RefId& cellId = cellStore->getCell()->getId();
            const ESM::FormId* formId = cellId.getIf<ESM::FormId>();
            return formId ? formId->toUint32() : 0;
        }

        void writeIdentityMatrix(float out[9])
        {
            out[0] = 1.f;
            out[1] = 0.f;
            out[2] = 0.f;
            out[3] = 0.f;
            out[4] = 1.f;
            out[5] = 0.f;
            out[6] = 0.f;
            out[7] = 0.f;
            out[8] = 1.f;
        }

        bool writeRotationMatrix(float out[9], const osg::Quat& quat)
        {
            const double x = quat.x();
            const double y = quat.y();
            const double z = quat.z();
            const double w = quat.w();
            const double lengthSquared = x * x + y * y + z * z + w * w;
            if (!std::isfinite(lengthSquared) || lengthSquared < 0.25 || lengthSquared > 4.0)
            {
                writeIdentityMatrix(out);
                return false;
            }

            const double scale = 2.0 / lengthSquared;
            const double xx = x * x * scale;
            const double yy = y * y * scale;
            const double zz = z * z * scale;
            const double xy = x * y * scale;
            const double xz = x * z * scale;
            const double yz = y * z * scale;
            const double wx = w * x * scale;
            const double wy = w * y * scale;
            const double wz = w * z * scale;

            out[0] = static_cast<float>(1.0 - yy - zz);
            out[1] = static_cast<float>(xy - wz);
            out[2] = static_cast<float>(xz + wy);
            out[3] = static_cast<float>(xy + wz);
            out[4] = static_cast<float>(1.0 - xx - zz);
            out[5] = static_cast<float>(yz - wx);
            out[6] = static_cast<float>(xz - wy);
            out[7] = static_cast<float>(yz + wx);
            out[8] = static_cast<float>(1.0 - xx - yy);
            return finiteArray(out, 9);
        }

#ifdef _WIN32
        bool tryReadCameraActive(void* view, bool currentMode, std::uint32_t* magicOut, std::uint32_t* versionOut,
            bool* activeOut, bool* tornOut)
        {
            if (!view || !magicOut || !versionOut || !activeOut || !tornOut)
                return false;

            __try
            {
                auto* shared = static_cast<SharedCameraState*>(view);
                const std::uint32_t magic = shared->magic;
                const std::uint32_t version = shared->version;
                const std::int32_t sequenceBefore = shared->sequence;
                const std::uint32_t active = shared->active;
                const std::int32_t sequenceAfter = shared->sequence;
                *magicOut = magic;
                *versionOut = version;
                *activeOut = active != 0;
                *tornOut = (sequenceBefore & 1) != 0 || sequenceBefore != sequenceAfter;
                if (*tornOut)
                    *activeOut = currentMode;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool tryReadPlayerState(void* view, SharedPlayerSnapshot* snapshot, std::uint32_t* magicOut,
            std::uint32_t* versionOut, bool* tornOut)
        {
            if (!view || !snapshot || !magicOut || !versionOut || !tornOut)
                return false;

            __try
            {
                auto* shared = static_cast<SharedPlayerState*>(view);
                const std::int32_t sequenceBefore = shared->sequence;
                *magicOut = shared->magic;
                *versionOut = shared->version;
                snapshot->frame = shared->frame;
                snapshot->flags = shared->flags;
                snapshot->currentCellFormId = shared->currentCellFormId;
                snapshot->playerAddress = shared->playerAddress;
                snapshot->playerNodeAddress = shared->playerNodeAddress;
                snapshot->cameraNodeAddress = shared->cameraNodeAddress;
                std::memcpy(snapshot->playerWorldRot, shared->playerWorldRot, sizeof(snapshot->playerWorldRot));
                std::memcpy(snapshot->playerWorldPos, shared->playerWorldPos, sizeof(snapshot->playerWorldPos));
                std::memcpy(snapshot->cameraWorldRot, shared->cameraWorldRot, sizeof(snapshot->cameraWorldRot));
                std::memcpy(snapshot->cameraWorldPos, shared->cameraWorldPos, sizeof(snapshot->cameraWorldPos));
                const std::int32_t sequenceAfter = shared->sequence;
                *tornOut = (sequenceBefore & 1) != 0 || sequenceBefore != sequenceAfter;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool tryReadRuntimePhase(void* view, std::uint32_t* phaseOut, bool* tornOut)
        {
            if (!view || !phaseOut || !tornOut)
                return false;

            __try
            {
                auto* shared = static_cast<SharedRuntimeState*>(view);
                const std::int32_t sequenceBefore = shared->sequence;
                const std::uint32_t magic = shared->magic;
                const std::uint32_t version = shared->version;
                const std::uint32_t phase = shared->phase;
                const std::int32_t sequenceAfter = shared->sequence;
                if (magic != RuntimeSharedMagic || version != RuntimeSharedVersion)
                    return false;
                *phaseOut = phase;
                *tornOut = (sequenceBefore & 1) != 0 || sequenceBefore != sequenceAfter;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool tryReadRuntimeWorldReady(void* view, bool* readyOut, bool* tornOut)
        {
            if (!view || !readyOut || !tornOut)
                return false;

            __try
            {
                auto* shared = static_cast<SharedRuntimeState*>(view);
                const std::int32_t sequenceBefore = shared->sequence;
                const std::uint32_t magic = shared->magic;
                const std::uint32_t version = shared->version;
                const std::uint32_t menuBits = shared->menuBits;
                const std::uint32_t phase = shared->phase;
                const std::uint32_t cameraActive = shared->cameraActive;
                const std::uint32_t showroomActive = shared->showroomActive;
                const std::int32_t sequenceAfter = shared->sequence;
                if (magic != RuntimeSharedMagic || version != RuntimeSharedVersion)
                    return false;
                *readyOut = phase == RuntimePhaseGameplay && showroomActive == 0
                    && cameraActive != 0
                    && (menuBits & RuntimeBlockingMenuBits) == 0;
                *tornOut = (sequenceBefore & 1) != 0 || sequenceBefore != sequenceAfter;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool tryReadStereoWorldReady(void* view, std::int32_t lastSequence, bool* readyOut, std::int32_t* sequenceOut,
            std::int32_t* widthOut, std::int32_t* heightOut, std::int32_t* separatedOut,
            std::int32_t* worldCandidateOut, std::int32_t* uiActiveOut)
        {
            if (!view || !readyOut || !sequenceOut || !widthOut || !heightOut || !separatedOut
                || !worldCandidateOut || !uiActiveOut)
                return false;

            __try
            {
                auto* header = static_cast<const SharedStereoHeader*>(view);
                if (header->magic != SharedStereoMagic || header->writing)
                    return false;

                const std::int32_t sequenceBefore = header->sequence;
                const std::int32_t width = header->width;
                const std::int32_t height = header->height;
                const std::int32_t separated = header->separated;
                const std::int32_t worldCandidate = header->worldCandidate;
                const std::int32_t uiActive = header->uiActive;
                const std::int32_t sequenceAfter = header->sequence;
                if (sequenceBefore != sequenceAfter || sequenceBefore == lastSequence)
                    return false;

                *sequenceOut = sequenceBefore;
                *widthOut = width;
                *heightOut = height;
                *separatedOut = separated;
                *worldCandidateOut = worldCandidate;
                *uiActiveOut = uiActive;
                *readyOut = width > 0 && height > 0
                    && width <= static_cast<std::int32_t>(SharedVideoMaxWidth)
                    && height <= static_cast<std::int32_t>(SharedVideoMaxHeight)
                    && separated != 0
                    && worldCandidate != 0
                    && uiActive == 0;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
#endif

        class LiveFrameUserData : public osg::Referenced
        {
        };

        osg::ref_ptr<osg::Texture2D> createLiveFrameTexture(osg::Image* image)
        {
            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
            texture->setDataVariance(osg::Object::DYNAMIC);
            texture->setName("diffuseMap");
            texture->setUnRefImageDataAfterApply(false);
            texture->setResizeNonPowerOfTwoHint(false);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setImage(image);
            return texture;
        }

        class StereoTextureStateSetUpdater : public SceneUtil::StateSetUpdater
        {
        public:
            StereoTextureStateSetUpdater(osg::Texture2D* monoTexture, osg::Texture2D* leftTexture,
                osg::Texture2D* rightTexture, const bool* stereoActive)
                : mMonoTexture(monoTexture)
                , mLeftTexture(leftTexture)
                , mRightTexture(rightTexture)
                , mStereoActive(stereoActive)
            {
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                if (active())
                    applyStereoTextures(stateset);
                else
                    applyTexture(stateset, 0, mMonoTexture.get());
            }

            void applyLeft(osg::StateSet* stateset, osgUtil::CullVisitor*) override
            {
                if (active())
                    applyTexture(stateset, 0, mLeftTexture.get());
            }

            void applyRight(osg::StateSet* stateset, osgUtil::CullVisitor*) override
            {
                if (active())
                    applyTexture(stateset, 0, mRightTexture.get());
            }

        private:
            bool active() const
            {
                return mStereoActive && *mStereoActive && mLeftTexture.valid() && mRightTexture.valid();
            }

            void applyStereoTextures(osg::StateSet* stateset)
            {
                applyTexture(stateset, 0, mLeftTexture.get());
                applyTexture(stateset, 1, mRightTexture.get());
            }

            static void applyTexture(osg::StateSet* stateset, unsigned unit, osg::Texture2D* texture)
            {
                if (!stateset || !texture)
                    return;
                stateset->setTextureAttributeAndModes(unit, texture, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }

            osg::ref_ptr<osg::Texture2D> mMonoTexture;
            osg::ref_ptr<osg::Texture2D> mLeftTexture;
            osg::ref_ptr<osg::Texture2D> mRightTexture;
            const bool* mStereoActive = nullptr;
        };

        osg::ref_ptr<osg::Geometry> makeDebugPanelGeometry()
        {
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;

            const auto addQuad = [&](float left, float right, float bottom, float top, const osg::Vec4& color) {
                const unsigned int start = vertices->size();
                vertices->push_back(osg::Vec3(left, 0.995f, bottom));
                vertices->push_back(osg::Vec3(right, 0.995f, bottom));
                vertices->push_back(osg::Vec3(right, 0.995f, top));
                vertices->push_back(osg::Vec3(left, 0.995f, top));
                for (int i = 0; i < 4; ++i)
                    colors->push_back(color);
                osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
                indices->push_back(start);
                indices->push_back(start + 1);
                indices->push_back(start + 2);
                indices->push_back(start);
                indices->push_back(start + 2);
                indices->push_back(start + 3);
                return indices;
            };

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setName("FNVXRLiveFrameSurfaceDebugFrame");

            const osg::Vec4 magenta(1.f, 0.f, 0.85f, 0.85f);
            const osg::Vec4 cyan(0.f, 0.95f, 1.f, 0.85f);
            const osg::Vec4 amber(1.f, 0.75f, 0.f, 0.85f);
            geometry->addPrimitiveSet(addQuad(-0.58f, 0.58f, -0.58f, -0.53f, magenta));
            geometry->addPrimitiveSet(addQuad(-0.58f, 0.58f, 0.53f, 0.58f, cyan));
            geometry->addPrimitiveSet(addQuad(-0.58f, -0.53f, -0.58f, 0.58f, cyan));
            geometry->addPrimitiveSet(addQuad(0.53f, 0.58f, -0.58f, 0.58f, magenta));
            geometry->addPrimitiveSet(addQuad(-0.04f, 0.04f, -0.58f, 0.58f, amber));
            geometry->addPrimitiveSet(addQuad(-0.58f, 0.58f, -0.04f, 0.04f, amber));

            geometry->setVertexArray(vertices);
            geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
            geometry->setDataVariance(osg::Object::DYNAMIC);
            geometry->setSupportsDisplayList(false);
            return geometry;
        }
    }

    FNVXRLiveFrameSurface& FNVXRLiveFrameSurface::instance()
    {
        static FNVXRLiveFrameSurface sInstance;
        return sInstance;
    }

    FNVXRLiveFrameSurface::~FNVXRLiveFrameSurface()
    {
#ifdef _WIN32
        if (mVideoView)
            UnmapViewOfFile(mVideoView);
        if (mVideoMapping)
            CloseHandle(static_cast<HANDLE>(mVideoMapping));
        if (mDInputView)
            UnmapViewOfFile(mDInputView);
        if (mDInputMapping)
            CloseHandle(static_cast<HANDLE>(mDInputMapping));
        if (mCameraView)
            UnmapViewOfFile(mCameraView);
        if (mCameraMapping)
            CloseHandle(static_cast<HANDLE>(mCameraMapping));
        if (mPlayerView)
            UnmapViewOfFile(mPlayerView);
        if (mPlayerMapping)
            CloseHandle(static_cast<HANDLE>(mPlayerMapping));
        if (mRuntimeView)
            UnmapViewOfFile(mRuntimeView);
        if (mRuntimeMapping)
            CloseHandle(static_cast<HANDLE>(mRuntimeMapping));
        if (mStereoView)
            UnmapViewOfFile(mStereoView);
        if (mStereoMapping)
            CloseHandle(static_cast<HANDLE>(mStereoMapping));
        if (mVrPoseView)
            UnmapViewOfFile(mVrPoseView);
        if (mVrPoseMapping)
            CloseHandle(static_cast<HANDLE>(mVrPoseMapping));
        if (mOpenMwPlayerView)
            UnmapViewOfFile(mOpenMwPlayerView);
        if (mOpenMwPlayerMapping)
            CloseHandle(static_cast<HANDLE>(mOpenMwPlayerMapping));
#endif
    }

    bool FNVXRLiveFrameSurface::enabled() const
    {
        return envEnabled("OPENMW_FNVXR_RETAIL_SURFACE", true);
    }

    void FNVXRLiveFrameSurface::init(osg::Group* geometryRoot)
    {
        mGeometryRoot = geometryRoot;
        ensureSceneObjects();
        mVisible = false;
        mFocused = false;
        clearPointer();
        if (mTransform)
            mTransform->setNodeMask(0);
        if (mGeometryRoot && mTransform && !mGeometryRoot->containsNode(mTransform))
            mGeometryRoot->addChild(mTransform);
    }

    void FNVXRLiveFrameSurface::update(osg::NodeVisitor*)
    {
        if (!enabled())
        {
            mUseStereoTextures = false;
            mStereoFrameFresh = false;
            mStereoFrameReady = false;
            mCapturedStereoViewsValid = false;
            requestProjectionLayer(false);
            setVisible(false);
            return;
        }

        publishOpenMwPlayerState();

        const bool retailGameplay = retailRuntimeWorldReady();
        const bool panelAllowed = retailPanelAllowed();
        const bool projectionTakeoverWasActive = !mGripMenuOverride
            && (mProjectionLayerRequested || mProjectionLayerInserted || mRetailProjectionTakeoverActive || mRetailWorldMode);
        if (!retailGameplay && projectionTakeoverWasActive)
        {
            Log(Debug::Warning) << "FNVXR retail surface: retail world proof dropped; tearing down to normal OpenMW world";
            mUseStereoTextures = false;
            mRetailWorldMode = false;
            mStereoFrameFresh = false;
            mStereoFrameReady = false;
            mCapturedStereoViewsValid = false;
            requestProjectionLayer(false);
            setVisible(false);
            clearPointer();
            return;
        }
        if (!retailGameplay)
        {
            mUseStereoTextures = false;
            mCapturedStereoViewsValid = false;
            requestProjectionLayer(false);
        }
        if (!panelAllowed && !retailGameplay)
        {
            setVisible(false);
            clearPointer();
            return;
        }

        const bool haveMonoFrame = readFrame();
        if (haveMonoFrame)
        {
            ensureSceneObjects();
            updateTexture();
        }

        const bool sampledStereoReady = retailGameplay && retailStereoWorldReady();
        const float projectionStaleMs = std::max(50.f, envFloat("OPENMW_FNVXR_RETAIL_PROJECTION_STALE_MS", 5000.f));
        const bool stereoStreamFresh = retailGameplay
            && mStereoFrameReady
            && mLastStereoFreshTime.time_since_epoch().count() != 0
            && std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - mLastStereoFreshTime).count()
                <= projectionStaleMs;
        mUseStereoTextures = stereoStreamFresh;
        if (stereoStreamFresh)
        {
            mLoggedStereoStale = false;
            updateStereoTextures();
        }
        else if (retailGameplay && sampledStereoReady && !mLoggedStereoStale)
        {
            mLoggedStereoStale = true;
            const float staleMs = mLastStereoFreshTime.time_since_epoch().count() == 0
                ? -1.f
                : std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - mLastStereoFreshTime).count();
            Log(Debug::Warning) << "FNVXR retail surface: stereo world frame is valid but stale; refusing projection flip ageMs="
                                << staleMs
                                << " sequence=" << mCurrentStereoSequence;
        }
        requestProjectionLayer(stereoStreamFresh && projectionLayerEnabled());

        const bool projectionPathActive = projectionLayerEnabled() && !mGripMenuOverride
            && (mProjectionLayerRequested || mProjectionLayerInserted || mRetailProjectionTakeoverActive);
        if (projectionPathActive && !envEnabled("OPENMW_FNVXR_KEEP_RETAIL_PANEL_IN_WORLD", false))
        {
            if (mVisible)
                Log(Debug::Info) << "FNVXR retail surface: projection path active; hiding proof surface";
            setVisible(false);
            clearPointer();
            return;
        }

        if (stereoStreamFresh && !envEnabled("OPENMW_FNVXR_KEEP_RETAIL_PANEL_IN_WORLD", false))
        {
            const bool showWorldPanelFallback = !projectionLayerEnabled()
                && envEnabled("OPENMW_FNVXR_SHOW_RETAIL_WORLD_PANEL", false);
            if (showWorldPanelFallback)
            {
                ensureSceneObjects();
                if (!mRetailWorldMode)
                    Log(Debug::Info) << "FNVXR retail surface: stereo world ready; presenting per-eye retail world surface";
                updatePlacement(true);
                setVisible(true);
            }
            else
            {
                if (!mRetailWorldMode)
                    Log(Debug::Info) << "FNVXR retail surface: stereo world ready; hiding panel fallback and waiting for projection takeover";
                mRetailWorldMode = true;
                setVisible(false);
            }
            clearPointer();
            return;
        }

        if (retailGameplay
            && !envEnabled("OPENMW_FNVXR_KEEP_RETAIL_PANEL_IN_WORLD", false)
            && !envEnabled("OPENMW_FNVXR_SHOW_RETAIL_WORLD_PANEL", false))
        {
            if (mVisible)
                Log(Debug::Info) << "FNVXR retail surface: retail gameplay active; hiding in-world surface geometry";
            setVisible(false);
            clearPointer();
            return;
        }

        if (!haveMonoFrame)
            return;

        const bool retailWorldMode = stereoStreamFresh
            && envEnabled("OPENMW_FNVXR_GAMEPLAY_FORCES_WORLD_VIEW", true);
        if (retailGameplay && !sampledStereoReady && !mStereoFrameReady)
        {
            if (!mLoggedStereoWaiting)
            {
                mLoggedStereoWaiting = true;
                Log(Debug::Info) << "FNVXR retail surface: runtime gameplay ready; keeping mono portal until stereo world frame is ready";
            }
        }

        updatePlacement(retailWorldMode);
        setVisible(true);
        updateAimPointer();
    }

    bool FNVXRLiveFrameSurface::projectionLayerEnabled() const
    {
        return envEnabled("OPENMW_FNVXR_USE_RETAIL_PROJECTION_LAYER", true);
    }

    void FNVXRLiveFrameSurface::requestProjectionLayer(bool active)
    {
        if (mProjectionLayerRequested == active)
            return;

        mProjectionLayerRequested = active;
        if (!active)
        {
            if (mProjectionLayerInserted && mProjectionLayer)
            {
                VR::Viewer::instance().removeLayer(mProjectionLayer);
                mProjectionLayerInserted = false;
            }
            VR::Viewer::instance().setPrimaryProjectionLayerEnabled(true);
            mProjectionLayerReady = false;
            mRetailProjectionTakeoverActive = false;
            return;
        }

        if (!mLoggedProjectionLayer)
        {
            mLoggedProjectionLayer = true;
            Log(Debug::Info) << "FNVXR retail surface: retail stereo projection layer requested";
        }
    }

    bool FNVXRLiveFrameSurface::ensureProjectionLayer()
    {
        if (!mProjectionLayerRequested || mStereoWidth == 0 || mStereoHeight == 0)
            return false;

        if (mProjectionLayer && mProjectionWidth == mStereoWidth && mProjectionHeight == mStereoHeight)
            return true;

        if (mProjectionLayerInserted && mProjectionLayer)
        {
            VR::Viewer::instance().removeLayer(mProjectionLayer);
            mProjectionLayerInserted = false;
        }

        mProjectionLayer = std::make_shared<VR::ProjectionLayer>();
        mProjectionWidth = mStereoWidth;
        mProjectionHeight = mStereoHeight;

        for (std::uint32_t i = 0; i < 2; ++i)
        {
            mProjectionColorSwapchains[i] = VR::Session::instance().createSwapchain(mProjectionWidth, mProjectionHeight,
                1, 1, VR::Swapchain::Attachment::Color, i == 0 ? "FNVXR Retail Left" : "FNVXR Retail Right");
            mProjectionLayer->views[i].colorSwapchain = mProjectionColorSwapchains[i];
            mProjectionLayer->views[i].subImage.index = 0;
            mProjectionLayer->views[i].subImage.x = 0;
            mProjectionLayer->views[i].subImage.y = 0;
            mProjectionLayer->views[i].subImage.width = mProjectionWidth;
            mProjectionLayer->views[i].subImage.height = mProjectionHeight;
        }

        mProjectionLayerReady = false;
        mLoggedProjectionLayerUpload = false;
        Log(Debug::Info) << "FNVXR retail surface: created retail projection swapchains "
                         << mProjectionWidth << "x" << mProjectionHeight;
        return true;
    }

    void FNVXRLiveFrameSurface::onSpaceUpdate()
    {
        if (!enabled())
            return;

        if (mLastPredictedDisplayTime != 0)
            publishVrPose(mLastPredictedDisplayTime);
    }

    void FNVXRLiveFrameSurface::onFrameUpdate(VR::Frame& frame)
    {
        if (frame.shouldRender && frame.predictedDisplayTime != 0)
            mLastPredictedDisplayTime = frame.predictedDisplayTime;

        if (!mProjectionLayerRequested || !mProjectionLayerReady || !mProjectionLayer || !frame.shouldRender)
            return;
        if (!mCapturedStereoViewsValid)
            return;

        try
        {
            auto localSpace = VR::Session::instance().getReferenceSpace(VR::ReferenceSpace::Local);
            mProjectionLayer->space = localSpace;
            for (std::uint32_t i = 0; i < 2; ++i)
                mProjectionLayer->views[i].view = mCapturedStereoViews[i];

            if (!mProjectionLayerInserted)
            {
                VR::Viewer::instance().insertLayer(mProjectionLayer);
                mProjectionLayerInserted = true;
                mRetailProjectionTakeoverActive = true;
                VR::Viewer::instance().setPrimaryProjectionLayerEnabled(false);
                Log(Debug::Info) << "FNVXR retail surface: inserted retail projection layer";
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNVXR retail surface: projection layer update failed: " << e.what();
        }
    }

    void FNVXRLiveFrameSurface::onFrameEnd(osg::RenderInfo& info, VR::Frame&)
    {
        if (!mProjectionLayerRequested || !mStereoFrameReady || !mStereoFrameFresh)
            return;

        uploadProjectionLayer(info);
    }

    void FNVXRLiveFrameSurface::uploadProjectionLayer(osg::RenderInfo& info)
    {
        if (!ensureProjectionLayer())
            return;

        auto* state = info.getState();
        if (!state)
        {
            markProjectionLayerUploadFailed("missing OSG state");
            return;
        }
        auto* gc = state->getGraphicsContext();
        if (!gc)
        {
            markProjectionLayerUploadFailed("missing graphics context");
            return;
        }

        const bool havePixels = !mStereoLeftPixels.empty() && !mStereoRightPixels.empty();
        if (!havePixels)
        {
            markProjectionLayerUploadFailed("missing stereo pixels");
            return;
        }

        const std::uint8_t* stereoPixels[2] = { mStereoLeftPixels.data(), mStereoRightPixels.data() };
        const bool flipY = envEnabled("OPENMW_FNVXR_RETAIL_PROJECTION_FLIP_Y", true);
        const std::size_t pitchBytes = static_cast<std::size_t>(mProjectionWidth) * 4;
        const std::size_t imageBytes = pitchBytes * static_cast<std::size_t>(mProjectionHeight);
        for (std::uint32_t eye = 0; eye < 2; ++eye)
        {
            if (mProjectionUploadPixels[eye].size() != imageBytes)
                mProjectionUploadPixels[eye].resize(imageBytes);
            for (std::uint32_t y = 0; y < mProjectionHeight; ++y)
            {
                const auto srcY = flipY ? (mProjectionHeight - 1 - y) : y;
                std::memcpy(mProjectionUploadPixels[eye].data() + static_cast<std::size_t>(y) * pitchBytes,
                    stereoPixels[eye] + static_cast<std::size_t>(srcY) * pitchBytes, pitchBytes);
            }
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        for (std::uint32_t i = 0; i < 2; ++i)
        {
            auto& swapchain = mProjectionColorSwapchains[i];
            if (!swapchain)
            {
                markProjectionLayerUploadFailed("missing projection swapchain");
                return;
            }

            swapchain->beginFrame(gc);
            if (!swapchain->image())
            {
                swapchain->endFrame(gc);
                markProjectionLayerUploadFailed("projection swapchain image unavailable");
                return;
            }
            const GLuint texture = swapchain->image()->glImage();
            const GLenum target = static_cast<GLenum>(swapchain->textureTarget());
            glBindTexture(target, texture);
            glTexSubImage2D(target, 0, 0, 0, static_cast<GLsizei>(mProjectionWidth),
                static_cast<GLsizei>(mProjectionHeight), GL_BGRA, GL_UNSIGNED_BYTE, mProjectionUploadPixels[i].data());
            glBindTexture(target, 0);
            swapchain->endFrame(gc);
        }

        mProjectionLayerReady = true;
        mUploadedStereoSequence = mCurrentStereoSequence;
        mStereoFrameFresh = false;
        if (!mLoggedProjectionLayerUpload)
        {
            mLoggedProjectionLayerUpload = true;
            Log(Debug::Info) << "FNVXR retail surface: uploaded first retail stereo projection frame sequence="
                             << mUploadedStereoSequence;
        }
    }

    void FNVXRLiveFrameSurface::markProjectionLayerUploadFailed(const char* reason)
    {
        mProjectionLayerReady = false;
        if (mProjectionLayerInserted && mProjectionLayer)
        {
            VR::Viewer::instance().removeLayer(mProjectionLayer);
            mProjectionLayerInserted = false;
        }
        VR::Viewer::instance().setPrimaryProjectionLayerEnabled(true);
        mRetailProjectionTakeoverActive = false;
        Log(Debug::Warning) << "FNVXR retail surface: retail projection upload failed; keeping proof surface active: "
                            << reason;
    }

    bool FNVXRLiveFrameSurface::ensureRuntimeMapping()
    {
#ifdef _WIN32
        if (mRuntimeView)
            return true;

        mRuntimeMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FNVXR_Runtime_State");
        if (!mRuntimeMapping)
            return false;

        mRuntimeView = MapViewOfFile(static_cast<HANDLE>(mRuntimeMapping), FILE_MAP_READ, 0, 0, sizeof(SharedRuntimeState));
        if (!mRuntimeView)
        {
            CloseHandle(static_cast<HANDLE>(mRuntimeMapping));
            mRuntimeMapping = nullptr;
            return false;
        }

        if (!mLoggedRuntime)
        {
            mLoggedRuntime = true;
            Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_Runtime_State";
        }
        return true;
#else
        return false;
#endif
    }

    std::uint32_t FNVXRLiveFrameSurface::retailRuntimePhase()
    {
#ifdef _WIN32
        if (!ensureRuntimeMapping())
            return 0;

        std::uint32_t phase = 0;
        bool torn = false;
        if (!tryReadRuntimePhase(mRuntimeView, &phase, &torn))
            return 0;
        return torn ? 0 : phase;
#else
        return 0;
#endif
    }

    bool FNVXRLiveFrameSurface::retailRuntimeWorldReady()
    {
#ifdef _WIN32
        if (!ensureRuntimeMapping())
        {
            mRetailWorldReadyFrames = 0;
            return false;
        }

        bool ready = false;
        bool torn = false;
        if (!tryReadRuntimeWorldReady(mRuntimeView, &ready, &torn) || torn)
        {
            mRetailWorldReadyFrames = 0;
            return false;
        }

        if (!ready)
        {
            const float latchMs = envEnabled("OPENMW_FNVXR_ALLOW_RETAIL_WORLD_LATCH", false)
                ? std::max(0.f, envFloat("OPENMW_FNVXR_RETAIL_WORLD_LATCH_MS", 0.f))
                : 0.f;
            if (mRetailWorldReadyFrames > 0 && mStereoFrameReady && latchMs > 0.f
                && std::chrono::steady_clock::now() < mRetailWorldLatchUntil)
            {
                return true;
            }
            mRetailWorldReadyFrames = 0;
            return false;
        }

        const int requiredFrames = static_cast<int>(std::max(1.f,
            envFloat("OPENMW_FNVXR_RETAIL_WORLD_READY_DEBOUNCE_FRAMES", 6.f)));
        mRetailWorldReadyFrames = std::min(mRetailWorldReadyFrames + 1, requiredFrames);
        const bool debounced = mRetailWorldReadyFrames >= requiredFrames;
        if (debounced)
        {
            const float latchMs = envEnabled("OPENMW_FNVXR_ALLOW_RETAIL_WORLD_LATCH", false)
                ? std::max(0.f, envFloat("OPENMW_FNVXR_RETAIL_WORLD_LATCH_MS", 0.f))
                : 0.f;
            mRetailWorldLatchUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(latchMs));
        }
        return debounced;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::ensureStereoMapping()
    {
#ifdef _WIN32
        if (mStereoView)
            return true;

        mStereoMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FNVXR_D3D9_StereoFrame_v1");
        if (!mStereoMapping)
            return false;

        mStereoView = static_cast<std::uint8_t*>(
            MapViewOfFile(static_cast<HANDLE>(mStereoMapping), FILE_MAP_READ, 0, 0, SharedStereoMappingBytes));
        if (!mStereoView)
        {
            CloseHandle(static_cast<HANDLE>(mStereoMapping));
            mStereoMapping = nullptr;
            return false;
        }

        if (!mLoggedStereo)
        {
            mLoggedStereo = true;
            Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_D3D9_StereoFrame_v1";
        }
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::retailStereoWorldReady()
    {
#ifdef _WIN32
        mStereoFrameFresh = false;
        if (!ensureStereoMapping())
        {
            mStereoFrameReady = false;
            mCapturedStereoViewsValid = false;
            return false;
        }

        const auto* header = reinterpret_cast<const SharedStereoHeader*>(mStereoView);
        if (header->magic != SharedStereoMagic || header->writing)
        {
            return false;
        }

        const std::int32_t sequence = header->sequence;
        const std::int32_t width = header->width;
        const std::int32_t height = header->height;
        const std::int32_t pitchBytes = header->pitchBytes;
        const std::int32_t separated = header->separated;
        const std::int32_t worldCandidate = header->worldCandidate;
        const std::int32_t uiActive = header->uiActive;
        const std::int32_t poseValid = header->poseValid;

        if (sequence != 0 && sequence == mLastStereoSequence)
            return mStereoFrameReady;

        const bool ready = width > 0 && height > 0
            && width <= static_cast<std::int32_t>(SharedVideoMaxWidth)
            && height <= static_cast<std::int32_t>(SharedVideoMaxHeight)
            && pitchBytes >= width * 4
            && pitchBytes <= static_cast<std::int32_t>(SharedVideoMaxWidth * 4)
            && separated != 0
            && worldCandidate != 0
            && uiActive == 0
            && poseValid != 0
            && poseArrayLooksUsable(header->leftEyeRot, header->leftEyePos)
            && poseArrayLooksUsable(header->rightEyeRot, header->rightEyePos)
            && fovArrayLooksUsable(header->leftFov)
            && fovArrayLooksUsable(header->rightFov);
        mLastStereoSequence = sequence;
        if (!ready)
        {
            mStereoFrameReady = false;
            mCapturedStereoViewsValid = false;
            return false;
        }

        std::vector<std::uint8_t> leftPixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        std::vector<std::uint8_t> rightPixels(leftPixels.size());
        const auto* leftSrc = mStereoView + sizeof(SharedStereoHeader);
        const auto* rightSrc = leftSrc + static_cast<std::size_t>(pitchBytes) * static_cast<std::size_t>(height);
        for (std::int32_t y = 0; y < height; ++y)
        {
            auto* leftDstRow = leftPixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            auto* rightDstRow = rightPixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            std::memcpy(leftDstRow, leftSrc + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitchBytes),
                static_cast<std::size_t>(width) * 4);
            std::memcpy(rightDstRow, rightSrc + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitchBytes),
                static_cast<std::size_t>(width) * 4);
            for (std::int32_t x = 0; x < width; ++x)
            {
                leftDstRow[static_cast<std::size_t>(x) * 4 + 3] = 0xff;
                rightDstRow[static_cast<std::size_t>(x) * 4 + 3] = 0xff;
            }
        }

        if (header->writing || header->sequence != sequence)
        {
            return false;
        }

        mStereoLeftPixels.swap(leftPixels);
        mStereoRightPixels.swap(rightPixels);
        mStereoWidth = static_cast<std::uint32_t>(width);
        mStereoHeight = static_cast<std::uint32_t>(height);
        mCurrentStereoSequence = sequence;
        mCapturedStereoViews[0] = readStereoView(header->leftEyeRot, header->leftEyePos, header->leftFov);
        mCapturedStereoViews[1] = readStereoView(header->rightEyeRot, header->rightEyePos, header->rightFov);
        mCapturedStereoViewsValid = true;
        mLastStereoFreshTime = std::chrono::steady_clock::now();
        mStereoFrameFresh = true;
        mStereoFrameReady = true;

        if (!mLoggedStereoReady)
        {
            mLoggedStereoReady = true;
            Log(Debug::Info) << "FNVXR retail surface: stereo world ready sequence=" << sequence
                             << " size=" << width << "x" << height
                             << " separated=" << separated
                             << " worldCandidate=" << worldCandidate
                             << " uiActive=" << uiActive
                             << " poseSequence=" << header->poseSequence;
        }
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::ensureVrPoseMapping()
    {
#ifdef _WIN32
        if (mVrPoseView)
            return true;

        mVrPoseMapping = OpenFileMappingA(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, "Local\\FNVXR_VR_Pose_State");
        if (!mVrPoseMapping)
            return false;

        mVrPoseView = MapViewOfFile(
            static_cast<HANDLE>(mVrPoseMapping), FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedVrPoseState));
        if (!mVrPoseView)
        {
            CloseHandle(static_cast<HANDLE>(mVrPoseMapping));
            mVrPoseMapping = nullptr;
            return false;
        }

        Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_VR_Pose_State for OpenMW pose publishing";
        return true;
#else
        return false;
#endif
    }

    void FNVXRLiveFrameSurface::publishVrPose(std::uint64_t predictedDisplayTime)
    {
#ifdef _WIN32
        try
        {
            if (predictedDisplayTime == 0)
                return;

            if (!ensureVrPoseMapping())
                return;

            auto localRef = VR::Session::instance().getReferenceSpace(VR::ReferenceSpace::Local);
            auto viewSpace = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView);
            if (!localRef || !viewSpace)
                return;

            auto hmd = viewSpace->locate(*localRef);
            if (!static_cast<bool>(hmd.status))
                return;
            auto eyeViews = VR::Session::instance().locateViews(
                static_cast<std::int64_t>(predictedDisplayTime), *localRef);

            auto* shared = static_cast<SharedVrPoseState*>(mVrPoseView);
            if (shared->magic != VrPoseSharedMagic || shared->version != VrPoseSharedVersion)
                return;

            Stereo::Pose leftPose = hmd.pose;
            Stereo::Pose rightPose = hmd.pose;
            if (auto leftSpace = OpenXRInput::instance().getSpace(OpenXRInput::LeftHandAim))
            {
                auto tracked = leftSpace->locate(*localRef);
                if (static_cast<bool>(tracked.status))
                    leftPose = tracked.pose;
            }
            if (auto rightSpace = OpenXRInput::instance().getSpace(OpenXRInput::RightHandAim))
            {
                auto tracked = rightSpace->locate(*localRef);
                if (static_cast<bool>(tracked.status))
                    rightPose = tracked.pose;
            }

            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->sequence));
            MemoryBarrier();
            shared->frame = ++mVrPoseFrame;
            writePose(shared->hmdRot, shared->hmdPos, hmd.pose);
            writePose(shared->leftRot, shared->leftPos, leftPose);
            writePose(shared->rightRot, shared->rightPos, rightPose);
            writePose(shared->leftEyeRot, shared->leftEyePos, eyeViews[0].pose);
            writePose(shared->rightEyeRot, shared->rightEyePos, eyeViews[1].pose);
            writeFov(shared->leftFov, eyeViews[0].fov);
            writeFov(shared->rightFov, eyeViews[1].fov);
            MemoryBarrier();
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->sequence));
        }
        catch (const std::exception& e)
        {
            static bool loggedPoseFailure = false;
            if (!loggedPoseFailure)
            {
                loggedPoseFailure = true;
                Log(Debug::Warning) << "FNVXR retail surface: VR pose publish skipped after OpenXR locate failure: "
                                    << e.what();
            }
        }
#endif
    }

    bool FNVXRLiveFrameSurface::ensureOpenMwPlayerMapping()
    {
#ifdef _WIN32
        if (mOpenMwPlayerView)
            return true;

        mOpenMwPlayerMapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedPlayerState), "Local\\FNVXR_OpenMW_Player_State");
        if (!mOpenMwPlayerMapping)
            return false;

        mOpenMwPlayerView = MapViewOfFile(
            static_cast<HANDLE>(mOpenMwPlayerMapping), FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedPlayerState));
        if (!mOpenMwPlayerView)
        {
            CloseHandle(static_cast<HANDLE>(mOpenMwPlayerMapping));
            mOpenMwPlayerMapping = nullptr;
            return false;
        }

        auto* shared = static_cast<SharedPlayerState*>(mOpenMwPlayerView);
        std::memset(shared, 0, sizeof(SharedPlayerState));
        shared->magic = OpenMwPlayerSharedMagic;
        shared->version = OpenMwPlayerSharedVersion;
        Log(Debug::Info) << "FNVXR retail surface: publishing Local\\FNVXR_OpenMW_Player_State";
        return true;
#else
        return false;
#endif
    }

    void FNVXRLiveFrameSurface::publishOpenMwPlayerState()
    {
#ifdef _WIN32
        if (!envEnabled("OPENMW_FNVXR_PUBLISH_PLAYER_STATE", true))
            return;

        try
        {
            if (!ensureOpenMwPlayerMapping())
                return;

            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (!world)
                return;

            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty() || !player.isInCell())
                return;

            const ESM::Position& position = player.getRefData().getPosition();
            if (!finiteArray(position.pos, 3))
                return;

            const std::uint32_t currentCellFormId = cellFormId(player.getCell());
            float playerWorldRot[9] {};
            writeIdentityMatrix(playerWorldRot);
            const SceneUtil::PositionAttitudeTransform* baseNode = player.getRefData().getBaseNode();
            const bool playerNodeValid = baseNode != nullptr && writeRotationMatrix(playerWorldRot, baseNode->getAttitude());
            std::uint32_t flags = playerNodeValid ? PlayerSharedFlagPlayerNodeValid : 0;
            if (currentCellFormId != 0)
                flags |= PlayerSharedFlagCellKnown;

            auto* shared = static_cast<SharedPlayerState*>(mOpenMwPlayerView);
            if (shared->magic != OpenMwPlayerSharedMagic || shared->version != OpenMwPlayerSharedVersion)
            {
                shared->magic = OpenMwPlayerSharedMagic;
                shared->version = OpenMwPlayerSharedVersion;
            }

            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->sequence));
            MemoryBarrier();
            shared->magic = OpenMwPlayerSharedMagic;
            shared->version = OpenMwPlayerSharedVersion;
            shared->frame = ++mOpenMwPlayerFrame;
            shared->flags = flags;
            shared->currentCellFormId = currentCellFormId;
            shared->playerAddress = 0;
            shared->playerNodeAddress = 0;
            shared->cameraNodeAddress = 0;
            std::memcpy(shared->playerWorldRot, playerWorldRot, sizeof(shared->playerWorldRot));
            shared->playerWorldPos[0] = position.pos[0];
            shared->playerWorldPos[1] = position.pos[1];
            shared->playerWorldPos[2] = position.pos[2];
            writeIdentityMatrix(shared->cameraWorldRot);
            shared->cameraWorldPos[0] = 0.f;
            shared->cameraWorldPos[1] = 0.f;
            shared->cameraWorldPos[2] = 0.f;
            std::memset(shared->reserved, 0, sizeof(shared->reserved));
            MemoryBarrier();
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->sequence));

            if (!mLoggedOpenMwPlayer || (mOpenMwPlayerFrame % 300) == 0)
            {
                mLoggedOpenMwPlayer = true;
                Log(Debug::Info) << "FNVXR retail surface: OpenMW player state frame=" << mOpenMwPlayerFrame
                                 << " playerNodeValid=" << playerNodeValid
                                 << " cellKnown=" << ((flags & PlayerSharedFlagCellKnown) != 0)
                                 << " cellFormId=" << currentCellFormId
                                 << " pos=(" << position.pos[0] << ", " << position.pos[1] << ", "
                                 << position.pos[2] << ")";
            }
        }
        catch (const std::exception& e)
        {
            if (!mLoggedOpenMwPlayerPublishFailure)
            {
                mLoggedOpenMwPlayerPublishFailure = true;
                Log(Debug::Warning) << "FNVXR retail surface: OpenMW player state publish failed: " << e.what();
            }
        }
#endif
    }

    bool FNVXRLiveFrameSurface::ensureVideoMapping()
    {
#ifdef _WIN32
        if (mVideoView)
            return true;

        mVideoMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FNVXR_D3D9_Frame_v1");
        if (!mVideoMapping)
            return false;

        mVideoView = static_cast<std::uint8_t*>(
            MapViewOfFile(static_cast<HANDLE>(mVideoMapping), FILE_MAP_READ, 0, 0, SharedVideoMappingBytes));
        if (!mVideoView)
        {
            CloseHandle(static_cast<HANDLE>(mVideoMapping));
            mVideoMapping = nullptr;
            return false;
        }

        Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_D3D9_Frame_v1";
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::readFrame()
    {
#ifdef _WIN32
        if (!ensureVideoMapping())
            return false;

        const auto* header = reinterpret_cast<const SharedVideoHeader*>(mVideoView);
        if (header->magic != SharedVideoMagic || header->writing)
            return false;

        const std::int32_t sequence = header->sequence;
        if (sequence == 0 || sequence == mLastSequence)
            return false;

        const std::int32_t width = header->width;
        const std::int32_t height = header->height;
        const std::int32_t pitchBytes = header->pitchBytes;
        if (width <= 0 || height <= 0 || width > static_cast<std::int32_t>(SharedVideoMaxWidth)
            || height > static_cast<std::int32_t>(SharedVideoMaxHeight) || pitchBytes < width * 4
            || pitchBytes > static_cast<std::int32_t>(SharedVideoMaxWidth * 4))
        {
            return false;
        }

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        const auto* src = mVideoView + SharedVideoHeaderBytes;
        for (std::int32_t y = 0; y < height; ++y)
        {
            auto* dstRow = pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
            std::memcpy(dstRow, src + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitchBytes),
                static_cast<std::size_t>(width) * 4);
            for (std::int32_t x = 0; x < width; ++x)
                dstRow[static_cast<std::size_t>(x) * 4 + 3] = 0xff;
        }

        if (header->writing || header->sequence != sequence)
            return false;

        mPixels.swap(pixels);
        mWidth = static_cast<std::uint32_t>(width);
        mHeight = static_cast<std::uint32_t>(height);
        mLastSequence = sequence;

        if (!mLoggedFrame)
        {
            mLoggedFrame = true;
            Log(Debug::Info) << "FNVXR retail surface: first frame " << mWidth << "x" << mHeight
                             << " sequence=" << mLastSequence;
        }
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::retailPanelAllowed()
    {
        auto windowManager = MWBase::Environment::get().getWindowManager();

        const bool blockedByStartup = windowManager->containsMode(MWGui::GM_MainMenu)
            || windowManager->containsMode(MWGui::GM_Loading)
            || windowManager->containsMode(MWGui::GM_LoadingWallpaper);
        const bool inGameWorld = !blockedByStartup && !windowManager->isGuiMode()
            && windowManager->getMode() == MWGui::GM_None;

        if (!inGameWorld)
        {
            if (mWorldReadyTimerRunning || mRetailPanelArmed)
                Log(Debug::Info) << "FNVXR retail surface: waiting for settled in-world OpenMW scene";
            mWorldReadyTimerRunning = false;
            if (mGripMenuOverride && !blockedByStartup)
            {
                mRetailPanelArmed = true;
                return true;
            }
            if (!mGripMenuOverride)
            {
                mRetailPanelArmed = false;
                mHaveAnchorPose = false;
            }
            return false;
        }

        if (mGripMenuOverride)
            return true;

        if (envEnabled("OPENMW_FNVXR_RETAIL_PANEL_REQUIRES_GRIP", true))
            return false;

        const auto now = std::chrono::steady_clock::now();
        if (!mWorldReadyTimerRunning)
        {
            mWorldReadySince = now;
            mWorldReadyTimerRunning = true;
            Log(Debug::Info) << "FNVXR retail surface: OpenMW scene ready, settling before retail panel";
            return false;
        }

        if (now - mWorldReadySince < std::chrono::seconds(2))
            return false;

        if (!mRetailPanelArmed)
        {
            mRetailPanelArmed = true;
            mHaveAnchorPose = false;
            Log(Debug::Info) << "FNVXR retail surface: retail panel armed after scene settle";
        }

        return true;
    }

    void FNVXRLiveFrameSurface::ensureSceneObjects()
    {
        if (mGeometry)
            return;

        mTransform = new osg::PositionAttitudeTransform;
        mTransform->setName("FNVXRLiveFrameSurfaceTransform");
        mTransform->setNodeMask(0);
        mTransform->setCullingActive(false);

        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array(4);
        (*vertices)[0] = osg::Vec3(-0.5f, 1.f, -0.5f);
        (*vertices)[1] = osg::Vec3(-0.5f, 1.f, 0.5f);
        (*vertices)[2] = osg::Vec3(0.5f, 1.f, -0.5f);
        (*vertices)[3] = osg::Vec3(0.5f, 1.f, 0.5f);

        osg::ref_ptr<osg::Vec2Array> texCoords = new osg::Vec2Array(4);
        (*texCoords)[0] = osg::Vec2(0.f, 1.f);
        (*texCoords)[1] = osg::Vec2(0.f, 0.f);
        (*texCoords)[2] = osg::Vec2(1.f, 1.f);
        (*texCoords)[3] = osg::Vec2(1.f, 0.f);

        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array(1);
        (*normals)[0] = osg::Vec3(0.f, -1.f, 0.f);

        mGeometry = new osg::Geometry;
        mGeometry->setName("FNVXRLiveFrameSurface");
        mGeometry->setVertexArray(vertices);
        mGeometry->setTexCoordArray(0, texCoords);
        mGeometry->setNormalArray(normals, osg::Array::BIND_OVERALL);
        mGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        mGeometry->setDataVariance(osg::Object::DYNAMIC);
        mGeometry->setSupportsDisplayList(false);
        mGeometry->setUserData(new LiveFrameUserData);

        mDebugGeometry = makeDebugPanelGeometry();
        mDebugStateSet = new osg::StateSet;
        mDebugStateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
        mDebugStateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        mDebugStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        mDebugStateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        mDebugStateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        mDebugStateSet->setRenderBinDetails(9999, "RenderBin");
        mDebugStateSet->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        mDebugGeometry->setStateSet(mDebugStateSet);

        mImage = new osg::Image;
        mImage->setDataVariance(osg::Object::DYNAMIC);
        mStereoLeftImage = new osg::Image;
        mStereoLeftImage->setDataVariance(osg::Object::DYNAMIC);
        mStereoRightImage = new osg::Image;
        mStereoRightImage->setDataVariance(osg::Object::DYNAMIC);
        mTexture = createLiveFrameTexture(mImage);
        mStereoLeftTexture = createLiveFrameTexture(mStereoLeftImage);
        mStereoRightTexture = createLiveFrameTexture(mStereoRightImage);

        mStateSet = new osg::StateSet;
        mStateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        mStateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        mStateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        mStateSet->setMode(GL_ALPHA_TEST, osg::StateAttribute::OFF);
        mStateSet->setRenderingHint(osg::StateSet::OPAQUE_BIN);
        mStateSet->setRenderBinDetails(1000, "RenderBin");
        mStateSet->setTextureAttributeAndModes(0, mTexture, osg::StateAttribute::ON);
        mStateSet->addUniform(new osg::Uniform("diffuseMapRight", 1));

        Shader::ShaderManager::DefineMap defineMap;
        Stereo::shaderStereoDefines(defineMap);
        auto& shaderManager = MWBase::Environment::get().getResourceSystem()->getSceneManager()->getShaderManager();
        osg::ref_ptr<osg::Shader> vertexShader
            = shaderManager.getShader("3dgui_vertex.glsl", defineMap, osg::Shader::VERTEX);
        osg::ref_ptr<osg::Shader> fragmentShader
            = shaderManager.getShader("3dgui_fragment.glsl", defineMap, osg::Shader::FRAGMENT);
        if (vertexShader && fragmentShader)
            mStateSet->setAttributeAndModes(shaderManager.getProgram(vertexShader, fragmentShader));

        osg::ref_ptr<osg::Material> material = new osg::Material;
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        mStateSet->setAttribute(material);
        mGeometry->setStateSet(mStateSet);

        if (debugFrameEnabled())
            mTransform->addChild(mDebugGeometry);
        mTransform->addChild(mGeometry);
        mTransform->setCullCallback(
            new StereoTextureStateSetUpdater(mTexture, mStereoLeftTexture, mStereoRightTexture, &mUseStereoTextures));
        if (mGeometryRoot && !mGeometryRoot->containsNode(mTransform))
            mGeometryRoot->addChild(mTransform);
        Log(Debug::Info) << "FNVXR retail surface: panel geometry attached debugFrame=" << debugFrameEnabled();
    }

    void FNVXRLiveFrameSurface::updateTexture()
    {
        if (!mImage || mPixels.empty() || !mWidth || !mHeight)
            return;

        if (mImage->s() != static_cast<int>(mWidth) || mImage->t() != static_cast<int>(mHeight))
            mImage->allocateImage(static_cast<int>(mWidth), static_cast<int>(mHeight), 1, GL_BGRA, GL_UNSIGNED_BYTE);

        std::memcpy(mImage->data(), mPixels.data(), mPixels.size());
        mImage->dirty();
    }

    void FNVXRLiveFrameSurface::updateStereoTextures()
    {
        if (!mStereoLeftImage || !mStereoRightImage || mStereoLeftPixels.empty() || mStereoRightPixels.empty()
            || !mStereoWidth || !mStereoHeight)
        {
            return;
        }

        const int width = static_cast<int>(mStereoWidth);
        const int height = static_cast<int>(mStereoHeight);
        if (mStereoLeftImage->s() != width || mStereoLeftImage->t() != height)
            mStereoLeftImage->allocateImage(width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE);
        if (mStereoRightImage->s() != width || mStereoRightImage->t() != height)
            mStereoRightImage->allocateImage(width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE);

        std::memcpy(mStereoLeftImage->data(), mStereoLeftPixels.data(), mStereoLeftPixels.size());
        std::memcpy(mStereoRightImage->data(), mStereoRightPixels.data(), mStereoRightPixels.size());
        mStereoLeftImage->dirty();
        mStereoRightImage->dirty();
    }

    void FNVXRLiveFrameSurface::updatePlacement(bool worldMode)
    {
        if (!mTransform || !mWidth || !mHeight)
            return;

        if (worldMode != mRetailWorldMode)
        {
            mRetailWorldMode = worldMode;
            mHaveAnchorPose = false;
            Log(Debug::Info) << "FNVXR retail surface: mode=" << (worldMode ? "retail-world-view" : "menu-panel");
        }

        const float heightMeters = worldMode
            ? envFloat("OPENMW_FNVXR_RETAIL_WORLD_HEIGHT_METERS", 3.6f)
            : envFloat("OPENMW_FNVXR_RETAIL_MENU_HEIGHT_METERS", 0.62f);
        const float aspect = static_cast<float>(mWidth) / static_cast<float>(mHeight);
        mTransform->setScale(
            osg::Vec3(aspect * heightMeters * Constants::UnitsPerMeter, 1.f, heightMeters * Constants::UnitsPerMeter));

        if (worldMode)
        {
            if (!mHaveAnchorPose)
            {
                if (recenterMenuAnchor())
                    Log(Debug::Info) << "FNVXR retail surface: latched retail-world anchor";
            }

            Stereo::Pose localOffset = {};
            localOffset.position = Stereo::Position::fromMeters(
                envFloat("OPENMW_FNVXR_RETAIL_WORLD_OFFSET_X_METERS", 0.f),
                envFloat("OPENMW_FNVXR_RETAIL_WORLD_OFFSET_Y_METERS", 1.05f),
                envFloat("OPENMW_FNVXR_RETAIL_WORLD_OFFSET_Z_METERS", -0.02f));
            Stereo::Pose pose = mHaveAnchorPose ? (mAnchorPose + localOffset) : localOffset;
            mTransform->setAttitude(pose.orientation);
            mTransform->setPosition(pose.position.asMWUnits());
            return;
        }

        if (!mHaveAnchorPose)
        {
            if (recenterMenuAnchor())
                Log(Debug::Info) << "FNVXR retail surface: latched menu-space anchor";
        }

        Stereo::Pose localOffset = {};
        localOffset.position = Stereo::Position::fromMeters(0.f, 1.18f, -0.12f);
        Stereo::Pose pose = mHaveAnchorPose ? (mAnchorPose + localOffset) : localOffset;
        mTransform->setAttitude(pose.orientation);
        mTransform->setPosition(pose.position.asMWUnits());
    }

    bool FNVXRLiveFrameSurface::ensureCameraMapping()
    {
#ifdef _WIN32
        if (mCameraView)
            return true;

        mCameraMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FNVXR_Camera_State");
        if (!mCameraMapping)
            return false;

        mCameraView = MapViewOfFile(static_cast<HANDLE>(mCameraMapping), FILE_MAP_READ, 0, 0, sizeof(SharedCameraState));
        if (!mCameraView)
        {
            CloseHandle(static_cast<HANDLE>(mCameraMapping));
            mCameraMapping = nullptr;
            return false;
        }

        if (!mLoggedCamera)
        {
            mLoggedCamera = true;
            Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_Camera_State";
        }
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::ensurePlayerMapping()
    {
#ifdef _WIN32
        if (mPlayerView)
            return true;

        mPlayerMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\FNVXR_Player_State");
        if (!mPlayerMapping)
            return false;

        mPlayerView = MapViewOfFile(static_cast<HANDLE>(mPlayerMapping), FILE_MAP_READ, 0, 0, sizeof(SharedPlayerState));
        if (!mPlayerView)
        {
            CloseHandle(static_cast<HANDLE>(mPlayerMapping));
            mPlayerMapping = nullptr;
            return false;
        }

        Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_Player_State";
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::retailWorldActive()
    {
#ifdef _WIN32
        if (!envEnabled("OPENMW_FNVXR_RETAIL_WORLD_VIEW", true))
            return false;

        if (!ensureCameraMapping())
            return false;

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        bool active = false;
        bool torn = false;
        if (!tryReadCameraActive(mCameraView, mRetailWorldMode, &magic, &version, &active, &torn))
        {
            if (!mLoggedCameraReadFailure)
            {
                mLoggedCameraReadFailure = true;
                Log(Debug::Warning) << "FNVXR retail surface: camera state read failed; falling back to menu-panel";
            }
            if (mCameraView)
            {
                UnmapViewOfFile(mCameraView);
                mCameraView = nullptr;
            }
            if (mCameraMapping)
            {
                CloseHandle(static_cast<HANDLE>(mCameraMapping));
                mCameraMapping = nullptr;
            }
            return false;
        }

        if (magic != CameraSharedMagic || version != CameraSharedVersion)
        {
            if (!mLoggedCameraInvalid)
            {
                mLoggedCameraInvalid = true;
                Log(Debug::Warning) << "FNVXR retail surface: camera state invalid magic=0x" << std::hex << magic
                                    << " version=" << std::dec << version;
            }
            return false;
        }

        if (ensurePlayerMapping())
        {
            SharedPlayerSnapshot player;
            std::uint32_t playerMagic = 0;
            std::uint32_t playerVersion = 0;
            bool playerTorn = false;
            if (!tryReadPlayerState(mPlayerView, &player, &playerMagic, &playerVersion, &playerTorn))
            {
                if (!mLoggedPlayerReadFailure)
                {
                    mLoggedPlayerReadFailure = true;
                    Log(Debug::Warning) << "FNVXR retail surface: player state read failed";
                }
                if (mPlayerView)
                {
                    UnmapViewOfFile(mPlayerView);
                    mPlayerView = nullptr;
                }
                if (mPlayerMapping)
                {
                    CloseHandle(static_cast<HANDLE>(mPlayerMapping));
                    mPlayerMapping = nullptr;
                }
            }
            else if (playerMagic != PlayerSharedMagic || playerVersion != PlayerSharedVersion)
            {
                if (!mLoggedPlayerInvalid)
                {
                    mLoggedPlayerInvalid = true;
                    Log(Debug::Warning) << "FNVXR retail surface: player state invalid magic=0x" << std::hex
                                        << playerMagic << " version=" << std::dec << playerVersion;
                }
            }
            else if (!playerTorn)
            {
                const bool playerNodeValid = (player.flags & PlayerSharedFlagPlayerNodeValid) != 0;
                const bool cameraValid = (player.flags & PlayerSharedFlagCameraValid) != 0;
                const bool cellKnown = (player.flags & PlayerSharedFlagCellKnown) != 0;
                const bool thirdPerson = (player.flags & PlayerSharedFlagThirdPerson) != 0;
                const bool gameplay = (player.flags & PlayerSharedFlagGameplay) != 0;

                if (!envEnabled("OPENMW_FNVXR_SYNC_RETAIL_PLAYER", true))
                {
                    if (!mLoggedPlayerSyncDisabled)
                    {
                        mLoggedPlayerSyncDisabled = true;
                        Log(Debug::Info)
                            << "FNVXR retail surface: retail player sync disabled by OPENMW_FNVXR_SYNC_RETAIL_PLAYER";
                    }
                }
                else if (!playerNodeValid || !cellKnown || player.currentCellFormId == 0
                    || !finiteArray(player.playerWorldPos, 3) || !finiteArray(player.playerWorldRot, 9))
                {
                    if (!mLoggedPlayerSyncBlocked)
                    {
                        mLoggedPlayerSyncBlocked = true;
                        Log(Debug::Warning) << "FNVXR retail surface: retail player sync waiting for valid cell and "
                                               "player transform"
                                            << " playerNodeValid=" << playerNodeValid
                                            << " cellKnown=" << cellKnown
                                            << " cellFormId=" << player.currentCellFormId
                                            << " posFinite=" << finiteArray(player.playerWorldPos, 3)
                                            << " rotFinite=" << finiteArray(player.playerWorldRot, 9);
                    }
                }
                else
                {
                    const float minDelta = std::max(0.f, envFloat("OPENMW_FNVXR_SYNC_RETAIL_PLAYER_MIN_DELTA", 1.f));
                    const float minRotationDelta
                        = std::max(0.f, envFloat("OPENMW_FNVXR_SYNC_RETAIL_PLAYER_ROTATION_DELTA", 0.0001f));
                    const std::uint64_t minFrameDelta = static_cast<std::uint64_t>(
                        std::max(1.f, envFloat("OPENMW_FNVXR_SYNC_RETAIL_PLAYER_FRAME_DELTA", 1.f)));
                    const bool firstSync = mLastSyncedPlayerFrame == 0;
                    const bool cellChanged = firstSync || player.currentCellFormId != mLastSyncedCellFormId;
                    const bool frameAdvanced = firstSync || player.frame > mLastSyncedPlayerFrame;
                    const std::uint64_t frameDelta
                        = frameAdvanced && !firstSync ? player.frame - mLastSyncedPlayerFrame : 0;
                    const bool movedEnough = firstSync || minDelta == 0.f
                        || distanceSquared(player.playerWorldPos, mLastSyncedPlayerPos) >= minDelta * minDelta;
                    const bool rotatedEnough = firstSync || minRotationDelta == 0.f
                        || distanceSquaredN(player.playerWorldRot, mLastSyncedPlayerRot, 9)
                            >= minRotationDelta * minRotationDelta;
                    if (cellChanged || (frameAdvanced && frameDelta >= minFrameDelta && (movedEnough || rotatedEnough)))
                    {
                        MWBase::World* world = MWBase::Environment::get().getWorld();
                        if (!world)
                        {
                            if (!mLoggedPlayerSyncBlocked)
                            {
                                mLoggedPlayerSyncBlocked = true;
                                Log(Debug::Warning)
                                    << "FNVXR retail surface: retail player sync blocked because OpenMW world is null";
                            }
                        }
                        else
                        {
                            const ESM::Position position = retailPlayerPosition(player);
                            const ESM::RefId cellId = retailCellRefId(player.currentCellFormId);
                            try
                            {
                                if (cellChanged)
                                    world->changeToCell(cellId, position, false, true);
                                MWWorld::Ptr playerPtr
                                    = world->moveObject(world->getPlayerPtr(), position.asVec3(), true, true);
                                world->rotateObject(playerPtr, position.asRotationVec3(), MWBase::RotationFlag_none);
                                mLastSyncedPlayerFrame = player.frame;
                                mLastSyncedCellFormId = player.currentCellFormId;
                                std::memcpy(mLastSyncedPlayerPos, player.playerWorldPos, sizeof(mLastSyncedPlayerPos));
                                std::memcpy(mLastSyncedPlayerRot, player.playerWorldRot, sizeof(mLastSyncedPlayerRot));
                                mLoggedPlayerSyncBlocked = false;
                                if (!mLoggedPlayerSyncApplied || cellChanged)
                                {
                                    mLoggedPlayerSyncApplied = true;
                                    Log(Debug::Info) << "FNVXR retail surface: synced OpenMW player to retail cell="
                                                     << cellId.toDebugString() << " frame=" << player.frame
                                                     << " pos=(" << player.playerWorldPos[0] << ", "
                                                      << player.playerWorldPos[1] << ", " << player.playerWorldPos[2]
                                                      << ") yaw=" << position.rot[2];
                                }
                            }
                            catch (const std::exception& e)
                            {
                                if (!mLoggedPlayerSyncBlocked)
                                {
                                    mLoggedPlayerSyncBlocked = true;
                                    Log(Debug::Warning)
                                        << "FNVXR retail surface: retail player sync failed for cell="
                                        << cellId.toDebugString() << ": " << e.what();
                                }
                            }
                        }
                    }
                }

                if (!mLoggedPlayer || mLastLoggedPlayerFlags != player.flags
                    || player.frame >= mLastLoggedPlayerFrame + 300)
                {
                    mLoggedPlayer = true;
                    mLastLoggedPlayerFrame = player.frame;
                    mLastLoggedPlayerFlags = player.flags;
                    Log(Debug::Info) << "FNVXR retail surface: player state frame=" << player.frame
                                     << " flags=0x" << std::hex << player.flags << std::dec
                                     << " playerNodeValid=" << playerNodeValid
                                     << " cameraValid=" << cameraValid
                                     << " gameplay=" << gameplay
                                     << " thirdPerson=" << thirdPerson
                                     << " cellKnown=" << cellKnown
                                     << " cellFormId=" << player.currentCellFormId
                                     << " player=0x" << std::hex << player.playerAddress
                                     << " playerNode=0x" << player.playerNodeAddress
                                     << " cameraNode=0x" << player.cameraNodeAddress << std::dec
                                     << " playerPos=(" << player.playerWorldPos[0] << ", "
                                     << player.playerWorldPos[1] << ", " << player.playerWorldPos[2] << ")"
                                     << " cameraPos=(" << player.cameraWorldPos[0] << ", "
                                     << player.cameraWorldPos[1] << ", " << player.cameraWorldPos[2] << ")";
                }
            }
        }

        return torn ? mRetailWorldMode : active;
#else
        return false;
#endif
    }

    void FNVXRLiveFrameSurface::updateAimPointer()
    {
        if (!mVisible || !mTransform || !mWidth || !mHeight)
            return;

        try
        {
            Stereo::Pose aimPose = {};
            const auto locateAim = [&](const char* action) {
                auto aim = OpenXRInput::instance().getSpace(action);
                if (!aim)
                    return false;
                auto tracked = aim->locateInWorld();
                if (!tracked.status)
                    return false;
                aimPose = tracked.pose;
                return true;
            };

            if (!locateAim(OpenXRInput::RightHandAim) && !locateAim(OpenXRInput::LeftHandAim))
            {
                clearPointer();
                return;
            }

            const osg::Vec3 originWorld = aimPose.position.asMWUnits();
            const osg::Vec3 directionWorld = aimPose.orientation * osg::Vec3(0.f, 1.f, 0.f);
            const osg::Quat inversePanel = mTransform->getAttitude().inverse();
            const osg::Vec3 panelScale = mTransform->getScale();
            const osg::Vec3 originRotated = inversePanel * (originWorld - mTransform->getPosition());
            const osg::Vec3 directionRotated = inversePanel * directionWorld;
            const osg::Vec3 originLocal(
                originRotated.x() / panelScale.x(),
                originRotated.y() / std::max(0.0001f, panelScale.y()),
                originRotated.z() / panelScale.z());
            const osg::Vec3 directionLocal(
                directionRotated.x() / panelScale.x(),
                directionRotated.y() / std::max(0.0001f, panelScale.y()),
                directionRotated.z() / panelScale.z());

            if (std::abs(directionLocal.y()) < 0.00001f)
            {
                clearPointer();
                return;
            }

            const float t = (1.f - originLocal.y()) / directionLocal.y();
            if (t < 0.f)
            {
                clearPointer();
                return;
            }

            const osg::Vec3 hit = originLocal + directionLocal * t;
            const bool inside = hit.x() >= -0.5f && hit.x() <= 0.5f && hit.z() >= -0.5f && hit.z() <= 0.5f;
            mFocused = inside;
            if (!inside)
            {
                clearPointer();
                return;
            }

            const float u = std::clamp(hit.x() + 0.5f, 0.f, 1.f);
            const float v = std::clamp(0.5f - hit.z(), 0.f, 1.f);
            if (mFocusLogCount < 24)
            {
                ++mFocusLogCount;
                Log(Debug::Info) << "FNVXR retail surface: aim-plane focus uv=(" << u << "," << v
                                 << ") localHit=(" << hit.x() << "," << hit.y() << "," << hit.z() << ")";
            }
            publishPointer(u, v);
        }
        catch (const std::exception& e)
        {
            clearPointer();
            Log(Debug::Warning) << "FNVXR retail surface: aim-plane pointer failed: " << e.what();
        }
    }

    void FNVXRLiveFrameSurface::setVisible(bool visible)
    {
        mVisible = visible;
        if (!mGeometryRoot || !mTransform)
            return;

        if (visible)
        {
            if (!mGeometryRoot->containsNode(mTransform))
                mGeometryRoot->addChild(mTransform);
            mTransform->setNodeMask(MWRender::Mask_3DGUI);
        }
        else
        {
            mFocused = false;
            mPointerActive = false;
            mTransform->setNodeMask(0);
            clearPointer();
        }
    }

    bool FNVXRLiveFrameSurface::updateFocus(osg::Node* focusNode, osg::Vec3f hitPoint)
    {
        const bool hit = focusNode
            && (focusNode->getName() == "FNVXRLiveFrameSurface"
                || focusNode->getName() == "FNVXRLiveFrameSurfaceDebugFrame")
            && mWidth && mHeight;
        mFocused = hit;
        if (!hit)
        {
            clearPointer();
            return false;
        }

        const float u = std::clamp(hitPoint.x() + 0.5f, 0.f, 1.f);
        const float v = std::clamp(0.5f - hitPoint.z(), 0.f, 1.f);
        if (mFocusLogCount < 24)
        {
            ++mFocusLogCount;
            Log(Debug::Info) << "FNVXR retail surface: hand focus node=" << focusNode->getName() << " uv=(" << u
                             << "," << v << ") localHit=(" << hitPoint.x() << "," << hitPoint.y() << ","
                             << hitPoint.z() << ")";
        }
        publishPointer(u, v);
        return true;
    }

    bool FNVXRLiveFrameSurface::ensureDInputMapping()
    {
#ifdef _WIN32
        if (mDInputView)
            return true;

        mDInputMapping = OpenFileMappingA(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, "Local\\FNVXR_DInput_State");
        if (!mDInputMapping)
            return false;

        mDInputView = MapViewOfFile(
            static_cast<HANDLE>(mDInputMapping), FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedDInputState));
        if (!mDInputView)
        {
            CloseHandle(static_cast<HANDLE>(mDInputMapping));
            mDInputMapping = nullptr;
            return false;
        }

        if (!mLoggedDInput)
        {
            mLoggedDInput = true;
            Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_DInput_State";
        }
        return true;
#else
        return false;
#endif
    }

    void FNVXRLiveFrameSurface::publishPointer(float u, float v)
    {
#ifdef _WIN32
        if (!ensureDInputMapping())
            return;

        auto* shared = static_cast<SharedDInputState*>(mDInputView);
        if (shared->magic != DInputSharedMagic || shared->version != DInputSharedVersion)
            return;

        const float pointerWidth = std::max(1.f, envFloat("OPENMW_FNVXR_RETAIL_POINTER_WIDTH", 1280.f));
        const float pointerHeight = std::max(1.f, envFloat("OPENMW_FNVXR_RETAIL_POINTER_HEIGHT", 720.f));
        shared->clientX = static_cast<std::int32_t>(u * (pointerWidth - 1.f));
        shared->clientY = static_cast<std::int32_t>(v * (pointerHeight - 1.f));
        shared->pointerActive = 1;
        mPointerActive = true;
        ++shared->frame;
        if (mPointerLogCount < 24)
        {
            ++mPointerLogCount;
            Log(Debug::Info) << "FNVXR retail surface: published mouse x=" << shared->clientX
                             << " y=" << shared->clientY << " frame=" << shared->frame;
        }
#endif
    }

    void FNVXRLiveFrameSurface::clearPointer()
    {
        const bool wasActive = mPointerActive;
        mPointerActive = false;
#ifdef _WIN32
        if (!mDInputView)
            return;
        auto* shared = static_cast<SharedDInputState*>(mDInputView);
        if (shared->magic == DInputSharedMagic && shared->version == DInputSharedVersion
            && (shared->pointerActive != 0 || wasActive))
        {
            shared->pointerActive = 0;
            ++shared->frame;
        }
#endif
    }

    bool FNVXRLiveFrameSurface::injectMouseClick()
    {
#ifdef _WIN32
        updateAimPointer();
        if ((!mVisible || !mPointerActive) || !ensureDInputMapping())
        {
            Log(Debug::Info) << "FNVXR retail surface: click skipped visible=" << mVisible
                             << " focused=" << mFocused << " pointerActive=" << mPointerActive;
            return false;
        }

        auto* shared = static_cast<SharedDInputState*>(mDInputView);
        if (shared->magic != DInputSharedMagic || shared->version != DInputSharedVersion)
            return false;

        ++shared->mouseClickPacket;
        ++shared->frame;
        Log(Debug::Info) << "FNVXR retail surface: click packet=" << shared->mouseClickPacket;
        return true;
#else
        return false;
#endif
    }

    bool FNVXRLiveFrameSurface::recenterMenuAnchor()
    {
        if (auto view = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView))
        {
            auto tracked = view->locateInWorld();
            if (!!tracked.status)
            {
                mAnchorPose = tracked.pose;
                mHaveAnchorPose = true;
                Log(Debug::Info) << "FNVXR retail surface: grip recentered menu-space anchor";
                return true;
            }
        }

        Log(Debug::Warning) << "FNVXR retail surface: grip recenter failed; HMD pose unavailable";
        return false;
    }

    bool FNVXRLiveFrameSurface::recenterMenuPortal()
    {
        if (!enabled())
            return false;

        ensureSceneObjects();
        if (!mTransform || !recenterMenuAnchor())
            return false;

        mGripMenuOverride = true;
        mRetailWorldMode = false;
        mRetailPanelArmed = true;
        mWorldReadyTimerRunning = true;
        mWorldReadySince = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        mRetailWorldReadyFrames = 0;

        if (mWidth && mHeight)
            updatePlacement(false);
        setVisible(true);
        updateAimPointer();

        Log(Debug::Info) << "FNVXR retail surface: grip recentered menu portal";
        return true;
    }

    void FNVXRLiveFrameSurface::setGripMenuOverride(bool active)
    {
        if (active == mGripMenuOverride)
            return;

        mGripMenuOverride = active;
        if (active)
        {
            recenterMenuPortal();
            if (mRetailWorldMode)
                Log(Debug::Info) << "FNVXR retail surface: grip forced menu-panel override";
        }
        else
            Log(Debug::Info) << "FNVXR retail surface: grip menu override released";
    }
}
