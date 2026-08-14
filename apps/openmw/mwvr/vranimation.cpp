#include "openxrinput.hpp"
#include "fnvxrliveframesurface.hpp"
#include "vranimation.hpp"
#include "vrgui.hpp"
#include "vrpointer.hpp"

#include <osg/BlendFunc>
#include <osg/Array>
#include <osg/ComputeBoundsVisitor>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/Depth>
#include <osg/Drawable>
#include <osg/Fog>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LightModel>
#include <osg/LineWidth>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Object>
#include <osg/PolygonMode>
#include <osg/PositionAttitudeTransform>
#include <osg/Quat>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osg/Image>
#include <osgDB/WriteFile>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <components/debug/debuglog.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <components/settings/settings.hpp>
#include <components/settings/values.hpp>

#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadench.hpp>

#include <components/vr/session.hpp>
#include <components/vr/space.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/trackingtransform.hpp>
#include <components/vr/vr.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/xr/session.hpp>

#include "../mwworld/esmstore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"

#include "vrpointer.hpp"
#include "vrtracking.hpp"

namespace MWVR
{
    float getEnvFloat(std::string_view name, float fallback)
    {
        if (const char* value = std::getenv(std::string(name).c_str()))
            return std::atof(value);
        return fallback;
    }

    osg::Quat makeEulerDegrees(float x, float y, float z)
    {
        return osg::Quat(osg::DegreesToRadians(x), osg::Vec3f(1.f, 0.f, 0.f))
            * osg::Quat(osg::DegreesToRadians(y), osg::Vec3f(0.f, 1.f, 0.f))
            * osg::Quat(osg::DegreesToRadians(z), osg::Vec3f(0.f, 0.f, 1.f));
    }

    float getHandEnvFloat(bool left, std::string_view suffix, float fallback)
    {
        std::string sideName = left ? "OPENMW_FNV_LEFT_HAND_" : "OPENMW_FNV_RIGHT_HAND_";
        sideName += suffix;
        if (const char* value = std::getenv(sideName.c_str()))
            return std::atof(value);

        std::string sharedName = "OPENMW_FNV_HAND_";
        sharedName += suffix;
        return getEnvFloat(sharedName, fallback);
    }

    float getPipBoyEnvFloat(bool left, std::string_view suffix, float fallback)
    {
        std::string sideName = left ? "OPENMW_FNV_LEFT_PIPBOY_" : "OPENMW_FNV_RIGHT_PIPBOY_";
        sideName += suffix;
        if (const char* value = std::getenv(sideName.c_str()))
            return std::atof(value);

        std::string sharedName = "OPENMW_FNV_PIPBOY_";
        sharedName += suffix;
        return getEnvFloat(sharedName, fallback);
    }

    osg::Vec3f scaleVec3(const osg::Vec3f& lhs, const osg::Vec3f& rhs)
    {
        return osg::Vec3f(lhs.x() * rhs.x(), lhs.y() * rhs.y(), lhs.z() * rhs.z());
    }

    float clampUnit(float value)
    {
        return std::max(-1.f, std::min(1.f, value));
    }

    osg::Vec3f normalizeOr(const osg::Vec3f& value, const osg::Vec3f& fallback)
    {
        const float length = value.length();
        if (!std::isfinite(length) || length <= 1e-5f)
            return fallback;
        return value * (1.f / length);
    }

    osg::Vec3f rejectFromAxis(const osg::Vec3f& value, const osg::Vec3f& axis)
    {
        return value - axis * (value * axis);
    }

    osg::Quat shortestArcQuat(const osg::Vec3f& from, const osg::Vec3f& to)
    {
        const osg::Vec3f source = normalizeOr(from, osg::Vec3f(1.f, 0.f, 0.f));
        const osg::Vec3f target = normalizeOr(to, osg::Vec3f(1.f, 0.f, 0.f));
        const float dot = clampUnit(source * target);
        if (dot > 0.9999f)
            return osg::Quat();
        if (dot < -0.9999f)
        {
            const osg::Vec3f probe = std::abs(source.x()) < 0.9f ? osg::Vec3f(1.f, 0.f, 0.f) : osg::Vec3f(0.f, 1.f, 0.f);
            return osg::Quat(3.14159265358979323846, normalizeOr(source ^ probe, osg::Vec3f(0.f, 0.f, 1.f)));
        }

        return osg::Quat(std::acos(dot), normalizeOr(source ^ target, osg::Vec3f(0.f, 0.f, 1.f)));
    }

    float signedAngleAroundAxis(const osg::Vec3f& from, const osg::Vec3f& to, const osg::Vec3f& axis)
    {
        const osg::Vec3f source = normalizeOr(from, osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Vec3f target = normalizeOr(to, osg::Vec3f(0.f, 0.f, 1.f));
        return std::atan2((source ^ target) * axis, clampUnit(source * target));
    }

    osg::Quat alignFrames(
        const osg::Vec3f& modelForward, const osg::Vec3f& modelUp, const osg::Vec3f& targetForward, const osg::Vec3f& targetUp)
    {
        const osg::Vec3f sourceForward = normalizeOr(modelForward, osg::Vec3f(1.f, 0.f, 0.f));
        const osg::Vec3f destForward = normalizeOr(targetForward, osg::Vec3f(1.f, 0.f, 0.f));
        const osg::Quat forwardRotation = shortestArcQuat(sourceForward, destForward);
        const osg::Vec3f rotatedUp = normalizeOr(
            rejectFromAxis(forwardRotation * modelUp, destForward), osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Vec3f desiredUp
            = normalizeOr(rejectFromAxis(targetUp, destForward), osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Quat rollRotation(signedAngleAroundAxis(rotatedUp, desiredUp, destForward), destForward);
        return rollRotation * forwardRotation;
    }

    // Some weapon types, such as spellcast, are classified as melee even though they are not. At least not in the way i
    // want. All the false melee types have negative enum values, but also so does hand to hand. I think this covers all
    // cases
    static bool isMeleeWeapon(int type)
    {
        if (MWMechanics::getWeaponType(type)->mWeaponClass != ESM::WeaponType::Melee)
            return false;
        if (type == ESM::Weapon::HandToHand)
            return true;
        if (type >= 0)
            return true;

        return false;
    }

    enum class FingerCurlSource
    {
        Thumb,
        Trigger,
        Grip,
    };

    struct ActionSample
    {
        float value = 0.f;
        std::string id;
        bool boolean = false;
    };

    std::optional<ActionSample> getActionSample(const std::vector<std::string>& ids, bool preferActive = false)
    {
        auto& actionSet = OpenXRInput::instance().getActionSet(MWActionSet::Actions);
        std::optional<ActionSample> firstSample;
        for (const std::string& id : ids)
        {
            try
            {
                std::optional<XR::InputAction::Value> value = actionSet.getValue(id);
                if (!value)
                    continue;

                if (const float* floatValue = std::get_if<float>(&*value))
                {
                    ActionSample sample{ clampUnit(*floatValue), id, false };
                    if (!firstSample)
                        firstSample = sample;
                    if (!preferActive || std::abs(sample.value) > 0.001f)
                        return sample;
                }
                if (const bool* boolValue = std::get_if<bool>(&*value))
                {
                    ActionSample sample{ *boolValue ? 1.f : 0.f, id, true };
                    if (!firstSample)
                        firstSample = sample;
                    if (!preferActive || std::abs(sample.value) > 0.001f)
                        return sample;
                }
            }
            catch (const std::out_of_range&)
            {
            }
        }

        return firstSample;
    }

    std::vector<std::string> getFingerSensorIds(const std::string& hand)
    {
        return {
            hand + "/input/trigger/curl_meta",
            hand + "/input/trigger/curl_fb",
            hand + "/input/trigger/value",
            hand + "/input/trigger/slide_meta",
            hand + "/input/trigger/slide_fb",
            hand + "/input/trigger/force",
            hand + "/input/trigger/touch",
            hand + "/input/trigger/proximity_meta",
            hand + "/input/trigger/proximity_fb",
            hand + "/input/squeeze/value",
            hand + "/input/squeeze/force",
            hand + "/input/squeeze/click",
            hand + "/input/squeeze/touch",
            hand + "/input/thumb_meta/proximity_meta",
            hand + "/input/thumb_fb/proximity_fb",
            hand + "/input/thumbrest/touch",
            hand + "/input/thumbrest/force",
            hand + "/input/thumbstick/touch",
            hand + "/input/thumbstick/click",
        };
    }

    std::vector<std::string> getCurlActionIds(FingerCurlSource source, const std::string& hand)
    {
        switch (source)
        {
            case FingerCurlSource::Thumb:
                return { hand + "/input/thumbrest/force", hand + "/input/thumb_meta/proximity_meta",
                    hand + "/input/thumb_fb/proximity_fb", hand + "/input/thumbrest/touch",
                    hand + "/input/thumbstick/touch" };
            case FingerCurlSource::Trigger:
                return { hand + "/input/trigger/curl_meta", hand + "/input/trigger/curl_fb",
                    hand + "/input/trigger/value" };
            case FingerCurlSource::Grip:
            {
                std::vector<std::string> actions = { hand + "/input/squeeze/value", hand + "/input/squeeze/force",
                    hand + "/input/squeeze/click" };
                if (getEnvFloat("OPENMW_FNV_VR_GRIP_FALLBACK_TO_TRIGGER", 1.f) != 0.f)
                {
                    actions.push_back(hand + "/input/trigger/curl_meta");
                    actions.push_back(hand + "/input/trigger/curl_fb");
                    actions.push_back(hand + "/input/trigger/value");
                }
                return actions;
            }
        }

        return {};
    }

    std::vector<std::string> getGripCurlActionIds(const std::string& hand)
    {
        return { hand + "/input/squeeze/value", hand + "/input/squeeze/force", hand + "/input/squeeze/click" };
    }

    std::vector<std::string> getGripFallbackCurlActionIds(const std::string& hand)
    {
        return { hand + "/input/trigger/curl_meta", hand + "/input/trigger/curl_fb", hand + "/input/trigger/value" };
    }

    std::string getEnvString(std::string_view name, const std::string& fallback)
    {
        if (const char* value = std::getenv(std::string(name).c_str()))
            return value;
        return fallback;
    }

    bool vrDebugSnapshotEnabled()
    {
        return getEnvFloat("OPENMW_FNV_VR_DEBUG_SNAPSHOT", 0.f) != 0.f;
    }

    bool vrDebugRunningLogEnabled()
    {
        return getEnvFloat("OPENMW_FNV_VR_DEBUG_RUNNING_LOG", 0.f) != 0.f;
    }

    std::filesystem::path vrDebugSnapshotDir()
    {
        const std::string configured = getEnvString("OPENMW_FNV_VR_DEBUG_SNAPSHOT_DIR", "");
        if (!configured.empty())
            return std::filesystem::path(configured);
        return std::filesystem::current_path() / "vr-debug-snapshots";
    }

    std::string vrDebugTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream stream;
        stream << std::put_time(&tm, "%Y%m%d_%H%M%S");
        return stream.str();
    }

    std::string jsonVec(const osg::Vec3f& value)
    {
        std::ostringstream stream;
        stream << "[" << value.x() << "," << value.y() << "," << value.z() << "]";
        return stream.str();
    }

    std::string jsonQuat(const osg::Quat& value)
    {
        std::ostringstream stream;
        stream << "[" << value.x() << "," << value.y() << "," << value.z() << "," << value.w() << "]";
        return stream.str();
    }

    std::string jsonMatrix(const osg::Matrixf& value)
    {
        std::ostringstream stream;
        stream << "[";
        for (int row = 0; row < 4; ++row)
        {
            if (row != 0)
                stream << ",";
            stream << "[";
            for (int col = 0; col < 4; ++col)
            {
                if (col != 0)
                    stream << ",";
                stream << value(row, col);
            }
            stream << "]";
        }
        stream << "]";
        return stream.str();
    }

    std::string jsonString(std::string_view value)
    {
        std::ostringstream stream;
        stream << '"';
        for (char ch : value)
        {
            switch (ch)
            {
                case '\\':
                    stream << "\\\\";
                    break;
                case '"':
                    stream << "\\\"";
                    break;
                case '\n':
                    stream << "\\n";
                    break;
                case '\r':
                    stream << "\\r";
                    break;
                case '\t':
                    stream << "\\t";
                    break;
                default:
                    stream << ch;
                    break;
            }
        }
        stream << '"';
        return stream.str();
    }

    struct MeshTriangle
    {
        unsigned int a = 0;
        unsigned int b = 0;
        unsigned int c = 0;
    };

    void appendTriangle(std::vector<MeshTriangle>& triangles, unsigned int a, unsigned int b, unsigned int c)
    {
        if (a == b || b == c || c == a)
            return;
        triangles.push_back({ a, b, c });
    }

    std::vector<MeshTriangle> collectTriangles(const osg::Geometry& geometry)
    {
        std::vector<MeshTriangle> triangles;
        for (unsigned int primitiveIndex = 0; primitiveIndex < geometry.getNumPrimitiveSets(); ++primitiveIndex)
        {
            const osg::PrimitiveSet* primitive = geometry.getPrimitiveSet(primitiveIndex);
            if (primitive == nullptr)
                continue;

            std::vector<unsigned int> indices;
            indices.reserve(primitive->getNumIndices());
            for (unsigned int i = 0; i < primitive->getNumIndices(); ++i)
                indices.push_back(primitive->index(i));

            switch (primitive->getMode())
            {
                case GL_TRIANGLES:
                    for (std::size_t i = 2; i < indices.size(); i += 3)
                        appendTriangle(triangles, indices[i - 2], indices[i - 1], indices[i]);
                    break;
                case GL_TRIANGLE_STRIP:
                    for (std::size_t i = 2; i < indices.size(); ++i)
                    {
                        if ((i % 2) == 0)
                            appendTriangle(triangles, indices[i - 2], indices[i - 1], indices[i]);
                        else
                            appendTriangle(triangles, indices[i - 1], indices[i - 2], indices[i]);
                    }
                    break;
                case GL_TRIANGLE_FAN:
                case GL_POLYGON:
                    for (std::size_t i = 2; i < indices.size(); ++i)
                        appendTriangle(triangles, indices[0], indices[i - 1], indices[i]);
                    break;
                case GL_QUADS:
                    for (std::size_t i = 3; i < indices.size(); i += 4)
                    {
                        appendTriangle(triangles, indices[i - 3], indices[i - 2], indices[i - 1]);
                        appendTriangle(triangles, indices[i - 3], indices[i - 1], indices[i]);
                    }
                    break;
                case GL_QUAD_STRIP:
                    for (std::size_t i = 3; i < indices.size(); i += 2)
                    {
                        appendTriangle(triangles, indices[i - 3], indices[i - 2], indices[i - 1]);
                        appendTriangle(triangles, indices[i - 2], indices[i], indices[i - 1]);
                    }
                    break;
                default:
                    break;
            }
        }
        return triangles;
    }

    void writeHandMeshProofJson(const std::string& handName, const std::string& sourceName, const std::string& rootBone,
        const osg::Geometry& geometry, const std::array<std::vector<float>, 15>& fingerBoneWeights,
        const SceneUtil::RigGeometry* rig)
    {
        if (!vrDebugSnapshotEnabled())
            return;

        const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (vertices == nullptr)
            return;

        try
        {
            std::filesystem::create_directories(vrDebugSnapshotDir());
            const std::string safeName = Misc::StringUtils::lowerCase(handName.empty() ? sourceName : handName);
            std::string fileName;
            fileName.reserve(safeName.size());
            for (char ch : safeName)
                fileName.push_back((std::isalnum(static_cast<unsigned char>(ch)) != 0) ? ch : '_');

            const std::filesystem::path path
                = vrDebugSnapshotDir() / ("hand_mesh_proof_" + vrDebugTimestamp() + "_" + fileName + ".json");
            const std::vector<MeshTriangle> triangles = collectTriangles(geometry);
            std::vector<SceneUtil::RigGeometry::BoneInfo> bones;
            std::vector<SceneUtil::RigGeometry::BoneWeights> vertexInfluences;
            std::vector<osg::Matrixf> localBoneMatrices;
            std::vector<osg::Matrixf> skeletonBoneMatrices;
            osg::Matrixf rigTransform;
            osg::Matrixf skinToSkelMatrix;
            const bool hasSkinningDebugData = rig != nullptr
                && rig->getSkinningDebugData(
                    bones, vertexInfluences, localBoneMatrices, skeletonBoneMatrices, rigTransform, skinToSkelMatrix);

            std::ofstream stream(path);
            stream << "{\n"
                   << "  \"type\": \"vr-hand-mesh-proof\",\n"
                   << "  \"handName\": " << jsonString(handName) << ",\n"
                   << "  \"sourceName\": " << jsonString(sourceName) << ",\n"
                   << "  \"rootBone\": " << jsonString(rootBone) << ",\n"
                   << "  \"vertexCount\": " << vertices->size() << ",\n"
                   << "  \"triangleCount\": " << triangles.size() << ",\n"
                   << "  \"vertices\": [";
            for (std::size_t i = 0; i < vertices->size(); ++i)
            {
                if (i != 0)
                    stream << ",";
                stream << jsonVec((*vertices)[i]);
            }
            stream << "],\n  \"triangles\": [";
            for (std::size_t i = 0; i < triangles.size(); ++i)
            {
                if (i != 0)
                    stream << ",";
                stream << "[" << triangles[i].a << "," << triangles[i].b << "," << triangles[i].c << "]";
            }
            stream << "],\n  \"fingerBoneWeights\": [";
            for (std::size_t slot = 0; slot < fingerBoneWeights.size(); ++slot)
            {
                if (slot != 0)
                    stream << ",";
                stream << "[";
                for (std::size_t i = 0; i < fingerBoneWeights[slot].size(); ++i)
                {
                    if (i != 0)
                        stream << ",";
                    stream << fingerBoneWeights[slot][i];
                }
                stream << "]";
            }
            stream << "]";
            if (hasSkinningDebugData)
            {
                stream << ",\n  \"skinning\": {\n"
                       << "    \"contract\": \"OpenXR XR_FB_hand_tracking_mesh style: bind matrices, joint hierarchy, "
                          "vertex blend indices, vertex blend weights\",\n"
                       << "    \"boneCount\": " << bones.size() << ",\n"
                       << "    \"transform\": " << jsonMatrix(rigTransform) << ",\n"
                       << "    \"skinToSkelMatrix\": " << jsonMatrix(skinToSkelMatrix) << ",\n"
                       << "    \"bones\": [";
                for (std::size_t i = 0; i < bones.size(); ++i)
                {
                    if (i != 0)
                        stream << ",";
                    stream << "{"
                           << "\"index\":" << i << ","
                           << "\"name\":" << jsonString(bones[i].mName) << ","
                           << "\"invBindMatrix\":" << jsonMatrix(bones[i].mInvBindMatrix) << ","
                           << "\"localMatrix\":"
                           << jsonMatrix(i < localBoneMatrices.size() ? localBoneMatrices[i] : osg::Matrixf()) << ","
                           << "\"skeletonMatrix\":"
                           << jsonMatrix(i < skeletonBoneMatrices.size() ? skeletonBoneMatrices[i] : osg::Matrixf())
                           << "}";
                }
                stream << "],\n    \"vertexBlendIndices\": [";
                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    if (i != 0)
                        stream << ",";
                    SceneUtil::RigGeometry::BoneWeights weights
                        = i < vertexInfluences.size() ? vertexInfluences[i] : SceneUtil::RigGeometry::BoneWeights();
                    std::sort(weights.begin(), weights.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.second > rhs.second;
                    });
                    stream << "[";
                    for (std::size_t slot = 0; slot < 4; ++slot)
                    {
                        if (slot != 0)
                            stream << ",";
                        stream << (slot < weights.size() ? static_cast<int>(weights[slot].first) : -1);
                    }
                    stream << "]";
                }
                stream << "],\n    \"vertexBlendWeights\": [";
                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    if (i != 0)
                        stream << ",";
                    SceneUtil::RigGeometry::BoneWeights weights
                        = i < vertexInfluences.size() ? vertexInfluences[i] : SceneUtil::RigGeometry::BoneWeights();
                    std::sort(weights.begin(), weights.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.second > rhs.second;
                    });
                    stream << "[";
                    for (std::size_t slot = 0; slot < 4; ++slot)
                    {
                        if (slot != 0)
                            stream << ",";
                        stream << (slot < weights.size() ? weights[slot].second : 0.f);
                    }
                    stream << "]";
                }
                stream << "]\n  }";
            }
            stream << "\n}\n";
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand mesh proof exported path=" << path.string()
                             << " vertices=" << vertices->size() << " triangles=" << triangles.size();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: VR hand mesh proof export failed hand=" << handName
                                << " error=" << e.what();
        }
    }

    void smoothStaticHandFingerWeights(
        const osg::Geometry& geometry, std::array<std::vector<float>, 15>& fingerBoneWeights)
    {
        const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (vertices == nullptr || vertices->empty())
            return;

        const std::size_t vertexCount = vertices->size();
        for (const auto& weights : fingerBoneWeights)
        {
            if (weights.size() != vertexCount)
                return;
        }

        std::vector<std::vector<std::size_t>> adjacency(vertexCount);
        const auto addNeighbor = [&](std::size_t a, std::size_t b) {
            if (a >= vertexCount || b >= vertexCount || a == b)
                return;
            if (std::find(adjacency[a].begin(), adjacency[a].end(), b) == adjacency[a].end())
                adjacency[a].push_back(b);
        };
        for (const MeshTriangle& triangle : collectTriangles(geometry))
        {
            addNeighbor(triangle.a, triangle.b);
            addNeighbor(triangle.b, triangle.a);
            addNeighbor(triangle.b, triangle.c);
            addNeighbor(triangle.c, triangle.b);
            addNeighbor(triangle.c, triangle.a);
            addNeighbor(triangle.a, triangle.c);
        }

        std::vector<float> originalSums(vertexCount, 0.f);
        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            for (const auto& weights : fingerBoneWeights)
                originalSums[vertex] += weights[vertex];
        }

        constexpr int passes = 2;
        constexpr float blend = 0.5f;
        for (int pass = 0; pass < passes; ++pass)
        {
            auto next = fingerBoneWeights;
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
            {
                const auto& neighbors = adjacency[vertex];
                if (neighbors.empty())
                    continue;

                for (std::size_t slot = 0; slot < fingerBoneWeights.size(); ++slot)
                {
                    float neighborSum = 0.f;
                    for (std::size_t neighbor : neighbors)
                        neighborSum += fingerBoneWeights[slot][neighbor];
                    const float average = neighborSum / static_cast<float>(neighbors.size());
                    next[slot][vertex] = fingerBoneWeights[slot][vertex] * (1.f - blend) + average * blend;
                }

                float newSum = 0.f;
                for (const auto& weights : next)
                    newSum += weights[vertex];
                if (newSum > 1e-6f && originalSums[vertex] > 1e-6f)
                {
                    const float renormalize = originalSums[vertex] / newSum;
                    for (auto& weights : next)
                        weights[vertex] *= renormalize;
                }
            }
            fingerBoneWeights = std::move(next);
        }
    }

    const char* getFingerCurlSourceName(FingerCurlSource source)
    {
        switch (source)
        {
            case FingerCurlSource::Thumb:
                return "thumb";
            case FingerCurlSource::Trigger:
                return "trigger";
            case FingerCurlSource::Grip:
                return "grip";
        }

        return "unknown";
    }

    enum class FingerPoseGroup
    {
        Thumb = 0,
        Index = 1,
        Middle = 2,
        Ring = 3,
        Pinky = 4,
        Count = 5
    };

    const char* getFingerPoseGroupName(FingerPoseGroup group)
    {
        switch (group)
        {
            case FingerPoseGroup::Thumb:
                return "thumb";
            case FingerPoseGroup::Index:
                return "index";
            case FingerPoseGroup::Middle:
                return "middle";
            case FingerPoseGroup::Ring:
                return "ring";
            case FingerPoseGroup::Pinky:
                return "pinky";
            case FingerPoseGroup::Count:
                break;
        }

        return "unknown";
    }

    float getFingerCurlDirection(bool left, FingerPoseGroup group)
    {
        (void)left;
        (void)group;
        return 1.f;
    }

    osg::Vec3f getFingerCurlAnglesDegrees(FingerPoseGroup group)
    {
        switch (group)
        {
            case FingerPoseGroup::Thumb:
                return osg::Vec3f(26.f, 34.f, 18.f);
            case FingerPoseGroup::Index:
                return osg::Vec3f(46.f, 58.f, 28.f);
            case FingerPoseGroup::Middle:
            case FingerPoseGroup::Ring:
                return osg::Vec3f(50.f, 66.f, 32.f);
            case FingerPoseGroup::Pinky:
                return osg::Vec3f(46.f, 62.f, 30.f);
            case FingerPoseGroup::Count:
                break;
        }

        return osg::Vec3f(70.f, 95.f, 55.f);
    }

    osg::Vec3f getFingerOpenAnglesDegrees(FingerPoseGroup group)
    {
        switch (group)
        {
            case FingerPoseGroup::Thumb:
                return osg::Vec3f(34.f, 22.f, 10.f);
            case FingerPoseGroup::Index:
            case FingerPoseGroup::Middle:
            case FingerPoseGroup::Ring:
            case FingerPoseGroup::Pinky:
            case FingerPoseGroup::Count:
                break;
        }

        return osg::Vec3f();
    }

    struct VrFingerDebugSample
    {
        bool valid = false;
        bool left = false;
        FingerPoseGroup group = FingerPoseGroup::Middle;
        float curl = 0.f;
        float maxDegrees = 0.f;
        osg::Vec3f baseRoot;
        osg::Vec3f baseMid;
        osg::Vec3f baseTip;
        osg::Vec3f baseEnd;
        osg::Vec3f posedRoot;
        osg::Vec3f posedMid;
        osg::Vec3f posedTip;
        osg::Vec3f posedEnd;
        osg::Vec3f axis0;
        osg::Vec3f axis1;
        osg::Vec3f axis2;
        osg::Quat rootRotation;
        osg::Quat midRotation;
        osg::Quat tipRotation;
    };

    struct VrFingerPoseCertificate
    {
        bool finite = false;
        bool lengthOk = false;
        bool planeOk = false;
        bool monotoneOk = false;
        bool pass = false;
        float lengthErrorMax = 0.f;
        float hingeDrift = 0.f;
        float hingeDriftRatio = 0.f;
        float axisProjectionErrorMax = 0.f;
        float bend01Degrees = 0.f;
        float bend12Degrees = 0.f;
        int zigzagSignChanges = 0;
    };

    bool finiteVec3(const osg::Vec3f& value)
    {
        return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
    }

    float angleDegreesBetween(const osg::Vec3f& lhs, const osg::Vec3f& rhs)
    {
        const osg::Vec3f a = normalizeOr(lhs, osg::Vec3f(1.f, 0.f, 0.f));
        const osg::Vec3f b = normalizeOr(rhs, osg::Vec3f(1.f, 0.f, 0.f));
        return osg::RadiansToDegrees(std::acos(clampUnit(a * b)));
    }

    int turnSignAroundAxis(const osg::Vec3f& from, const osg::Vec3f& to, const osg::Vec3f& axis)
    {
        const float turn = (normalizeOr(from, osg::Vec3f(1.f, 0.f, 0.f))
            ^ normalizeOr(to, osg::Vec3f(1.f, 0.f, 0.f))) * normalizeOr(axis, osg::Vec3f(0.f, 1.f, 0.f));
        if (turn > 0.04f)
            return 1;
        if (turn < -0.04f)
            return -1;
        return 0;
    }

    float normalizedAxisProjection(const osg::Vec3f& segment, const osg::Vec3f& axis)
    {
        return normalizeOr(segment, osg::Vec3f(1.f, 0.f, 0.f)) * normalizeOr(axis, osg::Vec3f(0.f, 1.f, 0.f));
    }

    VrFingerPoseCertificate makeFingerPoseCertificate(const VrFingerDebugSample& sample)
    {
        VrFingerPoseCertificate certificate;
        const osg::Vec3f baseSegments[3] = { sample.baseMid - sample.baseRoot, sample.baseTip - sample.baseMid,
            sample.baseEnd - sample.baseTip };
        const osg::Vec3f posedSegments[3] = { sample.posedMid - sample.posedRoot, sample.posedTip - sample.posedMid,
            sample.posedEnd - sample.posedTip };

        certificate.finite = finiteVec3(sample.baseRoot) && finiteVec3(sample.baseMid) && finiteVec3(sample.baseTip)
            && finiteVec3(sample.baseEnd) && finiteVec3(sample.posedRoot) && finiteVec3(sample.posedMid)
            && finiteVec3(sample.posedTip) && finiteVec3(sample.posedEnd) && finiteVec3(sample.axis0);
        if (!certificate.finite)
            return certificate;

        float baseTotalLength = 0.f;
        for (int i = 0; i < 3; ++i)
        {
            const float baseLength = baseSegments[i].length();
            const float posedLength = posedSegments[i].length();
            baseTotalLength += baseLength;
            certificate.lengthErrorMax
                = std::max(certificate.lengthErrorMax, std::abs(baseLength - posedLength));
            const osg::Vec3f axis = i == 0 ? sample.axis0 : (i == 1 ? sample.axis1 : sample.axis2);
            certificate.axisProjectionErrorMax = std::max(certificate.axisProjectionErrorMax,
                std::abs(normalizedAxisProjection(baseSegments[i], axis) - normalizedAxisProjection(posedSegments[i], axis)));
        }

        const osg::Vec3f hingeAxis = normalizeOr(sample.axis0, osg::Vec3f(0.f, 1.f, 0.f));
        certificate.hingeDrift = std::abs(((sample.posedEnd - sample.posedRoot) * hingeAxis)
            - ((sample.baseEnd - sample.baseRoot) * hingeAxis));
        certificate.hingeDriftRatio = baseTotalLength > 1e-5f ? certificate.hingeDrift / baseTotalLength : 1.f;
        certificate.bend01Degrees = angleDegreesBetween(posedSegments[0], posedSegments[1]);
        certificate.bend12Degrees = angleDegreesBetween(posedSegments[1], posedSegments[2]);

        const int turn01 = turnSignAroundAxis(posedSegments[0], posedSegments[1], hingeAxis);
        const int turn12 = turnSignAroundAxis(posedSegments[1], posedSegments[2], hingeAxis);
        certificate.zigzagSignChanges = turn01 != 0 && turn12 != 0 && turn01 != turn12 ? 1 : 0;

        certificate.lengthOk = certificate.lengthErrorMax <= 0.01f;
        certificate.planeOk = certificate.hingeDriftRatio <= 0.12f && certificate.axisProjectionErrorMax <= 0.18f;
        certificate.monotoneOk = certificate.zigzagSignChanges == 0;
        certificate.pass = certificate.finite && certificate.lengthOk && certificate.planeOk && certificate.monotoneOk;
        return certificate;
    }

    void writeFingerCertificateJson(std::ostream& stream, const VrFingerPoseCertificate& certificate)
    {
        stream << "      \"certificate\": {\n"
               << "        \"finite\": " << (certificate.finite ? "true" : "false") << ",\n"
               << "        \"lengthOk\": " << (certificate.lengthOk ? "true" : "false") << ",\n"
               << "        \"planeOk\": " << (certificate.planeOk ? "true" : "false") << ",\n"
               << "        \"monotoneOk\": " << (certificate.monotoneOk ? "true" : "false") << ",\n"
               << "        \"verdict\": \"" << (certificate.pass ? "PASS" : "FAIL") << "\",\n"
               << "        \"lengthErrorMax\": " << certificate.lengthErrorMax << ",\n"
               << "        \"hingeDrift\": " << certificate.hingeDrift << ",\n"
               << "        \"hingeDriftRatio\": " << certificate.hingeDriftRatio << ",\n"
               << "        \"axisProjectionErrorMax\": " << certificate.axisProjectionErrorMax << ",\n"
               << "        \"bend01Degrees\": " << certificate.bend01Degrees << ",\n"
               << "        \"bend12Degrees\": " << certificate.bend12Degrees << ",\n"
               << "        \"zigzagSignChanges\": " << certificate.zigzagSignChanges << "\n"
               << "      }";
    }

    std::array<std::array<VrFingerDebugSample, static_cast<std::size_t>(FingerPoseGroup::Count)>, 2>
        sVrFingerDebugSamples;
    struct VrStaticHandDebugSample
    {
        bool valid = false;
        bool left = false;
        bool accepted = false;
        bool sharedBasis = false;
        std::string key;
        std::size_t vertices = 0;
        std::array<float, static_cast<std::size_t>(FingerPoseGroup::Count)> curls{};
        float growth = 0.f;
        float maxDisplacement = 0.f;
        float baseDiagonal = 0.f;
        bool worldBoundsValid = false;
        osg::Vec3f worldMin;
        osg::Vec3f worldMax;
        std::string basisSource;
    };

    std::vector<VrStaticHandDebugSample> sVrStaticHandDebugSamples;
    int sVrDebugSnapshotSerial = 0;
    bool sVrDebugSnapshotModeLogged = false;
    bool sVrDebugLastLeftSnapshot = false;
    bool sVrDebugLastRightSnapshot = false;
    int sVrDebugSnapshotFrameCount = 0;
    bool sVrDebugAutoSnapshotCaptured = false;

    void writeFingerSampleJson(std::ostream& stream, const VrFingerDebugSample& sample)
    {
        stream << "    {\n"
               << "      \"hand\": \"" << (sample.left ? "left" : "right") << "\",\n"
               << "      \"group\": \"" << getFingerPoseGroupName(sample.group) << "\",\n"
               << "      \"curl\": " << sample.curl << ",\n"
               << "      \"maxDegrees\": " << sample.maxDegrees << ",\n"
               << "      \"baseRoot\": " << jsonVec(sample.baseRoot) << ",\n"
               << "      \"baseMid\": " << jsonVec(sample.baseMid) << ",\n"
               << "      \"baseTip\": " << jsonVec(sample.baseTip) << ",\n"
               << "      \"baseEnd\": " << jsonVec(sample.baseEnd) << ",\n"
               << "      \"posedRoot\": " << jsonVec(sample.posedRoot) << ",\n"
               << "      \"posedMid\": " << jsonVec(sample.posedMid) << ",\n"
               << "      \"posedTip\": " << jsonVec(sample.posedTip) << ",\n"
               << "      \"posedEnd\": " << jsonVec(sample.posedEnd) << ",\n"
               << "      \"axis0\": " << jsonVec(sample.axis0) << ",\n"
               << "      \"axis1\": " << jsonVec(sample.axis1) << ",\n"
               << "      \"axis2\": " << jsonVec(sample.axis2) << ",\n"
               << "      \"rootRotation\": " << jsonQuat(sample.rootRotation) << ",\n"
               << "      \"midRotation\": " << jsonQuat(sample.midRotation) << ",\n"
               << "      \"tipRotation\": " << jsonQuat(sample.tipRotation) << ",\n";
        writeFingerCertificateJson(stream, makeFingerPoseCertificate(sample));
        stream << "\n    }";
    }

    void writeStaticHandSampleJson(std::ostream& stream, const VrStaticHandDebugSample& sample)
    {
        stream << "    {\n"
               << "      \"hand\": \"" << (sample.left ? "left" : "right") << "\",\n"
               << "      \"key\": " << jsonString(sample.key) << ",\n"
               << "      \"accepted\": " << (sample.accepted ? "true" : "false") << ",\n"
               << "      \"sharedBasis\": " << (sample.sharedBasis ? "true" : "false") << ",\n"
               << "      \"vertices\": " << sample.vertices << ",\n"
               << "      \"curls\": [";
        for (std::size_t i = 0; i < sample.curls.size(); ++i)
        {
            if (i != 0)
                stream << ",";
            stream << sample.curls[i];
        }
        stream << "],\n"
               << "      \"growth\": " << sample.growth << ",\n"
               << "      \"maxDisplacement\": " << sample.maxDisplacement << ",\n"
               << "      \"baseDiagonal\": " << sample.baseDiagonal << ",\n"
               << "      \"basisSource\": " << jsonString(sample.basisSource) << ",\n"
               << "      \"worldBoundsValid\": " << (sample.worldBoundsValid ? "true" : "false") << ",\n"
               << "      \"worldMin\": " << jsonVec(sample.worldMin) << ",\n"
               << "      \"worldMax\": " << jsonVec(sample.worldMax) << "\n"
               << "    }";
    }

    void storeVrStaticHandDebugSample(const VrStaticHandDebugSample& sample)
    {
        for (VrStaticHandDebugSample& existing : sVrStaticHandDebugSamples)
        {
            if (existing.left == sample.left && existing.vertices == sample.vertices)
            {
                existing = sample;
                return;
            }
        }

        sVrStaticHandDebugSamples.push_back(sample);
    }

    void storeVrFingerDebugSample(const VrFingerDebugSample& sample)
    {
        const std::size_t side = sample.left ? 0 : 1;
        const std::size_t group = static_cast<std::size_t>(sample.group);
        if (group >= static_cast<std::size_t>(FingerPoseGroup::Count))
            return;
        sVrFingerDebugSamples[side][group] = sample;

        if (!vrDebugRunningLogEnabled())
            return;

        try
        {
            std::filesystem::create_directories(vrDebugSnapshotDir());
            std::ofstream stream(vrDebugSnapshotDir() / "vr-debug-running.jsonl", std::ios::app);
            stream << "{ \"type\": \"finger\", \"timestamp\": \"" << vrDebugTimestamp() << "\", ";
            stream << "\"sample\": ";
            writeFingerSampleJson(stream, sample);
            stream << " }\n";
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: VR debug running log failed: " << e.what();
        }
    }

    bool readDebugSnapshotButton(const std::string& hand)
    {
        const std::optional<ActionSample> sample = getActionSample({ hand + "/input/thumbstick/click" }, true);
        return sample && sample->value > 0.5f;
    }

    void logVrDebugSnapshotModeOnce()
    {
        if (sVrDebugSnapshotModeLogged)
            return;
        if (!vrDebugSnapshotEnabled() && !vrDebugRunningLogEnabled())
            return;

        sVrDebugSnapshotModeLogged = true;
        Log(Debug::Verbose) << "FNV/ESM4 diag: VR debug capture modes snapshot="
                         << vrDebugSnapshotEnabled()
                         << " runningLog=" << vrDebugRunningLogEnabled()
                         << " image=true"
                         << " dir=" << vrDebugSnapshotDir().string();
    }

    void writeVrDebugSnapshot(const std::string& trigger)
    {
        if (!vrDebugSnapshotEnabled())
            return;

        const std::filesystem::path dir = vrDebugSnapshotDir();
        const int serial = ++sVrDebugSnapshotSerial;
        std::ostringstream baseName;
        baseName << "shot_" << vrDebugTimestamp() << "_" << std::setw(4) << std::setfill('0') << serial << "_"
                 << trigger;
        const std::filesystem::path jsonPath = dir / (baseName.str() + ".json");
        const std::filesystem::path imagePath = dir / (baseName.str() + ".png");

        bool imageRequested = true;
        bool imageWritten = false;
        try
        {
            std::filesystem::create_directories(dir);
            if (imageRequested)
            {
                osg::ref_ptr<osg::Image> image = new osg::Image;
                MWBase::Environment::get().getWorld()->screenshot(image.get(), 1280, 720);
                imageWritten = osgDB::writeImageFile(*image, imagePath.string());
            }

            std::ofstream stream(jsonPath);
            stream << "{\n"
                   << "  \"type\": \"vr-debug-snapshot\",\n"
                   << "  \"timestamp\": \"" << vrDebugTimestamp() << "\",\n"
                   << "  \"serial\": " << serial << ",\n"
                   << "  \"trigger\": \"" << trigger << "\",\n"
                   << "  \"imageRequested\": " << (imageRequested ? "true" : "false") << ",\n"
                   << "  \"imageWritten\": " << (imageWritten ? "true" : "false") << ",\n"
                   << "  \"image\": \"" << imagePath.filename().string() << "\",\n"
                   << "  \"flags\": {\n"
                   << "    \"snapshot\": " << (vrDebugSnapshotEnabled() ? "true" : "false") << ",\n"
                   << "    \"runningLog\": " << (vrDebugRunningLogEnabled() ? "true" : "false") << "\n"
                   << "  },\n"
                   << "  \"fingers\": [\n";

            bool first = true;
            for (const auto& handSamples : sVrFingerDebugSamples)
            {
                for (const VrFingerDebugSample& sample : handSamples)
                {
                    if (!sample.valid)
                        continue;
                    if (!first)
                        stream << ",\n";
                    first = false;
                    writeFingerSampleJson(stream, sample);
                }
            }

            stream << "\n  ],\n"
                   << "  \"staticHands\": [\n";

            first = true;
            for (const VrStaticHandDebugSample& sample : sVrStaticHandDebugSamples)
            {
                if (!sample.valid)
                    continue;
                if (!first)
                    stream << ",\n";
                first = false;
                writeStaticHandSampleJson(stream, sample);
            }

            stream << "\n  ]\n"
                   << "}\n";
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR debug snapshot trigger=" << trigger
                             << " json=" << jsonPath.string()
                             << " image=" << (imageWritten ? imagePath.string() : "not-written");
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "FNV/ESM4 diag: VR debug snapshot failed trigger=" << trigger
                                << " error=" << e.what();
        }
    }

    void updateVrDebugSnapshotControls()
    {
        logVrDebugSnapshotModeOnce();
        if (!vrDebugSnapshotEnabled())
            return;

        const int autoCaptureFrames
            = std::max(0, static_cast<int>(getEnvFloat("OPENMW_FNV_VR_DEBUG_SNAPSHOT_AUTO_FRAMES", 0.f)));
        if (!sVrDebugAutoSnapshotCaptured && autoCaptureFrames > 0
            && ++sVrDebugSnapshotFrameCount >= autoCaptureFrames)
        {
            sVrDebugAutoSnapshotCaptured = true;
            writeVrDebugSnapshot("auto_frame");
        }

        const bool leftDown = readDebugSnapshotButton("/user/hand/left");
        const bool rightDown = false;
        if (leftDown && !sVrDebugLastLeftSnapshot)
            writeVrDebugSnapshot("left_thumbstick_click");
        sVrDebugLastLeftSnapshot = leftDown;
        sVrDebugLastRightSnapshot = rightDown;
    }

    struct FingerBindingSpec
    {
        std::string boneSuffix;
        FingerPoseGroup group = FingerPoseGroup::Middle;
        int chainIndex = 0;
        bool indexFinger = false;
        FingerCurlSource source = FingerCurlSource::Grip;
        float scale = 1.f;
    };

    std::vector<FingerBindingSpec> getFalloutFingerBindingSpecs()
    {
        return {
            { "Thumb1", FingerPoseGroup::Thumb, 0, false, FingerCurlSource::Thumb, 0.6f },
            { "Thumb11", FingerPoseGroup::Thumb, 1, false, FingerCurlSource::Thumb, 1.f },
            { "Thumb12", FingerPoseGroup::Thumb, 2, false, FingerCurlSource::Thumb, 0.8f },
            { "Finger1", FingerPoseGroup::Index, 0, true, FingerCurlSource::Trigger, 0.2f },
            { "Finger11", FingerPoseGroup::Index, 1, true, FingerCurlSource::Trigger, 0.8f },
            { "Finger12", FingerPoseGroup::Index, 2, true, FingerCurlSource::Trigger, 0.6f },
            { "Finger2", FingerPoseGroup::Middle, 0, false, FingerCurlSource::Grip, 0.2f },
            { "Finger21", FingerPoseGroup::Middle, 1, false, FingerCurlSource::Grip, 0.8f },
            { "Finger22", FingerPoseGroup::Middle, 2, false, FingerCurlSource::Grip, 0.6f },
            { "Finger3", FingerPoseGroup::Ring, 0, false, FingerCurlSource::Grip, 0.2f },
            { "Finger31", FingerPoseGroup::Ring, 1, false, FingerCurlSource::Grip, 0.8f },
            { "Finger32", FingerPoseGroup::Ring, 2, false, FingerCurlSource::Grip, 0.6f },
            { "Finger4", FingerPoseGroup::Pinky, 0, false, FingerCurlSource::Grip, 0.2f },
            { "Finger41", FingerPoseGroup::Pinky, 1, false, FingerCurlSource::Grip, 0.8f },
            { "Finger42", FingerPoseGroup::Pinky, 2, false, FingerCurlSource::Grip, 0.6f },
        };
    }

    void logFingerSensorAudit(bool left)
    {
        if (getEnvFloat("OPENMW_FNV_VR_FINGER_SENSOR_LOG", 0.f) == 0.f)
            return;

        const int side = left ? 0 : 1;
        static int pollCounter[2] = { 0, 0 };
        static std::string lastLine[2];
        static int repeatedLineCount[2] = { 0, 0 };
        if ((++pollCounter[side] % 30) != 0)
            return;

        const std::string hand = left ? "/user/hand/left" : "/user/hand/right";
        std::ostringstream stream;
        for (const std::string& id : getFingerSensorIds(hand))
        {
            const std::optional<ActionSample> sample = getActionSample({ id });
            if (!sample || std::abs(sample->value) <= 0.001f)
                continue;

            stream << " " << id.substr(hand.size()) << "=" << sample->value;
            if (sample->boolean)
                stream << "b";
        }

        const std::string line = stream.str();
        if (line.empty())
            return;

        if (line == lastLine[side])
        {
            if ((++repeatedLineCount[side] % 120) != 0)
                return;
        }
        else
        {
            lastLine[side] = line;
            repeatedLineCount[side] = 0;
        }

        Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger sensors hand=" << (left ? "left" : "right") << line;
    }

    class DirectHandFingerPoseState
    {
    public:
        explicit DirectHandFingerPoseState(bool left)
            : mLeft(left)
        {
        }

        void captureBase(const FingerBindingSpec& spec, const osg::Matrixf& matrix)
        {
            if (spec.chainIndex < 0 || spec.chainIndex >= 3)
                return;

            BonePose& bone = mBones[groupIndex(spec.group)][spec.chainIndex];
            if (bone.captured)
                return;

            bone.captured = true;
            bone.scale = spec.scale;
            bone.baseMatrix = matrix;
            bone.baseRotation = matrix.getRotate();
            bone.baseTrans = matrix.getTrans();
        }

        bool makePose(const FingerBindingSpec& spec, float curl, float maxDegrees, osg::Matrixf& matrix)
        {
            if (getEnvFloat("OPENMW_FNV_VR_FINGER_CHAIN_POSE", 1.f) == 0.f)
                return false;
            if (spec.chainIndex < 0 || spec.chainIndex >= 3)
                return false;

            const int group = groupIndex(spec.group);
            if (!chainReady(group))
                return false;

            const BonePose& root = mBones[group][0];
            const BonePose& mid = mBones[group][1];
            const BonePose& tip = mBones[group][2];

            const osg::Matrixf rootBaseMatrix = root.baseMatrix;
            const osg::Matrixf midBaseMatrix = mid.baseMatrix * rootBaseMatrix;
            const osg::Matrixf tipBaseMatrix = tip.baseMatrix * midBaseMatrix;
            const osg::Vec3f rootBaseTrans = rootBaseMatrix.getTrans();
            const osg::Vec3f midBaseTrans = midBaseMatrix.getTrans();
            const osg::Vec3f tipBaseTrans = tipBaseMatrix.getTrans();

            const osg::Vec3f segment0 = midBaseTrans - rootBaseTrans;
            const osg::Vec3f segment1 = tipBaseTrans - midBaseTrans;
            if (segment0.length2() <= 1e-5f || segment1.length2() <= 1e-5f)
                return false;

            const osg::Vec3f curlAngles = getFingerCurlAnglesDegrees(spec.group);
            const osg::Vec3f baseEnd = tipBaseTrans + segment1;

            const osg::Vec3f curlAxis = chainCurlAxis(segment0, segment1);
            const float baseDirection = getFingerCurlDirection(mLeft, spec.group);
            struct PoseCandidate
            {
                float direction = 1.f;
                osg::Vec3f posedRoot;
                osg::Vec3f posedMid;
                osg::Vec3f posedTip;
                osg::Vec3f posedEnd;
                osg::Vec3f posedSegment0;
                osg::Vec3f posedSegment1;
                osg::Vec3f posedSegment2;
                osg::Quat rootRotation;
                osg::Quat midRotation;
                osg::Quat tipRotation;
                VrFingerPoseCertificate certificate;
            };

            const auto makeCandidate = [&](float direction) {
                PoseCandidate candidate;
                candidate.direction = direction;
                const float angle0 = osg::DegreesToRadians(curlAngles.x() * curl * direction);
                const float angle1 = osg::DegreesToRadians(curlAngles.y() * curl * direction);
                const float angle2 = osg::DegreesToRadians(curlAngles.z() * curl * direction);
                const osg::Quat q0(angle0, curlAxis);
                const osg::Quat q1(angle1, curlAxis);
                const osg::Quat q2(angle2, curlAxis);

                const osg::Quat rootDelta = q0;
                const osg::Quat midDelta = q1 * q0;
                const osg::Quat tipDelta = q2 * q1 * q0;
                candidate.posedRoot = rootBaseTrans;
                candidate.posedSegment0 = rootDelta * segment0;
                candidate.posedSegment1 = midDelta * segment1;
                candidate.posedSegment2 = tipDelta * segment1;
                candidate.posedMid = candidate.posedRoot + candidate.posedSegment0;
                candidate.posedTip = candidate.posedMid + candidate.posedSegment1;
                candidate.posedEnd = candidate.posedTip + candidate.posedSegment2;
                candidate.rootRotation
                    = alignSegmentRotation(segment0, candidate.posedSegment0, root.baseRotation);
                candidate.midRotation = alignSegmentRotation(segment1, candidate.posedSegment1, mid.baseRotation);
                candidate.tipRotation = alignSegmentRotation(segment1, candidate.posedSegment2, tip.baseRotation);
                candidate.certificate = makeFingerPoseCertificate({ true, mLeft, spec.group, curl, maxDegrees,
                    rootBaseTrans, midBaseTrans, tipBaseTrans, baseEnd, candidate.posedRoot, candidate.posedMid,
                    candidate.posedTip, candidate.posedEnd, curlAxis, curlAxis, curlAxis, candidate.rootRotation,
                    candidate.midRotation, candidate.tipRotation });
                return candidate;
            };

            PoseCandidate pose = makeCandidate(baseDirection);
            if (!mLeft)
            {
                const PoseCandidate mirroredPose = makeCandidate(-baseDirection);
                if ((!pose.certificate.pass && mirroredPose.certificate.pass)
                    || (pose.certificate.zigzagSignChanges > mirroredPose.certificate.zigzagSignChanges))
                {
                    pose = mirroredPose;
                }
            }

            logChainPoseDetailOnce(spec.group, curl, maxDegrees, rootBaseTrans, midBaseTrans, tipBaseTrans,
                baseEnd, pose.posedRoot, pose.posedMid, pose.posedTip, pose.posedEnd, curlAxis, curlAxis, curlAxis);

            storeVrFingerDebugSample({ true, mLeft, spec.group, curl, maxDegrees, rootBaseTrans, midBaseTrans,
                tipBaseTrans, baseEnd, pose.posedRoot, pose.posedMid, pose.posedTip, pose.posedEnd, curlAxis,
                curlAxis, curlAxis, pose.rootRotation, pose.midRotation, pose.tipRotation });

            osg::Vec3f translation;
            osg::Quat rotation;
            if (spec.chainIndex == 0)
            {
                matrix = root.baseMatrix;
                translation = root.baseTrans;
                rotation = pose.rootRotation;
            }
            else if (spec.chainIndex == 1)
            {
                matrix = mid.baseMatrix;
                translation = mid.baseTrans;
                rotation = pose.midRotation;
            }
            else
            {
                matrix = tip.baseMatrix;
                translation = tip.baseTrans;
                rotation = pose.tipRotation;
            }

            matrix.setRotate(rotation);
            matrix.setTrans(translation);
            logChainReadyOnce(spec.group, segment0, segment1, curlAxis, curlAxis, curlAxis);
            return true;
        }

    private:
        struct BonePose
        {
            bool captured = false;
            float scale = 1.f;
            osg::Matrixf baseMatrix;
            osg::Quat baseRotation;
            osg::Vec3f baseTrans;
        };

        static int groupIndex(FingerPoseGroup group)
        {
            return static_cast<int>(group);
        }

        bool chainReady(int group) const
        {
            if (group < 0 || group >= groupIndex(FingerPoseGroup::Count))
                return false;
            return mBones[group][0].captured && mBones[group][1].captured && mBones[group][2].captured;
        }

        osg::Vec3f chainCurlAxis(const osg::Vec3f& segment0, const osg::Vec3f& segment1) const
        {
            return normalizeOr(segment0 ^ segment1, osg::Vec3f(0.f, 1.f, 0.f));
        }

        osg::Quat alignSegmentRotation(
            const osg::Vec3f& baseSegment, const osg::Vec3f& posedSegment, const osg::Quat& baseRotation) const
        {
            const osg::Vec3f from = normalizeOr(baseSegment, osg::Vec3f(0.f, 0.f, 1.f));
            const osg::Vec3f to = normalizeOr(posedSegment, from);
            osg::Quat swing;
            swing.makeRotate(from, to);
            return swing * baseRotation;
        }

        void logChainReadyOnce(FingerPoseGroup group, const osg::Vec3f& segment0, const osg::Vec3f& segment1,
            const osg::Vec3f& axis0, const osg::Vec3f& axis1, const osg::Vec3f& axis2)
        {
            const int index = groupIndex(group);
            if (index < 0 || index >= groupIndex(FingerPoseGroup::Count) || mLoggedChain[index])
                return;

            mLoggedChain[index] = true;
            if (getEnvFloat("OPENMW_FNV_VR_FINGER_CHAIN_LOG", 1.f) == 0.f)
                return;

            Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger chain pose ready hand=" << (mLeft ? "left" : "right")
                             << " group=" << getFingerPoseGroupName(group)
                             << " rootLen=" << segment0.length()
                             << " midLen=" << segment1.length()
                             << " axis0=(" << axis0.x() << "," << axis0.y() << "," << axis0.z() << ")"
                             << " axis1=(" << axis1.x() << "," << axis1.y() << "," << axis1.z() << ")"
                             << " axis2=(" << axis2.x() << "," << axis2.y() << "," << axis2.z() << ")";
        }

        void logChainPoseDetailOnce(FingerPoseGroup group, float curl, float maxDegrees, const osg::Vec3f& baseRoot,
            const osg::Vec3f& baseMid, const osg::Vec3f& baseTip, const osg::Vec3f& baseEnd,
            const osg::Vec3f& posedRoot, const osg::Vec3f& posedMid, const osg::Vec3f& posedTip,
            const osg::Vec3f& posedEnd, const osg::Vec3f& axis0, const osg::Vec3f& axis1,
            const osg::Vec3f& axis2)
        {
            const int index = groupIndex(group);
            if (index < 0 || index >= groupIndex(FingerPoseGroup::Count) || mLoggedPoseDetail[index])
                return;
            if (getEnvFloat("OPENMW_FNV_VR_FINGER_CHAIN_POSE_DETAIL_LOG", 1.f) == 0.f || curl < 0.45f)
                return;

            mLoggedPoseDetail[index] = true;
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger chain pose detail hand=" << (mLeft ? "left" : "right")
                             << " group=" << getFingerPoseGroupName(group)
                             << " curl=" << curl
                             << " maxDegrees=" << maxDegrees
                             << " baseRoot=(" << baseRoot.x() << "," << baseRoot.y() << "," << baseRoot.z() << ")"
                             << " baseMid=(" << baseMid.x() << "," << baseMid.y() << "," << baseMid.z() << ")"
                             << " baseTip=(" << baseTip.x() << "," << baseTip.y() << "," << baseTip.z() << ")"
                             << " baseEnd=(" << baseEnd.x() << "," << baseEnd.y() << "," << baseEnd.z() << ")"
                             << " posedRoot=(" << posedRoot.x() << "," << posedRoot.y() << "," << posedRoot.z()
                             << ")"
                             << " posedMid=(" << posedMid.x() << "," << posedMid.y() << "," << posedMid.z() << ")"
                             << " posedTip=(" << posedTip.x() << "," << posedTip.y() << "," << posedTip.z() << ")"
                             << " posedEnd=(" << posedEnd.x() << "," << posedEnd.y() << "," << posedEnd.z() << ")"
                             << " axis0=(" << axis0.x() << "," << axis0.y() << "," << axis0.z() << ")"
                             << " axis1=(" << axis1.x() << "," << axis1.y() << "," << axis1.z() << ")"
                             << " axis2=(" << axis2.x() << "," << axis2.y() << "," << axis2.z() << ")";
        }

        bool mLeft = false;
        std::array<std::array<BonePose, 3>, static_cast<std::size_t>(FingerPoseGroup::Count)> mBones;
        std::array<bool, static_cast<std::size_t>(FingerPoseGroup::Count)> mLoggedChain = {};
        std::array<bool, static_cast<std::size_t>(FingerPoseGroup::Count)> mLoggedPoseDetail = {};
    };

    /// Implements control of a finger by overriding rotation
    class FingerController : public osg::NodeCallback
    {
    public:
        FingerController(bool left, const FingerBindingSpec& spec, SceneUtil::Skeleton* skeleton = nullptr,
            std::shared_ptr<DirectHandFingerPoseState> directPoseState = nullptr)
            : mLeft(left)
            , mSpec(spec)
            , mSkeleton(skeleton)
            , mDirectPoseState(std::move(directPoseState))
        {
        }

        void setEnabled(bool enabled) { mPointingMode = enabled && mSpec.indexFinger; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        float getCurl() const;

    private:
        bool mLeft = false;
        FingerBindingSpec mSpec;
        SceneUtil::Skeleton* mSkeleton = nullptr;
        std::shared_ptr<DirectHandFingerPoseState> mDirectPoseState;
        bool mPointingMode = false;
        bool mHaveBaseMatrix = false;
        osg::Matrixf mBaseMatrix;
        osg::Quat mBaseRotation;
        mutable int mCurlLogCount = 0;
        mutable int mCurlLogFrame = 0;
        mutable int mApplyLogCount = 0;
        mutable int mApplyLogFrame = 0;
    };

    float FingerController::getCurl() const
    {
        if (getEnvFloat("OPENMW_FNV_VR_FINGER_CURL_ENABLE", 1.f) == 0.f)
            return 0.f;

        const float forcedCurl = getEnvFloat("OPENMW_FNV_VR_FINGER_CURL_FORCE", 0.f);
        if (forcedCurl > 0.f)
            return clampUnit(forcedCurl);

        const std::string hand = mLeft ? "/user/hand/left" : "/user/hand/right";
        logFingerSensorAudit(mLeft);
        std::optional<ActionSample> curl;
        bool usedGripFallback = false;
        if (mSpec.source == FingerCurlSource::Grip)
        {
            curl = getActionSample(getGripCurlActionIds(hand), true);
            if (!curl && getEnvFloat("OPENMW_FNV_VR_GRIP_FALLBACK_TO_TRIGGER", 1.f) != 0.f)
            {
                curl = getActionSample(getGripFallbackCurlActionIds(hand), true);
                usedGripFallback = static_cast<bool>(curl);
            }
        }
        else
            curl = getActionSample(getCurlActionIds(mSpec.source, hand), true);

        const float value = curl ? curl->value : 0.f;
        const bool shouldLog = getEnvFloat("OPENMW_FNV_VR_FINGER_CURL_LOG", 0.f) != 0.f
            && ((value > 0.01f && mCurlLogCount < 40) || mCurlLogCount < 20 || (++mCurlLogFrame % 300) == 0);
        if (shouldLog)
        {
            ++mCurlLogCount;
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger curl hand=" << (mLeft ? "left" : "right")
                             << " source=" << getFingerCurlSourceName(mSpec.source)
                             << " group=" << getFingerPoseGroupName(mSpec.group)
                             << " chainIndex=" << mSpec.chainIndex
                             << " indexFinger=" << mSpec.indexFinger
                             << " action=" << (curl ? curl->id : "<none>")
                             << " value=" << value
                             << " scale=" << mSpec.scale
                             << " gripFallback=" << usedGripFallback;
        }

        return value;
    }

    void FingerController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        auto transform = node != nullptr ? node->asTransform() : nullptr;
        auto matrixTransform = transform != nullptr ? transform->asMatrixTransform() : nullptr;
        if (matrixTransform == nullptr)
        {
            traverse(node, nv);
            return;
        }

        if (!mHaveBaseMatrix)
        {
            mBaseMatrix = matrixTransform->getMatrix();
            mBaseRotation = mBaseMatrix.getRotate();
            if (mDirectPoseState)
                mDirectPoseState->captureBase(mSpec, mBaseMatrix);
            mHaveBaseMatrix = true;
        }

        const bool curlEnabled = getEnvFloat("OPENMW_FNV_VR_FINGER_CURL_ENABLE", 1.f) != 0.f;
        const float curl = curlEnabled ? getCurl() : 0.f;
        const bool holdPointingPose = mPointingMode && curl <= 0.01f;

        bool overrideRotation = false;
        if (holdPointingPose)
        {
            // Keep the authored rig pose. Pointing can still suppress nested hand animation without
            // replacing a phalanx's bind rotation with identity.
            auto matrix = mBaseMatrix;
            matrix.setRotate(mBaseRotation);
            matrixTransform->setMatrix(matrix);
            if (mSkeleton)
                mSkeleton->markBoneMatriceDirty();
            overrideRotation = true;
        }
        else if (curlEnabled)
        {
            float maxDegrees = getEnvFloat("OPENMW_FNV_VR_GRIP_CURL_DEGREES", 68.f);
            if (mSpec.source == FingerCurlSource::Thumb)
                maxDegrees = getEnvFloat("OPENMW_FNV_VR_THUMB_CURL_DEGREES", 42.f);
            else if (mSpec.source == FingerCurlSource::Trigger)
                maxDegrees = getEnvFloat("OPENMW_FNV_VR_INDEX_CURL_DEGREES", 55.f);
            osg::Matrixf matrix;
            bool usedChainPose = false;
            const bool directPoseOnly = mDirectPoseState != nullptr;
            if (directPoseOnly)
            {
                osg::Matrixf chainProofMatrix;
                usedChainPose = mDirectPoseState->makePose(mSpec, curl, maxDegrees, chainProofMatrix);
                matrix = mBaseMatrix;
            }

            osg::Vec3f localAxis(0.f, 0.f, 0.f);
            osg::Vec3f axis(0.f, 0.f, 0.f);
            float angleDegrees = 0.f;
            if (!directPoseOnly)
            {
                matrix = mBaseMatrix;
            }
            matrixTransform->setMatrix(matrix);
            if (mSkeleton)
                mSkeleton->markBoneMatriceDirty();
            const bool shouldLogApply = getEnvFloat("OPENMW_FNV_VR_FINGER_APPLY_LOG", 0.f) != 0.f
                && ((std::abs(curl) > 0.01f && mApplyLogCount < 40) || (++mApplyLogFrame % 300) == 0);
            if (shouldLogApply)
            {
                ++mApplyLogCount;
                Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger apply node="
                                 << (node != nullptr ? node->getName() : std::string("<null>"))
                                 << " hand=" << (mLeft ? "left" : "right")
                                 << " context=" << (mSkeleton != nullptr ? "trackedSkeleton" : "directWrapper")
                                 << " source=" << getFingerCurlSourceName(mSpec.source)
                                 << " group=" << getFingerPoseGroupName(mSpec.group)
                                 << " chainIndex=" << mSpec.chainIndex
                                 << " pointingMode=" << mPointingMode
                                 << " chainPose=" << usedChainPose
                                 << " curl=" << curl
                                 << " angleDegrees=" << angleDegrees
                                 << " localAxis=(" << localAxis.x() << "," << localAxis.y() << ","
                                 << localAxis.z() << ")"
                                 << " parentAxis=(" << axis.x() << "," << axis.y() << "," << axis.z() << ")";
            }
            overrideRotation = true;
        }

        if (overrideRotation)
        {
            // Omit nested callbacks to override animations of this node.
            osg::ref_ptr<osg::Callback> ncb = getNestedCallback();
            setNestedCallback(nullptr);
            traverse(node, nv);
            setNestedCallback(ncb);
        }
        else
            traverse(node, nv);
    }

    class StaticHandFingerDeformCallback : public osg::Drawable::UpdateCallback
    {
    public:
        StaticHandFingerDeformCallback(
            bool left, std::string sharedBasisKey, std::array<std::vector<float>, 15>&& fingerBoneWeights)
            : mLeft(left)
            , mSharedBasisKey(std::move(sharedBasisKey))
            , mFingerBoneWeights(std::move(fingerBoneWeights))
        {
        }

        void update(osg::NodeVisitor*, osg::Drawable* drawable) override
        {
            osg::Geometry* geometry = dynamic_cast<osg::Geometry*>(drawable);
            if (geometry == nullptr)
                return;

            osg::Vec3Array* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
            if (vertices == nullptr || vertices->empty())
                return;

            if (mBaseVertices.size() != vertices->size())
                captureBaseVertices(*vertices);
            if (mBaseVertices.empty() || !mBounds.valid() || !fingerBoneWeightsReady())
                return;

            updateSharedBindBasisState();
            const auto transforms = makeFingerTransforms(activeSharedBindBasis());

            bool changed = false;
            const std::vector<osg::Vec3f> nextVertices = skinVertices(transforms);
            for (std::size_t i = 0; i < nextVertices.size(); ++i)
            {
                const osg::Vec3f& updated = nextVertices[i];
                changed = changed || ((updated - (*vertices)[i]).length2() > 0.000001f);
            }

            const StageSafety stageSafety = evaluateStageSafety(nextVertices);
            logStaticStageOnce(*drawable, nextVertices, stageSafety);
            if (!stageSafety.safe)
                return;

            for (std::size_t i = 0; i < nextVertices.size(); ++i)
                (*vertices)[i] = nextVertices[i];

            if (changed)
            {
                vertices->dirty();
                geometry->dirtyDisplayList();
                geometry->dirtyBound();
            }

            if (!mLogged)
            {
                mLogged = true;
                Log(Debug::Verbose) << "FNV/ESM4 diag: VR static hand finger deform attached hand="
                                 << (mLeft ? "left" : "right") << " vertices=" << vertices->size()
                                 << " weightedSlots=" << countWeightedSlots()
                                 << " weightedVertices=" << countWeightedVertices();
            }
        }

    private:
        struct FingerBoneTransform
        {
            osg::Vec3f baseJoint;
            osg::Vec3f posedJoint;
            osg::Quat swing;

            osg::Vec3f apply(const osg::Vec3f& point) const { return posedJoint + swing * (point - baseJoint); }
        };

        struct FingerBindChain
        {
            osg::Vec3f root;
            osg::Vec3f mid;
            osg::Vec3f tip;
            osg::Vec3f end;
        };

        struct SharedBindBasis
        {
            bool valid = false;
            bool fullCurlSafe = false;
            std::size_t completeChainCount = 0;
            float score = -1.f;
            osg::Vec3f palm;
            std::array<bool, 5> chainValid{};
            std::array<FingerBindChain, 5> chains;
        };

        static std::map<std::string, SharedBindBasis>& sharedBindBasisCache()
        {
            static std::map<std::string, SharedBindBasis> cache;
            return cache;
        }

        static std::optional<SharedBindBasis>& mirroredLeftBindBasisCache()
        {
            static std::optional<SharedBindBasis> cache;
            return cache;
        }

        static osg::Vec3f mirrorBindPointX(const osg::Vec3f& point)
        {
            return osg::Vec3f(-point.x(), point.y(), point.z());
        }

        static SharedBindBasis mirrorBindBasisX(const SharedBindBasis& source)
        {
            SharedBindBasis mirrored = source;
            mirrored.palm = mirrorBindPointX(source.palm);
            for (std::size_t groupIndex = 0; groupIndex < mirrored.chains.size(); ++groupIndex)
            {
                if (!source.chainValid[groupIndex])
                    continue;

                mirrored.chains[groupIndex].root = mirrorBindPointX(source.chains[groupIndex].root);
                mirrored.chains[groupIndex].mid = mirrorBindPointX(source.chains[groupIndex].mid);
                mirrored.chains[groupIndex].tip = mirrorBindPointX(source.chains[groupIndex].tip);
                mirrored.chains[groupIndex].end = mirrorBindPointX(source.chains[groupIndex].end);
            }
            mirrored.score = source.score + 50000.f;
            return mirrored;
        }

        bool fingerBoneWeightsReady() const
        {
            for (const auto& weights : mFingerBoneWeights)
            {
                if (weights.size() != mBaseVertices.size())
                    return false;
            }
            return true;
        }

        struct StageSafety
        {
            bool safe = false;
            float baseDiagonal = 0.f;
            float stagedDiagonal = 0.f;
            float boundsGrowth = 0.f;
            float maxDisplacement = 0.f;
        };

        StageSafety evaluateStageSafety(const std::vector<osg::Vec3f>& vertices) const
        {
            StageSafety result;
            if (vertices.size() != mBaseVertices.size())
                return result;

            osg::BoundingBox bounds;
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                const osg::Vec3f& vertex = vertices[i];
                if (!finiteVec3(vertex))
                    return result;

                bounds.expandBy(vertex);
                result.maxDisplacement = std::max(result.maxDisplacement, (vertex - mBaseVertices[i]).length());
            }

            result.baseDiagonal = (mBounds._max - mBounds._min).length();
            result.stagedDiagonal = (bounds._max - bounds._min).length();
            if (result.baseDiagonal <= 1e-5f || !std::isfinite(result.stagedDiagonal)
                || !std::isfinite(result.maxDisplacement))
                return result;

            result.boundsGrowth = result.stagedDiagonal / result.baseDiagonal;
            result.safe = result.stagedDiagonal <= result.baseDiagonal * 1.35f
                && result.maxDisplacement <= result.baseDiagonal * 0.65f;
            return result;
        }

        bool stagedVerticesSafe(const std::vector<osg::Vec3f>& vertices) const
        {
            return evaluateStageSafety(vertices).safe;
        }

        const SharedBindBasis* activeSharedBindBasis() const
        {
            if (mUseMirroredLeftBindBasis)
            {
                const std::optional<SharedBindBasis>& mirrored = mirroredLeftBindBasisCache();
                if (mirrored && mirrored->valid)
                    return &*mirrored;
            }

            if (!mUseSharedBindBasis || mSharedBasisKey.empty())
                return nullptr;

            const auto found = sharedBindBasisCache().find(mSharedBasisKey);
            if (found == sharedBindBasisCache().end() || !found->second.valid)
                return nullptr;

            return &found->second;
        }

        std::array<FingerBoneTransform, 15> makeFingerTransforms(
            const SharedBindBasis* sharedBasis, std::optional<float> forcedCurl = std::nullopt) const
        {
            std::array<FingerBoneTransform, 15> transforms;
            for (FingerBoneTransform& transform : transforms)
                transform = identityTransform();

            const std::size_t side = mLeft ? 0 : 1;
            for (std::size_t groupIndex = 0; groupIndex < static_cast<std::size_t>(FingerPoseGroup::Count);
                 ++groupIndex)
            {
                const VrFingerDebugSample& sample = sVrFingerDebugSamples[side][groupIndex];
                const std::size_t slot = groupIndex * 3;
                const FingerPoseGroup group = static_cast<FingerPoseGroup>(groupIndex);
                const float sensorCurl = forcedCurl ? *forcedCurl : sample.curl;
                float curl = sensorCurl;
                float open = 0.f;
                if (!forcedCurl && group == FingerPoseGroup::Thumb)
                {
                    curl = sensorCurl * gripCurlAmount(side);
                    open = clampUnit(1.f - sensorCurl);
                }
                if ((!forcedCurl && !sample.valid) || (curl <= 0.001f && open <= 0.001f))
                    continue;

                FingerBindChain chain;
                if (sharedBasis != nullptr && sharedBasis->chainValid[groupIndex])
                    chain = sharedBasis->chains[groupIndex];
                else if (!deriveFingerBindChain(slot, chain))
                    continue;

                const osg::Vec3f root = chain.root;
                const osg::Vec3f mid = chain.mid;
                const osg::Vec3f tip = chain.tip;
                const osg::Vec3f end = chain.end;
                const osg::Vec3f segment0 = mid - root;
                const osg::Vec3f segment1 = tip - mid;
                const osg::Vec3f segment2 = end - tip;
                if (segment0.length2() <= 1e-5f || segment1.length2() <= 1e-5f || segment2.length2() <= 1e-5f)
                    continue;

                const osg::Vec3f fingerDirection = normalizeOr(end - root, normalizeOr(segment0, osg::Vec3f(1.f, 0.f, 0.f)));
                const osg::Vec3f palm = sharedBasis != nullptr ? sharedBasis->palm : palmCenter();
                osg::Vec3f palmDirection = palm - root;
                palmDirection = normalizeOr(palmDirection - fingerDirection * (palmDirection * fingerDirection), -fingerDirection);
                if ((fingerDirection ^ palmDirection).length2() <= 1e-5f)
                    palmDirection = normalizeOr(osg::Vec3f(0.f, mLeft ? 1.f : -1.f, 0.f), -fingerDirection);

                osg::Vec3f hingeAxis = chooseCurlAxis(fingerDirection, palmDirection);
                if (group != FingerPoseGroup::Thumb)
                {
                    const std::optional<osg::Vec3f> gripAxis = averageGripCurlAxis(sharedBasis);
                    if (gripAxis && (*gripAxis * hingeAxis) < 0.f)
                        hingeAxis = -*gripAxis;
                    else if (gripAxis)
                        hingeAxis = *gripAxis;
                }
                const osg::Vec3f curlAngles = getFingerCurlAnglesDegrees(group);
                const osg::Vec3f openAngles = getFingerOpenAnglesDegrees(group);
                const float curlAngle0 = osg::DegreesToRadians(curlAngles.x() * curl);
                const float curlAngle1 = osg::DegreesToRadians(curlAngles.y() * curl);
                const float curlAngle2 = osg::DegreesToRadians(curlAngles.z() * curl);
                const float openAngle0 = osg::DegreesToRadians(openAngles.x() * open);
                const float openAngle1 = osg::DegreesToRadians(openAngles.y() * open);
                const float openAngle2 = osg::DegreesToRadians(openAngles.z() * open);
                const float sign = chooseCurlSign(fingerDirection, palmDirection, hingeAxis,
                    std::max(curlAngle0, osg::DegreesToRadians(1.f)));
                const osg::Quat q0((curlAngle0 - openAngle0) * sign, hingeAxis);
                const osg::Quat q1((curlAngle1 - openAngle1) * sign, hingeAxis);
                const osg::Quat q2((curlAngle2 - openAngle2) * sign, hingeAxis);
                const osg::Quat rootDelta = q0;
                const osg::Quat midDelta = q1 * q0;
                const osg::Quat tipDelta = q2 * q1 * q0;

                const osg::Vec3f posedRoot = root;
                const osg::Vec3f posedMid = posedRoot + rootDelta * segment0;
                const osg::Vec3f posedTip = posedMid + midDelta * segment1;
                const osg::Vec3f posedEnd = posedTip + tipDelta * segment2;
                if (!safeFingerPose(root, mid, tip, end, posedRoot, posedMid, posedTip, posedEnd))
                    continue;

                transforms[slot + 0] = makeTransform(root, mid, posedRoot, posedMid);
                transforms[slot + 1] = makeTransform(mid, tip, posedMid, posedTip);
                transforms[slot + 2] = makeTransform(tip, end, posedTip, posedEnd);
            }
            return transforms;
        }

        std::vector<osg::Vec3f> skinVertices(const std::array<FingerBoneTransform, 15>& transforms) const
        {
            std::vector<osg::Vec3f> nextVertices(mBaseVertices.size());
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                const osg::Vec3f& base = mBaseVertices[i];
                float totalWeight = 0.f;
                osg::Vec3f skinned;
                for (std::size_t slot = 0; slot < mFingerBoneWeights.size(); ++slot)
                {
                    const float weight = mFingerBoneWeights[slot][i];
                    if (weight <= 0.001f)
                        continue;

                    totalWeight += weight;
                    skinned += transforms[slot].apply(base) * weight;
                }

                if (totalWeight <= 0.001f)
                    nextVertices[i] = base;
                else if (totalWeight > 1.f)
                    nextVertices[i] = skinned * (1.f / totalWeight);
                else
                    nextVertices[i] = skinned + base * (1.f - totalWeight);
            }
            return nextVertices;
        }

        std::optional<osg::BoundingBox> computeWorldBounds(
            osg::Drawable& drawable, const std::vector<osg::Vec3f>& vertices) const
        {
            if (drawable.getNumParents() == 0)
                return std::nullopt;

            osg::Node* parent = drawable.getParent(0);
            if (parent == nullptr || parent->getParentalNodePaths().empty())
                return std::nullopt;

            const osg::Matrix localToWorld = osg::computeLocalToWorld(parent->getParentalNodePaths().front());
            osg::BoundingBox bounds;
            for (const osg::Vec3f& vertex : vertices)
                bounds.expandBy(vertex * localToWorld);
            if (!bounds.valid())
                return std::nullopt;

            return bounds;
        }

        void logStaticStageOnce(
            osg::Drawable& drawable, const std::vector<osg::Vec3f>& nextVertices, const StageSafety& stageSafety)
        {
            const std::size_t side = mLeft ? 0 : 1;
            float curls[5] = {};
            bool anyCurl = false;
            for (std::size_t groupIndex = 0; groupIndex < static_cast<std::size_t>(FingerPoseGroup::Count);
                 ++groupIndex)
            {
                curls[groupIndex] = sVrFingerDebugSamples[side][groupIndex].valid
                    ? sVrFingerDebugSamples[side][groupIndex].curl
                    : 0.f;
                anyCurl = anyCurl || curls[groupIndex] > 0.01f;
            }

            VrStaticHandDebugSample sample;
            sample.valid = true;
            sample.left = mLeft;
            sample.accepted = stageSafety.safe;
            sample.sharedBasis = mUseSharedBindBasis || mUseMirroredLeftBindBasis;
            sample.key = mUseMirroredLeftBindBasis ? "right|mirrored-left-static-basis" : mSharedBasisKey;
            sample.basisSource = mUseMirroredLeftBindBasis ? "mirroredLeft" : (mUseSharedBindBasis ? "shared" : "local");
            sample.vertices = mBaseVertices.size();
            for (std::size_t groupIndex = 0; groupIndex < sample.curls.size(); ++groupIndex)
                sample.curls[groupIndex] = curls[groupIndex];
            sample.growth = stageSafety.boundsGrowth;
            sample.maxDisplacement = stageSafety.maxDisplacement;
            sample.baseDiagonal = stageSafety.baseDiagonal;
            if (const std::optional<osg::BoundingBox> worldBounds = computeWorldBounds(drawable, nextVertices))
            {
                sample.worldBoundsValid = true;
                sample.worldMin = worldBounds->_min;
                sample.worldMax = worldBounds->_max;
            }
            storeVrStaticHandDebugSample(sample);

            if (!anyCurl && stageSafety.safe)
                return;
            if (mLoggedStageAccepted && stageSafety.safe)
                return;
            if (mLoggedStageRejected && !stageSafety.safe)
                return;

            if (stageSafety.safe)
                mLoggedStageAccepted = true;
            else
                mLoggedStageRejected = true;

            Log(Debug::Verbose) << "FNV/ESM4 diag: VR static hand stage " << (stageSafety.safe ? "accepted" : "rejected")
                             << " hand=" << (mLeft ? "left" : "right")
                             << " key=" << mSharedBasisKey
                             << " sharedBasis=" << mUseSharedBindBasis
                             << " curls=(" << curls[0] << "," << curls[1] << "," << curls[2] << ","
                             << curls[3] << "," << curls[4] << ")"
                             << " growth=" << stageSafety.boundsGrowth
                             << " maxDisplacement=" << stageSafety.maxDisplacement
                             << " baseDiagonal=" << stageSafety.baseDiagonal
                             << " vertices=" << mBaseVertices.size();
        }

        FingerBoneTransform identityTransform() const
        {
            return { osg::Vec3f(), osg::Vec3f(), osg::Quat() };
        }

        FingerBoneTransform makeTransform(const osg::Vec3f& baseJoint, const osg::Vec3f& baseChild,
            const osg::Vec3f& posedJoint, const osg::Vec3f& posedChild) const
        {
            const osg::Vec3f baseSegment = normalizeOr(baseChild - baseJoint, osg::Vec3f(0.f, 0.f, 1.f));
            osg::Quat swing;
            swing.makeRotate(
                baseSegment,
                normalizeOr(posedChild - posedJoint, baseSegment));
            return { baseJoint, posedJoint, swing };
        }

        bool deriveFingerBindChain(std::size_t slot, FingerBindChain& chain) const
        {
            osg::Vec3f center0;
            osg::Vec3f center1;
            osg::Vec3f center2;
            const bool hasCenter0 = weightedCenter(slot + 0, center0);
            const bool hasCenter1 = weightedCenter(slot + 1, center1);
            const bool hasCenter2 = weightedCenter(slot + 2, center2);
            const int centerCount = (hasCenter0 ? 1 : 0) + (hasCenter1 ? 1 : 0) + (hasCenter2 ? 1 : 0);
            if (centerCount < 2)
                return false;

            if (!hasCenter0)
                center0 = center1 + (center1 - center2);
            if (!hasCenter1)
                center1 = (center0 + center2) * 0.5f;
            if (!hasCenter2)
                center2 = center1 + (center1 - center0);

            const osg::Vec3f axis = normalizeOr(center2 - center0, osg::Vec3f(1.f, 0.f, 0.f));
            const std::optional<float> low = weightedProjectionQuantile(slot, axis, 0.04f);
            const std::optional<float> high = weightedProjectionQuantile(slot, axis, 0.96f);
            if (!low || !high)
                return false;

            const float span = std::abs(*high - *low);
            const float band = std::max(span * 0.09f, 0.01f);
            chain.root = centerNearProjection(slot, axis, *low, band).value_or(center0 + (center0 - center1) * 0.5f);
            chain.mid = weightedBoundaryCenter(slot + 0, slot + 1).value_or((center0 + center1) * 0.5f);
            chain.tip = weightedBoundaryCenter(slot + 1, slot + 2).value_or((center1 + center2) * 0.5f);
            chain.end = centerNearProjection(slot, axis, *high, band).value_or(center2 + (center2 - center1) * 0.5f);

            if (((chain.end - chain.root) * axis) < 0.f)
            {
                std::swap(chain.root, chain.end);
                std::swap(chain.mid, chain.tip);
            }

            return (chain.mid - chain.root).length2() > 1e-5f && (chain.tip - chain.mid).length2() > 1e-5f
                && (chain.end - chain.tip).length2() > 1e-5f;
        }

        std::optional<float> weightedProjectionQuantile(std::size_t slot, const osg::Vec3f& axis, float quantile) const
        {
            if (slot + 2 >= mFingerBoneWeights.size())
                return std::nullopt;

            std::vector<std::pair<float, float>> values;
            values.reserve(mBaseVertices.size());
            float total = 0.f;
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                const float weight = mFingerBoneWeights[slot + 0][i] + mFingerBoneWeights[slot + 1][i]
                    + mFingerBoneWeights[slot + 2][i];
                if (weight <= 0.001f)
                    continue;

                const float projection = mBaseVertices[i] * axis;
                if (!std::isfinite(projection))
                    continue;

                values.emplace_back(projection, weight);
                total += weight;
            }

            if (values.empty() || total <= 0.f)
                return std::nullopt;

            std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
            });

            const float target = total * quantile;
            float running = 0.f;
            for (const auto& value : values)
            {
                running += value.second;
                if (running >= target)
                    return value.first;
            }

            return values.back().first;
        }

        std::optional<osg::Vec3f> centerNearProjection(
            std::size_t slot, const osg::Vec3f& axis, float targetProjection, float band) const
        {
            if (slot + 2 >= mFingerBoneWeights.size())
                return std::nullopt;

            float totalWeight = 0.f;
            osg::Vec3f weighted;
            const float safeBand = std::max(band, 1e-6f);
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                const float slotWeight = mFingerBoneWeights[slot + 0][i] + mFingerBoneWeights[slot + 1][i]
                    + mFingerBoneWeights[slot + 2][i];
                if (slotWeight <= 0.001f)
                    continue;

                const float distance = std::abs((mBaseVertices[i] * axis) - targetProjection);
                if (distance > safeBand)
                    continue;

                const float weight = slotWeight * (1.f - distance / safeBand);
                totalWeight += weight;
                weighted += mBaseVertices[i] * weight;
            }

            if (totalWeight <= 0.f)
                return std::nullopt;

            return weighted * (1.f / totalWeight);
        }

        std::optional<osg::Vec3f> weightedBoundaryCenter(std::size_t a, std::size_t b) const
        {
            if (a >= mFingerBoneWeights.size() || b >= mFingerBoneWeights.size())
                return std::nullopt;

            float totalWeight = 0.f;
            osg::Vec3f weighted;
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                const float weight = std::min(mFingerBoneWeights[a][i], mFingerBoneWeights[b][i]);
                if (weight <= 0.001f)
                    continue;

                totalWeight += weight;
                weighted += mBaseVertices[i] * weight;
            }

            if (totalWeight <= 0.05f)
                return std::nullopt;

            return weighted * (1.f / totalWeight);
        }

        osg::Vec3f palmCenter() const
        {
            float totalWeight = 0.f;
            osg::Vec3f weighted;
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                float fingerWeight = 0.f;
                for (const auto& weights : mFingerBoneWeights)
                    fingerWeight += weights[i];
                const float palmWeight = std::max(0.f, 1.f - std::min(1.f, fingerWeight));
                if (palmWeight <= 0.001f)
                    continue;

                totalWeight += palmWeight;
                weighted += mBaseVertices[i] * palmWeight;
            }

            if (totalWeight <= 0.f)
                return (mBounds._min + mBounds._max) * 0.5f;

            return weighted * (1.f / totalWeight);
        }

        bool weightedCenter(std::size_t slot, osg::Vec3f& center) const
        {
            if (slot >= mFingerBoneWeights.size() || mFingerBoneWeights[slot].size() != mBaseVertices.size())
                return false;

            float totalWeight = 0.f;
            osg::Vec3f weighted;
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                const float weight = mFingerBoneWeights[slot][i];
                if (weight <= 0.001f)
                    continue;

                totalWeight += weight;
                weighted += mBaseVertices[i] * weight;
            }

            if (totalWeight <= 0.001f)
                return false;

            center = weighted * (1.f / totalWeight);
            return true;
        }

        osg::Vec3f chooseCurlAxis(const osg::Vec3f& fingerDirection, const osg::Vec3f& palmDirection) const
        {
            osg::Vec3f axis = fingerDirection ^ palmDirection;
            if (axis.length2() <= 1e-5f)
                axis = fingerDirection ^ osg::Vec3f(0.f, 0.f, 1.f);
            return normalizeOr(axis, osg::Vec3f(0.f, 1.f, 0.f));
        }

        std::optional<osg::Vec3f> averageGripCurlAxis(const SharedBindBasis* sharedBasis) const
        {
            osg::Vec3f total;
            int count = 0;
            for (std::size_t groupIndex = static_cast<std::size_t>(FingerPoseGroup::Middle);
                 groupIndex <= static_cast<std::size_t>(FingerPoseGroup::Pinky); ++groupIndex)
            {
                FingerBindChain chain;
                if (sharedBasis != nullptr && sharedBasis->chainValid[groupIndex])
                    chain = sharedBasis->chains[groupIndex];
                else if (!deriveFingerBindChain(groupIndex * 3, chain))
                    continue;

                const osg::Vec3f fingerDirection = normalizeOr(chain.end - chain.root, osg::Vec3f(1.f, 0.f, 0.f));
                const osg::Vec3f palm = sharedBasis != nullptr ? sharedBasis->palm : palmCenter();
                osg::Vec3f palmDirection = palm - chain.root;
                palmDirection = normalizeOr(
                    palmDirection - fingerDirection * (palmDirection * fingerDirection), -fingerDirection);
                if ((fingerDirection ^ palmDirection).length2() <= 1e-5f)
                    palmDirection = normalizeOr(osg::Vec3f(0.f, mLeft ? 1.f : -1.f, 0.f), -fingerDirection);

                osg::Vec3f axis = chooseCurlAxis(fingerDirection, palmDirection);
                if (count > 0 && (axis * total) < 0.f)
                    axis = -axis;
                total += axis;
                ++count;
            }

            if (count == 0 || total.length2() <= 1e-5f)
                return std::nullopt;
            return normalizeOr(total, osg::Vec3f(0.f, 1.f, 0.f));
        }

        float chooseCurlSign(const osg::Vec3f& fingerDirection, const osg::Vec3f& palmDirection,
            const osg::Vec3f& hingeAxis, float angleRadians) const
        {
            const osg::Quat positive(angleRadians, hingeAxis);
            const osg::Quat negative(-angleRadians, hingeAxis);
            const float positiveTowardPalm = (positive * fingerDirection) * palmDirection;
            const float negativeTowardPalm = (negative * fingerDirection) * palmDirection;
            return positiveTowardPalm >= negativeTowardPalm ? 1.f : -1.f;
        }

        bool safeFingerPose(const osg::Vec3f& baseRoot, const osg::Vec3f& baseMid, const osg::Vec3f& baseTip,
            const osg::Vec3f& baseEnd, const osg::Vec3f& posedRoot, const osg::Vec3f& posedMid,
            const osg::Vec3f& posedTip, const osg::Vec3f& posedEnd) const
        {
            if (!finiteVec3(posedRoot) || !finiteVec3(posedMid) || !finiteVec3(posedTip) || !finiteVec3(posedEnd))
                return false;

            const float baseLength = (baseMid - baseRoot).length() + (baseTip - baseMid).length()
                + (baseEnd - baseTip).length();
            const float posedLength = (posedMid - posedRoot).length() + (posedTip - posedMid).length()
                + (posedEnd - posedTip).length();
            if (baseLength <= 1e-5f || std::abs(baseLength - posedLength) > 0.05f)
                return false;

            const float maxReach = baseLength * 1.5f;
            return (posedMid - baseRoot).length() <= maxReach && (posedTip - baseRoot).length() <= maxReach
                && (posedEnd - baseRoot).length() <= maxReach;
        }

        std::size_t countWeightedSlots() const
        {
            return static_cast<std::size_t>(std::count_if(mFingerBoneWeights.begin(), mFingerBoneWeights.end(),
                [](const std::vector<float>& weights) {
                    return std::any_of(
                        weights.begin(), weights.end(), [](float weight) { return weight > 0.001f; });
                }));
        }

        std::size_t countWeightedVertices() const
        {
            if (!fingerBoneWeightsReady())
                return 0;

            std::size_t count = 0;
            for (std::size_t i = 0; i < mBaseVertices.size(); ++i)
            {
                for (const auto& weights : mFingerBoneWeights)
                {
                    if (weights[i] > 0.001f)
                    {
                        ++count;
                        break;
                    }
                }
            }
            return count;
        }

        float readCurl(FingerCurlSource source, const std::string& hand) const
        {
            std::optional<ActionSample> sample;
            if (source == FingerCurlSource::Grip)
            {
                sample = getActionSample(getGripCurlActionIds(hand), true);
                if (!sample && getEnvFloat("OPENMW_FNV_VR_GRIP_FALLBACK_TO_TRIGGER", 1.f) != 0.f)
                    sample = getActionSample(getGripFallbackCurlActionIds(hand), true);
            }
            else
                sample = getActionSample(getCurlActionIds(source, hand), true);
            return sample ? clampUnit(sample->value) : 0.f;
        }

        float gripCurlAmount(std::size_t side) const
        {
            float total = 0.f;
            int count = 0;
            for (std::size_t groupIndex = static_cast<std::size_t>(FingerPoseGroup::Index);
                 groupIndex <= static_cast<std::size_t>(FingerPoseGroup::Pinky); ++groupIndex)
            {
                const VrFingerDebugSample& sample = sVrFingerDebugSamples[side][groupIndex];
                if (!sample.valid)
                    continue;

                total += clampUnit(sample.curl);
                ++count;
            }

            if (count == 0)
                return 0.f;

            const float average = total / static_cast<float>(count);
            return clampUnit((average - 0.35f) / 0.65f);
        }

        void captureBaseVertices(const osg::Vec3Array& vertices)
        {
            mBaseVertices.assign(vertices.begin(), vertices.end());
            mBounds.init();
            for (const osg::Vec3f& vertex : mBaseVertices)
                mBounds.expandBy(vertex);
            mLocalBasisEvaluated = false;
            mUseSharedBindBasis = false;
        }

        SharedBindBasis buildLocalBindBasis() const
        {
            SharedBindBasis basis;
            basis.palm = palmCenter();
            std::size_t validChains = 0;
            for (std::size_t groupIndex = 0; groupIndex < basis.chains.size(); ++groupIndex)
            {
                if (hasCompleteWeightedCenters(groupIndex * 3))
                    ++basis.completeChainCount;

                FingerBindChain chain;
                if (!deriveFingerBindChain(groupIndex * 3, chain))
                    continue;

                basis.chainValid[groupIndex] = true;
                basis.chains[groupIndex] = chain;
                ++validChains;
            }

            if (validChains != basis.chains.size())
            {
                basis.score = static_cast<float>(validChains);
                return basis;
            }

            const auto fullCurlTransforms = makeFingerTransforms(&basis, 1.f);
            basis.fullCurlSafe = stagedVerticesSafe(skinVertices(fullCurlTransforms));
            basis.valid = basis.fullCurlSafe && basis.completeChainCount >= 4;
            basis.score = basis.valid ? 100000.f + static_cast<float>(basis.completeChainCount * 1000)
                    + static_cast<float>(countWeightedVertices())
                                             : static_cast<float>(validChains);
            return basis;
        }

        bool hasCompleteWeightedCenters(std::size_t slot) const
        {
            osg::Vec3f center;
            return weightedCenter(slot + 0, center) && weightedCenter(slot + 1, center)
                && weightedCenter(slot + 2, center);
        }

        void updateSharedBindBasisState()
        {
            if (mSharedBasisKey.empty())
                return;

            if (!mLocalBasisEvaluated)
            {
                mLocalBindBasis = buildLocalBindBasis();
                mLocalBasisEvaluated = true;
                if (mLocalBindBasis.valid)
                {
                    SharedBindBasis& cached = sharedBindBasisCache()[mSharedBasisKey];
                    if (!cached.valid || mLocalBindBasis.score > cached.score)
                    {
                        cached = mLocalBindBasis;
                        if (mLeft)
                            mirroredLeftBindBasisCache() = mirrorBindBasisX(mLocalBindBasis);
                        if (!mLoggedSharedBasisPublish)
                        {
                            mLoggedSharedBasisPublish = true;
                            Log(Debug::Verbose) << "FNV/ESM4 diag: VR static hand shared finger basis published key="
                                             << mSharedBasisKey << " hand=" << (mLeft ? "left" : "right")
                                             << " score=" << mLocalBindBasis.score
                                             << " completeChains=" << mLocalBindBasis.completeChainCount
                                             << " weightedVertices=" << countWeightedVertices();
                        }
                    }
                }
                else if (!mLoggedRejectedLocalBasis)
                {
                    mLoggedRejectedLocalBasis = true;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VR static hand local finger basis rejected key="
                                     << mSharedBasisKey << " hand=" << (mLeft ? "left" : "right")
                                     << " fullCurlSafe=" << mLocalBindBasis.fullCurlSafe
                                     << " completeChains=" << mLocalBindBasis.completeChainCount
                                     << " score=" << mLocalBindBasis.score
                                     << " weightedVertices=" << countWeightedVertices();
                }
            }

            mUseMirroredLeftBindBasis = false;
            if (!mLeft)
            {
                const std::optional<SharedBindBasis>& mirrored = mirroredLeftBindBasisCache();
                if (mirrored && mirrored->valid && mirrored->completeChainCount >= 4
                    && stagedVerticesSafe(skinVertices(makeFingerTransforms(&*mirrored, 1.f))))
                {
                    if (!mLoggedMirroredLeftBasisUse)
                    {
                        mLoggedMirroredLeftBasisUse = true;
                        Log(Debug::Verbose)
                            << "FNV/ESM4 diag: VR static hand using mirrored left finger basis hand=right"
                            << " localScore=" << mLocalBindBasis.score
                            << " mirroredScore=" << mirrored->score
                            << " completeChains=" << mirrored->completeChainCount;
                    }
                    mUseMirroredLeftBindBasis = true;
                    mUseSharedBindBasis = false;
                    return;
                }
            }

            const auto found = sharedBindBasisCache().find(mSharedBasisKey);
            const bool shouldUseShared = found != sharedBindBasisCache().end() && found->second.valid
                && (!mLocalBindBasis.valid || found->second.score > mLocalBindBasis.score);
            if (shouldUseShared && !mUseSharedBindBasis && !mLoggedSharedBasisUse)
            {
                mLoggedSharedBasisUse = true;
                Log(Debug::Verbose) << "FNV/ESM4 diag: VR static hand using shared finger basis key=" << mSharedBasisKey
                                 << " hand=" << (mLeft ? "left" : "right")
                                 << " localScore=" << mLocalBindBasis.score
                                 << " sharedScore=" << found->second.score;
            }
            mUseSharedBindBasis = shouldUseShared;
        }

        bool mLeft = false;
        bool mLogged = false;
        bool mLocalBasisEvaluated = false;
        bool mUseSharedBindBasis = false;
        bool mUseMirroredLeftBindBasis = false;
        bool mLoggedSharedBasisPublish = false;
        bool mLoggedSharedBasisUse = false;
        bool mLoggedMirroredLeftBasisUse = false;
        bool mLoggedRejectedLocalBasis = false;
        mutable bool mLoggedStageAccepted = false;
        mutable bool mLoggedStageRejected = false;
        std::string mSharedBasisKey;
        SharedBindBasis mLocalBindBasis;
        osg::BoundingBox mBounds;
        std::vector<osg::Vec3f> mBaseVertices;
        std::array<std::vector<float>, 15> mFingerBoneWeights;
    };

    /// Implements control of a finger by overriding rotation
    class HandController : public osg::NodeCallback
    {
    public:
        HandController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void setFingerPointingMode(bool fingerPointingMode) { mFingerPointingMode = fingerPointingMode; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
        bool mFingerPointingMode = false;
    };

    void HandController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (mEnabled)
        {
        float PI_2 = osg::PI_2;
        if (node->getName() == "Bip01 L Hand")
            PI_2 = -PI_2;
        float PI_4 = PI_2 / 2.f;

        osg::Quat rotate{ 0, 0, 0, 1 };
        auto windowManager = MWBase::Environment::get().getWindowManager();
        auto weaponType = MWBase::Environment::get().getWorld()->getActiveWeaponType();
        // Morrowind models do not hold most weapons at a natural angle, so i rotate the hand
        // to more natural angles on weapons to allow more comfortable combat.
        if ((!windowManager->isGuiMode() || FNVXRLiveFrameSurface::instance().visible()) && !mFingerPointingMode)
        {

            switch (weaponType)
            {
                case ESM::Weapon::None:
                case ESM::Weapon::HandToHand:
                case ESM::Weapon::MarksmanThrown:
                case ESM::Weapon::Spell:
                case ESM::Weapon::Arrow:
                case ESM::Weapon::Bolt:
                    // No adjustment
                    break;
                case ESM::Weapon::MarksmanCrossbow:
                    // Crossbow points upwards. Assumedly because i am overriding hand animations.
                    rotate = osg::Quat(PI_4 / 1.05, osg::Vec3{ 0, 1, 0 }) * osg::Quat(0.06, osg::Vec3{ 0, 0, 1 });
                    break;
                case ESM::Weapon::MarksmanBow:
                    // Bow points down by default, rotate it back up a little
                    rotate = osg::Quat(-PI_2 * .10f, osg::Vec3{ 0, 1, 0 });
                    break;
                default:
                    // Melee weapons Need adjustment
                    rotate = osg::Quat(PI_4, osg::Vec3{ 0, 1, 0 });
                    break;
            }
        }

            auto matrixTransform = node->asTransform()->asMatrixTransform();
            auto matrix = matrixTransform->getMatrix();
            matrix.setRotate(rotate);
            matrixTransform->setMatrix(matrix);
        }

        // Omit nested callbacks to override animations of this node
        osg::ref_ptr<osg::Callback> ncb = getNestedCallback();
        setNestedCallback(nullptr);
        traverse(node, nv);
        setNestedCallback(ncb);
    }

    class ConfigureCullVisitor : public osg::NodeVisitor
    {
    public:
        ConfigureCullVisitor(bool enable)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mEnable(enable)
        {
        }

        void apply(osg::Drawable& node) override { node.setCullingActive(mEnable); }
        void apply(osg::Geometry& node) override { node.setCullingActive(mEnable); }

        bool mEnable;
    };

    class StaticizeFalloutVrRiggedGeometryVisitor : public osg::NodeVisitor
    {
    public:
        StaticizeFalloutVrRiggedGeometryVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::ref_ptr<osg::Geometry> staticGeometry = makeStaticGeometry(*geode.getDrawable(i));
                if (staticGeometry == nullptr)
                    continue;

                geode.setDrawable(i, staticGeometry);
                ++mStaticizedRigGeometryCount;
            }

            traverse(geode);
        }

        void apply(osg::Drawable& drawable) override
        {
            osg::ref_ptr<osg::Geometry> staticGeometry = makeStaticGeometry(drawable);
            if (staticGeometry == nullptr)
                return;

            bool replaced = false;
            while (drawable.getNumParents() > 0)
            {
                osg::Group* parent = drawable.getParent(0);
                if (osg::Geode* geode = dynamic_cast<osg::Geode*>(parent))
                {
                    if (!geode->replaceDrawable(&drawable, staticGeometry.get()))
                        break;
                    replaced = true;
                    continue;
                }

                osg::Node* drawableNode = dynamic_cast<osg::Node*>(&drawable);
                if (parent == nullptr || drawableNode == nullptr)
                    break;

                osg::ref_ptr<osg::Geode> staticGeode = new osg::Geode;
                staticGeode->setName(drawable.getName().empty() ? std::string("FNV VR Staticized Rig Drawable")
                                                                 : "FNV VR Staticized " + drawable.getName());
                staticGeode->addDrawable(staticGeometry.get());
                if (!parent->replaceChild(drawableNode, staticGeode.get()))
                    break;
                replaced = true;
            }

            if (replaced)
                ++mStaticizedRigGeometryCount;
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly staticize could not replace rig drawable name="
                                    << drawable.getName() << " parents=" << drawable.getNumParents();
        }

        osg::ref_ptr<osg::Geometry> makeStaticGeometry(osg::Drawable& drawable)
        {
            osg::Geometry* source = nullptr;
            std::string sourceName;
            std::string rootBone;
            std::size_t boneCount = 0;
            const char* kind = nullptr;
            bool hasFingerWeights = false;
            std::array<std::vector<float>, 15> fingerBoneWeights;
            SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);

            if (rig != nullptr)
            {
                ++mSeenRigGeometryCount;
                source = rig->getSourceGeometry();
                if ((source == nullptr || source->getVertexArray() == nullptr
                        || source->getVertexArray()->getNumElements() == 0))
                {
                    for (unsigned int i = 0; i < 2; ++i)
                    {
                        osg::Geometry* renderGeometry = rig->getRenderGeometry(i);
                        if (renderGeometry == nullptr || renderGeometry->getVertexArray() == nullptr
                            || renderGeometry->getVertexArray()->getNumElements() == 0)
                            continue;
                        source = renderGeometry;
                        break;
                    }
                }
                sourceName = source != nullptr ? source->getName() : std::string();
                rootBone = std::string(rig->getRootBone());
                boneCount = rig->getBoneCount();
                hasFingerWeights = rig->getFalloutFingerBoneVertexWeights(fingerBoneWeights);
                kind = "SceneUtil::RigGeometry";
            }
            else if (SceneUtil::RigGeometryHolder* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
            {
                ++mSeenRigGeometryCount;
                osg::ref_ptr<SceneUtil::OsgaRigGeometry> holderSource = holder->getSourceRigGeometry();
                source = dynamic_cast<osg::Geometry*>(holderSource.get());
                sourceName = source != nullptr ? source->getName() : std::string();
                kind = "SceneUtil::RigGeometryHolder";
            }
            else
                return nullptr;

            if (source == nullptr)
            {
                ++mMissingSourceGeometryCount;
                Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly staticize rig drawable has no source kind=" << kind
                                    << " name=" << drawable.getName() << " rootBone=" << rootBone
                                    << " bones=" << boneCount;
                return nullptr;
            }

            osg::ref_ptr<osg::Geometry> staticGeometry = osg::clone(source, osg::CopyOp::DEEP_COPY_ALL);
            staticGeometry->setName(drawable.getName().empty() ? sourceName : drawable.getName());
            staticGeometry->setNodeMask(~0u);
            staticGeometry->setCullingActive(false);
            staticGeometry->setComputeBoundingBoxCallback(nullptr);
            staticGeometry->setComputeBoundingSphereCallback(nullptr);
            staticGeometry->setDataVariance(osg::Object::DYNAMIC);
            if (staticGeometry->getVertexArray() != nullptr)
                staticGeometry->getVertexArray()->setDataVariance(osg::Object::DYNAMIC);
            staticGeometry->dirtyBound();
            if (drawable.getStateSet() != nullptr)
                staticGeometry->setStateSet(osg::clone(drawable.getStateSet(), osg::CopyOp::DEEP_COPY_ALL));

            const std::string staticSource = Misc::StringUtils::lowerCase(
                sourceName + " " + drawable.getName() + " " + rootBone);
            const bool staticHand = staticSource.find("hand") != std::string::npos
                || staticSource.find("glove") != std::string::npos;
            const bool leftHand = staticSource.find("left") != std::string::npos
                || staticSource.find("bip01 l ") != std::string::npos;
            const bool rightHand = staticSource.find("right") != std::string::npos
                || staticSource.find("bip01 r ") != std::string::npos;
            if (staticHand && (leftHand || rightHand) && hasFingerWeights)
            {
                smoothStaticHandFingerWeights(*staticGeometry, fingerBoneWeights);
                writeHandMeshProofJson(drawable.getName(), sourceName, rootBone, *staticGeometry, fingerBoneWeights, rig);
                std::string sharedBasisName = drawable.getName().empty() ? sourceName : drawable.getName();
                const std::size_t drawablePartSeparator = sharedBasisName.find(':');
                if (drawablePartSeparator != std::string::npos)
                    sharedBasisName.erase(drawablePartSeparator);
                sharedBasisName = Misc::StringUtils::lowerCase(sharedBasisName + "|" + rootBone);
                const std::string sharedBasisKey = (leftHand ? "left|" : "right|") + sharedBasisName;
                staticGeometry->setUpdateCallback(
                    new StaticHandFingerDeformCallback(leftHand, sharedBasisKey, std::move(fingerBoneWeights)));
            }
            else if (staticHand && (leftHand || rightHand))
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: VR static hand finger deform skipped no rig weights hand="
                                    << (leftHand ? "left" : "right") << " kind=" << kind
                                    << " name=" << drawable.getName() << " source=" << sourceName;
            }

            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly staticized rig drawable kind=" << kind
                             << " name=" << drawable.getName() << " source=" << sourceName
                             << " rootBone=" << rootBone << " bones=" << boneCount;
            return staticGeometry;
        }

        unsigned int mStaticizedRigGeometryCount = 0;
        unsigned int mSeenRigGeometryCount = 0;
        unsigned int mMissingSourceGeometryCount = 0;
    };

    void addLocalAxisMarker(osg::Group& parent, const osg::Vec3f& position, float length);
    void applyHandDebugState(osg::StateSet& stateSet);
    bool applyFingerWeightDebugColors(SceneUtil::RigGeometry& rig);

    std::string formatVec3(const osg::Vec3f& value)
    {
        std::ostringstream stream;
        stream << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
        return stream.str();
    }

    std::string formatMatrix4(const osg::Matrixf& value)
    {
        std::ostringstream stream;
        stream << "[";
        for (int row = 0; row < 4; ++row)
        {
            if (row != 0)
                stream << ";";
            for (int col = 0; col < 4; ++col)
            {
                if (col != 0)
                    stream << ",";
                stream << value(row, col);
            }
        }
        stream << "]";
        return stream.str();
    }

    class BindFalloutVrHandFingerControllersVisitor : public osg::NodeVisitor
    {
    public:
        explicit BindFalloutVrHandFingerControllersVisitor(bool left, bool directPoseOnly = false)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mLeft(left)
            , mDirectPoseOnly(directPoseOnly)
            , mDirectPoseState(std::make_shared<DirectHandFingerPoseState>(left))
        {
            const std::string prefix = mLeft ? "Bip01 L " : "Bip01 R ";
            for (const FingerBindingSpec& spec : getFalloutFingerBindingSpecs())
                mBindings.emplace_back(prefix + spec.boneSuffix, spec);
        }

        void apply(SceneUtil::Skeleton& skeleton)
        {
            if (mDirectPoseOnly)
            {
                traverse(skeleton);
                return;
            }

            SceneUtil::Skeleton* previousSkeleton = mSkeleton;
            mSkeleton = &skeleton;
            skeleton.setActive(SceneUtil::Skeleton::Active);
            skeleton.setIsTracked(true);
            traverse(skeleton);
            mSkeleton = previousSkeleton;
        }

        void apply(osg::MatrixTransform& node) override
        {
            for (const auto& [boneName, spec] : mBindings)
            {
                if (node.getName() != boneName)
                    continue;

                dumpBindBone(node, spec);
                node.addUpdateCallback(
                    new FingerController(mLeft, spec, mSkeleton, mSkeleton == nullptr ? mDirectPoseState : nullptr));
                if (getEnvFloat("OPENMW_FNV_VR_FINGER_BONE_AXES", 0.f) != 0.f)
                {
                    addLocalAxisMarker(
                        node, osg::Vec3f(), getEnvFloat("OPENMW_FNV_VR_FINGER_BONE_AXIS_LENGTH", 4.f));
                    ++mAxisCount;
                }
                ++mBoundCount;
                break;
            }

            traverse(node);
        }

        int mBoundCount = 0;
        int mAxisCount = 0;

    private:
        void dumpBindBone(osg::MatrixTransform& node, const FingerBindingSpec& spec)
        {
            if (getEnvFloat("OPENMW_FNV_VR_FINGER_BIND_DUMP", 0.f) == 0.f)
                return;

            const osg::Matrixf local = node.getMatrix();
            osg::Matrixf world;
            const osg::NodePath& visitorPath = getNodePath();
            if (!visitorPath.empty())
                world = osg::computeLocalToWorld(visitorPath);
            else if (!node.getParentalNodePaths().empty())
                world = osg::computeLocalToWorld(node.getParentalNodePaths().front());
            else
                world = local;

            std::string parentName = "<none>";
            if (visitorPath.size() >= 2 && visitorPath[visitorPath.size() - 2] != nullptr)
                parentName = visitorPath[visitorPath.size() - 2]->getName();
            else if (node.getNumParents() > 0 && node.getParent(0) != nullptr)
                parentName = node.getParent(0)->getName();

            std::string childName = "<none>";
            osg::Vec3f childLocalTrans;
            osg::Vec3f childWorldTrans;
            bool childMatrixFound = false;
            for (unsigned int i = 0; i < node.getNumChildren(); ++i)
            {
                osg::Node* child = node.getChild(i);
                osg::MatrixTransform* childMatrix = child != nullptr && child->asTransform() != nullptr
                    ? child->asTransform()->asMatrixTransform()
                    : nullptr;
                if (childMatrix == nullptr)
                    continue;

                childName = childMatrix->getName();
                childLocalTrans = childMatrix->getMatrix().getTrans();
                childWorldTrans = childLocalTrans * world;
                childMatrixFound = true;
                break;
            }

            Log(Debug::Verbose) << "FNV/ESM4 diag: VR finger bind dump hand=" << (mLeft ? "left" : "right")
                             << " context=" << (mSkeleton != nullptr ? "trackedSkeleton" : "directWrapper")
                             << " bone=" << node.getName()
                             << " parent=" << parentName
                             << " child=" << childName
                             << " source=" << getFingerCurlSourceName(spec.source)
                             << " indexFinger=" << spec.indexFinger
                             << " scale=" << spec.scale
                             << " localTrans=" << formatVec3(local.getTrans())
                             << " worldTrans=" << formatVec3(world.getTrans())
                             << " childLocalTrans=" << (childMatrixFound ? formatVec3(childLocalTrans) : std::string("<none>"))
                             << " childWorldTrans=" << (childMatrixFound ? formatVec3(childWorldTrans) : std::string("<none>"))
                             << " localMatrix=" << formatMatrix4(local)
                             << " worldMatrix=" << formatMatrix4(world);
        }

        bool mLeft = false;
        bool mDirectPoseOnly = false;
        SceneUtil::Skeleton* mSkeleton = nullptr;
        std::shared_ptr<DirectHandFingerPoseState> mDirectPoseState;
        std::vector<std::pair<std::string, FingerBindingSpec>> mBindings;
    };

    class ApplyFalloutVrHandDebugStyleVisitor : public osg::NodeVisitor
    {
    public:
        ApplyFalloutVrHandDebugStyleVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (mWireframe || mWeightDebug)
                applyHandDebugState(*node.getOrCreateStateSet());
            traverse(node);
        }

        void apply(osg::Drawable& drawable) override
        {
            if (mWireframe || mWeightDebug)
                applyHandDebugState(*drawable.getOrCreateStateSet());

            if (mWeightDebug)
            {
                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    if (applyFingerWeightDebugColors(*rig))
                        ++mWeightColoredRigCount;
                    else
                        ++mWeightColorMissCount;
                }
            }
        }

        bool mWireframe = getEnvFloat("OPENMW_FNV_VR_HAND_WIREFRAME", 0.f) != 0.f;
        bool mWeightDebug = getEnvFloat("OPENMW_FNV_VR_HAND_WEIGHT_DEBUG", 0.f) != 0.f;
        unsigned int mWeightColoredRigCount = 0;
        unsigned int mWeightColorMissCount = 0;
    };

    osg::BoundingBox computeNodeBounds(osg::Node& node)
    {
        osg::ComputeBoundsVisitor boundsVisitor;
        node.accept(boundsVisitor);
        return boundsVisitor.getBoundingBox();
    }

    osg::Vec3f boundsExtent(const osg::BoundingBox& box)
    {
        return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
    }

    void applyRightPipBoyCalibrationStyle(osg::Node& node)
    {
        osg::StateSet* stateSet = node.getOrCreateStateSet();
        osg::ref_ptr<osg::Material> material = new osg::Material;
        material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
        material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.85f, 1.f, 1.f));
        material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.85f, 1.f, 1.f));
        material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.35f, 0.55f, 1.f));
        stateSet->setAttributeAndModes(material, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        stateSet->setAttributeAndModes(
            new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        stateSet->setAttributeAndModes(
            new osg::LineWidth(getEnvFloat("OPENMW_FNV_RIGHT_PIPBOY_DEBUG_LINE_WIDTH", 6.f)),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    }

    bool mirrorScaleFlipsHandedness(const osg::Vec3f& scale)
    {
        return scale.x() * scale.y() * scale.z() < 0.f;
    }

    void applyMirroredFrontFaceState(osg::Node& node)
    {
        osg::ref_ptr<osg::FrontFace> frontFace = new osg::FrontFace;
        frontFace->setMode(osg::FrontFace::CLOCKWISE);
        node.getOrCreateStateSet()->setAttributeAndModes(frontFace, osg::StateAttribute::ON);
    }

    void expandTransformedBounds(osg::BoundingBox& target, const osg::BoundingBox& source, const osg::Matrix& matrix)
    {
        if (!source.valid())
            return;

        for (int x = 0; x < 2; ++x)
        {
            for (int y = 0; y < 2; ++y)
            {
                for (int z = 0; z < 2; ++z)
                {
                    target.expandBy(osg::Vec3f(x == 0 ? source.xMin() : source.xMax(),
                                        y == 0 ? source.yMin() : source.yMax(),
                                        z == 0 ? source.zMin() : source.zMax())
                        * matrix);
                }
            }
        }
    }

    struct HandCuffAnchor
    {
        osg::Vec3f mAnchor;
        osg::Vec3f mSliceMin;
        osg::Vec3f mSliceMax;
        unsigned int mVertexCount = 0;
        bool mValid = false;
    };

    class HandCuffAnchorVisitor : public osg::NodeVisitor
    {
    public:
        HandCuffAnchorVisitor(bool left, const osg::BoundingBox& bounds)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mLeft(left)
            , mWristX(left ? bounds.xMax() : bounds.xMin())
            , mSliceThickness(std::max(0.5f, (bounds.xMax() - bounds.xMin()) * 0.12f))
            , mSum(0.f, 0.f, 0.f)
        {
            mMatrixStack.push_back(osg::Matrix::identity());
        }

        void apply(osg::Transform& transform) override
        {
            osg::Matrix matrix = mMatrixStack.back();
            transform.computeLocalToWorldMatrix(matrix, this);
            mMatrixStack.push_back(matrix);
            traverse(transform);
            mMatrixStack.pop_back();
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                osg::Geometry* geometry = geode.getDrawable(i) != nullptr ? geode.getDrawable(i)->asGeometry() : nullptr;
                if (geometry == nullptr)
                    continue;

                const osg::Vec3Array* vertices = dynamic_cast<const osg::Vec3Array*>(geometry->getVertexArray());
                if (vertices == nullptr)
                    continue;

                for (const osg::Vec3f& vertex : *vertices)
                {
                    const osg::Vec3f local = vertex * mMatrixStack.back();
                    const float distance = mLeft ? (mWristX - local.x()) : (local.x() - mWristX);
                    if (distance < 0.f || distance > mSliceThickness)
                        continue;

                    mSum += local;
                    mSliceBounds.expandBy(local);
                    ++mVertexCount;
                }
            }

            traverse(geode);
        }

        HandCuffAnchor result() const
        {
            if (mVertexCount == 0)
                return {};
            const osg::Vec3f sliceCenter = mSliceBounds.valid()
                ? mSliceBounds.center()
                : mSum / static_cast<float>(mVertexCount);
            return { osg::Vec3f(mWristX, sliceCenter.y(), sliceCenter.z()),
                osg::Vec3f(mSliceBounds.xMin(), mSliceBounds.yMin(), mSliceBounds.zMin()),
                osg::Vec3f(mSliceBounds.xMax(), mSliceBounds.yMax(), mSliceBounds.zMax()), mVertexCount, true };
        }

    private:
        bool mLeft;
        float mWristX;
        float mSliceThickness;
        osg::Vec3f mSum;
        osg::BoundingBox mSliceBounds;
        unsigned int mVertexCount = 0;
        std::vector<osg::Matrix> mMatrixStack;
    };

    osg::ref_ptr<osg::Geometry> createAxisMarkerGeometry(float length)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        const float negativeStub = -length * 0.25f;
        vertices->push_back(osg::Vec3f(negativeStub, 0.f, 0.f));
        vertices->push_back(osg::Vec3f(length, 0.f, 0.f));
        vertices->push_back(osg::Vec3f(0.f, negativeStub, 0.f));
        vertices->push_back(osg::Vec3f(0.f, length, 0.f));
        vertices->push_back(osg::Vec3f(0.f, 0.f, negativeStub));
        vertices->push_back(osg::Vec3f(0.f, 0.f, length));
        geometry->setVertexArray(vertices);

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(osg::Vec4f(1.f, 0.f, 0.f, 1.f));
        colors->push_back(osg::Vec4f(1.f, 0.f, 0.f, 1.f));
        colors->push_back(osg::Vec4f(0.f, 1.f, 0.f, 1.f));
        colors->push_back(osg::Vec4f(0.f, 1.f, 0.f, 1.f));
        colors->push_back(osg::Vec4f(0.f, 0.4f, 1.f, 1.f));
        colors->push_back(osg::Vec4f(0.f, 0.4f, 1.f, 1.f));
        geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        geometry->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::LINES, 0, vertices->size()));
        geometry->setCullingActive(false);

        osg::StateSet* stateset = geometry->getOrCreateStateSet();
        stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        stateset->setAttributeAndModes(
            new osg::LineWidth(getEnvFloat("OPENMW_FNV_VR_DEBUG_AXIS_WIDTH", 8.f)), osg::StateAttribute::ON);
        return geometry;
    }

    osg::ref_ptr<osg::ShapeDrawable> createAxisOriginMarker(float radius, const osg::Vec4f& color)
    {
        osg::ref_ptr<osg::ShapeDrawable> drawable = new osg::ShapeDrawable(new osg::Sphere(osg::Vec3f(), radius));
        drawable->setColor(color);
        drawable->setCullingActive(false);

        osg::StateSet* stateset = drawable->getOrCreateStateSet();
        stateset->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        return drawable;
    }

    void addLocalAxisMarker(osg::Group& parent, const osg::Vec3f& position, float length)
    {
        osg::ref_ptr<osg::Geode> axis = new osg::Geode;
        axis->setName("FNV VR local XYZ debug axis");
        axis->addDrawable(createAxisMarkerGeometry(length));

        osg::ref_ptr<osg::PositionAttitudeTransform> marker = new osg::PositionAttitudeTransform;
        marker->setName("FNV VR local XYZ debug axis transform");
        marker->setPosition(position);
        marker->addChild(axis);
        parent.addChild(marker);
    }

    void applyHandDebugState(osg::StateSet& stateSet)
    {
        stateSet.setMode(GL_LIGHTING, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
        if (getEnvFloat("OPENMW_FNV_VR_HAND_WIREFRAME", 0.f) != 0.f)
        {
            stateSet.setAttributeAndModes(
                new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::LINE),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            stateSet.setAttributeAndModes(
                new osg::LineWidth(getEnvFloat("OPENMW_FNV_VR_HAND_WIREFRAME_WIDTH", 4.f)),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        }
    }

    osg::Vec4f getFingerWeightDebugColor(float thumb, float index, float grip)
    {
        const float strongest = std::max({ thumb, index, grip });
        if (strongest <= 0.001f)
            return osg::Vec4f(0.08f, 0.08f, 0.08f, 1.f);

        const float alpha = 0.35f + 0.65f * std::min(1.f, strongest);
        if (thumb >= index && thumb >= grip)
            return osg::Vec4f(1.f, 0.25f, 0.05f, alpha);
        if (index >= thumb && index >= grip)
            return osg::Vec4f(0.1f, 0.75f, 1.f, alpha);
        return osg::Vec4f(0.1f, 1.f, 0.25f, alpha);
    }

    bool applyFingerWeightDebugColors(SceneUtil::RigGeometry& rig)
    {
        std::vector<float> thumbWeights;
        std::vector<float> indexWeights;
        std::vector<float> gripWeights;
        if (!rig.getFalloutFingerVertexWeights(thumbWeights, indexWeights, gripWeights))
            return false;

        const std::size_t vertexCount = thumbWeights.size();
        if (vertexCount == 0 || indexWeights.size() != vertexCount || gripWeights.size() != vertexCount)
            return false;

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->reserve(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i)
            colors->push_back(getFingerWeightDebugColor(thumbWeights[i], indexWeights[i], gripWeights[i]));

        bool applied = false;
        if (osg::Geometry* source = rig.getSourceGeometry().get())
        {
            source->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
            source->dirtyGLObjects();
            applied = true;
        }

        for (unsigned int i = 0; i < 2; ++i)
        {
            osg::Geometry* renderGeometry = rig.getRenderGeometry(i);
            if (renderGeometry == nullptr)
                continue;

            osg::ref_ptr<osg::Vec4Array> renderColors
                = static_cast<osg::Vec4Array*>(colors->clone(osg::CopyOp::DEEP_COPY_ALL));
            renderGeometry->setColorArray(renderColors.get(), osg::Array::BIND_PER_VERTEX);
            renderGeometry->dirtyGLObjects();
            applied = true;
        }

        return applied;
    }

    osg::Vec3f mirrorAroundPoint(const osg::Vec3f& point, const osg::Vec3f& pivot, const osg::Vec3f& mirrorScale)
    {
        return pivot + scaleVec3(mirrorScale, point - pivot);
    }

    void wrapChildrenInSocketMirror(
        osg::PositionAttitudeTransform& transform, const osg::Vec3f& socketModel, const osg::Vec3f& mirrorScale)
    {
        if (mirrorScale == osg::Vec3f(1.f, 1.f, 1.f) || transform.getNumChildren() == 0)
            return;

        std::vector<osg::ref_ptr<osg::Node>> children;
        children.reserve(transform.getNumChildren());
        for (unsigned int i = 0; i < transform.getNumChildren(); ++i)
            children.push_back(transform.getChild(i));
        transform.removeChildren(0, transform.getNumChildren());

        osg::ref_ptr<osg::PositionAttitudeTransform> visualMirror = new osg::PositionAttitudeTransform;
        visualMirror->setName("FNV right PipBoy socket-pivot visual mirror");
        visualMirror->setScale(mirrorScale);
        if (mirrorScaleFlipsHandedness(mirrorScale))
            applyMirroredFrontFaceState(*visualMirror);
        visualMirror->setPosition(socketModel - scaleVec3(mirrorScale, socketModel));
        for (const osg::ref_ptr<osg::Node>& child : children)
            visualMirror->addChild(child);
        transform.addChild(visualMirror);
    }

    void addAxisMarker(osg::Group& parent, const osg::Vec3f& position, const osg::Quat& attitude, float length)
    {
        osg::ref_ptr<osg::PositionAttitudeTransform> marker = new osg::PositionAttitudeTransform;
        marker->setName("FNV VR hand socket debug axis");
        marker->setPosition(position);
        marker->setAttitude(attitude);
        marker->addChild(createAxisMarkerGeometry(length));
        parent.addChild(marker);
    }

    HandCuffAnchor computeHandCuffAnchor(osg::Node& node, const osg::BoundingBox& bounds, bool left)
    {
        HandCuffAnchorVisitor visitor(left, bounds);
        node.accept(visitor);
        return visitor.result();
    }

    /// Implements control of weapon direction
    class WeaponDirectionController : public osg::NodeCallback
    {
    public:
        WeaponDirectionController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
    };

    void WeaponDirectionController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }

        // Arriving here implies a parent, no need to check
        auto parent = static_cast<osg::MatrixTransform*>(node->getParent(0));

        osg::Quat rotate{ 0, 0, 0, 1 };
        auto weaponType = MWBase::Environment::get().getWorld()->getActiveWeaponType();
        switch (weaponType)
        {
            case ESM::Weapon::MarksmanThrown:
            case ESM::Weapon::Spell:
            case ESM::Weapon::Arrow:
            case ESM::Weapon::Bolt:
            case ESM::Weapon::HandToHand:
            case ESM::Weapon::MarksmanBow:
            case ESM::Weapon::MarksmanCrossbow:
                // Rotate to point straight forward, reverting any rotation of the hand to keep aim consistent.
                rotate = parent->getInverseMatrix().getRotate();
                rotate = osg::Quat(-osg::PI_2, osg::Vec3{ 0, 0, 1 }) * rotate;
                break;
            default:
                // Melee weapons point straight up from the hand
                rotate = osg::Quat(osg::PI_2, osg::Vec3{ 1, 0, 0 });
                break;
        }

        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto matrix = matrixTransform->getMatrix();
        matrix.setRotate(rotate);
        matrixTransform->setMatrix(matrix);

        traverse(node, nv);
    }

    /// Implements control of the weapon pointer
    class WeaponPointerController : public osg::NodeCallback
    {
    public:
        WeaponPointerController() = default;
        void setEnabled(bool enabled) { mEnabled = enabled; }
        void operator()(osg::Node* node, osg::NodeVisitor* nv);

    private:
        bool mEnabled = true;
    };

    void WeaponPointerController::operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }

        auto matrixTransform = node->asTransform()->asMatrixTransform();
        auto world = MWBase::Environment::get().getWorld();
        auto weaponType = world->getActiveWeaponType();
        auto windowManager = MWBase::Environment::get().getWindowManager();

        if (!isMeleeWeapon(weaponType) && !windowManager->isGuiMode())
        {
            // Ranged weapons should show a pointer to where they are targeting
            matrixTransform->setMatrix(osg::Matrix::scale(1.f, 64.f, 1.f));
        }
        else
        {
            matrixTransform->setMatrix(osg::Matrix::scale(1.f, 64.f, 1.f));
        }

        // First, update the base of the finger to the overriding orientation

        traverse(node, nv);
    }

    struct CachedControllerPose
    {
        osg::Vec3f mWorldPosition;
        osg::Quat mWorldOrientation;
        bool mValid = false;
    };

    std::map<std::string, CachedControllerPose> sCachedControllerPoses;

    class TrackingController
    {
    public:
        TrackingController(std::shared_ptr<VR::Space> space, std::string auditSourceSpaceName,
            std::shared_ptr<VR::Space> auditAimSpace, std::string auditAimSpaceName, osg::Vec3 baseOffset, bool left, bool mirror,
            bool useNativeGripOrientation, std::string debugName)
            : mSpace(space)
            , mAuditSourceSpaceName(std::move(auditSourceSpaceName))
            , mAuditAimSpace(auditAimSpace)
            , mAuditAimSpaceName(std::move(auditAimSpaceName))
            , mTransform(nullptr)
            , mBaseOffset(baseOffset)
            , mBaseOrientation(useNativeGripOrientation ? osg::Quat() : osg::Quat(osg::PI_2, osg::Vec3f(0, 0, 1)))
            , mDebugName(std::move(debugName))
            , mLeft(left)
            , mMirror(mirror)
            , mUseNativeGripOrientation(useNativeGripOrientation)
        {
            if (left && !mUseNativeGripOrientation)
                mBaseOrientation = osg::Quat(osg::PI, osg::Vec3f(1, 0, 0)) * mBaseOrientation;
        }

        void update(osg::MatrixTransform& transform)
        {
            if (!mTransform)
                return;

            auto tp = mSpace->locateInWorld();
            if (!tp.status)
                return;

            auto orientation = mBaseOrientation * tp.pose.orientation;
            sCachedControllerPoses[mDebugName] = { tp.pose.position.asMWUnits(), orientation, true };

            // Undo the wrist translate
            // TODO: I'm sure this could bee a lot less hacky
            // But i'll defer that to whenever we get inverse cinematics so i can track the hand directly.
            osg::Matrix handMatrix = osg::Matrix::identity();
            for (unsigned int i = 0; i < mTransform->getNumChildren(); ++i) {
                auto* child = mTransform->getChild(i);
                if (child->getName().find(" Hand") != std::string::npos) {
                    if (auto* t = child->asTransform()) {
                        if (auto* mt = t->asMatrixTransform()) {
                            handMatrix = mt->getMatrix();
                            break;
                        }
                    }
                }
            }
            auto position = tp.pose.position.asMWUnits() - (orientation * handMatrix.getTrans());

            // Center hand mesh on tracking
            // The base offset is an estimate from trial and error.
            position -= orientation * mBaseOffset;
            auto offset = VR::Session::instance().getHandsOffset();
            if (mLeft)
                offset.x() = -offset.x();
            position += tp.pose.orientation * offset;

            const osg::Vec3 rayOrigin = tp.pose.position.asMWUnits();
            const osg::Vec3 worldDelta = position - rayOrigin;
            const osg::Vec3 aimLocalDelta = tp.pose.orientation.inverse() * worldDelta;
            const bool handTrackingLog = getEnvFloat("OPENMW_FNV_VR_HAND_TRACKING_LOG", 0.f) != 0.f;
            if (handTrackingLog && mAuditAimSpace != nullptr
                && (mAimAuditLogCount < 40 || (++mAimAuditLogFrame % 300) == 0))
            {
                const auto aimTp = mAuditAimSpace->locateInWorld();
                if (!!aimTp.status)
                {
                    ++mAimAuditLogCount;
                    const osg::Vec3 aimWorld = aimTp.pose.position.asMWUnits();
                    const osg::Quat aimOrientation = aimTp.pose.orientation;
                    const osg::Vec3 sourceWorld = tp.pose.position.asMWUnits();
                    const osg::Quat sourceOrientation = tp.pose.orientation;
                    const osg::Vec3 sourceMinusAimWorld = sourceWorld - aimWorld;
                    const osg::Vec3 sourceMinusAimLocal = aimOrientation.inverse() * sourceMinusAimWorld;
                    const osg::Vec3 handMinusAimWorld = position - aimWorld;
                    const osg::Vec3 handMinusAimLocal = aimOrientation.inverse() * handMinusAimWorld;
                    const osg::Quat sourceFromAim = sourceOrientation * aimOrientation.inverse();
                    const osg::Quat handFromAim = orientation * aimOrientation.inverse();
                    double sourceAngle = 0.0;
                    osg::Vec3 sourceAxis;
                    sourceFromAim.getRotate(sourceAngle, sourceAxis);
                    double handAngle = 0.0;
                    osg::Vec3 handAxis;
                    handFromAim.getRotate(handAngle, handAxis);
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand aim-frame audit hand=" << mDebugName
                                     << " handSpace=" << mAuditSourceSpaceName
                                     << " aimSpace=" << mAuditAimSpaceName
                                     << " aimWorld=(" << aimWorld.x() << "," << aimWorld.y() << ","
                                     << aimWorld.z() << ")"
                                     << " sourceWorld=(" << sourceWorld.x() << "," << sourceWorld.y()
                                     << "," << sourceWorld.z() << ")"
                                     << " computedHandWorld=(" << position.x() << "," << position.y()
                                     << "," << position.z() << ")"
                                     << " sourceMinusAimWorld=(" << sourceMinusAimWorld.x() << ","
                                     << sourceMinusAimWorld.y() << "," << sourceMinusAimWorld.z() << ")"
                                     << " sourceMinusAimLocal=(" << sourceMinusAimLocal.x() << ","
                                     << sourceMinusAimLocal.y() << "," << sourceMinusAimLocal.z() << ")"
                                     << " sourceDistance=" << sourceMinusAimWorld.length()
                                     << " handMinusAimWorld=(" << handMinusAimWorld.x() << ","
                                     << handMinusAimWorld.y() << "," << handMinusAimWorld.z() << ")"
                                     << " handMinusAimLocal=(" << handMinusAimLocal.x() << ","
                                     << handMinusAimLocal.y() << "," << handMinusAimLocal.z() << ")"
                                     << " handDistance=" << handMinusAimWorld.length()
                                     << " sourceRotationDeltaDeg=" << osg::RadiansToDegrees(sourceAngle)
                                     << " sourceRotationAxis=(" << sourceAxis.x() << "," << sourceAxis.y()
                                     << "," << sourceAxis.z() << ")"
                                     << " handRotationDeltaDeg=" << osg::RadiansToDegrees(handAngle)
                                     << " handRotationAxis=(" << handAxis.x() << "," << handAxis.y()
                                     << "," << handAxis.z() << ")"
                                     << " baseOffset=(" << mBaseOffset.x() << "," << mBaseOffset.y()
                                     << "," << mBaseOffset.z() << ")"
                                     << " handsOffset=(" << offset.x() << "," << offset.y() << ","
                                     << offset.z() << ")"
                                     << " handChildLocal=(" << handMatrix.getTrans().x() << ","
                                     << handMatrix.getTrans().y() << "," << handMatrix.getTrans().z() << ")";
                }
            }
            if (handTrackingLog && (mAlignmentLogCount < 20 || (++mAlignmentLogFrame % 300) == 0))
            {
                ++mAlignmentLogCount;
                Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand pointer alignment hand=" << mDebugName
                                 << " rayOriginMW=(" << rayOrigin.x() << "," << rayOrigin.y() << ","
                                 << rayOrigin.z() << ") computedHandMW=(" << position.x() << ","
                                 << position.y() << "," << position.z() << ") worldDeltaMW=("
                                 << worldDelta.x() << "," << worldDelta.y() << "," << worldDelta.z()
                                 << ") aimLocalDeltaMW=(" << aimLocalDelta.x() << "," << aimLocalDelta.y()
                                 << "," << aimLocalDelta.z() << ") distanceMW=" << worldDelta.length()
                                 << " baseOffset=(" << mBaseOffset.x() << "," << mBaseOffset.y() << ","
                                 << mBaseOffset.z() << ") handsOffset=(" << offset.x() << ","
                                 << offset.y() << "," << offset.z() << ") handChildLocal=("
                                 << handMatrix.getTrans().x() << "," << handMatrix.getTrans().y() << ","
                                 << handMatrix.getTrans().z() << ")";
            }

            osg::Matrix worldToLocal = osg::computeWorldToLocal(mTransform->getParentalNodePaths()[0]);
            osg::Matrix localToWorld = osg::Matrix::identity();
            // New transform based on tracking.
            localToWorld.preMultTranslate(position);
            localToWorld.preMultRotate(orientation);

            // Finally, set transform
            auto scale = osg::Matrix::scale(1.f, mMirror ? -1.f : 1.f, 1.f);
            mTransform->setMatrix(scale * localToWorld * worldToLocal * mTransform->getMatrix());
        }

        void setTransform(osg::MatrixTransform* transform) {
            if (mTransform)
                mTransform->setCullingActive(true);
            mTransform = transform;
            if (mTransform)
                mTransform->setCullingActive(false);
        }

        std::shared_ptr<VR::Space> mSpace;
        std::string mAuditSourceSpaceName;
        std::shared_ptr<VR::Space> mAuditAimSpace;
        std::string mAuditAimSpaceName;
        osg::ref_ptr<osg::MatrixTransform> mTransform;
        osg::Vec3 mBaseOffset;
        osg::Quat mBaseOrientation;
        std::string mDebugName;
        int mAlignmentLogCount = 0;
        int mAlignmentLogFrame = 0;
        int mAimAuditLogCount = 0;
        int mAimAuditLogFrame = 0;
        bool mLeft;
        bool mMirror;
        bool mUseNativeGripOrientation;
    };

    class FalloutSpaceAxisLogCallback : public osg::NodeCallback
    {
    public:
        FalloutSpaceAxisLogCallback(std::string label)
            : mLabel(std::move(label))
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            if (mLogCount < 8 || (++mLogFrame % 300) == 0)
            {
                const auto paths = node->getParentalNodePaths();
                if (!paths.empty())
                {
                    const osg::Matrix localToWorld = osg::computeLocalToWorld(paths.front());
                    const osg::Vec3f worldPosition = localToWorld.getTrans();
                    const osg::Quat worldAttitude = localToWorld.getRotate();
                    ++mLogCount;
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VR tracked XYZ axis label=" << mLabel
                                     << " worldPosition=(" << worldPosition.x() << "," << worldPosition.y() << ","
                                     << worldPosition.z() << ") worldAttitude=(" << worldAttitude.x() << ","
                                     << worldAttitude.y() << "," << worldAttitude.z() << "," << worldAttitude.w()
                                     << ")";
                }
            }

            traverse(node, nv);
        }

    private:
        std::string mLabel;
        int mLogCount = 0;
        int mLogFrame = 0;
    };

    class FalloutControllerAxisCallback : public osg::NodeCallback
    {
    public:
        FalloutControllerAxisCallback(std::string side, std::string spaceName)
            : mSide(std::move(side))
            , mSpaceName(std::move(spaceName))
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            osg::PositionAttitudeTransform* transform = dynamic_cast<osg::PositionAttitudeTransform*>(node);
            const auto cached = sCachedControllerPoses.find(mSide);
            if (transform != nullptr && cached != sCachedControllerPoses.end() && cached->second.mValid
                && node->getNumParents() > 0)
            {
                osg::Group* parent = node->getParent(0);
                const auto parentPaths = parent->getParentalNodePaths();
                if (!parentPaths.empty())
                {
                    const osg::Matrix parentLocalToWorld = osg::computeLocalToWorld(parentPaths.front());
                    osg::Matrix worldToParent;
                    osg::Matrix worldToParentRotation;
                    const osg::Matrix parentWorldRotation = osg::Matrix::rotate(parentLocalToWorld.getRotate());
                    if (worldToParent.invert(parentLocalToWorld) && worldToParentRotation.invert(parentWorldRotation))
                    {
                        const osg::Vec3f localPosition = cached->second.mWorldPosition * worldToParent;
                        const osg::Matrix localRotation
                            = osg::Matrix::rotate(cached->second.mWorldOrientation) * worldToParentRotation;
                        transform->setPosition(localPosition);
                        transform->setAttitude(localRotation.getRotate());

                        if (mLogCount < 8 || (++mLogFrame % 300) == 0)
                        {
                            ++mLogCount;
                            Log(Debug::Verbose) << "FNV/ESM4 diag: VR wrist/tracking XYZ axis side=" << mSide
                                             << " space=" << mSpaceName
                                             << " controllerWorld=(" << cached->second.mWorldPosition.x() << ","
                                             << cached->second.mWorldPosition.y() << ","
                                             << cached->second.mWorldPosition.z() << ")"
                                             << " localPosition=(" << localPosition.x() << ","
                                             << localPosition.y() << "," << localPosition.z() << ")"
                                             << " localAttitude=(" << transform->getAttitude().x() << ","
                                             << transform->getAttitude().y() << "," << transform->getAttitude().z()
                                             << "," << transform->getAttitude().w() << ")";
                        }
                    }
                }
            }

            traverse(node, nv);
        }

    private:
        std::string mSide;
        std::string mSpaceName;
        int mLogCount = 0;
        int mLogFrame = 0;
    };

    class FalloutHandCuffControllerLocalAudit : public osg::NodeCallback
    {
    public:
        FalloutHandCuffControllerLocalAudit(std::string spaceName, std::string side, std::string model,
            osg::Vec3f modelCuffAnchor, osg::Vec3f handLocalTarget)
            : mSpaceName(std::move(spaceName))
            , mSide(std::move(side))
            , mModel(std::move(model))
            , mModelCuffAnchor(modelCuffAnchor)
            , mHandLocalTarget(handLocalTarget)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            traverse(node, nv);

            if (mLogCount >= 30 && (++mLogFrame % 300) != 0)
                return;

            const auto cached = sCachedControllerPoses.find(mSide);
            if (cached == sCachedControllerPoses.end() || !cached->second.mValid)
                return;

            const auto paths = node->getParentalNodePaths();
            if (paths.empty())
                return;

            ++mLogCount;
            const osg::Matrix localToWorld = osg::computeLocalToWorld(paths.front());
            osg::Matrix handLocalToWorld = osg::Matrix::identity();
            if (node->getNumParents() > 0)
            {
                osg::Group* handNode = node->getParent(0);
                const auto handPaths = handNode->getParentalNodePaths();
                if (!handPaths.empty())
                    handLocalToWorld = osg::computeLocalToWorld(handPaths.front());
            }
            const osg::Vec3f cuffWorld = mModelCuffAnchor * localToWorld;
            const osg::Vec3f targetWorld = mHandLocalTarget * handLocalToWorld;
            const osg::Vec3f controllerWorld = cached->second.mWorldPosition;
            const osg::Quat controllerOrientation = cached->second.mWorldOrientation;
            const osg::Vec3f controllerLocal = controllerOrientation.inverse() * (cuffWorld - controllerWorld);
            const osg::Vec3f targetControllerLocal = controllerOrientation.inverse() * (targetWorld - controllerWorld);
            const osg::Vec3f handOriginLocal
                = controllerOrientation.inverse() * (handLocalToWorld.getTrans() - controllerWorld);
            const osg::Vec3f handAxisXLocal
                = controllerOrientation.inverse()
                * osg::Matrix::transform3x3(osg::Vec3f(1.f, 0.f, 0.f), handLocalToWorld);
            const osg::Vec3f handAxisYLocal
                = controllerOrientation.inverse()
                * osg::Matrix::transform3x3(osg::Vec3f(0.f, 1.f, 0.f), handLocalToWorld);
            const osg::Vec3f handAxisZLocal
                = controllerOrientation.inverse()
                * osg::Matrix::transform3x3(osg::Vec3f(0.f, 0.f, 1.f), handLocalToWorld);
            const osg::Vec3f surfaceXLocal
                = controllerOrientation.inverse() * osg::Matrix::transform3x3(osg::Vec3f(1.f, 0.f, 0.f), localToWorld);
            const osg::Vec3f surfaceYLocal
                = controllerOrientation.inverse() * osg::Matrix::transform3x3(osg::Vec3f(0.f, 1.f, 0.f), localToWorld);
            const osg::Vec3f surfaceZLocal
                = controllerOrientation.inverse() * osg::Matrix::transform3x3(osg::Vec3f(0.f, 0.f, 1.f), localToWorld);

            Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand cuff controller-local audit side=" << mSide
                             << " model=" << mModel
                             << " space=" << mSpaceName
                             << " controllerWorld=(" << controllerWorld.x() << "," << controllerWorld.y()
                             << "," << controllerWorld.z() << ")"
                             << " cuffWorld=(" << cuffWorld.x() << "," << cuffWorld.y() << ","
                             << cuffWorld.z() << ")"
                             << " controllerLocal=(" << controllerLocal.x() << "," << controllerLocal.y()
                             << "," << controllerLocal.z() << ")"
                             << " targetControllerLocal=(" << targetControllerLocal.x() << ","
                             << targetControllerLocal.y() << "," << targetControllerLocal.z() << ")"
                             << " distance=" << controllerLocal.length()
                             << " handLocalTarget=(" << mHandLocalTarget.x() << "," << mHandLocalTarget.y()
                             << "," << mHandLocalTarget.z() << ")"
                             << " handOriginLocal=(" << handOriginLocal.x() << "," << handOriginLocal.y()
                             << "," << handOriginLocal.z() << ")"
                             << " handAxisXLocal=(" << handAxisXLocal.x() << "," << handAxisXLocal.y()
                             << "," << handAxisXLocal.z() << ")"
                             << " handAxisYLocal=(" << handAxisYLocal.x() << "," << handAxisYLocal.y()
                             << "," << handAxisYLocal.z() << ")"
                             << " handAxisZLocal=(" << handAxisZLocal.x() << "," << handAxisZLocal.y()
                             << "," << handAxisZLocal.z() << ")"
                             << " modelCuffAnchor=(" << mModelCuffAnchor.x() << "," << mModelCuffAnchor.y()
                             << "," << mModelCuffAnchor.z() << ")"
                             << " surfaceXLocal=(" << surfaceXLocal.x() << "," << surfaceXLocal.y()
                             << "," << surfaceXLocal.z() << ")"
                             << " surfaceYLocal=(" << surfaceYLocal.x() << "," << surfaceYLocal.y()
                             << "," << surfaceYLocal.z() << ")"
                             << " surfaceZLocal=(" << surfaceZLocal.x() << "," << surfaceZLocal.y()
                             << "," << surfaceZLocal.z() << ")";
        }

    private:
        std::string mSpaceName;
        std::string mSide;
        std::string mModel;
        osg::Vec3f mModelCuffAnchor;
        osg::Vec3f mHandLocalTarget;
        int mLogCount = 0;
        int mLogFrame = 0;
    };

    class FalloutHandControllerSpacePosition : public osg::NodeCallback
    {
    public:
        FalloutHandControllerSpacePosition(std::string side, std::string model, osg::Vec3f modelCuffAnchor,
            osg::Vec3f controllerLocalTarget, osg::Quat controllerLocalAttitude)
            : mSide(std::move(side))
            , mModel(std::move(model))
            , mModelCuffAnchor(modelCuffAnchor)
            , mControllerLocalTarget(controllerLocalTarget)
            , mControllerLocalAttitude(controllerLocalAttitude)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            osg::PositionAttitudeTransform* transform = dynamic_cast<osg::PositionAttitudeTransform*>(node);
            const auto cached = sCachedControllerPoses.find(mSide);
            if (transform != nullptr && cached != sCachedControllerPoses.end() && cached->second.mValid
                && node->getNumParents() > 0)
            {
                osg::Group* parent = node->getParent(0);
                const auto parentPaths = parent->getParentalNodePaths();
                if (!parentPaths.empty())
                {
                    const osg::Matrix parentLocalToWorld = osg::computeLocalToWorld(parentPaths.front());
                    osg::Matrix worldToParent;
                    osg::Matrix worldToParentRotation;
                    const osg::Matrix parentWorldRotation = osg::Matrix::rotate(parentLocalToWorld.getRotate());
                    const osg::Vec3f targetWorld = cached->second.mWorldPosition
                        + cached->second.mWorldOrientation * mControllerLocalTarget;
                    if (worldToParent.invert(parentLocalToWorld) && worldToParentRotation.invert(parentWorldRotation))
                    {
                        const osg::Matrix targetWorldRotation
                            = osg::Matrix::rotate(mControllerLocalAttitude)
                            * osg::Matrix::rotate(cached->second.mWorldOrientation);
                        const osg::Matrix localRotation = targetWorldRotation * worldToParentRotation;
                        const osg::Vec3f localTarget = targetWorld * worldToParent;
                        const osg::Vec3f rotatedModelCuff = mModelCuffAnchor * localRotation;
                        const osg::Vec3f localPosition = localTarget - rotatedModelCuff;
                        transform->setAttitude(localRotation.getRotate());
                        transform->setPosition(localPosition);

                        if (mLogCount < 20 || (++mLogFrame % 300) == 0)
                        {
                            ++mLogCount;
                            const osg::BoundingBox parentLocalBounds = computeNodeBounds(*node);
                            osg::BoundingBox controllerLocalBounds;
                            const osg::Matrix parentToController
                                = parentLocalToWorld
                                * osg::Matrix::translate(-cached->second.mWorldPosition)
                                * osg::Matrix::rotate(cached->second.mWorldOrientation.inverse());
                            expandTransformedBounds(controllerLocalBounds, parentLocalBounds, parentToController);
                            const osg::Vec3f visibleCenter = controllerLocalBounds.valid()
                                ? controllerLocalBounds.center()
                                : osg::Vec3f();
                            const osg::Vec3f visibleExtent = controllerLocalBounds.valid()
                                ? boundsExtent(controllerLocalBounds)
                                : osg::Vec3f();
                            const osg::Vec3f cuffToVisibleCenter = visibleCenter - mControllerLocalTarget;
                            Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand controller-space position solve side="
                                             << mSide << " model=" << mModel
                                             << " controllerLocalTarget=(" << mControllerLocalTarget.x() << ","
                                             << mControllerLocalTarget.y() << "," << mControllerLocalTarget.z() << ")"
                                             << " targetWorld=(" << targetWorld.x() << "," << targetWorld.y() << ","
                                             << targetWorld.z() << ")"
                                             << " localTarget=(" << localTarget.x() << "," << localTarget.y() << ","
                                             << localTarget.z() << ")"
                                             << " rotatedModelCuff=(" << rotatedModelCuff.x() << ","
                                             << rotatedModelCuff.y() << "," << rotatedModelCuff.z() << ")"
                                             << " localPosition=(" << localPosition.x() << "," << localPosition.y()
                                             << "," << localPosition.z() << ")"
                                             << " localAttitude=(" << transform->getAttitude().x() << ","
                                             << transform->getAttitude().y() << "," << transform->getAttitude().z()
                                             << "," << transform->getAttitude().w() << ")"
                                             << " visibleBoundsControllerLocal=(" << controllerLocalBounds.xMin()
                                             << "," << controllerLocalBounds.yMin() << ","
                                             << controllerLocalBounds.zMin() << ")-(" << controllerLocalBounds.xMax()
                                             << "," << controllerLocalBounds.yMax() << ","
                                             << controllerLocalBounds.zMax() << ")"
                                             << " visibleCenterControllerLocal=(" << visibleCenter.x() << ","
                                             << visibleCenter.y() << "," << visibleCenter.z() << ")"
                                             << " visibleExtentControllerLocal=(" << visibleExtent.x() << ","
                                             << visibleExtent.y() << "," << visibleExtent.z() << ")"
                                             << " cuffToVisibleCenter=(" << cuffToVisibleCenter.x() << ","
                                             << cuffToVisibleCenter.y() << "," << cuffToVisibleCenter.z() << ")";
                        }
                    }
                }
            }

            traverse(node, nv);
        }

    private:
        std::string mSide;
        std::string mModel;
        osg::Vec3f mModelCuffAnchor;
        osg::Vec3f mControllerLocalTarget;
        osg::Quat mControllerLocalAttitude;
        int mLogCount = 0;
        int mLogFrame = 0;
    };

    VRAnimation::VRAnimation(const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode,
        Resource::ResourceSystem* resourceSystem, bool disableSounds, osg::ref_ptr<osg::Group> sceneRoot)
        // Note that i let it construct as 3rd person and then later update it to VM_VRFirstPerson
        // when the character controller updates
        : MWRender::NpcAnimation(ptr, parentNode, resourceSystem, disableSounds, VM_Normal, 55.f)
        // The player model needs to be pushed back a little to make sure the player's view point is naturally
        // protruding Pushing the camera forward instead would produce an unnatural extra movement when rotating the
        // player model.
        , mModelOffset(new osg::MatrixTransform(osg::Matrix::translate(osg::Vec3(0, -15, 0))))
        , mCrosshairsEnabled(false)
        , mSceneRoot(sceneRoot)
    {
        // mReferenceSpace = XR::Session::instance().getReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
        mLeftHandPath = VR::stringToXrPath(VR::Paths::LEFT_HAND);
        mRightHandPath = VR::stringToXrPath(VR::Paths::RIGHT_HAND);

        mWeaponDirectionTransform = new osg::MatrixTransform();
        mWeaponDirectionTransform->setName("Weapon Direction");
        mWeaponDirectionTransform->setUpdateCallback(new WeaponDirectionController);

        mModelOffset->setName("ModelOffset");

        mWeaponPointerTransform = new osg::MatrixTransform();
        mWeaponPointerTransform->setMatrix(osg::Matrix::scale(0.f, 0.f, 0.f));
        mWeaponPointerTransform->setName("Weapon Pointer");
        mWeaponPointerTransform->setUpdateCallback(new WeaponPointerController);
        // mWeaponDirectionTransform->addChild(mWeaponPointerTransform);

        auto& xrInput = OpenXRInput::instance();

        Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand tracking mode inventoryHands=1";

        for (int i = 0; i < 2; i++)
        {
            XrPath path = i == 0 ? mLeftHandPath : mRightHandPath;
            auto& ctx = mVrControllers[path] = {};
            ctx.topLevelPath = path;
            if (VR::getLeftHandedMode())
                ctx.spaceName = i == 1 ? OpenXRInput::LeftHandGrip : OpenXRInput::RightHandGrip;
            else
                ctx.spaceName = i == 0 ? OpenXRInput::LeftHandGrip : OpenXRInput::RightHandGrip;
            const std::string gripSpace = ctx.spaceName;
            const std::string wristSpace = i == 0 ? "LeftWristTop" : "RightWristTop";
            const bool useWristTop = xrInput.getSpace(wristSpace) != nullptr;
            if (useWristTop)
            {
                ctx.spaceName = wristSpace;
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 diag: VR wrist hand space missing " << wristSpace
                                    << "; using grip tracking space " << gripSpace;
            const osg::Vec3 offset = useWristTop ? osg::Vec3(0, 0, 0) : osg::Vec3(15, 0, 0);
            const bool useNativeGripOrientation = useWristTop;
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR hand tracking source hand=" << (i == 0 ? "left" : "right")
                             << " space=" << ctx.spaceName
                             << " nativeOrientation=" << useNativeGripOrientation
                             << " baseOffset=(" << offset.x() << "," << offset.y() << "," << offset.z() << ")";
            ctx.forearmBone = i == 0 ? "Bip01 L Forearm" : "Bip01 R Forearm";
            const std::string aimSpace = i == 0 ? OpenXRInput::LeftHandAim : OpenXRInput::RightHandAim;
            ctx.forearmController = std::make_unique<TrackingController>(
                xrInput.getSpace(ctx.spaceName), ctx.spaceName, xrInput.getSpace(aimSpace), aimSpace, offset, i == 0,
                VR::getLeftHandedMode(), useNativeGripOrientation, i == 0 ? "left" : "right");
            ctx.handBone = i == 0 ? "Bip01 L Hand" : "Bip01 R Hand";
            ctx.handController = new HandController;
            const bool left = i == 0;
            const std::string prefix = left ? "Bip01 L " : "Bip01 R ";
            for (const FingerBindingSpec& spec : getFalloutFingerBindingSpecs())
            {
                ctx.fingerBindings.push_back({ prefix + spec.boneSuffix, new FingerController(left, spec) });
            }
        }
    }

    VRAnimation::~VRAnimation()
    {
        clearFalloutVrHandSurfaces();
    }

    void VRAnimation::setViewMode(NpcAnimation::ViewMode viewMode)
    {
        if (viewMode != VM_VRFirstPerson && viewMode != VM_VRNormal)
        {
            static int sLoggedCoerce = 0;
            if (sLoggedCoerce < 24)
            {
                ++sLoggedCoerce;
                Log(Debug::Warning)
                    << "FNV/ESM4 diag: coerced non-VR player view mode to VM_VRFirstPerson to keep VR hands";
            }
            viewMode = VM_VRFirstPerson;
        }
        NpcAnimation::setViewMode(viewMode);
        return;
    }

    void VRAnimation::updateParts()
    {
        NpcAnimation::updateParts();

        if (mViewMode == VM_VRFirstPerson)
        {
            // Hide everything other than hands
            removeIndividualPart(ESM::PartReferenceType::PRT_Hair);
            removeIndividualPart(ESM::PartReferenceType::PRT_Head);
            // removeIndividualPart(ESM::PartReferenceType::PRT_LForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_LUpperarm);
            // removeIndividualPart(ESM::PartReferenceType::PRT_LWrist);
            removeIndividualPart(ESM::PartReferenceType::PRT_RForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_RUpperarm);
            removeIndividualPart(ESM::PartReferenceType::PRT_RWrist);
            removeIndividualPart(ESM::PartReferenceType::PRT_Cuirass);
            removeIndividualPart(ESM::PartReferenceType::PRT_Groin);
            removeIndividualPart(ESM::PartReferenceType::PRT_Neck);
            removeIndividualPart(ESM::PartReferenceType::PRT_Skirt);
            removeIndividualPart(ESM::PartReferenceType::PRT_Tail);
            removeIndividualPart(ESM::PartReferenceType::PRT_LLeg);
            removeIndividualPart(ESM::PartReferenceType::PRT_RLeg);
            removeIndividualPart(ESM::PartReferenceType::PRT_LAnkle);
            removeIndividualPart(ESM::PartReferenceType::PRT_RAnkle);
            removeIndividualPart(ESM::PartReferenceType::PRT_LKnee);
            removeIndividualPart(ESM::PartReferenceType::PRT_RKnee);
            removeIndividualPart(ESM::PartReferenceType::PRT_LFoot);
            removeIndividualPart(ESM::PartReferenceType::PRT_RFoot);
            removeIndividualPart(ESM::PartReferenceType::PRT_LPauldron);
            removeIndividualPart(ESM::PartReferenceType::PRT_RPauldron);
        }
        else if (mViewMode == VM_VRNormal)
        {
            removeIndividualPart(ESM::PartReferenceType::PRT_LForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_LWrist);
            removeIndividualPart(ESM::PartReferenceType::PRT_RForearm);
            removeIndividualPart(ESM::PartReferenceType::PRT_RWrist);
        }

        attachFalloutVrHandSurfaces();
        updateTrackingControllers();
        updateCharHeight();
    }

    void VRAnimation::setFalloutVrHandSurfaces(std::vector<FalloutVrHandSurface> surfaces)
    {
        const bool sameSurfaces = surfaces.size() == mFalloutVrHandSurfaces.size()
            && std::equal(surfaces.begin(), surfaces.end(), mFalloutVrHandSurfaces.begin(),
                [](const FalloutVrHandSurface& left, const FalloutVrHandSurface& right) {
                    return left.model == right.model && left.diffuseTexture == right.diffuseTexture
                        && left.source == right.source && left.left == right.left;
                });

        if (sameSurfaces)
        {
            attachFalloutVrHandSurfaces();
            updateTrackingControllers();
            return;
        }

        clearFalloutVrHandSurfaces();
        mFalloutVrHandSurfaces = std::move(surfaces);
        attachFalloutVrHandSurfaces();
        updateTrackingControllers();
    }

    void VRAnimation::clearFalloutVrHandSurfaces()
    {
        int parentLinksRemoved = 0;
        for (const osg::ref_ptr<osg::Node>& node : mFalloutVrHandSurfaceNodes)
        {
            if (node == nullptr)
                continue;

            while (node->getNumParents() > 0)
            {
                osg::Group* parent = node->getParent(0);
                if (parent == nullptr || !parent->removeChild(node.get()))
                    break;
                ++parentLinksRemoved;
            }
        }

        if (!mFalloutVrHandSurfaceNodes.empty() || parentLinksRemoved > 0)
            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly cleared attached surfaces nodes="
                             << mFalloutVrHandSurfaceNodes.size() << " parentLinks=" << parentLinksRemoved;

        mFalloutVrHandSurfaceNodes.clear();
        mFalloutVrHandSurfacesAttached = false;
    }

    void VRAnimation::attachFalloutVrHandSurfaces()
    {
        if (mViewMode != VM_VRFirstPerson)
            return;

        if (mFalloutVrHandSurfacesAttached || mFalloutVrHandSurfaces.empty() || mObjectRoot == nullptr)
            return;

        if (mSceneRoot != nullptr && getEnvFloat("OPENMW_FNV_VR_CONTROLLER_DEBUG_AXES", 0.f) != 0.f)
        {
            const float axisLength = getEnvFloat(
                "OPENMW_FNV_VR_CONTROLLER_DEBUG_AXIS_LENGTH", getEnvFloat("OPENMW_FNV_VR_DEBUG_AXIS_LENGTH", 16.f));
            const float wristAxisLength = getEnvFloat("OPENMW_FNV_VR_WRIST_DEBUG_AXIS_LENGTH", axisLength);
            for (const auto& sidePath : { std::make_pair(std::string("left"), mLeftHandPath),
                     std::make_pair(std::string("right"), mRightHandPath) })
            {
                const auto ctx = mVrControllers.find(sidePath.second);
                const std::string spaceName = ctx != mVrControllers.end() ? ctx->second.spaceName : std::string();
                osg::ref_ptr<osg::Geode> axis = new osg::Geode;
                axis->setName("FNV VR " + sidePath.first + " wrist tracking XYZ debug axis");
                axis->addDrawable(createAxisMarkerGeometry(wristAxisLength));
                axis->addDrawable(createAxisOriginMarker(
                    getEnvFloat("OPENMW_FNV_VR_WRIST_ORIGIN_RADIUS", 3.f), osg::Vec4f(1.f, 0.7f, 0.f, 1.f)));
                axis->setCullingActive(false);

                osg::ref_ptr<osg::PositionAttitudeTransform> marker = new osg::PositionAttitudeTransform;
                marker->setName("FNV VR " + sidePath.first + " wrist tracking XYZ debug axis transform");
                marker->addChild(axis);
                marker->addUpdateCallback(new FalloutControllerAxisCallback(sidePath.first, spaceName));
                marker->setCullingActive(false);
                mSceneRoot->addChild(marker);
                mFalloutVrHandSurfaceNodes.push_back(marker);
            }

            struct TrackedAxisSpec
            {
                std::string label;
                std::string spaceName;
                float length;
                osg::Vec4f originColor;
            };

            const float gripAxisLength = getEnvFloat("OPENMW_FNV_VR_GRIP_DEBUG_AXIS_LENGTH", axisLength * 1.35f);
            const float aimAxisLength = getEnvFloat("OPENMW_FNV_VR_AIM_DEBUG_AXIS_LENGTH", axisLength * 1.1f);
            const std::vector<TrackedAxisSpec> trackedAxes = {
                { "left-grip", OpenXRInput::LeftHandGrip, gripAxisLength, osg::Vec4f(1.f, 0.f, 1.f, 1.f) },
                { "right-grip", OpenXRInput::RightHandGrip, gripAxisLength, osg::Vec4f(1.f, 0.f, 1.f, 1.f) },
                { "left-aim", OpenXRInput::LeftHandAim, aimAxisLength, osg::Vec4f(0.f, 1.f, 1.f, 1.f) },
                { "right-aim", OpenXRInput::RightHandAim, aimAxisLength, osg::Vec4f(0.f, 1.f, 1.f, 1.f) },
            };
            auto& xrInput = OpenXRInput::instance();
            for (const TrackedAxisSpec& spec : trackedAxes)
            {
                std::shared_ptr<VR::Space> space = xrInput.getSpace(spec.spaceName);
                if (space == nullptr)
                {
                    Log(Debug::Warning) << "FNV/ESM4 diag: VR tracked XYZ axis skipped missing label="
                                        << spec.label << " space=" << spec.spaceName;
                    continue;
                }

                osg::ref_ptr<osg::Geode> axis = new osg::Geode;
                axis->setName("FNV VR " + spec.label + " tracked XYZ debug axis");
                axis->addDrawable(createAxisMarkerGeometry(spec.length));
                axis->addDrawable(createAxisOriginMarker(
                    getEnvFloat("OPENMW_FNV_VR_TRACKED_ORIGIN_RADIUS", 4.f), spec.originColor));
                axis->setCullingActive(false);

                osg::ref_ptr<VR::SpaceTransform> marker = new VR::SpaceTransform(space);
                marker->setName("FNV VR " + spec.label + " tracked XYZ debug axis transform");
                marker->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
                marker->addChild(axis);
                marker->addUpdateCallback(new FalloutSpaceAxisLogCallback(spec.label));
                marker->setCullingActive(false);
                mSceneRoot->addChild(marker);
                mFalloutVrHandSurfaceNodes.push_back(marker);
            }
        }

        const NodeMap& nodeMap = getNodeMap();
        osg::Group* leftHand = nullptr;
        osg::Group* rightHand = nullptr;
        osg::Group* bip01 = nullptr;
        if (const auto found = nodeMap.find("Bip01"); found != nodeMap.end())
            bip01 = found->second.get();
        if (const auto found = nodeMap.find("Bip01 L Hand"); found != nodeMap.end())
            leftHand = found->second.get();
        if (const auto found = nodeMap.find("Bip01 R Hand"); found != nodeMap.end())
            rightHand = found->second.get();

        osg::Group* master = mSkeleton != nullptr ? static_cast<osg::Group*>(mSkeleton) : mObjectRoot.get();
        const float pipBoyRotX = getEnvFloat("OPENMW_FNV_PIPBOY_ROT_X", 0.f);
        const float pipBoyRotY = getEnvFloat("OPENMW_FNV_PIPBOY_ROT_Y", 0.f);
        const float pipBoyRotZ = getEnvFloat("OPENMW_FNV_PIPBOY_ROT_Z", 90.f);
        const osg::Quat pipBoyAttitude = makeEulerDegrees(pipBoyRotX, pipBoyRotY, pipBoyRotZ);
        const osg::Vec3f pipBoyOffset(getEnvFloat("OPENMW_FNV_PIPBOY_OFFSET_X", -3.f),
            getEnvFloat("OPENMW_FNV_PIPBOY_OFFSET_Y", -13.f), getEnvFloat("OPENMW_FNV_PIPBOY_OFFSET_Z", -6.5f));
        std::array<osg::Vec3f, 2> pipBoySocketTargets;
        std::array<bool, 2> pipBoySocketTargetValid = { false, false };
        for (const FalloutVrHandSurface& surface : mFalloutVrHandSurfaces)
        {
            const std::string loweredModel = Misc::StringUtils::lowerCase(surface.model);
            if (loweredModel.find("pipboyarm") == std::string::npos)
                continue;

            const VFS::Path::Normalized correctedModel
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(surface.model));
            osg::ref_ptr<const osg::Node> templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);
            if (templateNode == nullptr)
                continue;

            osg::ref_ptr<osg::Node> socketTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
            osg::BoundingBox pipBoyBounds = computeNodeBounds(*socketTemplate);
            if (!pipBoyBounds.valid())
                continue;

            const osg::Quat surfacePipBoyAttitude
                = makeEulerDegrees(getPipBoyEnvFloat(surface.left, "ROT_X", pipBoyRotX),
                    getPipBoyEnvFloat(surface.left, "ROT_Y", pipBoyRotY),
                    getPipBoyEnvFloat(surface.left, "ROT_Z", pipBoyRotZ));
            const osg::Vec3f surfacePipBoyOffset(
                getPipBoyEnvFloat(surface.left, "OFFSET_X", pipBoyOffset.x()),
                getPipBoyEnvFloat(surface.left, "OFFSET_Y", pipBoyOffset.y()),
                getPipBoyEnvFloat(surface.left, "OFFSET_Z", pipBoyOffset.z()));
            osg::Vec3f socketModel(getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_X", pipBoyBounds.xMax()),
                getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_Y", pipBoyBounds.center().y()),
                getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_Z", pipBoyBounds.center().z()));
            osg::Vec3f socketMirrorScale(1.f, 1.f, 1.f);
            const bool rightPipBoyCalibrationSocket
                = !surface.left && surface.source.find("right-pipboy-calibration") != std::string::npos;
            if (rightPipBoyCalibrationSocket)
            {
                socketMirrorScale = osg::Vec3f(
                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_X", getPipBoyEnvFloat(false, "MIRROR_SCALE_X", 1.f)),
                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_Y", getPipBoyEnvFloat(false, "MIRROR_SCALE_Y", -1.f)),
                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_Z", getPipBoyEnvFloat(false, "MIRROR_SCALE_Z", 1.f)));
                socketModel = scaleVec3(socketMirrorScale, socketModel);
            }
            const std::size_t socketIndex = surface.left ? 0 : 1;
            pipBoySocketTargets[socketIndex] = surfacePipBoyOffset + surfacePipBoyAttitude * socketModel;
            pipBoySocketTargetValid[socketIndex] = true;
            const osg::Vec3f extent = boundsExtent(pipBoyBounds);
            Log(Debug::Verbose) << "FNV/ESM4 diag: PipBoy socket target side=" << (surface.left ? "left" : "right")
                             << " model=" << correctedModel.value()
                             << " boundsMin=(" << pipBoyBounds.xMin() << "," << pipBoyBounds.yMin() << ","
                             << pipBoyBounds.zMin() << ") boundsMax=(" << pipBoyBounds.xMax() << ","
                             << pipBoyBounds.yMax() << "," << pipBoyBounds.zMax() << ") extent=("
                             << extent.x() << "," << extent.y() << "," << extent.z() << ") socketModel=("
                             << socketModel.x() << "," << socketModel.y() << "," << socketModel.z()
                             << ") mirrorScale=(" << socketMirrorScale.x() << "," << socketMirrorScale.y()
                             << "," << socketMirrorScale.z()
                             << ") socketTarget=(" << pipBoySocketTargets[socketIndex].x() << ","
                             << pipBoySocketTargets[socketIndex].y() << ","
                             << pipBoySocketTargets[socketIndex].z() << ")";
        }
        int attachedCount = 0;
        int leftHandSurfaceCount = 0;
        int rightHandSurfaceCount = 0;
        int leftPipBoySurfaceCount = 0;
        int rightPipBoySurfaceCount = 0;
        for (const FalloutVrHandSurface& surface : mFalloutVrHandSurfaces)
        {
            if (surface.model.empty())
                continue;

            std::string loweredModel = Misc::StringUtils::lowerCase(surface.model);
            const bool pipBoyArm = loweredModel.find("pipboyarm") != std::string::npos;
            const bool rightPipBoyCalibration = pipBoyArm && !surface.left
                && surface.source.find("right-pipboy-calibration") != std::string::npos;
            const bool riggedHandPart = !pipBoyArm
                && (loweredModel.find("hand") != std::string::npos || loweredModel.find("glove") != std::string::npos);

            osg::Group* attachNode = surface.left ? leftHand : rightHand;
            if (attachNode == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly attach skipped missing "
                                    << (surface.left ? "left" : "right")
                                    << " attach node model=" << surface.model;
                continue;
            }

            const VFS::Path::Normalized correctedModel
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(surface.model));
            osg::ref_ptr<const osg::Node> templateNode = mResourceSystem->getSceneManager()->getTemplate(correctedModel);
            osg::ref_ptr<const osg::Node> attachTemplateNode = templateNode;
            osg::ref_ptr<osg::Node> directStaticHandNode;
            osg::BoundingBox staticizedHandBounds;
            HandCuffAnchor staticizedHandCuffAnchor;
            bool staticizedRiggedHandPart = false;
            if (riggedHandPart)
            {
                osg::ref_ptr<osg::Node> staticTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
                StaticizeFalloutVrRiggedGeometryVisitor staticizeVisitor;
                staticTemplate->accept(staticizeVisitor);
                if (staticizeVisitor.mStaticizedRigGeometryCount > 0)
                {
                    attachTemplateNode = staticTemplate;
                    directStaticHandNode = staticTemplate;
                    staticizedRiggedHandPart = true;
                    staticizedHandBounds = computeNodeBounds(*staticTemplate);
                    if (staticizedHandBounds.valid())
                    {
                        staticizedHandCuffAnchor
                            = computeHandCuffAnchor(*staticTemplate, staticizedHandBounds, surface.left);
                        const osg::Vec3f center = staticizedHandBounds.center();
                        const osg::Vec3f extent = boundsExtent(staticizedHandBounds);
                        Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly staticized hand bounds model="
                                         << correctedModel.value()
                                         << " center=(" << center.x() << "," << center.y() << "," << center.z()
                                         << ") extent=(" << extent.x() << "," << extent.y() << "," << extent.z()
                                         << ") cuffAnchor=(" << staticizedHandCuffAnchor.mAnchor.x() << ","
                                         << staticizedHandCuffAnchor.mAnchor.y() << ","
                                         << staticizedHandCuffAnchor.mAnchor.z() << ") cuffVertices="
                                         << staticizedHandCuffAnchor.mVertexCount
                                         << " cuffValid=" << staticizedHandCuffAnchor.mValid;
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly staticized hand bounds invalid model="
                                            << correctedModel.value();
                }
                else
                    Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly failed to staticize rigged hand model="
                                        << correctedModel.value()
                                        << " seen=" << staticizeVisitor.mSeenRigGeometryCount
                                        << " missingSource=" << staticizeVisitor.mMissingSourceGeometryCount;
            }

            if (pipBoyArm)
                Log(Debug::Verbose) << "FNV/ESM4 diag: PipBoy wrist rotation degrees side="
                                 << (surface.left ? "left" : "right")
                                 << " calibration=" << rightPipBoyCalibration
                                 << " x=" << getPipBoyEnvFloat(surface.left, "ROT_X", pipBoyRotX)
                                 << " y=" << getPipBoyEnvFloat(surface.left, "ROT_Y", pipBoyRotY)
                                 << " z=" << getPipBoyEnvFloat(surface.left, "ROT_Z", pipBoyRotZ);
            osg::ref_ptr<osg::Node> attached;
            if (staticizedRiggedHandPart && directStaticHandNode != nullptr)
            {
                osg::ref_ptr<osg::PositionAttitudeTransform> handTransform = new osg::PositionAttitudeTransform;
                handTransform->setName("FNV VR Staticized Hand Surface");
                const float handRotX = getHandEnvFloat(surface.left, "ROT_X", 0.f);
                const float handRotY = getHandEnvFloat(surface.left, "ROT_Y", surface.left ? -90.f : 90.f);
                const float handRotZ = getHandEnvFloat(surface.left, "ROT_Z", 0.f);
                const osg::Quat baseHandAttitude = makeEulerDegrees(0.f, handRotY, handRotZ);
                const osg::Quat cuffRoll = osg::Quat(osg::DegreesToRadians(handRotX), osg::Vec3f(1.f, 0.f, 0.f));
                handTransform->setAttitude(baseHandAttitude * cuffRoll);
                ConfigureCullVisitor configureCullVisitor(false);
                directStaticHandNode->accept(configureCullVisitor);
                ApplyFalloutVrHandDebugStyleVisitor debugStyleVisitor;
                directStaticHandNode->accept(debugStyleVisitor);
                BindFalloutVrHandFingerControllersVisitor bindFingerVisitor(surface.left, true);
                directStaticHandNode->accept(bindFingerVisitor);
                handTransform->addChild(directStaticHandNode);
                attachNode->addChild(handTransform);
                attached = handTransform;
                Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly direct hand wrapper attached model="
                                 << correctedModel.value() << " attachNode=" << attachNode->getName()
                                 << " handRotationDegrees=(" << handRotX << "," << handRotY << "," << handRotZ
                                 << ") fingerBoneBindings=" << bindFingerVisitor.mBoundCount
                                 << " fingerBoneAxes=" << bindFingerVisitor.mAxisCount
                                 << " wireframe=" << debugStyleVisitor.mWireframe
                                 << " weightDebug=" << debugStyleVisitor.mWeightDebug
                                 << " weightColoredRigs=" << debugStyleVisitor.mWeightColoredRigCount
                                 << " weightColorMisses=" << debugStyleVisitor.mWeightColorMissCount;
            }
            else
            {
                const osg::Quat surfacePipBoyAttitude
                    = makeEulerDegrees(getPipBoyEnvFloat(surface.left, "ROT_X", pipBoyRotX),
                        getPipBoyEnvFloat(surface.left, "ROT_Y", pipBoyRotY),
                        getPipBoyEnvFloat(surface.left, "ROT_Z", pipBoyRotZ));
                attached = SceneUtil::attach(
                    std::move(attachTemplateNode), master, {}, attachNode, mResourceSystem->getSceneManager(),
                    pipBoyArm ? &surfacePipBoyAttitude : nullptr);
            }
            if (attached == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly attach failed model=" << correctedModel.value()
                                    << " source=" << surface.source;
                continue;
            }

            if (staticizedRiggedHandPart && staticizedHandBounds.valid())
            {
                if (osg::PositionAttitudeTransform* transform
                    = dynamic_cast<osg::PositionAttitudeTransform*>(attached.get()))
                {
                    const osg::Vec3f center = staticizedHandBounds.center();
                    const osg::Vec3f scale = transform->getScale();
                    const float wristX = surface.left ? staticizedHandBounds.xMax() : staticizedHandBounds.xMin();
                    osg::Vec3f cuffAnchor = staticizedHandCuffAnchor.mValid
                        ? staticizedHandCuffAnchor.mAnchor
                        : osg::Vec3f(wristX, center.y(), center.z());
                    cuffAnchor += osg::Vec3f(getHandEnvFloat(surface.left, "CUFF_MODEL_OFFSET_X", 0.f),
                        getHandEnvFloat(surface.left, "CUFF_MODEL_OFFSET_Y", 0.f),
                        getHandEnvFloat(surface.left, "CUFF_MODEL_OFFSET_Z", 0.f));
                    const osg::Vec3f modelCuffAnchor(
                        scale.x() * cuffAnchor.x(), scale.y() * cuffAnchor.y(), scale.z() * cuffAnchor.z());
                    const std::string auditSide = surface.left ? "left" : "right";
                    std::string auditSpaceName;
                    if (const auto ctx = mVrControllers.find(surface.left ? mLeftHandPath : mRightHandPath);
                        ctx != mVrControllers.end())
                    {
                        auditSpaceName = ctx->second.spaceName;
                    }
                    const osg::Vec3f modelCuffForward = normalizeOr(
                        osg::Vec3f(scale.x() * (center.x() - cuffAnchor.x()),
                            scale.y() * (center.y() - cuffAnchor.y()),
                            scale.z() * (center.z() - cuffAnchor.z())),
                        osg::Vec3f(surface.left ? -1.f : 1.f, 0.f, 0.f));
                    osg::Vec3f targetCuffAnchor(0.f, 0.f, 0.f);
                    const std::size_t socketIndex = surface.left ? 0 : 1;
                    const bool anchorToPipBoy = pipBoySocketTargetValid[socketIndex]
                        && getHandEnvFloat(surface.left, "ANCHOR_PIPBOY", 1.f) != 0.f;
                    const bool mirrorLeftPipBoySocket = !surface.left && !anchorToPipBoy
                        && pipBoySocketTargetValid[0]
                        && getHandEnvFloat(false, "MIRROR_LEFT_PIPBOY_SOCKET", 1.f) != 0.f;
                    if (anchorToPipBoy)
                    {
                        targetCuffAnchor = pipBoySocketTargets[socketIndex];
                        targetCuffAnchor += osg::Vec3f(getHandEnvFloat(surface.left, "SOCKET_X", 0.f),
                            getHandEnvFloat(surface.left, "SOCKET_Y", 0.f),
                            getHandEnvFloat(surface.left, "SOCKET_Z", 0.f));
                    }
                    else if (mirrorLeftPipBoySocket)
                    {
                        // Both wrist spaces are derived from their matching OpenXR aim pose. Mirror the
                        // calibrated left cuff target so the bare right hand uses the same anatomical
                        // wrist/palm reference instead of the raw RightWristTop origin.
                        targetCuffAnchor = pipBoySocketTargets[0]
                            + osg::Vec3f(getHandEnvFloat(true, "SOCKET_X", 0.f),
                                getHandEnvFloat(true, "SOCKET_Y", 0.f),
                                getHandEnvFloat(true, "SOCKET_Z", 0.f));
                        targetCuffAnchor.x() = -targetCuffAnchor.x();
                        targetCuffAnchor += osg::Vec3f(getHandEnvFloat(false, "OFFSET_X", 0.f),
                            getHandEnvFloat(false, "OFFSET_Y", 0.f),
                            getHandEnvFloat(false, "OFFSET_Z", 0.f));
                    }
                    else
                    {
                        targetCuffAnchor += osg::Vec3f(getHandEnvFloat(surface.left, "OFFSET_X", 0.f),
                            getHandEnvFloat(surface.left, "OFFSET_Y", 0.f),
                            getHandEnvFloat(surface.left, "OFFSET_Z", 0.f));
                    }
                    osg::Vec3f normalizeOffset = targetCuffAnchor - (transform->getAttitude() * modelCuffAnchor);
                    const osg::Vec3f modelPalmPivot(scale.x() * getHandEnvFloat(surface.left, "PIVOT_X", center.x()),
                        scale.y() * getHandEnvFloat(surface.left, "PIVOT_Y", center.y()),
                        scale.z() * getHandEnvFloat(surface.left, "PIVOT_Z", center.z()));
                    const float pivotRotX = getHandEnvFloat(surface.left, "PIVOT_ROT_X", 0.f);
                    const float pivotRotY = getHandEnvFloat(surface.left, "PIVOT_ROT_Y", 0.f);
                    const float pivotRotZ = getHandEnvFloat(surface.left, "PIVOT_ROT_Z", 0.f);
                    const bool frameSolve = anchorToPipBoy && getHandEnvFloat(surface.left, "FRAME_SOLVE", 1.f) != 0.f;
                    if (frameSolve)
                    {
                        const osg::Vec3f modelForward = normalizeOr(
                            osg::Vec3f(getHandEnvFloat(surface.left, "MODEL_FORWARD_X", modelCuffForward.x()),
                                getHandEnvFloat(surface.left, "MODEL_FORWARD_Y", modelCuffForward.y()),
                                getHandEnvFloat(surface.left, "MODEL_FORWARD_Z", modelCuffForward.z())),
                            modelCuffForward);
                        const osg::Vec3f modelUp = normalizeOr(
                            osg::Vec3f(getHandEnvFloat(surface.left, "MODEL_UP_X", 0.f),
                                getHandEnvFloat(surface.left, "MODEL_UP_Y", 0.f),
                                getHandEnvFloat(surface.left, "MODEL_UP_Z", 1.f)),
                            osg::Vec3f(0.f, 0.f, 1.f));
                        const osg::Vec3f pipBoyForward = normalizeOr(pipBoyAttitude * osg::Vec3f(1.f, 0.f, 0.f),
                            osg::Vec3f(0.f, 1.f, 0.f));
                        const osg::Vec3f pipBoyUp = normalizeOr(pipBoyAttitude * osg::Vec3f(0.f, 0.f, 1.f),
                            osg::Vec3f(0.f, 0.f, 1.f));
                        const osg::Vec3f targetForward = normalizeOr(
                            osg::Vec3f(getHandEnvFloat(surface.left, "TARGET_FORWARD_X", pipBoyForward.x()),
                                getHandEnvFloat(surface.left, "TARGET_FORWARD_Y", pipBoyForward.y()),
                                getHandEnvFloat(surface.left, "TARGET_FORWARD_Z", pipBoyForward.z())),
                            pipBoyForward);
                        const osg::Vec3f targetUp = normalizeOr(
                            osg::Vec3f(getHandEnvFloat(surface.left, "TARGET_UP_X", pipBoyUp.x()),
                                getHandEnvFloat(surface.left, "TARGET_UP_Y", pipBoyUp.y()),
                                getHandEnvFloat(surface.left, "TARGET_UP_Z", pipBoyUp.z())),
                            pipBoyUp);
                        osg::Quat solvedRotation = alignFrames(modelForward, modelUp, targetForward, targetUp);
                        const float solveRoll = getHandEnvFloat(surface.left, "SOLVE_ROLL", 0.f);
                        if (solveRoll != 0.f)
                            solvedRotation = osg::Quat(osg::DegreesToRadians(solveRoll), targetForward) * solvedRotation;
                        transform->setAttitude(solvedRotation);
                        normalizeOffset = targetCuffAnchor - (solvedRotation * modelCuffAnchor);
                        transform->setPosition(normalizeOffset);
                        Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly hand frame solved side="
                                         << (surface.left ? "left" : "right")
                                         << " model=" << correctedModel.value() << " modelForward=("
                                         << modelForward.x() << "," << modelForward.y() << ","
                                         << modelForward.z() << ") modelUp=("
                                         << modelUp.x() << "," << modelUp.y() << "," << modelUp.z()
                                         << ") targetForward=(" << targetForward.x() << "," << targetForward.y()
                                         << "," << targetForward.z() << ") targetUp=(" << targetUp.x() << ","
                                         << targetUp.y() << "," << targetUp.z() << ") solveRoll=" << solveRoll;
                    }
                    else
                    {
                        transform->setPosition(transform->getPosition() + normalizeOffset);
                    }
                    if (!frameSolve && (pivotRotX != 0.f || pivotRotY != 0.f || pivotRotZ != 0.f))
                    {
                        const osg::Quat baseRotation = transform->getAttitude();
                        const osg::Vec3f pivotTarget = transform->getPosition() + baseRotation * modelPalmPivot;
                        const osg::Quat pivotRotation = baseRotation * makeEulerDegrees(pivotRotX, pivotRotY, pivotRotZ);
                        transform->setAttitude(pivotRotation);
                        transform->setPosition(pivotTarget - pivotRotation * modelPalmPivot);
                    }
                    if (!surface.left && getHandEnvFloat(surface.left, "CONTROLLER_SPACE_POSITION", 0.f) != 0.f)
                    {
                        transform->addUpdateCallback(new FalloutHandControllerSpacePosition(
                            auditSide, correctedModel.value(), modelCuffAnchor, targetCuffAnchor,
                            transform->getAttitude()));
                    }
                    if (!surface.left && getEnvFloat("OPENMW_FNV_VR_DEBUG_AXES", 0.f) != 0.f)
                        addLocalAxisMarker(
                            *transform, modelCuffAnchor, getEnvFloat("OPENMW_FNV_VR_DEBUG_AXIS_LENGTH", 16.f));
                    if (!auditSpaceName.empty() && getEnvFloat("OPENMW_FNV_VR_UPDATE_AUDIT", 0.f) != 0.f)
                    {
                        transform->addUpdateCallback(new FalloutHandCuffControllerLocalAudit(
                            auditSpaceName, auditSide, correctedModel.value(), modelCuffAnchor, targetCuffAnchor));
                    }
                    const osg::Vec3f finalPosition = transform->getPosition();
                    const osg::Quat handRotation = transform->getAttitude();
                    const osg::Vec3f candidateXMin = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * staticizedHandBounds.xMin(), scale.y() * center.y(),
                                scale.z() * center.z());
                    const osg::Vec3f candidateXMax = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * staticizedHandBounds.xMax(), scale.y() * center.y(),
                                scale.z() * center.z());
                    const osg::Vec3f candidateYMin = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * center.x(), scale.y() * staticizedHandBounds.yMin(),
                                scale.z() * center.z());
                    const osg::Vec3f candidateYMax = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * center.x(), scale.y() * staticizedHandBounds.yMax(),
                                scale.z() * center.z());
                    const osg::Vec3f candidateWrist = finalPosition + handRotation * modelCuffAnchor;
                    const osg::Vec3f candidateZMin = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * center.x(), scale.y() * center.y(),
                                scale.z() * staticizedHandBounds.zMin());
                    const osg::Vec3f candidateZMax = finalPosition
                        + handRotation
                            * osg::Vec3f(scale.x() * center.x(), scale.y() * center.y(),
                                scale.z() * staticizedHandBounds.zMax());
                    const osg::Vec3f localAxisX = handRotation * osg::Vec3f(1.f, 0.f, 0.f);
                    const osg::Vec3f localAxisY = handRotation * osg::Vec3f(0.f, 1.f, 0.f);
                    const osg::Vec3f localAxisZ = handRotation * osg::Vec3f(0.f, 0.f, 1.f);
                    osg::Matrix parentToWorld = osg::Matrix::identity();
                    if (!attachNode->getParentalNodePaths().empty())
                        parentToWorld = osg::computeLocalToWorld(attachNode->getParentalNodePaths().front());
                    const osg::Vec3f worldWrist = candidateWrist * parentToWorld;
                    const osg::Vec3f worldAxisX = osg::Matrix::transform3x3(localAxisX, parentToWorld);
                    const osg::Vec3f worldAxisY = osg::Matrix::transform3x3(localAxisY, parentToWorld);
                    const osg::Vec3f worldAxisZ = osg::Matrix::transform3x3(localAxisZ, parentToWorld);
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly hand center normalized model="
                                     << correctedModel.value()
                                     << " offset=(" << normalizeOffset.x() << "," << normalizeOffset.y() << ","
                                     << normalizeOffset.z() << ") finalPosition=(" << transform->getPosition().x()
                                     << "," << transform->getPosition().y() << "," << transform->getPosition().z()
                                     << ") cuffAnchorTarget=(" << targetCuffAnchor.x() << ","
                                     << targetCuffAnchor.y() << "," << targetCuffAnchor.z()
                                     << ") cuffAnchorModel=(" << modelCuffAnchor.x() << ","
                                     << modelCuffAnchor.y() << "," << modelCuffAnchor.z() << ") rotation=("
                                     << transform->getAttitude().x() << "," << transform->getAttitude().y()
                                     << "," << transform->getAttitude().z() << ","
                                     << transform->getAttitude().w() << ") palmPivot=(" << modelPalmPivot.x() << ","
                                     << modelPalmPivot.y() << "," << modelPalmPivot.z() << ") pivotRotationDegrees=("
                                     << pivotRotX << "," << pivotRotY << "," << pivotRotZ << ") scale=("
                                     << scale.x() << "," << scale.y() << "," << scale.z()
                                     << ") modelCuffForward=(" << modelCuffForward.x() << ","
                                     << modelCuffForward.y() << "," << modelCuffForward.z()
                                     << ") cuffSliceMin=(" << staticizedHandCuffAnchor.mSliceMin.x() << ","
                                     << staticizedHandCuffAnchor.mSliceMin.y() << ","
                                     << staticizedHandCuffAnchor.mSliceMin.z() << ") cuffSliceMax=("
                                     << staticizedHandCuffAnchor.mSliceMax.x() << ","
                                     << staticizedHandCuffAnchor.mSliceMax.y() << ","
                                     << staticizedHandCuffAnchor.mSliceMax.z() << ")";
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly hand local anchor candidates model="
                                     << correctedModel.value()
                                     << " side=" << (surface.left ? "left" : "right")
                                     << " wrist=(" << candidateWrist.x() << "," << candidateWrist.y() << ","
                                     << candidateWrist.z() << ")"
                                     << " xMin=(" << candidateXMin.x() << "," << candidateXMin.y() << ","
                                     << candidateXMin.z() << ") xMax=(" << candidateXMax.x() << ","
                                     << candidateXMax.y() << "," << candidateXMax.z() << ") yMin=("
                                     << candidateYMin.x() << "," << candidateYMin.y() << "," << candidateYMin.z()
                                     << ") yMax=(" << candidateYMax.x() << "," << candidateYMax.y() << ","
                                     << candidateYMax.z() << ") zMin=(" << candidateZMin.x() << ","
                                     << candidateZMin.y() << "," << candidateZMin.z() << ") zMax=("
                                     << candidateZMax.x() << "," << candidateZMax.y() << ","
                                     << candidateZMax.z() << ")";
                    Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly hand rotation axes model="
                                     << correctedModel.value()
                                     << " side=" << (surface.left ? "left" : "right")
                                     << " attachNode=" << attachNode->getName()
                                     << " wristLocal=(" << candidateWrist.x() << "," << candidateWrist.y() << ","
                                     << candidateWrist.z() << ") wristWorld=(" << worldWrist.x() << ","
                                     << worldWrist.y() << "," << worldWrist.z() << ")"
                                     << " localX=(" << localAxisX.x() << "," << localAxisX.y() << ","
                                     << localAxisX.z() << ") localY=(" << localAxisY.x() << ","
                                     << localAxisY.y() << "," << localAxisY.z() << ") localZ=("
                                     << localAxisZ.x() << "," << localAxisZ.y() << "," << localAxisZ.z()
                                     << ") worldX=(" << worldAxisX.x() << "," << worldAxisX.y() << ","
                                     << worldAxisX.z() << ") worldY=(" << worldAxisY.x() << ","
                                     << worldAxisY.y() << "," << worldAxisY.z() << ") worldZ=("
                                     << worldAxisZ.x() << "," << worldAxisZ.y() << "," << worldAxisZ.z() << ")";
                }
                else
                    Log(Debug::Warning) << "FNV/ESM4 diag: VRHandsOnly hand center normalization skipped; attached "
                                           "node is not PAT model="
                                        << correctedModel.value();
            }

            if (pipBoyArm)
            {
                if (osg::PositionAttitudeTransform* transform
                    = dynamic_cast<osg::PositionAttitudeTransform*>(attached.get()))
                {
                    osg::Vec3f surfacePipBoyOffset(
                        getPipBoyEnvFloat(surface.left, "OFFSET_X", pipBoyOffset.x()),
                        getPipBoyEnvFloat(surface.left, "OFFSET_Y", pipBoyOffset.y()),
                        getPipBoyEnvFloat(surface.left, "OFFSET_Z", pipBoyOffset.z()));
                    osg::Vec3f pipBoyMirrorScale(1.f, 1.f, 1.f);
                    osg::Vec3f visualSocketMirrorScale(1.f, 1.f, 1.f);
                    osg::Vec3f visualSocketMirrorPivot;
                    bool visualSocketMirror = false;
                    if (rightPipBoyCalibration)
                    {
                        pipBoyMirrorScale = osg::Vec3f(getPipBoyEnvFloat(false, "MIRROR_SCALE_X", 1.f),
                            getPipBoyEnvFloat(false, "MIRROR_SCALE_Y", -1.f),
                            getPipBoyEnvFloat(false, "MIRROR_SCALE_Z", 1.f));

                        if (getPipBoyEnvFloat(false, "MIRROR_COMPENSATE_SOCKET", 0.f) != 0.f)
                        {
                            osg::ref_ptr<osg::Node> socketTemplate
                                = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
                            osg::BoundingBox pipBoyBounds = computeNodeBounds(*socketTemplate);
                            if (pipBoyBounds.valid())
                            {
                                const osg::Vec3f socketModel(
                                    getPipBoyEnvFloat(false, "SOCKET_MODEL_X", pipBoyBounds.xMax()),
                                    getPipBoyEnvFloat(false, "SOCKET_MODEL_Y", pipBoyBounds.center().y()),
                                    getPipBoyEnvFloat(false, "SOCKET_MODEL_Z", pipBoyBounds.center().z()));
                                const osg::Vec3f socketMirrorScale(
                                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_X",
                                        getPipBoyEnvFloat(false, "MIRROR_SCALE_X", 1.f)),
                                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_Y",
                                        getPipBoyEnvFloat(false, "MIRROR_SCALE_Y", -1.f)),
                                    getPipBoyEnvFloat(false, "SOCKET_MIRROR_SCALE_Z",
                                        getPipBoyEnvFloat(false, "MIRROR_SCALE_Z", 1.f)));
                                const osg::Vec3f socketTargetModel = scaleVec3(socketMirrorScale, socketModel);
                                const osg::Vec3f mirroredSocketModel = scaleVec3(pipBoyMirrorScale, socketModel);
                                const osg::Vec3f socketCompensation
                                    = transform->getAttitude() * (socketTargetModel - mirroredSocketModel);
                                surfacePipBoyOffset += socketCompensation;
                                Log(Debug::Verbose) << "FNV/ESM4 diag: right PipBoy visual mirror compensation socketScale=("
                                                 << socketMirrorScale.x() << "," << socketMirrorScale.y() << ","
                                                 << socketMirrorScale.z() << ") visualScale=("
                                                 << pipBoyMirrorScale.x() << "," << pipBoyMirrorScale.y() << ","
                                                 << pipBoyMirrorScale.z() << ") socketModel=(" << socketModel.x()
                                                 << "," << socketModel.y() << "," << socketModel.z()
                                                 << ") socketTargetModel=(" << socketTargetModel.x() << ","
                                                 << socketTargetModel.y() << "," << socketTargetModel.z()
                                                 << ") visualSocketModel=(" << mirroredSocketModel.x() << ","
                                                 << mirroredSocketModel.y() << "," << mirroredSocketModel.z()
                                                 << ") compensation=(" << socketCompensation.x() << ","
                                                 << socketCompensation.y() << "," << socketCompensation.z() << ")";
                            }
                            else
                            {
                                Log(Debug::Warning)
                                    << "FNV/ESM4 diag: right PipBoy mirror compensation skipped invalid bounds";
                            }
                        }
                        transform->setScale(osg::Vec3f(transform->getScale().x() * pipBoyMirrorScale.x(),
                            transform->getScale().y() * pipBoyMirrorScale.y(),
                            transform->getScale().z() * pipBoyMirrorScale.z()));
                        if (mirrorScaleFlipsHandedness(transform->getScale()))
                            applyMirroredFrontFaceState(*transform);

                        osg::ref_ptr<osg::Node> mirrorTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
                        const osg::BoundingBox pipBoyBounds = computeNodeBounds(*mirrorTemplate);
                        if (pipBoyBounds.valid())
                        {
                            visualSocketMirrorPivot = osg::Vec3f(
                                getPipBoyEnvFloat(false, "SOCKET_MODEL_X", pipBoyBounds.xMax()),
                                getPipBoyEnvFloat(false, "SOCKET_MODEL_Y", pipBoyBounds.center().y()),
                                getPipBoyEnvFloat(false, "SOCKET_MODEL_Z", pipBoyBounds.center().z()));
                            visualSocketMirrorScale = osg::Vec3f(
                                getPipBoyEnvFloat(false, "SOCKET_VISUAL_MIRROR_SCALE_X", 1.f),
                                getPipBoyEnvFloat(false, "SOCKET_VISUAL_MIRROR_SCALE_Y", 1.f),
                                getPipBoyEnvFloat(false, "SOCKET_VISUAL_MIRROR_SCALE_Z", 1.f));
                            visualSocketMirror = visualSocketMirrorScale != osg::Vec3f(1.f, 1.f, 1.f);
                            wrapChildrenInSocketMirror(*transform, visualSocketMirrorPivot, visualSocketMirrorScale);
                            if (visualSocketMirror)
                            {
                                Log(Debug::Verbose) << "FNV/ESM4 diag: right PipBoy socket visual mirror pivot=("
                                                 << visualSocketMirrorPivot.x() << "," << visualSocketMirrorPivot.y()
                                                 << "," << visualSocketMirrorPivot.z() << ") scale=("
                                                 << visualSocketMirrorScale.x() << "," << visualSocketMirrorScale.y()
                                                 << "," << visualSocketMirrorScale.z() << ")";
                            }
                        }
                    }
                    osg::Vec3f pipBoyVisualOffset(0.f, 0.f, 0.f);
                    if (rightPipBoyCalibration)
                    {
                        pipBoyVisualOffset = osg::Vec3f(getPipBoyEnvFloat(false, "VISUAL_OFFSET_X", 0.f),
                            getPipBoyEnvFloat(false, "VISUAL_OFFSET_Y", 0.f),
                            getPipBoyEnvFloat(false, "VISUAL_OFFSET_Z", 0.f));
                        surfacePipBoyOffset += pipBoyVisualOffset;
                    }
                    transform->setPosition(transform->getPosition() + surfacePipBoyOffset);
                    {
                        osg::ref_ptr<osg::Node> boundsTemplate = osg::clone(templateNode.get(), osg::CopyOp::DEEP_COPY_ALL);
                        const osg::BoundingBox pipBoyBounds = computeNodeBounds(*boundsTemplate);
                        if (pipBoyBounds.valid())
                        {
                            const osg::Vec3f rawSocketModel(
                                getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_X", pipBoyBounds.xMax()),
                                getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_Y", pipBoyBounds.center().y()),
                                getPipBoyEnvFloat(surface.left, "SOCKET_MODEL_Z", pipBoyBounds.center().z()));
                            const auto transformModelPoint = [&](const osg::Vec3f& point) {
                                const osg::Vec3f visualPoint = visualSocketMirror
                                    ? mirrorAroundPoint(point, visualSocketMirrorPivot, visualSocketMirrorScale)
                                    : point;
                                return transform->getPosition()
                                    + transform->getAttitude() * scaleVec3(transform->getScale(), visualPoint);
                            };

                            osg::BoundingBox visualBounds;
                            for (int x = 0; x < 2; ++x)
                            {
                                for (int y = 0; y < 2; ++y)
                                {
                                    for (int z = 0; z < 2; ++z)
                                    {
                                        visualBounds.expandBy(transformModelPoint(osg::Vec3f(
                                            x == 0 ? pipBoyBounds.xMin() : pipBoyBounds.xMax(),
                                            y == 0 ? pipBoyBounds.yMin() : pipBoyBounds.yMax(),
                                            z == 0 ? pipBoyBounds.zMin() : pipBoyBounds.zMax())));
                                    }
                                }
                            }

                            const osg::Vec3f visualSocket = transformModelPoint(rawSocketModel);
                            const osg::Vec3f visualCenter = transformModelPoint(pipBoyBounds.center());
                            const std::size_t socketIndex = surface.left ? 0 : 1;
                            const bool targetValid = pipBoySocketTargetValid[socketIndex];
                            const osg::Vec3f targetSocket
                                = targetValid ? pipBoySocketTargets[socketIndex] : osg::Vec3f();
                            const osg::Vec3f socketError = targetValid ? visualSocket - targetSocket : osg::Vec3f();
                            const osg::Vec3f centerFromSocket = visualCenter - visualSocket;
                            Log(Debug::Verbose) << "FNV/ESM4 diag: PipBoy visual anchors side="
                                             << (surface.left ? "left" : "right")
                                             << " calibration=" << rightPipBoyCalibration
                                             << " rawSocketModel=(" << rawSocketModel.x() << ","
                                             << rawSocketModel.y() << "," << rawSocketModel.z() << ")"
                                             << " visualSocket=(" << visualSocket.x() << ","
                                             << visualSocket.y() << "," << visualSocket.z() << ")"
                                             << " targetValid=" << targetValid
                                             << " targetSocket=(" << targetSocket.x() << ","
                                             << targetSocket.y() << "," << targetSocket.z() << ")"
                                             << " socketError=(" << socketError.x() << "," << socketError.y()
                                             << "," << socketError.z() << ")"
                                             << " visualCenter=(" << visualCenter.x() << ","
                                             << visualCenter.y() << "," << visualCenter.z() << ")"
                                             << " centerFromSocket=(" << centerFromSocket.x() << ","
                                             << centerFromSocket.y() << "," << centerFromSocket.z() << ")"
                                             << " visualBoundsMin=(" << visualBounds.xMin() << ","
                                             << visualBounds.yMin() << "," << visualBounds.zMin() << ")"
                                             << " visualBoundsMax=(" << visualBounds.xMax() << ","
                                             << visualBounds.yMax() << "," << visualBounds.zMax() << ")"
                                             << " attitude=(" << transform->getAttitude().x() << ","
                                             << transform->getAttitude().y() << "," << transform->getAttitude().z()
                                             << "," << transform->getAttitude().w() << ")"
                                             << " scale=(" << transform->getScale().x() << ","
                                             << transform->getScale().y() << "," << transform->getScale().z()
                                             << ") position=(" << transform->getPosition().x() << ","
                                             << transform->getPosition().y() << "," << transform->getPosition().z()
                                             << ")";
                        }
                    }
                    if (rightPipBoyCalibration)
                    {
                        if (getEnvFloat("OPENMW_FNV_RIGHT_PIPBOY_DEBUG_STYLE", 0.f) != 0.f)
                            applyRightPipBoyCalibrationStyle(*attached);
                        if (getEnvFloat("OPENMW_FNV_VR_DEBUG_AXES", 0.f) != 0.f)
                            addLocalAxisMarker(
                                *transform, osg::Vec3f(), getEnvFloat("OPENMW_FNV_VR_DEBUG_AXIS_LENGTH", 16.f));
                    }
                    Log(Debug::Verbose) << "FNV/ESM4 diag: PipBoy wrist offset applied side="
                                     << (surface.left ? "left" : "right")
                                     << " calibration=" << rightPipBoyCalibration
                                     << " x=" << surfacePipBoyOffset.x()
                                     << " y=" << surfacePipBoyOffset.y() << " z=" << surfacePipBoyOffset.z()
                                     << " visualOffset=(" << pipBoyVisualOffset.x() << "," << pipBoyVisualOffset.y()
                                     << "," << pipBoyVisualOffset.z() << ")"
                                     << " scale=(" << transform->getScale().x() << ","
                                     << transform->getScale().y() << "," << transform->getScale().z() << ")"
                                     << " finalPosition=(" << transform->getPosition().x() << ","
                                     << transform->getPosition().y() << "," << transform->getPosition().z() << ")";
                }
                else
                    Log(Debug::Warning) << "FNV/ESM4 diag: PipBoy wrist offset skipped; attached node is not PAT";
            }

            attached->setName("FNV VRHandsOnly " + correctedModel.value());
            mFalloutVrHandSurfaceNodes.push_back(attached);
            ++attachedCount;
            if (riggedHandPart)
                ++(surface.left ? leftHandSurfaceCount : rightHandSurfaceCount);
            if (pipBoyArm)
                ++(surface.left ? leftPipBoySurfaceCount : rightPipBoySurfaceCount);
            Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly attached model=" << correctedModel.value()
                             << " source=" << surface.source
                             << " side=" << (surface.left ? "left" : "right")
                             << " attachNode=" << attachNode->getName()
                             << " riggedHandPart=" << riggedHandPart
                             << " staticizedHandPart=" << staticizedRiggedHandPart
                             << " pipBoyArm=" << pipBoyArm
                             << " master=" << master->getName()
                             << " diffuse=" << surface.diffuseTexture;
        }

        mFalloutVrHandSurfacesAttached = true;
        const bool nativeRigReady = leftHandSurfaceCount == 1 && rightHandSurfaceCount == 1
            && leftPipBoySurfaceCount == 1 && rightPipBoySurfaceCount == 0;
        Log(nativeRigReady ? Debug::Info : Debug::Warning)
            << "OpenMW VR player rig status=" << (nativeRigReady ? "ready" : "degraded")
            << " leftHands=" << leftHandSurfaceCount << " rightHands=" << rightHandSurfaceCount
            << " leftPipBoys=" << leftPipBoySurfaceCount << " rightPipBoys=" << rightPipBoySurfaceCount
            << " attachedSurfaces=" << attachedCount << " requestedSurfaces=" << mFalloutVrHandSurfaces.size();
        Log(Debug::Verbose) << "FNV/ESM4 diag: VRHandsOnly attached surfaces count=" << attachedCount
                         << " requested=" << mFalloutVrHandSurfaces.size();
    }

    void VRAnimation::updateFalloutVrHandSurfaceVisibility()
    {
        // Native OpenMW VR hands must not depend on the experimental retail sidecar.
        // GUI mode is also when the player needs tracked hands for inventory and pointer input.
        const bool shouldRenderHands = mViewMode == VM_VRFirstPerson;
        if (!shouldRenderHands)
        {
            if (mFalloutVrHandSurfacesAttached || !mFalloutVrHandSurfaceNodes.empty())
                clearFalloutVrHandSurfaces();
            return;
        }

        attachFalloutVrHandSurfaces();
    }

    void VRAnimation::updateCrosshairs()
    {
        updateFalloutVrHandSurfaceVisibility();
        updateVrDebugSnapshotControls();

        if (!mCrosshairsEnabled)
            return;

        mCrosshairAmmo->hide();
        mCrosshairSpell->hide();
        mCrosshairThrown->hide();

        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;

        if (VR::getKBMouseModeActive())
        {
        }

        if (isArrowAttached())
        {
            if (VR::getKBMouseModeActive())
            {
                mCrosshairAmmo->setParent(mKBMouseCrosshairTransform);
                mCrosshairAmmo->setOffset(100.f);
                mCrosshairAmmo->setStretch(200.f);
            }
            else
            {
                mCrosshairAmmo->setParent(getArrowBone());
                mCrosshairAmmo->setStretch(100.f);
                mCrosshairAmmo->setOffset(15.f);
            }
            mCrosshairAmmo->show();
        }
        else
        {
            mCrosshairAmmo->hide();
            mCrosshairAmmo->setParent(nullptr);
        }

        const MWWorld::Class& cls = mPtr.getClass();
        MWWorld::InventoryStore& inv = cls.getInventoryStore(mPtr);
        MWMechanics::CreatureStats& stats = cls.getCreatureStats(mPtr);

        mCrosshairSpell->hide();
        mCrosshairSpell->setParent(nullptr);
        mCrosshairThrown->hide();
        mCrosshairThrown->setParent(nullptr);

        if (stats.getDrawState() == MWMechanics::DrawState::Spell)
        {
            auto selectedSpell = MWBase::Environment::get().getWindowManager()->getSelectedSpell();
            ;
            auto world = MWBase::Environment::get().getWorld();
            bool isMagicItem = false;

            if (selectedSpell.empty())
            {
                if (inv.getSelectedEnchantItem() != inv.end())
                {
                    const MWWorld::Ptr& enchantItem = *inv.getSelectedEnchantItem();
                    selectedSpell = enchantItem.getClass().getEnchantment(enchantItem);
                    isMagicItem = true;
                }
            }

            const MWWorld::ESMStore& store = world->getStore();
            const ESM::EffectList* effectList = nullptr;
            if (isMagicItem)
            {
                const ESM::Enchantment* enchantment = store.get<ESM::Enchantment>().find(selectedSpell);
                if (enchantment)
                    effectList = &enchantment->mEffects;
            }
            else
            {
                const ESM::Spell* spell = store.get<ESM::Spell>().find(selectedSpell);
                if (spell)
                    effectList = &spell->mEffects;
            }

            int rangeType = ESM::RT_Self;
            if (effectList)
            {
                for (auto& effect : effectList->mList)
                {
                    rangeType = std::max(rangeType, effect.mData.mRange);
                }
            }

            if (rangeType > 0)
            {
                if (VR::getKBMouseModeActive())
                {
                    mCrosshairSpell->setParent(mKBMouseCrosshairTransform);
                    mCrosshairSpell->setOffset(100.f);
                    mCrosshairSpell->setStretch(200.f);
                }
                else
                {
                    mCrosshairSpell->setParent(mWeaponDirectionTransform);
                    if (rangeType == 1)
                        mCrosshairSpell->setStretch(25.f);
                    else if (rangeType == 2)
                        mCrosshairSpell->setStretch(100.f);
                    mCrosshairSpell->setOffset(15.f);
                }
                mCrosshairSpell->show();
            }
        }
        else if (stats.getDrawState() == MWMechanics::DrawState::Weapon)
        {
            // TODO: Should probably create an accessor for Slot_CarriedRight's WeaponType so this verbose code
            // doens't have to be repeated everywhere.
            MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            if (weapon != inv.end() && weapon->getType() == ESM::Weapon::sRecordId)
            {
                int type = weapon->get<ESM::Weapon>()->mBase->mData.mType;
                ESM::WeaponType::Class weapclass = MWMechanics::getWeaponType(type)->mWeaponClass;
                if (weapclass == ESM::WeaponType::Thrown)
                {
                    if (VR::getKBMouseModeActive())
                    {
                        mCrosshairThrown->setParent(mKBMouseCrosshairTransform);
                        mCrosshairThrown->setOffset(100.f);
                        mCrosshairThrown->setStretch(200.f);
                    }
                    else
                    {
                        mCrosshairThrown->setParent(mWeaponDirectionTransform);
                        mCrosshairThrown->setStretch(100.f);
                        mCrosshairThrown->setOffset(15.f);
                    }
                    mCrosshairThrown->show();
                }
            }
        }
    }

    void VRAnimation::updateCharHeight()
    {
        // Compute an approximate character height (eye level)
        // auto playerPtr = MWMechanics::getPlayer();
        // const MWWorld::LiveCellRef<ESM::NPC>* ref = playerPtr.get<ESM::NPC>();
        // const ESM::Race* race
        //    = MWBase::Environment::get().getWorld()->getStore().get<ESM::Race>().find(ref->mBase->mRace);
        // bool isMale = ref->mBase->isMale();
        // float charHeightFactor = isMale ? race->mData.mMaleHeight : race->mData.mFemaleHeight;
        // auto charHeightBase
        //    = Stereo::Unit::fromMeters(Settings::Manager::getFloat("character base height", "VR Debug"));
        // auto charHeight = charHeightBase * charHeightFactor;
        // VR::Session::instance().setCharHeight(charHeight);
        // Log(Debug::Verbose) << "Calculated character height: " << charHeight.asMeters();

        // Use the actual animation to directly get the character's height
        auto root = mObjectRoot;
        auto head = getNode("Bip01 Head");
        if (root && head)
        {
            // The head geometry needs to exist to find a rough estimate of the eye line.
            // This assumes we're using the 3rd person animation, which should be the case.
            if (mPartPriorities[ESM::PRT_Head] < 1 && !mHeadModel.empty())
                addOrReplaceIndividualPart(ESM::PRT_Head, -1, 1, mHeadModel);
            if (head->computeBound().radius() > 1.f)
            {
                auto path = head->getParentalNodePaths(root)[0];
                auto m = osg::computeLocalToWorld(path);
                mCharHeight = m.getTrans().z() + head->computeBound().radius();
            }
            removeIndividualPart(ESM::PRT_Head);
        }
    }

    void VRAnimation::updateSpace()
    {
        auto localRef = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceLocal);
        auto viewRef = OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView);
        auto viewPose = viewRef->locate(*localRef);
        if (!viewPose.status)
            return;
        auto pose = viewPose.pose;
        float newYaw = 0.f;
        float oldYaw = 0.f;
        float pitch = 0.f;
        float roll = 0.f;
        Stereo::getEulerAngles(mHeadPoseInLocalSpace.orientation, oldYaw, pitch, roll);
        Stereo::getEulerAngles(pose.orientation, newYaw, pitch, roll);
        mHeadPoseInLocalSpace = pose;
        float characterYaw = mPtr.getRefData().getPosition().rot[2];
        float characterYawDiff = characterYaw - mCharacterYaw;

        mCharacterYaw = newYaw - oldYaw + characterYaw;
        auto world = MWBase::Environment::get().getWorld();
        world->rotateObject(mPtr, osg::Vec3f(pitch, 0.f, mCharacterYaw), MWBase::RotationFlag_none);

        if (mRecenter)
        {
            Log(Debug::Verbose) << "VRAnimation: Recenter( vertical=" << VR::getShouldRecenterZ() << ", horizontal=" << VR::getShouldRecenterXY() << ")";

            mRecenter = false;
        }
        // else
        {
            // Something else rotated the character. Modify the local space pose accordingly.
            mCharLocalSpacePose.orientation
                = osg::Quat(characterYawDiff, osg::Vec3d(0, 0, -1)) * mCharLocalSpacePose.orientation;
        }
        updateLocalSpaceWorldPose();

        for (auto& it : mVrControllers)
        {
            if (!it.second.enabled)
                continue;

            if (auto* bone = getBoneByName(it.second.forearmBone))
            {
                it.second.forearmController->update(*bone->asTransform()->asMatrixTransform());
            }
        }
        if (mSkeleton)
        {
            mSkeleton->markBoneMatriceDirty();
        }

        osg::Vec2 offset = osg::Vec2(0, 0);
        if (Settings::vr().mHandDirectedMovement && !VR::getKBMouseModeActive())
        {
            // The baseline behavior is view directed movement. To make hand directed movement, simply add
            // the rotation from view to hand.
            // I compute this by locating the hand relative to view and computing yaw/pitch of that, using a direction vector
            // to ignore roll
            auto hand = VR::getLeftHandedMode() ? MWVR::OpenXRInput::RightHandAim : MWVR::OpenXRInput::LeftHandAim;
            if (auto space = MWVR::OpenXRInput::instance().getSpace(hand))
            {
                auto tp = space->locate(*VR::Session::instance().getReferenceSpace(VR::ReferenceSpace::View));
                if (!!tp.status)
                {
                    // Compute the directional vector
                    auto forward = osg::Vec3(0, 1, 0);
                    auto dir = tp.pose.orientation * forward;
                    // Simple trigonometry to compute yaw/pitch
                    offset = osg::Vec2(std::asin(-dir.z()), std::atan2(dir.x(), dir.y()));
                }
            }
        }
        VR::Session::instance().setMovementAngleOffset(offset);
    }

    void VRAnimation::modifyMovement(osg::Vec3& movement) 
    {
    }

    void VRAnimation::addControllers()
    {
        NpcAnimation::addControllers();
        updateTrackingControllers();
        mSkeleton->setIsTracked(true);

        ConfigureCullVisitor configureCullVisitor(false);
        mObjectRoot->accept(configureCullVisitor);

        osg::ref_ptr<osg::StateSet> stateSet = mObjectRoot->getOrCreateStateSet();
        stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
    }

    void VRAnimation::updateTrackingControllers()
    {
        for (auto& it : mVrControllers)
        {
            disableTracking(it.second.topLevelPath);
            const bool controllerActive = VR::getControllerActive(it.second.topLevelPath);
            const bool trackingSpaceAvailable = OpenXRInput::instance().getSpace(it.second.spaceName) != nullptr;
            static int sLoggedControllerState = 0;
            if (sLoggedControllerState < 24)
            {
                ++sLoggedControllerState;
                Log(Debug::Warning) << "FNV/ESM4 diag: VR controller tracking gate hand="
                                    << (it.second.handBone.find(" L ") != std::string::npos ? "left" : "right")
                                    << " active=" << controllerActive
                                    << " trackingSpaceAvailable=" << trackingSpaceAvailable
                                    << " space=" << it.second.spaceName;
            }
            if (controllerActive || trackingSpaceAvailable)
                enableTracking(it.second.topLevelPath);
        }

        auto hand = mNodeMap.find("Bip01 R Hand");
        if (hand != mNodeMap.end())
        {
            hand->second->removeChild(mWeaponDirectionTransform);
            hand->second->addChild(mWeaponDirectionTransform);
        }
    }

    void VRAnimation::enableTracking(XrPath path)
    {
        auto& ctx = mVrControllers[path];

        auto forearm = mNodeMap.find(ctx.forearmBone);
        bool forearmBound = false;
        if (forearm != mNodeMap.end())
        {
            ctx.forearmController->setTransform(forearm->second);
            forearmBound = true;
        }

        auto hand = mNodeMap.find(ctx.handBone);
        bool handBound = false;
        if (hand != mNodeMap.end())
        {
            auto node = hand->second;
            node->addUpdateCallback(ctx.handController);
            handBound = true;
        }

        int fingerBindings = 0;
        for (const auto& finger : ctx.fingerBindings)
        {
            auto found = mNodeMap.find(finger.bone);
            if (found == mNodeMap.end())
                continue;

            found->second->addUpdateCallback(finger.controller);
            ++fingerBindings;
        }

        ctx.enabled = true;
        Log(Debug::Verbose) << "FNV/ESM4 diag: VR tracking enabled hand=" << (ctx.handBone.find(" L ") != std::string::npos ? "left" : "right")
                         << " active=" << VR::getControllerActive(ctx.topLevelPath)
                         << " forearmBound=" << forearmBound
                         << " handBound=" << handBound
                         << " fingerBindings=" << fingerBindings
                         << " space=" << ctx.spaceName;
    }

    void VRAnimation::disableTracking(XrPath path) {
        auto& ctx = mVrControllers[path];
        ctx.forearmController->setTransform(nullptr);
        auto forearm = mNodeMap.find(ctx.forearmBone);
        if (forearm != mNodeMap.end())
        {
            ConfigureCullVisitor configureCullVisitor(true);
            forearm->second->accept(configureCullVisitor);
        }

        auto hand = mNodeMap.find(ctx.handBone);
        if (hand != mNodeMap.end())
        {
            auto node = hand->second;
            node->removeUpdateCallback(ctx.handController);
        }

        for (const auto& finger : ctx.fingerBindings)
        {
            auto found = mNodeMap.find(finger.bone);
            if (found != mNodeMap.end())
                found->second->removeUpdateCallback(finger.controller);
        }
        ctx.enabled = false;
    }

    void VRAnimation::enablePointer(XrPath topLevelPath, bool enable) 
    {
        auto& ctx = mVrControllers[topLevelPath];
        ctx.handController->setFingerPointingMode(enable);
        for (const auto& finger : ctx.fingerBindings)
            finger.controller->setEnabled(enable);
    }

    void VRAnimation::enableHeadAnimation(bool)
    {
        NpcAnimation::enableHeadAnimation(false);
    }

    void VRAnimation::setAccurateAiming(bool)
    {
        NpcAnimation::setAccurateAiming(false);
    }

    void VRAnimation::enablePointers(bool left, bool right)
    {
        if (VR::getLeftHandedMode())
            std::swap(left, right);
        enablePointer(mLeftHandPath, left);
        enablePointer(mRightHandPath, right);
    }

    void VRAnimation::setEnableCrosshairs(bool enable)
    {
        if (enable == mCrosshairsEnabled)
            return;

        mCrosshairsEnabled = enable;

        if (enable)
        {
            mCrosshairAmmo = std::make_unique<Crosshair>(nullptr, osg::Vec3f(0.66f, 1.f, 0.66f), 0.1f, 0.40f, false);
            mCrosshairAmmo->setStretch(100.f);
            mCrosshairAmmo->setWidth(0.1f);
            mCrosshairAmmo->setOffset(15.f);
            mCrosshairAmmo->show();
            mCrosshairThrown = std::make_unique<Crosshair>(nullptr, osg::Vec3f(0.66f, 0.66f, 1.f), 0.1f, 0.40f, false);
            mCrosshairThrown->setStretch(100.f);
            mCrosshairThrown->setWidth(0.1f);
            mCrosshairThrown->setOffset(15.f);
            mCrosshairThrown->show();
            mCrosshairSpell = std::make_unique<Crosshair>(nullptr, osg::Vec3f(1.f, 1.f, 1.f), 0.1f, 0.40f, false);
            mCrosshairSpell->setStretch(100.f);
            mCrosshairSpell->setWidth(0.1f);
            mCrosshairSpell->setOffset(15.f);
            mCrosshairSpell->show();

            mKBMouseCrosshairTransform = new VR::SpaceTransform(OpenXRInput::instance().getSpace(OpenXRInput::DefaultReferenceSpaceView));
            //mKBMouseCrosshairTransform->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
            mSceneRoot->addChild(mKBMouseCrosshairTransform);
        }
        else
        {
            mCrosshairAmmo = nullptr;
            mCrosshairThrown = nullptr;
            mCrosshairSpell = nullptr;

            mSceneRoot->removeChild(mKBMouseCrosshairTransform);
            mKBMouseCrosshairTransform = nullptr;
        }
    }

    void VRAnimation::updateLocalSpaceWorldPose()
    {
        auto origin = mObjectRoot->getParent(0);
        auto worldMatrix = osg::computeLocalToWorld(origin->getParentalNodePaths()[0]);
        auto trans = worldMatrix.getTrans();
        trans.z() += mCharHeight;
        Stereo::Pose originPose = { Stereo::Position::fromMWUnits(trans), osg::Quat{ 0, 0, 0, 1 } };
        XR::Session::instance().setReferenceWorldPose(originPose + mCharLocalSpacePose);
    }

    void VRAnimation::recenter()
    {
        mRecenter = true;
    }

    void VRAnimation::onInteractionProfileActiveChanged(XrPath topLevelPath, bool isActive) 
    {
        updateParts();
        updateTrackingControllers();
    }

    void VRAnimation::addAnimSource(std::string_view model, const std::string& baseModel) 
    {
        Animation::addAnimSource(model, baseModel);
        updateCharHeight();
    }
}
