#include "riggeometry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <osg/MatrixTransform>
#include <osg/PrimitiveSet>

#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/resource/scenemanager.hpp>

#include "skeleton.hpp"
#include "util.hpp"

namespace SceneUtil
{
    namespace
    {
        float matrixDifference(const osg::Matrixf& left, const osg::Matrixf& right)
        {
            float result = 0.f;
            const float* leftPtr = left.ptr();
            const float* rightPtr = right.ptr();
            for (int i = 0; i < 16; ++i)
                result = std::max(result, std::abs(leftPtr[i] - rightPtr[i]));
            return result;
        }

        bool isFalloutHiddenMorphRig(std::string_view name)
        {
            return Misc::StringUtils::ciStartsWith(name, "Tri ");
        }

        bool shouldHideFalloutBadArmRig(std::string_view name)
        {
            const char* env = std::getenv("OPENMW_FNV_HIDE_BAD_ARM_RIG");
            if (env == nullptr || env[0] == '\0' || std::string_view(env) == "0")
                return false;

            return Misc::StringUtils::ciEqual(name, "arms:0");
        }

        bool shouldDisableFalloutArmSleeveProjection()
        {
            const char* env = std::getenv("OPENMW_FNV_DISABLE_ARM_SLEEVE_IK_PROJECTION");
            return env != nullptr && env[0] != '\0' && std::string_view(env) != "0";
        }

        bool isFalloutArmSleeveRig(std::string_view name, std::string_view rootBone,
            const std::vector<RigGeometry::BoneInfo>& bones)
        {
            if (!Misc::StringUtils::ciEqual(name, "arms:0"))
                return false;

            bool hasHand = false;
            bool hasForearm = false;
            bool hasUpperArm = false;
            for (const RigGeometry::BoneInfo& bone : bones)
            {
                hasHand = hasHand || Misc::StringUtils::ciFind(bone.mName, " hand") != std::string_view::npos;
                hasForearm = hasForearm || Misc::StringUtils::ciFind(bone.mName, "forearm") != std::string_view::npos
                    || Misc::StringUtils::ciFind(bone.mName, "foretwist") != std::string_view::npos;
                hasUpperArm = hasUpperArm || Misc::StringUtils::ciFind(bone.mName, "upperarm") != std::string_view::npos
                    || Misc::StringUtils::ciFind(bone.mName, "uparmtwist") != std::string_view::npos;
            }
            return hasHand && hasForearm && hasUpperArm;
        }

        osg::Vec3f boundingBoxExtent(const osg::BoundingBox& box)
        {
            if (!box.valid())
                return osg::Vec3f();
            return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
        }

        float maxFiniteExtentRatio(const osg::Vec3f& numerator, const osg::Vec3f& denominator)
        {
            float result = 1.f;
            for (int i = 0; i < 3; ++i)
            {
                if (denominator[i] <= 0.001f || numerator[i] <= 0.001f)
                    continue;
                result = std::max(result, std::max(numerator[i] / denominator[i], denominator[i] / numerator[i]));
            }
            return result;
        }

        struct FalloutFabricNoTwistStats
        {
            unsigned int mEdgeSamples = 0;
            unsigned int mOverstretchedEdges = 0;
            unsigned int mCollapsedEdges = 0;
            float mMaxEdgeStretchRatio = 1.f;
            float mMaxEdgeLengthDelta = 0.f;
            unsigned int mMaxEdgeA = 0;
            unsigned int mMaxEdgeB = 0;
        };

        void addFalloutFabricEdgeSample(FalloutFabricNoTwistStats& stats, const osg::Vec3Array& sourcePositions,
            const osg::Vec3Array& skinnedPositions, unsigned int a, unsigned int b)
        {
            if (a >= sourcePositions.size() || b >= sourcePositions.size() || a >= skinnedPositions.size()
                || b >= skinnedPositions.size() || a == b)
                return;

            const float sourceLength = (sourcePositions[a] - sourcePositions[b]).length();
            const float skinnedLength = (skinnedPositions[a] - skinnedPositions[b]).length();
            if (sourceLength <= 0.01f || skinnedLength <= 0.0001f)
                return;

            const float ratio = skinnedLength / sourceLength;
            const float stretchRatio = std::max(ratio, 1.f / ratio);
            const float lengthDelta = std::abs(skinnedLength - sourceLength);
            ++stats.mEdgeSamples;
            if (ratio > 3.5f && lengthDelta > 8.f)
                ++stats.mOverstretchedEdges;
            if (ratio < 0.25f && lengthDelta > 8.f)
                ++stats.mCollapsedEdges;
            if (stretchRatio > stats.mMaxEdgeStretchRatio || lengthDelta > stats.mMaxEdgeLengthDelta)
            {
                stats.mMaxEdgeStretchRatio = std::max(stats.mMaxEdgeStretchRatio, stretchRatio);
                stats.mMaxEdgeLengthDelta = std::max(stats.mMaxEdgeLengthDelta, lengthDelta);
                stats.mMaxEdgeA = a;
                stats.mMaxEdgeB = b;
            }
        }

        FalloutFabricNoTwistStats computeFalloutFabricNoTwistStats(
            const osg::Geometry& sourceGeometry, const osg::Vec3Array& sourcePositions,
            const osg::Vec3Array& skinnedPositions)
        {
            FalloutFabricNoTwistStats stats;
            constexpr unsigned int maxEdgeSamples = 50000;
            auto addEdge = [&](unsigned int a, unsigned int b) {
                if (stats.mEdgeSamples < maxEdgeSamples)
                    addFalloutFabricEdgeSample(stats, sourcePositions, skinnedPositions, a, b);
            };

            for (unsigned int primitiveIndex = 0; primitiveIndex < sourceGeometry.getNumPrimitiveSets(); ++primitiveIndex)
            {
                const osg::PrimitiveSet* primitive = sourceGeometry.getPrimitiveSet(primitiveIndex);
                if (primitive == nullptr || primitive->getNumIndices() < 2)
                    continue;

                switch (primitive->getMode())
                {
                    case osg::PrimitiveSet::TRIANGLES:
                        for (unsigned int i = 0; i + 2 < primitive->getNumIndices(); i += 3)
                        {
                            const unsigned int a = primitive->index(i);
                            const unsigned int b = primitive->index(i + 1);
                            const unsigned int c = primitive->index(i + 2);
                            addEdge(a, b);
                            addEdge(b, c);
                            addEdge(c, a);
                        }
                        break;
                    case osg::PrimitiveSet::TRIANGLE_STRIP:
                        for (unsigned int i = 2; i < primitive->getNumIndices(); ++i)
                        {
                            const unsigned int a = primitive->index(i - 2);
                            const unsigned int b = primitive->index(i - 1);
                            const unsigned int c = primitive->index(i);
                            addEdge(a, b);
                            addEdge(b, c);
                            addEdge(c, a);
                        }
                        break;
                    case osg::PrimitiveSet::TRIANGLE_FAN:
                        for (unsigned int i = 2; i < primitive->getNumIndices(); ++i)
                        {
                            const unsigned int a = primitive->index(0);
                            const unsigned int b = primitive->index(i - 1);
                            const unsigned int c = primitive->index(i);
                            addEdge(a, b);
                            addEdge(b, c);
                            addEdge(c, a);
                        }
                        break;
                    case osg::PrimitiveSet::QUADS:
                        for (unsigned int i = 0; i + 3 < primitive->getNumIndices(); i += 4)
                        {
                            const unsigned int a = primitive->index(i);
                            const unsigned int b = primitive->index(i + 1);
                            const unsigned int c = primitive->index(i + 2);
                            const unsigned int d = primitive->index(i + 3);
                            addEdge(a, b);
                            addEdge(b, c);
                            addEdge(c, d);
                            addEdge(d, a);
                            addEdge(a, c);
                        }
                        break;
                    default:
                        for (unsigned int i = 1; i < primitive->getNumIndices(); ++i)
                            addEdge(primitive->index(i - 1), primitive->index(i));
                        break;
                }
            }

            if (stats.mEdgeSamples == 0)
            {
                const unsigned int vertexCount = static_cast<unsigned int>(
                    std::min(sourcePositions.size(), skinnedPositions.size()));
                for (unsigned int i = 1; i < vertexCount && stats.mEdgeSamples < maxEdgeSamples; ++i)
                    addFalloutFabricEdgeSample(stats, sourcePositions, skinnedPositions, i - 1, i);
            }

            return stats;
        }

        std::string_view getFalloutSkinningMode()
        {
            if (const char* env = std::getenv("OPENMW_FNV_SKINNING_MODE"))
                return env;
            return "auto";
        }

        bool hasFalloutSkinningModeOverride()
        {
            const char* env = std::getenv("OPENMW_FNV_SKINNING_MODE");
            return env != nullptr && env[0] != '\0';
        }

        bool isFalloutHandRig(std::string_view name, std::string_view rootBone)
        {
            return Misc::StringUtils::ciFind(name, "hand") != std::string_view::npos
                || Misc::StringUtils::ciFind(name, "glove") != std::string_view::npos
                || Misc::StringUtils::ciFind(rootBone, " hand") != std::string_view::npos;
        }

        bool isFalloutVrArcadeHandSolveRig(
            std::string_view name, std::string_view rootBone, const std::vector<RigGeometry::BoneInfo>& bones)
        {
            if (isFalloutHandRig(name, rootBone))
                return true;

            if (Misc::StringUtils::ciFind(name, "arms") == std::string_view::npos)
                return false;

            bool hasHand = false;
            bool hasForearm = false;
            bool hasUpperArm = false;
            for (const RigGeometry::BoneInfo& bone : bones)
            {
                hasHand = hasHand || Misc::StringUtils::ciFind(bone.mName, " hand") != std::string_view::npos;
                hasForearm = hasForearm || Misc::StringUtils::ciFind(bone.mName, "forearm") != std::string_view::npos
                    || Misc::StringUtils::ciFind(bone.mName, "foretwist") != std::string_view::npos;
                hasUpperArm = hasUpperArm || Misc::StringUtils::ciFind(bone.mName, "upperarm") != std::string_view::npos
                    || Misc::StringUtils::ciFind(bone.mName, "uparmtwist") != std::string_view::npos;
            }
            return hasHand && hasForearm && hasUpperArm;
        }

        bool isFalloutInventoryPaperDollPath(const osg::NodePath& path)
        {
            for (const osg::Node* node : path)
            {
                if (node != nullptr && node->getName() == "FNV Inventory Paper Doll Preview")
                    return true;
            }
            return false;
        }

        std::string_view getFalloutSkinningMode(
            std::string_view name, std::string_view rootBone, bool inventoryPaperDoll)
        {
            if (inventoryPaperDoll)
                return "source";

            if (isFalloutHandRig(name, rootBone))
            {
                if (const char* env = std::getenv("OPENMW_FNV_VR_HAND_SKINNING_MODE"))
                    return env;

                if (hasFalloutSkinningModeOverride())
                    return getFalloutSkinningMode();

                return "current";
            }

            if (hasFalloutSkinningModeOverride())
                return getFalloutSkinningMode();

            return "current";
        }

        bool useFalloutSkinToSkelMatrix()
        {
            if (const char* env = std::getenv("OPENMW_FNV_USE_SKIN_TO_SKEL"))
                return std::string_view(env) != "0";
            return false;
        }

        bool shouldLogFalloutMatrixAudit(std::string_view name, std::string_view rootBone)
        {
            const char* env = std::getenv("OPENMW_FNV_SKINNING_MATRIX_AUDIT");
            if (env == nullptr)
                return false;

            const std::string_view filters(env);
            if (filters.empty() || filters == "1" || Misc::StringUtils::ciEqual(filters, "true")
                || Misc::StringUtils::ciEqual(filters, "all"))
                return true;

            std::size_t start = 0;
            while (start <= filters.size())
            {
                const std::size_t end = filters.find_first_of(",;", start);
                std::string_view filter = filters.substr(start, end == std::string_view::npos ? end : end - start);
                while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.front())))
                    filter.remove_prefix(1);
                while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.back())))
                    filter.remove_suffix(1);
                if (!filter.empty()
                    && (Misc::StringUtils::ciEqual(filter, "all")
                        || Misc::StringUtils::ciFind(name, filter) != std::string_view::npos
                        || Misc::StringUtils::ciFind(rootBone, filter) != std::string_view::npos))
                    return true;
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
            return false;
        }

        osg::Matrixf makeFalloutSkinningAccumulator()
        {
            return osg::Matrixf(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
        }

        void addWeightedFalloutMatrix(osg::Matrixf& result, const osg::Matrixf& matrix, float weight)
        {
            const float* source = matrix.ptr();
            float* destination = result.ptr();
            for (int i = 0; i < 16; ++i, ++source, ++destination)
            {
                if (i % 4 == 3)
                    continue;
                *destination += *source * weight;
            }
        }

        std::string vec3ToString(const osg::Vec3f& value)
        {
            std::ostringstream stream;
            stream << "(" << value.x() << "," << value.y() << "," << value.z() << ")";
            return stream.str();
        }

        osg::Matrixf composeFalloutBoneMatrix(
            const RigGeometry::BoneInfo& boneInfo, const Bone* bone, std::string_view mode)
        {
            if (bone == nullptr)
                return osg::Matrixf();

            const osg::Matrixf& invBind = boneInfo.mInvBindMatrix;
            const osg::Matrixf& skeleton = bone->mMatrixInSkeletonSpace;
            if (mode == "skeleton")
                return skeleton;
            if (mode == "skeletonThenInvBind")
                return skeleton * invBind;

            osg::Matrixf bind;
            const bool hasBind = bind.invert(invBind);
            if (mode == "bindThenSkeleton" && hasBind)
                return bind * skeleton;
            if (mode == "skeletonThenBind" && hasBind)
                return skeleton * bind;
            if (mode == "identity" || mode == "source")
                return osg::Matrixf();
            if (mode == "current" || mode == "legacy")
                return invBind * skeleton;
            if (mode == "invBindThenSkeleton")
                return invBind * skeleton;

            return invBind * skeleton;
        }

        bool findFalloutBoneIndex(const std::vector<RigGeometry::BoneInfo>& bones, std::string_view name,
            std::size_t& index)
        {
            for (std::size_t i = 0; i < bones.size(); ++i)
            {
                if (Misc::StringUtils::ciEqual(bones[i].mName, name))
                {
                    index = i;
                    return true;
                }
            }
            return false;
        }

        osg::Vec3f falloutBindPoint(const RigGeometry::BoneInfo& boneInfo)
        {
            osg::Matrixf bind;
            if (!bind.invert(boneInfo.mInvBindMatrix))
                return osg::Vec3f();
            return bind.preMult(osg::Vec3f());
        }

        osg::Vec3f falloutLivePoint(const Bone* bone, const osg::Matrixf& transform)
        {
            if (bone == nullptr)
                return osg::Vec3f();
            osg::Matrixf live = bone->mMatrixInSkeletonSpace;
            live *= transform;
            return live.preMult(osg::Vec3f());
        }

        osg::Quat makeFalloutSegmentRotation(osg::Vec3f from, osg::Vec3f to)
        {
            osg::Quat rotation;
            if (from.normalize() <= 0.0001f || to.normalize() <= 0.0001f)
                return rotation;
            rotation.makeRotate(from, to);
            return rotation;
        }

        float falloutClosestSegmentT(const osg::Vec3f& point, const osg::Vec3f& start, const osg::Vec3f& end)
        {
            const osg::Vec3f segment = end - start;
            const float length2 = segment.length2();
            if (length2 <= 0.0001f)
                return 0.f;
            return std::clamp(((point - start) * segment) / length2, 0.f, 1.f);
        }

        osg::Vec3f falloutSegmentPoint(const osg::Vec3f& start, const osg::Vec3f& end, float t)
        {
            return start + (end - start) * std::clamp(t, 0.f, 1.f);
        }

        struct FalloutArmChain
        {
            std::size_t mUpper = 0;
            std::size_t mForearm = 0;
            std::size_t mHand = 0;
            bool mValid = false;
        };


        void copySourceSkinningGeometry(const osg::Vec3Array* positionSrc, const osg::Vec3Array* normalSrc,
            const osg::Vec4Array* tangentSrc, osg::Vec3Array* positionDst, osg::Vec3Array* normalDst,
            osg::Vec4Array* tangentDst)
        {
            if (positionSrc != nullptr && positionDst != nullptr)
            {
                const std::size_t count = std::min(positionSrc->size(), positionDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*positionDst)[i] = (*positionSrc)[i];
            }

            if (normalSrc != nullptr && normalDst != nullptr)
            {
                const std::size_t count = std::min(normalSrc->size(), normalDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*normalDst)[i] = (*normalSrc)[i];
            }

            if (tangentSrc != nullptr && tangentDst != nullptr)
            {
                const std::size_t count = std::min(tangentSrc->size(), tangentDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*tangentDst)[i] = (*tangentSrc)[i];
            }
        }

        void copyTranslatedSourceSkinningGeometry(const osg::Vec3Array* positionSrc, const osg::Vec3Array* normalSrc,
            const osg::Vec4Array* tangentSrc, osg::Vec3Array* positionDst, osg::Vec3Array* normalDst,
            osg::Vec4Array* tangentDst, const osg::Vec3f& offset)
        {
            if (positionSrc != nullptr && positionDst != nullptr)
            {
                const std::size_t count = std::min(positionSrc->size(), positionDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*positionDst)[i] = (*positionSrc)[i] + offset;
            }

            if (normalSrc != nullptr && normalDst != nullptr)
            {
                const std::size_t count = std::min(normalSrc->size(), normalDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*normalDst)[i] = (*normalSrc)[i];
            }

            if (tangentSrc != nullptr && tangentDst != nullptr)
            {
                const std::size_t count = std::min(tangentSrc->size(), tangentDst->size());
                for (std::size_t i = 0; i < count; ++i)
                    (*tangentDst)[i] = (*tangentSrc)[i];
            }
        }

        bool getFalloutVrRigidHandSolveDefault()
        {
            if (const char* env = std::getenv("OPENMW_FNV_VR_ARCADE_HAND_SOLVER"))
                return std::string_view(env) != "0";
            return true;
        }
    }

    RigGeometry::RigGeometry()
    {
        setNumChildrenRequiringUpdateTraversal(1);
        // update done in accept(NodeVisitor&)
//## VR_PATCH BEGIN
// VR-TODO: What was the motivation for this?
        //setCullingActive(false);
//## VR_PATCH END
    }

    RigGeometry::RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop)
        : Drawable(copy, copyop)
        , mData(copy.mData)
        , mFalloutFlagSkinning(copy.mFalloutFlagSkinning)
        , mFalloutCharacterRig(copy.mFalloutCharacterRig)
    {
        setSourceGeometry(copy.mSourceGeometry);
        setNumChildrenRequiringUpdateTraversal(1);
    }

    bool RigGeometry::isFalloutCharacterRig() const
    {
        if (mFalloutCharacterRigComputed)
            return mFalloutCharacterRig;

        if (mData == nullptr)
            return false;

        const bool hasBipRoot = mData->mRootBone.empty() || Misc::StringUtils::ciStartsWith(mData->mRootBone, "Bip01")
            || Misc::StringUtils::ciEqual(mData->mRootBone, "Scene Root");
        bool hasFalloutLimb = false;
        for (const BoneInfo& info : mData->mBones)
        {
            if (Misc::StringUtils::ciStartsWith(info.mName, "bip01 "))
            {
                hasFalloutLimb = true;
                break;
            }
        }

        mFalloutCharacterRig = hasBipRoot && hasFalloutLimb;
        mFalloutCharacterRigComputed = true;
        return mFalloutCharacterRig;
    }

    void RigGeometry::setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeometry)
    {
        for (unsigned int i = 0; i < 2; ++i)
            mGeometry[i] = nullptr;

        mSourceGeometry = sourceGeometry;

        for (unsigned int i = 0; i < 2; ++i)
        {
            const osg::Geometry& from = *sourceGeometry;

            // DO NOT COPY AND PASTE THIS CODE. Cloning osg::Geometry without also cloning its contained Arrays is
            // generally unsafe. In this specific case the operation is safe under the following two assumptions:
            // - When Arrays are removed or replaced in the cloned geometry, the original Arrays in their place must
            // outlive the cloned geometry regardless. (ensured by mSourceGeometry)
            // - Arrays that we add or replace in the cloned geometry must be explicitely forbidden from reusing
            // BufferObjects of the original geometry. (ensured by vbo below)
            mGeometry[i] = new osg::Geometry(from, osg::CopyOp::SHALLOW_COPY);
            mGeometry[i]->getOrCreateUserDataContainer()->addUserObject(new Resource::TemplateRef(mSourceGeometry));

            osg::Geometry& to = *mGeometry[i];
            to.setSupportsDisplayList(false);
            to.setUseVertexBufferObjects(true);
            to.setCullingActive(false); // make sure to disable culling since that's handled by this class
            to.setComputeBoundingBoxCallback(new CopyBoundingBoxCallback());
            to.setComputeBoundingSphereCallback(new CopyBoundingSphereCallback());

            // vertices and normals are modified every frame, so we need to deep copy them.
            // assign a dedicated VBO to make sure that modifications don't interfere with source geometry's VBO.
            osg::ref_ptr<osg::VertexBufferObject> vbo(new osg::VertexBufferObject);
            vbo->setUsage(GL_DYNAMIC_DRAW_ARB);

            osg::ref_ptr<osg::Array> vertexArray
                = static_cast<osg::Array*>(from.getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            if (vertexArray)
            {
                vertexArray->setVertexBufferObject(vbo);
                to.setVertexArray(vertexArray);
            }

            if (const osg::Array* normals = from.getNormalArray())
            {
                osg::ref_ptr<osg::Array> normalArray
                    = static_cast<osg::Array*>(normals->clone(osg::CopyOp::DEEP_COPY_ALL));
                if (normalArray)
                {
                    normalArray->setVertexBufferObject(vbo);
                    to.setNormalArray(normalArray, osg::Array::BIND_PER_VERTEX);
                }
            }

            if (const osg::Vec4Array* tangents = dynamic_cast<const osg::Vec4Array*>(from.getTexCoordArray(7)))
            {
                mSourceTangents = tangents;
                osg::ref_ptr<osg::Array> tangentArray
                    = static_cast<osg::Array*>(tangents->clone(osg::CopyOp::DEEP_COPY_ALL));
                tangentArray->setVertexBufferObject(vbo);
                to.setTexCoordArray(7, tangentArray, osg::Array::BIND_PER_VERTEX);
            }
            else
                mSourceTangents = nullptr;
        }
    }

    osg::ref_ptr<osg::Geometry> RigGeometry::getSourceGeometry() const
    {
        return mSourceGeometry;
    }

    osg::Geometry* RigGeometry::getRenderGeometry(unsigned int index) const
    {
        if (index >= 2)
            return nullptr;

        return mGeometry[index].get();
    }

    osg::Geometry* RigGeometry::getLastFrameGeometry() const
    {
        return getGeometry(mLastFrameNumber);
    }

    void RigGeometry::applyFalloutLiveRigWeightDebug(osg::Geometry& geom)
    {
        const bool enabled = std::getenv("OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS") != nullptr
            || std::getenv("OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS") != nullptr
            || std::getenv("OPENMW_FNV_LIVE_RIG_WEIGHT_DEBUG") != nullptr;
        if (!enabled || !isFalloutCharacterRig() || !mData || !geom.getVertexArray())
            return;

        auto mixColor = [](const osg::Vec4f& low, const osg::Vec4f& high, float t) {
            t = std::clamp(t, 0.f, 1.f);
            return osg::Vec4f(low.r() * (1.f - t) + high.r() * t, low.g() * (1.f - t) + high.g() * t,
                low.b() * (1.f - t) + high.b() * t, 1.f);
        };
        auto heatColor = [&](float weight) {
            if (weight <= 0.001f)
                return osg::Vec4f(0.035f, 0.035f, 0.04f, 1.f);
            return mixColor(osg::Vec4f(0.05f, 0.16f, 0.75f, 1.f), osg::Vec4f(1.f, 0.92f, 0.12f, 1.f),
                std::sqrt(std::clamp(weight, 0.f, 1.f)));
        };
        auto paletteColor = [&](std::size_t index, float weight) {
            if (weight <= 0.001f)
                return osg::Vec4f(0.035f, 0.035f, 0.04f, 1.f);
            static const std::array<osg::Vec4f, 12> palette = {
                osg::Vec4f(0.95f, 0.18f, 0.12f, 1.f), osg::Vec4f(0.12f, 0.62f, 1.f, 1.f),
                osg::Vec4f(0.10f, 0.88f, 0.35f, 1.f), osg::Vec4f(0.95f, 0.78f, 0.12f, 1.f),
                osg::Vec4f(0.78f, 0.30f, 1.f, 1.f), osg::Vec4f(0.05f, 0.88f, 0.88f, 1.f),
                osg::Vec4f(1.f, 0.38f, 0.70f, 1.f), osg::Vec4f(0.98f, 0.50f, 0.10f, 1.f),
                osg::Vec4f(0.48f, 0.72f, 0.12f, 1.f), osg::Vec4f(0.58f, 0.48f, 1.f, 1.f),
                osg::Vec4f(0.92f, 0.12f, 0.46f, 1.f), osg::Vec4f(0.72f, 0.90f, 1.f, 1.f),
            };
            return mixColor(osg::Vec4f(0.03f, 0.03f, 0.035f, 1.f), palette[index % palette.size()],
                0.25f + 0.75f * std::sqrt(std::clamp(weight, 0.f, 1.f)));
        };

        const char* selectorEnv = std::getenv("OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE");
        const std::string selector = selectorEnv != nullptr && selectorEnv[0] != '\0' ? selectorEnv : "all";
        const std::string loweredSelector = Misc::StringUtils::lowerCase(selector);
        const bool allBones = loweredSelector.empty() || loweredSelector == "all" || loweredSelector == "*"
            || loweredSelector == "strongest" || loweredSelector == "all-bones" || loweredSelector == "bones";
        const bool fingers = loweredSelector == "fingers" || loweredSelector == "finger-bones";

        std::vector<bool> matchedBones(mData->mBones.size(), false);
        if (allBones)
            std::fill(matchedBones.begin(), matchedBones.end(), true);
        else
        {
            bool parsedIndex = true;
            std::size_t selectedIndex = 0;
            std::string indexText = loweredSelector;
            if (!indexText.empty() && indexText.front() == '#')
                indexText.erase(indexText.begin());
            for (char c : indexText)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                {
                    parsedIndex = false;
                    break;
                }
                selectedIndex = selectedIndex * 10 + static_cast<std::size_t>(c - '0');
            }
            if (parsedIndex && !indexText.empty())
            {
                if (selectedIndex < matchedBones.size())
                    matchedBones[selectedIndex] = true;
            }
            else
            {
                for (std::size_t i = 0; i < mData->mBones.size(); ++i)
                {
                    const std::string bone = Misc::StringUtils::lowerCase(mData->mBones[i].mName);
                    if ((fingers
                            && (bone.find("finger") != std::string::npos || bone.find("thumb") != std::string::npos))
                        || (!fingers && bone.find(loweredSelector) != std::string::npos))
                        matchedBones[i] = true;
                }
            }
        }

        unsigned int matchedBoneCount = 0;
        for (bool matched : matchedBones)
            if (matched)
                ++matchedBoneCount;
        if (matchedBoneCount == 0)
            return;

        const std::size_t vertexCount = geom.getVertexArray()->getNumElements();
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->assign(vertexCount, osg::Vec4f(0.035f, 0.035f, 0.04f, 1.f));
        std::vector<bool> weighted(vertexCount, false);
        float maxWeight = 0.f;
        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            std::size_t strongestBone = 0;
            float strongestWeight = 0.f;
            float selectedWeight = 0.f;
            for (const auto& [boneIndex, weight] : influences)
            {
                if (boneIndex >= matchedBones.size() || !matchedBones[boneIndex])
                    continue;
                selectedWeight += weight;
                if (weight > strongestWeight)
                {
                    strongestBone = boneIndex;
                    strongestWeight = weight;
                }
            }

            selectedWeight = std::clamp(selectedWeight, 0.f, 1.f);
            const float visibleWeight = allBones ? strongestWeight : selectedWeight;
            maxWeight = std::max(maxWeight, visibleWeight);
            const osg::Vec4f color = allBones ? paletteColor(strongestBone, strongestWeight) : heatColor(selectedWeight);
            for (unsigned short vertex : vertices)
            {
                if (vertex >= vertexCount)
                    continue;
                (*colors)[vertex] = color;
                if (visibleWeight > 0.001f)
                    weighted[vertex] = true;
            }
        }

        unsigned int weightedVertices = 0;
        for (bool isWeighted : weighted)
            if (isWeighted)
                ++weightedVertices;

        geom.setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geom.dirtyGLObjects();
        if (!mLoggedFalloutLiveRigWeightDebug)
        {
            mLoggedFalloutLiveRigWeightDebug = true;
            Log(Debug::Info) << "FNV/ESM4 proof: live RigGeometry weight debug rig='" << getName()
                             << "' rootBone='" << mData->mRootBone
                             << "' selector=" << selector
                             << " bones=" << mData->mBones.size()
                             << " matchedBones=" << matchedBoneCount
                             << " influenceGroups=" << mData->mInfluences.size()
                             << " vertices=" << vertexCount
                             << " weightedVertices=" << weightedVertices
                             << " maxWeight=" << maxWeight
                             << " runtime=runtime-supported gate=runtime-fnv-live-rig-weight-debug";
        }
    }

    void RigGeometry::forceNextUpdate()
    {
        mLastFrameNumber = 0;
        dirtyBound();
    }

    bool RigGeometry::refreshFalloutSkinningForCurrentPose()
    {
        if (!mData || !mSourceGeometry || !isFalloutCharacterRig() || mNodes.empty())
            return false;

        const osg::Vec3Array* positionSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        const osg::Vec3Array* normalSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getNormalArray());
        const osg::Vec4Array* tangentSrc = mSourceTangents;
        if (positionSrc == nullptr || positionSrc->empty())
            return false;

        const std::string_view falloutSkinningMode = getFalloutSkinningMode(getName(), mData->mRootBone, false);
        const bool sourceSkinOnly = falloutSkinningMode == "sourceSkinOnly" && mSkinToSkelMatrix != nullptr;
        const bool falloutSourceSkinning = falloutSkinningMode == "source" || sourceSkinOnly
            || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback);

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
            {
                boneMat = falloutSourceSkinning ? osg::Matrixf()
                    : composeFalloutBoneMatrix(*boneInfo, *bone, falloutSkinningMode);
            }
            ++bone;
            ++boneInfo;
        }

        osg::Matrixf transform;
        if (mFalloutFlagSkinning || falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && useFalloutSkinToSkelMatrix())
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        bool refreshed = false;
        for (osg::ref_ptr<osg::Geometry>& geometry : mGeometry)
        {
            if (geometry == nullptr)
                continue;

            osg::Vec3Array* positionDst = static_cast<osg::Vec3Array*>(geometry->getVertexArray());
            osg::Vec3Array* normalDst = static_cast<osg::Vec3Array*>(geometry->getNormalArray());
            osg::Vec4Array* tangentDst = static_cast<osg::Vec4Array*>(geometry->getTexCoordArray(7));
            if (positionDst == nullptr || positionDst->size() < positionSrc->size())
                continue;

            if (falloutSourceSkinning)
            {
                copySourceSkinningGeometry(positionSrc, normalSrc, tangentSrc, positionDst, normalDst, tangentDst);
            }
            else
            {
                for (const auto& [influences, vertices] : mData->mInfluences)
                {
                    osg::Matrixf resultMat = makeFalloutSkinningAccumulator();
                    for (const auto& [index, weight] : influences)
                    {
                        if (index >= boneMatrices.size() || mNodes[index] == nullptr)
                            continue;
                        addWeightedFalloutMatrix(resultMat, boneMatrices[index], weight);
                    }
                    resultMat *= transform;

                    for (unsigned short vertex : vertices)
                    {
                        if (vertex >= positionSrc->size() || vertex >= positionDst->size())
                            continue;
                        (*positionDst)[vertex] = resultMat.preMult((*positionSrc)[vertex]);
                        if (normalSrc != nullptr && normalDst != nullptr && vertex < normalSrc->size()
                            && vertex < normalDst->size())
                            (*normalDst)[vertex] = osg::Matrixf::transform3x3((*normalSrc)[vertex], resultMat);
                        if (tangentSrc != nullptr && tangentDst != nullptr && vertex < tangentSrc->size()
                            && vertex < tangentDst->size())
                        {
                            const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                            const osg::Vec3f transformedTangent = osg::Matrixf::transform3x3(
                                osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()), resultMat);
                            (*tangentDst)[vertex] = osg::Vec4f(transformedTangent, srcTangent.w());
                        }
                    }
                }
            }

            positionDst->dirty();
            if (normalDst != nullptr)
                normalDst->dirty();
            if (tangentDst != nullptr)
                tangentDst->dirty();
            geometry->dirtyBound();
            geometry->osg::Drawable::dirtyGLObjects();
            refreshed = true;
        }

        if (refreshed)
            dirtyBound();
        return refreshed;
    }

    bool RigGeometry::computeCurrentFalloutSkinningBounds(osg::NodeVisitor* nv, osg::BoundingBox& box)
    {
        box.init();
        if (nv == nullptr || !mData || !mSourceGeometry || !isFalloutCharacterRig())
            return false;

        nv->pushOntoNodePath(this);
        struct PopNodePath
        {
            osg::NodeVisitor* mVisitor;
            ~PopNodePath() { mVisitor->popFromNodePath(); }
        } popNodePath{ nv };

        if (!mSkeleton && !initFromParentSkeleton(nv))
            return false;
        if (mSkeleton == nullptr || isFalloutHiddenMorphRig(getName()))
            return false;

        const osg::Vec3Array* positionSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        if (positionSrc == nullptr || positionSrc->empty())
            return false;

        mSkeleton->updateBoneMatrices(nv->getTraversalNumber());
        updateSkinToSkelMatrix(nv->getNodePath());

        const bool falloutInventoryPaperDoll = isFalloutInventoryPaperDollPath(nv->getNodePath());
        const std::string_view falloutSkinningMode
            = getFalloutSkinningMode(getName(), mData->mRootBone, falloutInventoryPaperDoll);
        const bool sourceSkinOnly = falloutSkinningMode == "sourceSkinOnly" && mSkinToSkelMatrix != nullptr;
        const bool falloutSourceSkinning = falloutSkinningMode == "source" || sourceSkinOnly
            || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback);

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
            {
                boneMat = falloutSourceSkinning ? osg::Matrixf()
                    : composeFalloutBoneMatrix(*boneInfo, *bone, falloutSkinningMode);
            }
            ++bone;
            ++boneInfo;
        }

        osg::Matrixf transform;
        if (mFalloutFlagSkinning)
            transform.makeIdentity();
        else if (falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && useFalloutSkinToSkelMatrix())
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            osg::Matrixf resultMat = makeFalloutSkinningAccumulator();
            for (const auto& [index, weight] : influences)
            {
                if (index >= boneMatrices.size() || mNodes[index] == nullptr)
                    continue;
                addWeightedFalloutMatrix(resultMat, boneMatrices[index], weight);
            }

            resultMat *= transform;
            for (unsigned short vertex : vertices)
            {
                if (vertex < positionSrc->size())
                    box.expandBy(resultMat.preMult((*positionSrc)[vertex]));
            }
        }

        return box.valid();
    }

    bool RigGeometry::initFromParentSkeleton(osg::NodeVisitor* nv)
    {
        const osg::NodePath& path = nv->getNodePath();
        for (osg::NodePath::const_reverse_iterator it = path.rbegin() + 1; it != path.rend(); ++it)
        {
            osg::Node* node = *it;
            if (node->asTransform())
                continue;
            if (Skeleton* skel = dynamic_cast<Skeleton*>(node))
            {
                mSkeleton = skel;
                break;
            }
        }

        if (!mSkeleton)
        {
            Log(Debug::Error) << "Error: A RigGeometry did not find its parent skeleton";
            return false;
        }

        if (!mData)
        {
            Log(Debug::Error) << "Error: No influence data set on RigGeometry";
            return false;
        }

        mNodes.clear();
        std::size_t missingBones = 0;
        for (const BoneInfo& info : mData->mBones)
        {
            mNodes.push_back(mSkeleton->getBone(info.mName));
            if (!mNodes.back())
            {
                ++missingBones;
                Log(Debug::Error) << "Error: RigGeometry did not find bone " << info.mName;
            }
        }

        if (!mLoggedFalloutRigInit && isFalloutCharacterRig())
        {
            mLoggedFalloutRigInit = true;
            setCullingActive(false);
            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName() << "' initialized bones="
                             << mData->mBones.size() << " missing=" << missingBones
                             << " rootBone=" << mData->mRootBone << " influences=" << mData->mInfluences.size();
        }

        return true;
    }

    void RigGeometry::cull(osg::NodeVisitor* nv)
    {
        if (!mSkeleton)
        {
            if (!initFromParentSkeleton(nv))
            {
                Log(Debug::Error)
                    << "Error: RigGeometry rendering with no skeleton, should have been initialized by UpdateVisitor";
                return;
            }

            if (isFalloutCharacterRig() && !mLoggedFalloutCullInitRecovery)
            {
                mLoggedFalloutCullInitRecovery = true;
                Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                 << "' initialized skeleton during cull traversal after update miss";
            }
        }

        if (isFalloutCharacterRig() && isFalloutHiddenMorphRig(getName()))
        {
            setNodeMask(0);
            return;
        }

        if (isFalloutCharacterRig() && shouldHideFalloutBadArmRig(getName()))
        {
            setNodeMask(0);
            static std::unordered_set<std::string> loggedHiddenArmRigs;
            if (loggedHiddenArmRigs.insert(getName()).second)
                Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                 << "' hidden by OPENMW_FNV_HIDE_BAD_ARM_RIG";
            return;
        }

        unsigned int traversalNumber = nv->getTraversalNumber();
        if (mLastFrameNumber == traversalNumber || (mLastFrameNumber != 0 && !mSkeleton->getActive()))
        {
            osg::Geometry& geom = *getGeometry(mLastFrameNumber);
            applyFalloutLiveRigWeightDebug(geom);
            nv->pushOntoNodePath(&geom);
            nv->apply(geom);
            nv->popFromNodePath();
            return;
        }
        mLastFrameNumber = traversalNumber;
        osg::Geometry& geom = *getGeometry(mLastFrameNumber);

        const bool falloutRig = isFalloutCharacterRig();
        const bool falloutFlagRig = mFalloutFlagSkinning;
        if (falloutRig && std::getenv("OPENMW_FNV_RIG_DRAW_AUDIT") != nullptr)
        {
            std::ostringstream stream;
            stream << getName() << " path=";
            const osg::NodePath& path = nv->getNodePath();
            for (std::size_t i = 0; i < path.size(); ++i)
            {
                const osg::Node* node = path[i];
                if (i != 0)
                    stream << " > ";
                stream << (node != nullptr && !node->getName().empty() ? node->getName() : "<unnamed>");
                if (node != nullptr)
                    stream << "[mask=0x" << std::hex << node->getNodeMask() << std::dec << "]";
            }

            static std::unordered_set<std::string> loggedRigPaths;
            const std::string key = stream.str();
            if (loggedRigPaths.insert(key).second)
                Log(Debug::Info) << "FNV/ESM4 draw audit: rig=" << key;
        }

        if (falloutRig && !mLoggedFalloutCullTraversal)
        {
            mLoggedFalloutCullTraversal = true;
            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' reached cull traversal frame=" << traversalNumber
                             << " nodeMask=0x" << std::hex << getNodeMask() << std::dec
                             << " sourcePrimitives="
                             << (mSourceGeometry ? mSourceGeometry->getNumPrimitiveSets() : 0);
        }

        if (falloutRig && !mLoggedFalloutInfluenceSummary)
        {
            mLoggedFalloutInfluenceSummary = true;
            float minWeightSum = std::numeric_limits<float>::max();
            float maxWeightSum = 0.f;
            std::size_t zeroWeightGroups = 0;
            std::size_t overweightGroups = 0;
            for (const auto& [influences, vertices] : mData->mInfluences)
            {
                float sum = 0.f;
                for (const auto& [index, weight] : influences)
                    sum += weight;
                minWeightSum = std::min(minWeightSum, sum);
                maxWeightSum = std::max(maxWeightSum, sum);
                if (sum <= 0.0001f)
                    ++zeroWeightGroups;
                if (sum > 1.01f)
                    ++overweightGroups;
            }
            if (mData->mInfluences.empty())
                minWeightSum = 0.f;

            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' influence summary groups=" << mData->mInfluences.size()
                             << " minWeightSum=" << minWeightSum << " maxWeightSum=" << maxWeightSum
                             << " zeroGroups=" << zeroWeightGroups << " overweightGroups=" << overweightGroups;
        }

        mSkeleton->updateBoneMatrices(traversalNumber);

//## VR_PATCH BEGIN
        // Tracking in VR updates bone matrices out of order, and forces bounds to be recalculated during cull.
        if (mSkeleton->isTracked())
            updateBounds(nv);

//## VR_PATCH END
        // skinning
        const osg::Vec3Array* positionSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        const osg::Vec3Array* normalSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getNormalArray());
        const osg::Vec4Array* tangentSrc = mSourceTangents;

        osg::Vec3Array* positionDst = static_cast<osg::Vec3Array*>(geom.getVertexArray());
        osg::Vec3Array* normalDst = static_cast<osg::Vec3Array*>(geom.getNormalArray());
        osg::Vec4Array* tangentDst = static_cast<osg::Vec4Array*>(geom.getTexCoordArray(7));

        const bool falloutInventoryPaperDoll = falloutRig && isFalloutInventoryPaperDollPath(nv->getNodePath());
        const std::string_view falloutSkinningMode = falloutRig
            ? getFalloutSkinningMode(getName(), mData->mRootBone, falloutInventoryPaperDoll)
            : std::string_view();
        const bool falloutAutoMode = falloutRig && falloutSkinningMode == "auto";
        const bool sourceSkinOnly = falloutRig && falloutSkinningMode == "sourceSkinOnly"
            && mSkinToSkelMatrix != nullptr;
        const bool falloutSourceSkinning = falloutRig
            && (falloutSkinningMode == "source" || sourceSkinOnly
                || (falloutAutoMode && mFalloutUseSourceFallback));

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
            {
                boneMat = falloutSourceSkinning ? osg::Matrixf()
                    : falloutRig       ? composeFalloutBoneMatrix(*boneInfo, *bone, falloutSkinningMode)
                                       : boneInfo->mInvBindMatrix * (*bone)->mMatrixInSkeletonSpace;
            }
            ++bone;
            ++boneInfo;
        }

        if (falloutRig && !boneMatrices.empty())
        {
            if (!mLoggedFalloutMatrixChange)
            {
                if (!mHaveFalloutMatrixBaseline)
                {
                    mFalloutMatrixBaseline = boneMatrices;
                    mHaveFalloutMatrixBaseline = true;
                }
                else if (mFalloutMatrixBaseline.size() == boneMatrices.size())
                {
                    float maxDiff = 0.f;
                    std::size_t maxDiffBone = 0;
                    for (std::size_t i = 0; i < boneMatrices.size(); ++i)
                    {
                        const float diff = matrixDifference(boneMatrices[i], mFalloutMatrixBaseline[i]);
                        if (diff > maxDiff)
                        {
                            maxDiff = diff;
                            maxDiffBone = i;
                        }
                    }

                    if (maxDiff > 0.0001f)
                    {
                        mLoggedFalloutMatrixChange = true;
                        const std::string& boneName = maxDiffBone < mData->mBones.size() ? mData->mBones[maxDiffBone].mName : "";
                        Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                         << "' observed animated bone matrix delta=" << maxDiff
                                         << " boneIndex=" << maxDiffBone << " bone=" << boneName;
                    }
                }
            }
        }

        osg::Matrixf transform;
        if (falloutFlagRig)
            transform.makeIdentity();
        else if (falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && (!falloutRig || useFalloutSkinToSkelMatrix()))
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        if (falloutFlagRig && !mLoggedFalloutFlagSkinning)
        {
            mLoggedFalloutFlagSkinning = true;
            Log(Debug::Info) << "FNV/ESM4 diag: Fallout flag RigGeometry '" << getName()
                             << "' using animated bone deltas with identity skin root"
                             << " rootBone=" << mData->mRootBone
                             << " bones=" << mData->mBones.size()
                             << " hasSkinToSkel=" << static_cast<bool>(mSkinToSkelMatrix);
        }

        if (falloutRig && !mLoggedFalloutSkinningModes && positionSrc != nullptr && !mData->mInfluences.empty())
        {
            mLoggedFalloutSkinningModes = true;
            float maxCurrentDelta = 0.f;
            float maxNoSkinRootDelta = 0.f;
            float maxInvertedBindDelta = 0.f;
            float maxSkeletonDelta = 0.f;
            float maxSkeletonThenInvBindDelta = 0.f;
            float maxBindThenSkeletonDelta = 0.f;
            float maxSkeletonThenBindDelta = 0.f;
            for (const auto& [influences, vertices] : mData->mInfluences)
            {
                osg::Matrixf currentMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                osg::Matrixf invertedBindMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                osg::Matrixf skeletonMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                osg::Matrixf skeletonThenInvBindMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                osg::Matrixf bindThenSkeletonMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
                osg::Matrixf skeletonThenBindMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);

                for (const auto& [index, weight] : influences)
                {
                    if (mNodes[index] == nullptr)
                        continue;

                    const osg::Matrixf currentBoneMat
                        = mData->mBones[index].mInvBindMatrix * mNodes[index]->mMatrixInSkeletonSpace;
                    const osg::Matrixf skeletonBoneMat = mNodes[index]->mMatrixInSkeletonSpace;
                    const osg::Matrixf skeletonThenInvBindBoneMat
                        = mNodes[index]->mMatrixInSkeletonSpace * mData->mBones[index].mInvBindMatrix;
                    osg::Matrixf bindMatrix;
                    osg::Matrixf invertedBindBoneMat = boneMatrices[index];
                    osg::Matrixf bindThenSkeletonBoneMat = boneMatrices[index];
                    osg::Matrixf skeletonThenBindBoneMat = boneMatrices[index];
                    if (bindMatrix.invert(mData->mBones[index].mInvBindMatrix))
                    {
                        invertedBindBoneMat = bindMatrix * mNodes[index]->mMatrixInSkeletonSpace;
                        bindThenSkeletonBoneMat = bindMatrix * mNodes[index]->mMatrixInSkeletonSpace;
                        skeletonThenBindBoneMat = mNodes[index]->mMatrixInSkeletonSpace * bindMatrix;
                    }

                    const float* currentPtr = currentBoneMat.ptr();
                    const float* invertedPtr = invertedBindBoneMat.ptr();
                    const float* skeletonPtr = skeletonBoneMat.ptr();
                    const float* skeletonThenInvBindPtr = skeletonThenInvBindBoneMat.ptr();
                    const float* bindThenSkeletonPtr = bindThenSkeletonBoneMat.ptr();
                    const float* skeletonThenBindPtr = skeletonThenBindBoneMat.ptr();
                    float* currentResultPtr = currentMat.ptr();
                    float* invertedResultPtr = invertedBindMat.ptr();
                    float* skeletonResultPtr = skeletonMat.ptr();
                    float* skeletonThenInvBindResultPtr = skeletonThenInvBindMat.ptr();
                    float* bindThenSkeletonResultPtr = bindThenSkeletonMat.ptr();
                    float* skeletonThenBindResultPtr = skeletonThenBindMat.ptr();
                    for (int i = 0; i < 16;
                         ++i, ++currentPtr, ++invertedPtr, ++skeletonPtr, ++skeletonThenInvBindPtr,
                         ++bindThenSkeletonPtr, ++skeletonThenBindPtr, ++currentResultPtr, ++invertedResultPtr,
                         ++skeletonResultPtr, ++skeletonThenInvBindResultPtr, ++bindThenSkeletonResultPtr,
                         ++skeletonThenBindResultPtr)
                    {
                        if (i % 4 == 3)
                            continue;
                        *currentResultPtr += *currentPtr * weight;
                        *invertedResultPtr += *invertedPtr * weight;
                        *skeletonResultPtr += *skeletonPtr * weight;
                        *skeletonThenInvBindResultPtr += *skeletonThenInvBindPtr * weight;
                        *bindThenSkeletonResultPtr += *bindThenSkeletonPtr * weight;
                        *skeletonThenBindResultPtr += *skeletonThenBindPtr * weight;
                    }
                }

                const osg::Matrixf noSkinRootMat = currentMat * mData->mTransform;
                currentMat *= transform;
                invertedBindMat *= transform;
                skeletonMat *= transform;
                skeletonThenInvBindMat *= transform;
                bindThenSkeletonMat *= transform;
                skeletonThenBindMat *= transform;

                for (unsigned short vertex : vertices)
                {
                    const osg::Vec3f& source = (*positionSrc)[vertex];
                    maxCurrentDelta = std::max(maxCurrentDelta, (currentMat.preMult(source) - source).length());
                    maxNoSkinRootDelta
                        = std::max(maxNoSkinRootDelta, (noSkinRootMat.preMult(source) - source).length());
                    maxInvertedBindDelta
                        = std::max(maxInvertedBindDelta, (invertedBindMat.preMult(source) - source).length());
                    maxSkeletonDelta
                        = std::max(maxSkeletonDelta, (skeletonMat.preMult(source) - source).length());
                    maxSkeletonThenInvBindDelta = std::max(maxSkeletonThenInvBindDelta,
                        (skeletonThenInvBindMat.preMult(source) - source).length());
                    maxBindThenSkeletonDelta = std::max(maxBindThenSkeletonDelta,
                        (bindThenSkeletonMat.preMult(source) - source).length());
                    maxSkeletonThenBindDelta = std::max(maxSkeletonThenBindDelta,
                        (skeletonThenBindMat.preMult(source) - source).length());
                }
            }

            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' skinning mode delta current=" << maxCurrentDelta
                             << " noSkinRoot=" << maxNoSkinRootDelta
                             << " invertedBind=" << maxInvertedBindDelta
                             << " skeleton=" << maxSkeletonDelta
                             << " skeletonThenInvBind=" << maxSkeletonThenInvBindDelta
                             << " bindThenSkeleton=" << maxBindThenSkeletonDelta
                             << " skeletonThenBind=" << maxSkeletonThenBindDelta
                             << " selected=" << falloutSkinningMode
                             << " inventoryPaperDoll=" << falloutInventoryPaperDoll
                             << " sourceFallback=" << mFalloutUseSourceFallback
                             << " hasSkinToSkel=" << static_cast<bool>(mSkinToSkelMatrix)
                             << " useSkinToSkel=" << useFalloutSkinToSkelMatrix();
        }

        if (falloutRig && !mLoggedFalloutMatrixAudit
            && shouldLogFalloutMatrixAudit(getName(), mData->mRootBone) && positionSrc != nullptr
            && !mData->mInfluences.empty())
        {
            mLoggedFalloutMatrixAudit = true;

            osg::Matrixf identity;
            identity.makeIdentity();
            osg::Matrixf skinToSkel;
            skinToSkel.makeIdentity();
            if (mSkinToSkelMatrix != nullptr)
                skinToSkel = *mSkinToSkelMatrix;
            const osg::Matrixf skinToSkelThenTransform = skinToSkel * mData->mTransform;

            std::ostringstream pathStream;
            const osg::NodePath& path = nv->getNodePath();
            for (std::size_t i = 0; i < path.size(); ++i)
            {
                if (i != 0)
                    pathStream << "/";
                const osg::Node* node = path[i];
                pathStream << (node != nullptr && !node->getName().empty() ? node->getName() : "<unnamed>");
            }

            Log(Debug::Info) << "FNV/ESM4 SKIN MATRIX AUDIT rig='" << getName()
                             << "' rootBone='" << mData->mRootBone
                             << "' selected=" << falloutSkinningMode
                             << " inventoryPaperDoll=" << falloutInventoryPaperDoll
                             << " bones=" << mData->mBones.size()
                             << " groups=" << mData->mInfluences.size()
                             << " hasSkinToSkel=" << static_cast<bool>(mSkinToSkelMatrix)
                             << " transformT=" << vec3ToString(mData->mTransform.getTrans())
                             << " skinToSkelT=" << vec3ToString(skinToSkel.getTrans())
                             << " skinToSkelThenTransformT=" << vec3ToString(skinToSkelThenTransform.getTrans())
                             << " path='" << pathStream.str() << "'";

            std::vector<std::pair<std::size_t, unsigned short>> samples;
            auto addSample = [&](std::size_t groupIndex, unsigned short vertex) {
                if (samples.size() >= 6)
                    return;
                const auto duplicate = std::find_if(samples.begin(), samples.end(), [&](const auto& sample) {
                    return sample.first == groupIndex && sample.second == vertex;
                });
                if (duplicate == samples.end())
                    samples.emplace_back(groupIndex, vertex);
            };

            const std::array<std::string_view, 8> targetBones = { "r hand", "l hand", "r upperarm", "l upperarm",
                "spine2", "head", "r forearm", "l forearm" };
            for (std::string_view target : targetBones)
            {
                std::size_t groupIndex = 0;
                for (const auto& [influences, vertices] : mData->mInfluences)
                {
                    bool matched = false;
                    for (const auto& [index, weight] : influences)
                    {
                        if (weight <= 0.f || index >= mData->mBones.size())
                            continue;
                        if (Misc::StringUtils::ciFind(mData->mBones[index].mName, target) != std::string_view::npos)
                        {
                            matched = true;
                            break;
                        }
                    }
                    if (matched && !vertices.empty())
                    {
                        addSample(groupIndex, vertices.front());
                        break;
                    }
                    ++groupIndex;
                }
            }
            if (samples.empty() && !mData->mInfluences.front().second.empty())
                samples.emplace_back(0, mData->mInfluences.front().second.front());

            auto weightedMatrixFor = [&](std::string_view mode, const BoneWeights& influences) {
                osg::Matrixf result = makeFalloutSkinningAccumulator();
                for (const auto& [index, weight] : influences)
                {
                    if (index >= mNodes.size() || index >= mData->mBones.size() || mNodes[index] == nullptr)
                        continue;

                    osg::Matrixf boneMat;
                    if (mode == "selected")
                        boneMat = boneMatrices[index];
                    else if (mode == "legacy")
                        boneMat = mData->mBones[index].mInvBindMatrix * mNodes[index]->mMatrixInSkeletonSpace;
                    else
                        boneMat = composeFalloutBoneMatrix(mData->mBones[index], mNodes[index], mode);
                    addWeightedFalloutMatrix(result, boneMat, weight);
                }
                return result;
            };

            auto skinnedPosition = [&](const osg::Matrixf& weightedMatrix, const osg::Matrixf& skinTransform,
                                       const osg::Vec3f& source) {
                osg::Matrixf result = weightedMatrix;
                result *= skinTransform;
                return result.preMult(source);
            };

            for (std::size_t sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex)
            {
                const auto& [groupIndex, vertex] = samples[sampleIndex];
                if (groupIndex >= mData->mInfluences.size() || vertex >= positionSrc->size())
                    continue;

                const BoneWeights& influences = mData->mInfluences[groupIndex].first;
                const osg::Vec3f& source = (*positionSrc)[vertex];
                const osg::Matrixf selected = weightedMatrixFor("selected", influences);
                const osg::Matrixf legacy = weightedMatrixFor("legacy", influences);
                const osg::Matrixf skeleton = weightedMatrixFor("skeleton", influences);
                const osg::Matrixf skeletonThenInvBind = weightedMatrixFor("skeletonThenInvBind", influences);
                const osg::Matrixf bindThenSkeleton = weightedMatrixFor("bindThenSkeleton", influences);
                const osg::Matrixf skeletonThenBind = weightedMatrixFor("skeletonThenBind", influences);

                std::ostringstream influenceStream;
                for (std::size_t influenceIndex = 0; influenceIndex < influences.size(); ++influenceIndex)
                {
                    const auto& [boneIndex, weight] = influences[influenceIndex];
                    if (influenceIndex != 0)
                        influenceStream << ";";
                    influenceStream << boneIndex << ":"
                                    << (boneIndex < mData->mBones.size() ? mData->mBones[boneIndex].mName : "<bad>")
                                    << "@" << weight;
                    if (boneIndex < mData->mBones.size())
                    {
                        osg::Matrixf bind;
                        const bool hasBind = bind.invert(mData->mBones[boneIndex].mInvBindMatrix);
                        influenceStream << " invBindT="
                                        << vec3ToString(mData->mBones[boneIndex].mInvBindMatrix.getTrans());
                        if (hasBind)
                            influenceStream << " bindT=" << vec3ToString(bind.getTrans());
                    }
                    if (boneIndex < mNodes.size() && mNodes[boneIndex] != nullptr)
                    {
                        influenceStream << " skelT="
                                        << vec3ToString(mNodes[boneIndex]->mMatrixInSkeletonSpace.getTrans());
                        if (mNodes[boneIndex]->mNode != nullptr)
                            influenceStream << " localT=" << vec3ToString(mNodes[boneIndex]->mNode->getMatrix().getTrans());
                    }
                }

                Log(Debug::Info) << "FNV/ESM4 SKIN MATRIX AUDIT rig='" << getName()
                                 << "' sample=" << sampleIndex
                                 << " group=" << groupIndex
                                 << " vertex=" << vertex
                                 << " source=" << vec3ToString(source)
                                 << " selected=" << vec3ToString(skinnedPosition(selected, transform, source))
                                 << " legacyNoRoot=" << vec3ToString(skinnedPosition(legacy, mData->mTransform, source))
                                 << " legacySkinToSkel="
                                 << vec3ToString(skinnedPosition(legacy, skinToSkelThenTransform, source))
                                 << " skeletonNoRoot=" << vec3ToString(skinnedPosition(skeleton, mData->mTransform, source))
                                 << " skeletonSkinToSkel="
                                 << vec3ToString(skinnedPosition(skeleton, skinToSkelThenTransform, source))
                                 << " skeletonThenInvBindNoRoot="
                                 << vec3ToString(skinnedPosition(skeletonThenInvBind, mData->mTransform, source))
                                 << " skeletonThenInvBindSkinToSkel="
                                 << vec3ToString(skinnedPosition(skeletonThenInvBind, skinToSkelThenTransform, source))
                                 << " bindThenSkeletonNoRoot="
                                 << vec3ToString(skinnedPosition(bindThenSkeleton, mData->mTransform, source))
                                 << " skeletonThenBindNoRoot="
                                 << vec3ToString(skinnedPosition(skeletonThenBind, mData->mTransform, source))
                                 << " influences=[" << influenceStream.str() << "]";
            }
        }

        float maxFalloutVertexDelta = 0.f;
        unsigned short maxFalloutVertex = 0;

        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            osg::Matrixf resultMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);

            for (const auto& [index, weight] : influences)
            {
                if (mNodes[index] == nullptr)
                    continue;
                const float* boneMatPtr = boneMatrices[index].ptr();
                float* resultMatPtr = resultMat.ptr();
                for (int i = 0; i < 16; ++i, ++resultMatPtr, ++boneMatPtr)
                    if (i % 4 != 3)
                        *resultMatPtr += *boneMatPtr * weight;
            }

            resultMat *= transform;

            for (unsigned short vertex : vertices)
            {
                (*positionDst)[vertex] = resultMat.preMult((*positionSrc)[vertex]);
                if (falloutRig && !mLoggedFalloutVertexSkinning)
                {
                    const float delta = ((*positionDst)[vertex] - (*positionSrc)[vertex]).length();
                    if (delta > maxFalloutVertexDelta)
                    {
                        maxFalloutVertexDelta = delta;
                        maxFalloutVertex = vertex;
                    }
                }
                if (normalDst)
                    (*normalDst)[vertex] = osg::Matrixf::transform3x3((*normalSrc)[vertex], resultMat);

                if (tangentDst)
                {
                    const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                    osg::Vec3f transformedTangent = osg::Matrixf::transform3x3(
                        osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()), resultMat);
                    (*tangentDst)[vertex] = osg::Vec4f(transformedTangent, srcTangent.w());
                }
            }
        }

        const bool falloutArmSleeveRig = falloutRig && isFalloutArmSleeveRig(getName(), mData->mRootBone, mData->mBones);
        if (falloutArmSleeveRig && shouldDisableFalloutArmSleeveProjection())
        {
            if (!mLoggedFalloutArmSleeveProjection)
            {
                mLoggedFalloutArmSleeveProjection = true;
                Log(Debug::Info) << "FNV/ESM4 proof: arm sleeve IK projection disabled rig='" << getName()
                                 << "' env=OPENMW_FNV_DISABLE_ARM_SLEEVE_IK_PROJECTION"
                                 << " runtime=runtime-supported gate=runtime-fnv-arm-sleeve-ik-disabled";
            }
        }
        else if (falloutRig && !falloutSourceSkinning
            && falloutArmSleeveRig
            && positionSrc != nullptr && positionDst != nullptr && !positionSrc->empty() && !positionDst->empty())
        {
            FalloutArmChain left;
            FalloutArmChain right;
            left.mValid = findFalloutBoneIndex(mData->mBones, "bip01 l upperarm", left.mUpper)
                && findFalloutBoneIndex(mData->mBones, "bip01 l forearm", left.mForearm)
                && findFalloutBoneIndex(mData->mBones, "bip01 l hand", left.mHand);
            right.mValid = findFalloutBoneIndex(mData->mBones, "bip01 r upperarm", right.mUpper)
                && findFalloutBoneIndex(mData->mBones, "bip01 r forearm", right.mForearm)
                && findFalloutBoneIndex(mData->mBones, "bip01 r hand", right.mHand);

            if (left.mValid && right.mValid && left.mUpper < mNodes.size() && left.mForearm < mNodes.size()
                && left.mHand < mNodes.size() && right.mUpper < mNodes.size() && right.mForearm < mNodes.size()
                && right.mHand < mNodes.size() && mNodes[left.mUpper] != nullptr && mNodes[left.mForearm] != nullptr
                && mNodes[left.mHand] != nullptr && mNodes[right.mUpper] != nullptr && mNodes[right.mForearm] != nullptr
                && mNodes[right.mHand] != nullptr)
            {
                struct ChainPose
                {
                    osg::Vec3f mBindUpper;
                    osg::Vec3f mBindForearm;
                    osg::Vec3f mBindHand;
                    osg::Vec3f mLiveUpper;
                    osg::Vec3f mLiveForearm;
                    osg::Vec3f mLiveHand;
                    osg::Quat mUpperRotation;
                    osg::Quat mLowerRotation;
                };

                auto makeChainPose = [&](const FalloutArmChain& chain) {
                    ChainPose pose;
                    pose.mBindUpper = falloutBindPoint(mData->mBones[chain.mUpper]);
                    pose.mBindForearm = falloutBindPoint(mData->mBones[chain.mForearm]);
                    pose.mBindHand = falloutBindPoint(mData->mBones[chain.mHand]);
                    pose.mLiveUpper = falloutLivePoint(mNodes[chain.mUpper], transform);
                    pose.mLiveForearm = falloutLivePoint(mNodes[chain.mForearm], transform);
                    pose.mLiveHand = falloutLivePoint(mNodes[chain.mHand], transform);
                    pose.mUpperRotation = makeFalloutSegmentRotation(
                        pose.mBindForearm - pose.mBindUpper, pose.mLiveForearm - pose.mLiveUpper);
                    pose.mLowerRotation
                        = makeFalloutSegmentRotation(pose.mBindHand - pose.mBindForearm, pose.mLiveHand - pose.mLiveForearm);
                    return pose;
                };

                const ChainPose leftPose = makeChainPose(left);
                const ChainPose rightPose = makeChainPose(right);
                std::size_t projectedVertices = 0;
                float maxProjectionDelta = 0.f;

                auto sideWeights = [&](const BoneWeights& influences) {
                    std::array<float, 4> weights = { 0.f, 0.f, 0.f, 0.f };
                    for (const auto& [index, weight] : influences)
                    {
                        if (index >= mData->mBones.size())
                            continue;
                        const std::string& boneName = mData->mBones[index].mName;
                        const bool leftBone = Misc::StringUtils::ciFind(boneName, " l ") != std::string_view::npos
                            || Misc::StringUtils::ciFind(boneName, " luparm") != std::string_view::npos;
                        const bool rightBone = Misc::StringUtils::ciFind(boneName, " r ") != std::string_view::npos
                            || Misc::StringUtils::ciFind(boneName, " ruparm") != std::string_view::npos;
                        const bool lowerBone = Misc::StringUtils::ciFind(boneName, "fore") != std::string_view::npos
                            || Misc::StringUtils::ciFind(boneName, "hand") != std::string_view::npos;
                        if (leftBone)
                            weights[0] += weight;
                        if (rightBone)
                            weights[1] += weight;
                        if (leftBone || rightBone)
                            weights[2] += weight;
                        if (lowerBone)
                            weights[3] += weight;
                    }
                    return weights;
                };

                for (const auto& [influences, vertices] : mData->mInfluences)
                {
                    const std::array<float, 4> weights = sideWeights(influences);
                    if (weights[2] < 0.35f)
                        continue;

                    for (unsigned short vertex : vertices)
                    {
                        if (vertex >= positionSrc->size() || vertex >= positionDst->size())
                            continue;

                        const osg::Vec3f source = (*positionSrc)[vertex];
                        const bool useLeft = weights[0] > weights[1] || (weights[0] == weights[1] && source.x() < 0.f);
                        const ChainPose& pose = useLeft ? leftPose : rightPose;

                        const float upperT = falloutClosestSegmentT(source, pose.mBindUpper, pose.mBindForearm);
                        const float lowerT = falloutClosestSegmentT(source, pose.mBindForearm, pose.mBindHand);
                        const osg::Vec3f upperPoint = falloutSegmentPoint(pose.mBindUpper, pose.mBindForearm, upperT);
                        const osg::Vec3f lowerPoint = falloutSegmentPoint(pose.mBindForearm, pose.mBindHand, lowerT);
                        const bool useLower = weights[3] > 0.45f
                            || (source - lowerPoint).length2() < (source - upperPoint).length2();
                        const osg::Vec3f bindPoint = useLower ? lowerPoint : upperPoint;
                        const osg::Vec3f livePoint = useLower
                            ? falloutSegmentPoint(pose.mLiveForearm, pose.mLiveHand, lowerT)
                            : falloutSegmentPoint(pose.mLiveUpper, pose.mLiveForearm, upperT);
                        const osg::Quat& rotation = useLower ? pose.mLowerRotation : pose.mUpperRotation;
                        const osg::Vec3f projected = livePoint + rotation * (source - bindPoint);

                        const float upperTaper = std::clamp((upperT - 0.15f) / 0.7f, 0.f, 1.f);
                        const float projectionStrength = useLower
                            ? std::clamp(0.55f + weights[3] * 0.45f, 0.f, 1.f)
                            : std::clamp((0.10f + weights[2] * 0.30f) * upperTaper, 0.f, 0.40f);
                        if (projectionStrength <= 0.001f)
                            continue;
                        const osg::Vec3f before = (*positionDst)[vertex];
                        (*positionDst)[vertex] = before * (1.f - projectionStrength) + projected * projectionStrength;
                        maxProjectionDelta = std::max(maxProjectionDelta, ((*positionDst)[vertex] - before).length());

                        if (normalSrc != nullptr && normalDst != nullptr && vertex < normalSrc->size()
                            && vertex < normalDst->size())
                        {
                            osg::Vec3f normal = (*normalDst)[vertex] * (1.f - projectionStrength)
                                + (rotation * (*normalSrc)[vertex]) * projectionStrength;
                            if (normal.normalize() > 0.0001f)
                                (*normalDst)[vertex] = normal;
                        }

                        if (tangentSrc != nullptr && tangentDst != nullptr && vertex < tangentSrc->size()
                            && vertex < tangentDst->size())
                        {
                            const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                            osg::Vec3f tangent = osg::Vec3f((*tangentDst)[vertex].x(), (*tangentDst)[vertex].y(),
                                                   (*tangentDst)[vertex].z())
                                    * (1.f - projectionStrength)
                                + (rotation * osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()))
                                    * projectionStrength;
                            if (tangent.normalize() > 0.0001f)
                                (*tangentDst)[vertex] = osg::Vec4f(tangent, srcTangent.w());
                        }

                        ++projectedVertices;
                    }
                }

                if (projectedVertices > 0)
                {
                    mFalloutUseVrRigidHandSolve = true;
                    const bool armBaselinePose = std::getenv("OPENMW_FNV_ARM_BASELINE_POSE") != nullptr;
                    if (!mLoggedFalloutArmSleeveProjection)
                    {
                        mLoggedFalloutArmSleeveProjection = true;
                        Log(Debug::Info) << "FNV/ESM4 proof: arm sleeve IK projection active rig='" << getName()
                                         << "' projectedVertices=" << projectedVertices
                                         << " maxProjectionDelta=" << maxProjectionDelta
                                         << " leftBind=(" << vec3ToString(leftPose.mBindUpper) << ","
                                         << vec3ToString(leftPose.mBindForearm) << ","
                                         << vec3ToString(leftPose.mBindHand) << ")"
                                         << " leftLive=(" << vec3ToString(leftPose.mLiveUpper) << ","
                                         << vec3ToString(leftPose.mLiveForearm) << ","
                                         << vec3ToString(leftPose.mLiveHand) << ")"
                                         << " rightBind=(" << vec3ToString(rightPose.mBindUpper) << ","
                                         << vec3ToString(rightPose.mBindForearm) << ","
                                         << vec3ToString(rightPose.mBindHand) << ")"
                                         << " rightLive=(" << vec3ToString(rightPose.mLiveUpper) << ","
                                         << vec3ToString(rightPose.mLiveForearm) << ","
                                         << vec3ToString(rightPose.mLiveHand) << ")"
                                         << " poseBasis=" << (armBaselinePose ? "t-pose-baseline" : "runtime")
                                         << " runtime=runtime-supported gate=runtime-fnv-arm-sleeve-ik";
                    }
                    if (getFalloutVrRigidHandSolveDefault() && !mLoggedFalloutVrRigidHandSolve)
                    {
                        mLoggedFalloutVrRigidHandSolve = true;
                        Log(Debug::Info) << "FNV/ESM4 proof: VR arcade IK hand solver active rig='" << getName()
                                         << "' rootBone='" << mData->mRootBone
                                         << "' scope='limb'"
                                         << " projectedVertices=" << projectedVertices
                                         << " maxProjectionDelta=" << maxProjectionDelta
                                         << " leftHandAnchorTarget=" << vec3ToString(leftPose.mLiveHand)
                                         << " rightHandAnchorTarget=" << vec3ToString(rightPose.mLiveHand)
                                         << " runtime=runtime-supported gate=runtime-fnv-vr-arcade-hand-ik";
                    }
                }
            }
        }

        if (falloutRig && !mLoggedFalloutVertexSkinning && maxFalloutVertexDelta > 0.001f)
        {
            mLoggedFalloutVertexSkinning = true;
            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' skinned vertices this frame maxVertexSkinDelta=" << maxFalloutVertexDelta
                             << " vertex=" << maxFalloutVertex;
        }

        const bool handRig = isFalloutHandRig(getName(), mData->mRootBone);
        const bool falloutVrArcadeHandSolveRig
            = falloutRig && handRig && isFalloutVrArcadeHandSolveRig(getName(), mData->mRootBone, mData->mBones);
        if (falloutRig && !falloutSourceSkinning && falloutVrArcadeHandSolveRig
            && getFalloutVrRigidHandSolveDefault() && positionSrc != nullptr && positionDst != nullptr
            && !positionSrc->empty() && !positionDst->empty())
        {
            osg::BoundingBox sourceBox;
            osg::BoundingBox skinnedBox;
            const std::size_t vertexCount = std::min(positionSrc->size(), positionDst->size());
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                sourceBox.expandBy((*positionSrc)[i]);
                skinnedBox.expandBy((*positionDst)[i]);
            }

            const osg::Vec3f sourceExtent = boundingBoxExtent(sourceBox);
            const osg::Vec3f skinnedExtent = boundingBoxExtent(skinnedBox);
            const osg::Vec3f sourceCenter = sourceBox.valid() ? sourceBox.center() : osg::Vec3f();
            const osg::Vec3f skinnedCenter = skinnedBox.valid() ? skinnedBox.center() : osg::Vec3f();
            const float extentRatio = maxFiniteExtentRatio(skinnedExtent, sourceExtent);
            osg::Vec3f solveTarget = skinnedCenter;
            bool hasHandAnchor = false;
            if (handRig)
            {
                const std::string lowerName = Misc::StringUtils::lowerCase(getName());
                const std::string lowerRoot = Misc::StringUtils::lowerCase(mData->mRootBone);
                const bool left = lowerName.find("left") != std::string::npos
                    || lowerName.find(" l ") != std::string::npos || lowerRoot.find(" l ") != std::string::npos;
                const std::string_view targetBone = left ? "bip01 l hand" : "bip01 r hand";
                for (std::size_t i = 0; i < mData->mBones.size() && i < mNodes.size(); ++i)
                {
                    if (mNodes[i] == nullptr || !Misc::StringUtils::ciEqual(mData->mBones[i].mName, targetBone))
                        continue;

                    osg::Matrixf handMatrix = mNodes[i]->mMatrixInSkeletonSpace;
                    handMatrix *= transform;
                    solveTarget = handMatrix.preMult(osg::Vec3f());
                    hasHandAnchor = true;
                    break;
                }
            }
            const float handAnchorDistance = hasHandAnchor ? (skinnedCenter - solveTarget).length() : 0.f;
            const osg::Vec3f solveOffset = solveTarget - sourceCenter;
            if ((extentRatio > 1.75f || handAnchorDistance > 18.f) && solveOffset.length2() > 0.0001f)
            {
                for (osg::ref_ptr<osg::Geometry>& solvedGeometry : mGeometry)
                {
                    if (solvedGeometry == nullptr)
                        continue;

                    osg::Vec3Array* solvedPositionDst
                        = static_cast<osg::Vec3Array*>(solvedGeometry->getVertexArray());
                    osg::Vec3Array* solvedNormalDst = static_cast<osg::Vec3Array*>(solvedGeometry->getNormalArray());
                    osg::Vec4Array* solvedTangentDst
                        = static_cast<osg::Vec4Array*>(solvedGeometry->getTexCoordArray(7));
                    copyTranslatedSourceSkinningGeometry(positionSrc, normalSrc, tangentSrc, solvedPositionDst,
                        solvedNormalDst, solvedTangentDst, solveOffset);
                    if (solvedPositionDst != nullptr)
                        solvedPositionDst->dirty();
                    if (solvedNormalDst != nullptr)
                        solvedNormalDst->dirty();
                    if (solvedTangentDst != nullptr)
                        solvedTangentDst->dirty();
                    solvedGeometry->dirtyBound();
                    solvedGeometry->osg::Drawable::dirtyGLObjects();
                }
                mFalloutUseVrRigidHandSolve = true;
                mFalloutFallbackDecided = true;
                if (!mLoggedFalloutVrRigidHandSolve)
                {
                    mLoggedFalloutVrRigidHandSolve = true;
                    Log(Debug::Info) << "FNV/ESM4 proof: VR arcade IK hand solver active rig='" << getName()
                                     << "' rootBone='" << mData->mRootBone
                                     << "' scope='" << (handRig ? "hand" : "limb")
                                     << "' sourceCenter=" << vec3ToString(sourceCenter)
                                     << " skinnedTarget=" << vec3ToString(skinnedCenter)
                                     << " handAnchorTarget=" << vec3ToString(solveTarget)
                                     << " handAnchorDistance=" << handAnchorDistance
                                     << " solveOffset=" << vec3ToString(solveOffset)
                                     << " sourceExtent=" << vec3ToString(sourceExtent)
                                     << " skinnedExtent=" << vec3ToString(skinnedExtent)
                                     << " extentRatio=" << extentRatio
                                     << " runtime=runtime-supported gate=runtime-fnv-vr-arcade-hand-ik";
                }
            }
        }

        if (falloutRig && !mLoggedFalloutPoseSanity && positionSrc != nullptr && positionDst != nullptr
            && !positionSrc->empty() && !positionDst->empty())
        {
            mLoggedFalloutPoseSanity = true;
            osg::BoundingBox sourceBox;
            osg::BoundingBox skinnedBox;
            const std::size_t vertexCount = std::min(positionSrc->size(), positionDst->size());
            float maxVertexDelta = 0.f;
            unsigned short maxVertexDeltaIndex = 0;
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                const osg::Vec3f& source = (*positionSrc)[i];
                const osg::Vec3f& skinned = (*positionDst)[i];
                sourceBox.expandBy(source);
                skinnedBox.expandBy(skinned);
                const float delta = (skinned - source).length();
                if (delta > maxVertexDelta)
                {
                    maxVertexDelta = delta;
                    maxVertexDeltaIndex = static_cast<unsigned short>(i);
                }
            }

            const osg::Vec3f sourceExtent = boundingBoxExtent(sourceBox);
            const osg::Vec3f skinnedExtent = boundingBoxExtent(skinnedBox);
            const osg::Vec3f sourceCenter = sourceBox.valid() ? sourceBox.center() : osg::Vec3f();
            const osg::Vec3f skinnedCenter = skinnedBox.valid() ? skinnedBox.center() : osg::Vec3f();
            const float sourceDiag = sourceExtent.length();
            const float skinnedDiag = skinnedExtent.length();
            const float centerDelta = (skinnedCenter - sourceCenter).length();
            const float extentRatio = maxFiniteExtentRatio(skinnedExtent, sourceExtent);
            const float outlierRadius = std::max(64.f, sourceDiag * 1.75f);
            std::size_t outlierVertices = 0;
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                if (((*positionDst)[i] - sourceCenter).length() > outlierRadius)
                    ++outlierVertices;
            }

            const bool vrArcadeSolved = mFalloutUseVrRigidHandSolve;
            const bool badCenter = !vrArcadeSolved && centerDelta > std::max(24.f, sourceDiag * 1.25f);
            constexpr float falloutAutoFallbackExtentRatio = 2.25f;
            const bool badExtent = extentRatio > falloutAutoFallbackExtentRatio;
            const bool badDelta = maxVertexDelta > std::max(96.f, sourceDiag * 2.0f);
            const bool badOutliers
                = !vrArcadeSolved && vertexCount > 0 && outlierVertices > std::max<std::size_t>(24, vertexCount / 5);
            const bool bad = badCenter || badExtent || badDelta || badOutliers;
            const char* reason = badCenter   ? "center"
                : badExtent                  ? "extent"
                : badDelta                   ? "vertex"
                : badOutliers                ? "outliers"
                                             : "ok";

            if (falloutAutoMode && !mFalloutFallbackDecided && !mFalloutUseVrRigidHandSolve)
            {
                mFalloutFallbackDecided = true;
                if (bad)
                {
                    mFalloutUseSourceFallback = true;
                    for (osg::ref_ptr<osg::Geometry>& fallbackGeometry : mGeometry)
                    {
                        if (fallbackGeometry == nullptr)
                            continue;

                        osg::Vec3Array* fallbackPositionDst
                            = static_cast<osg::Vec3Array*>(fallbackGeometry->getVertexArray());
                        osg::Vec3Array* fallbackNormalDst
                            = static_cast<osg::Vec3Array*>(fallbackGeometry->getNormalArray());
                        osg::Vec4Array* fallbackTangentDst
                            = static_cast<osg::Vec4Array*>(fallbackGeometry->getTexCoordArray(7));
                        copySourceSkinningGeometry(positionSrc, normalSrc, tangentSrc, fallbackPositionDst,
                            fallbackNormalDst, fallbackTangentDst);
                        if (fallbackPositionDst != nullptr)
                            fallbackPositionDst->dirty();
                        if (fallbackNormalDst != nullptr)
                            fallbackNormalDst->dirty();
                        if (fallbackTangentDst != nullptr)
                            fallbackTangentDst->dirty();
                        fallbackGeometry->dirtyBound();
                        fallbackGeometry->osg::Drawable::dirtyGLObjects();
                    }
                    Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                     << "' auto skinning fallback=source reason=" << reason;
                }
            }

            Log(Debug::Info) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' pose sanity vertices=" << vertexCount
                             << " sourceExtent=(" << sourceExtent.x() << "," << sourceExtent.y() << ","
                             << sourceExtent.z() << ")"
                             << " skinnedExtent=(" << skinnedExtent.x() << "," << skinnedExtent.y() << ","
                             << skinnedExtent.z() << ")"
                             << " sourceCenter=(" << sourceCenter.x() << "," << sourceCenter.y() << ","
                             << sourceCenter.z() << ")"
                             << " skinnedCenter=(" << skinnedCenter.x() << "," << skinnedCenter.y() << ","
                             << skinnedCenter.z() << ")"
                             << " sourceDiag=" << sourceDiag << " skinnedDiag=" << skinnedDiag
                             << " centerDelta=" << centerDelta << " extentRatio=" << extentRatio
                             << " extentLimit=" << falloutAutoFallbackExtentRatio
                             << " maxVertexDelta=" << maxVertexDelta << " maxVertex=" << maxVertexDeltaIndex
                             << " outlierVertices=" << outlierVertices << " outlierRadius=" << outlierRadius
                             << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        }

        if (falloutRig && !mLoggedFalloutFabricNoTwist && mSourceGeometry != nullptr && positionSrc != nullptr
            && positionDst != nullptr && !positionSrc->empty() && !positionDst->empty())
        {
            mLoggedFalloutFabricNoTwist = true;
            const FalloutFabricNoTwistStats fabricStats
                = computeFalloutFabricNoTwistStats(*mSourceGeometry, *positionSrc, *positionDst);
            const unsigned int noisyEdgeLimit = std::max(12u, fabricStats.mEdgeSamples / 100u);
            const bool badStretch = fabricStats.mMaxEdgeStretchRatio > 6.f && fabricStats.mMaxEdgeLengthDelta > 12.f;
            const bool badMany = fabricStats.mOverstretchedEdges > noisyEdgeLimit
                || fabricStats.mCollapsedEdges > noisyEdgeLimit;
            const bool badFabric = fabricStats.mEdgeSamples == 0 || badStretch || badMany;
            const char* reason = fabricStats.mEdgeSamples == 0 ? "missing-edges"
                : badStretch                                 ? "max-edge-stretch"
                : badMany                                    ? "many-stretched-edges"
                                                             : "ok";
            const bool detailedFabricAudit = badFabric || std::getenv("OPENMW_FNV_FABRIC_NO_TWIST_DETAIL") != nullptr;

            std::string path;
            if (detailedFabricAudit && nv != nullptr)
            {
                const osg::NodePath& nodePath = nv->getNodePath();
                for (std::size_t i = 0; i < nodePath.size(); ++i)
                {
                    const osg::Node* node = nodePath[i];
                    if (i != 0)
                        path += "/";
                    path += node != nullptr && !node->getName().empty() ? node->getName() : "<unnamed>";
                }
            }

            auto vertexInfluences = [&](unsigned int vertex) {
                std::ostringstream stream;
                bool found = false;
                if (mData != nullptr)
                {
                    for (const auto& [influences, vertices] : mData->mInfluences)
                    {
                        if (std::find(vertices.begin(), vertices.end(), vertex) == vertices.end())
                            continue;

                        for (const auto& [boneIndex, weight] : influences)
                        {
                            if (found)
                                stream << ",";
                            if (boneIndex < mData->mBones.size())
                                stream << mData->mBones[boneIndex].mName;
                            else
                                stream << "<bone-" << boneIndex << ">";
                            stream << ":" << weight;
                            found = true;
                        }
                        break;
                    }
                }
                if (!found)
                    stream << "<none>";
                return stream.str();
            };

            const bool haveMaxEdgeVertices = fabricStats.mMaxEdgeA < positionSrc->size()
                && fabricStats.mMaxEdgeB < positionSrc->size() && fabricStats.mMaxEdgeA < positionDst->size()
                && fabricStats.mMaxEdgeB < positionDst->size();
            const osg::Vec3f maxEdgeSourceA = haveMaxEdgeVertices ? (*positionSrc)[fabricStats.mMaxEdgeA] : osg::Vec3f();
            const osg::Vec3f maxEdgeSourceB = haveMaxEdgeVertices ? (*positionSrc)[fabricStats.mMaxEdgeB] : osg::Vec3f();
            const osg::Vec3f maxEdgeSkinnedA = haveMaxEdgeVertices ? (*positionDst)[fabricStats.mMaxEdgeA] : osg::Vec3f();
            const osg::Vec3f maxEdgeSkinnedB = haveMaxEdgeVertices ? (*positionDst)[fabricStats.mMaxEdgeB] : osg::Vec3f();
            const float maxEdgeSourceLength = haveMaxEdgeVertices ? (maxEdgeSourceA - maxEdgeSourceB).length() : 0.f;
            const float maxEdgeSkinnedLength = haveMaxEdgeVertices ? (maxEdgeSkinnedA - maxEdgeSkinnedB).length() : 0.f;

            Log(Debug::Info) << "FNV/ESM4 proof: Fallout RigGeometry '" << getName()
                             << "' fabric no-twist edge audit vertices="
                             << std::min(positionSrc->size(), positionDst->size())
                             << " edgeSamples=" << fabricStats.mEdgeSamples
                             << " overstretchedEdges=" << fabricStats.mOverstretchedEdges
                             << " collapsedEdges=" << fabricStats.mCollapsedEdges
                             << " noisyEdgeLimit=" << noisyEdgeLimit
                             << " maxEdgeStretchRatio=" << fabricStats.mMaxEdgeStretchRatio
                             << " maxEdgeLengthDelta=" << fabricStats.mMaxEdgeLengthDelta
                             << " maxEdge=(" << fabricStats.mMaxEdgeA << "," << fabricStats.mMaxEdgeB << ")"
                             << " maxEdgeSourceLength=" << maxEdgeSourceLength
                             << " maxEdgeSkinnedLength=" << maxEdgeSkinnedLength
                             << " maxEdgeSourceA=" << vec3ToString(maxEdgeSourceA)
                             << " maxEdgeSourceB=" << vec3ToString(maxEdgeSourceB)
                             << " maxEdgeSkinnedA=" << vec3ToString(maxEdgeSkinnedA)
                             << " maxEdgeSkinnedB=" << vec3ToString(maxEdgeSkinnedB)
                             << " maxEdgeInfluencesA=\"" << vertexInfluences(fabricStats.mMaxEdgeA) << "\""
                             << " maxEdgeInfluencesB=\"" << vertexInfluences(fabricStats.mMaxEdgeB) << "\""
                             << " path=\"" << path << "\""
                             << " verdict=" << (badFabric ? "BAD" : "OK")
                             << " reason=" << reason
                             << " runtime=runtime-supported gate=runtime-fnv-fabric-no-twist";
        }

        positionDst->dirty();
        if (normalDst)
            normalDst->dirty();
        if (tangentDst)
            tangentDst->dirty();

        geom.dirtyBound();
        geom.osg::Drawable::dirtyGLObjects();
        applyFalloutLiveRigWeightDebug(geom);

        nv->pushOntoNodePath(&geom);
        nv->apply(geom);
        nv->popFromNodePath();
    }

    void RigGeometry::updateBounds(osg::NodeVisitor* nv)
    {
        if (!mSkeleton)
        {
            if (!initFromParentSkeleton(nv))
                return;
        }

        if (!mSkeleton->getActive() && !mBoundsFirstFrame)
            return;
        mBoundsFirstFrame = false;

        mSkeleton->updateBoneMatrices(nv->getTraversalNumber());

        updateSkinToSkelMatrix(nv->getNodePath());

        osg::BoundingBox box;
        const bool falloutRig = isFalloutCharacterRig();
        const bool falloutFlagRig = mFalloutFlagSkinning;
        osg::Matrixf transform;
        const bool falloutInventoryPaperDoll = falloutRig && isFalloutInventoryPaperDollPath(nv->getNodePath());
        const std::string_view falloutSkinningMode = falloutRig
            ? getFalloutSkinningMode(getName(), mData->mRootBone, falloutInventoryPaperDoll)
            : std::string_view();
        const bool sourceSkinOnly = falloutRig && falloutSkinningMode == "sourceSkinOnly"
            && mSkinToSkelMatrix != nullptr;
        const bool falloutSourceSkinning = falloutRig
            && (falloutSkinningMode == "source" || sourceSkinOnly
                || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback));
        if (falloutFlagRig)
            transform.makeIdentity();
        else if (falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && (!falloutRig || useFalloutSkinToSkelMatrix()))
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        size_t index = 0;
        for (const BoneInfo& info : mData->mBones)
        {
            const Bone* bone = mNodes[index++];
            if (bone == nullptr)
                continue;

            osg::BoundingSpheref bs = info.mBoundSphere;
            transformBoundingSphere(bone->mMatrixInSkeletonSpace * transform, bs);
            box.expandBy(bs);
        }

        if (box != _boundingBox)
        {
            _boundingBox = box;
            _boundingSphere = osg::BoundingSphere(_boundingBox);
            _boundingSphereComputed = true;
            for (unsigned int i = 0; i < getNumParents(); ++i)
                getParent(i)->dirtyBound();

            for (unsigned int i = 0; i < 2; ++i)
            {
                osg::Geometry& geom = *mGeometry[i];
                static_cast<CopyBoundingBoxCallback*>(geom.getComputeBoundingBoxCallback())->boundingBox = _boundingBox;
                static_cast<CopyBoundingSphereCallback*>(geom.getComputeBoundingSphereCallback())->boundingSphere
                    = _boundingSphere;
                geom.dirtyBound();
            }
        }
    }

    void RigGeometry::updateSkinToSkelMatrix(const osg::NodePath& nodePath)
    {
        if (mSkinToSkelMatrix)
            mSkinToSkelMatrix->makeIdentity();
        auto skeletonRoot = std::find(nodePath.begin(), nodePath.end(), mSkeleton);
        if (skeletonRoot == nodePath.end())
            return;
        skeletonRoot++;
        auto skinRoot = nodePath.end();
        if (!mData->mRootBone.empty())
            skinRoot = std::find_if(skeletonRoot, nodePath.end(),
                [&](const osg::Node* node) { return Misc::StringUtils::ciEqual(node->getName(), mData->mRootBone); });
        if (skinRoot == nodePath.end())
        {
            // Failed to find skin root, cancel out everything up till the trishape.
            // Our parent node is the trishape's transform
            skinRoot = nodePath.end() - 2;
            if ((*skinRoot)->getName() != getName()) // but maybe it can get optimized out
                skinRoot++;
        }
        else
            skinRoot++;
        for (auto it = skeletonRoot; it != skinRoot; ++it)
        {
            const osg::Node* node = *it;
            if (const osg::Transform* trans = node->asTransform())
            {
                const osg::MatrixTransform* matrixTrans = trans->asMatrixTransform();
                if (matrixTrans && matrixTrans->getMatrix().isIdentity())
                    continue;
                if (!mSkinToSkelMatrix)
                    mSkinToSkelMatrix = new osg::RefMatrix;
                trans->computeWorldToLocalMatrix(*mSkinToSkelMatrix, nullptr);
            }
        }
    }

    void RigGeometry::setBoneInfo(std::vector<BoneInfo>&& bones)
    {
        if (!mData)
            mData = new InfluenceData;

        mData->mBones = std::move(bones);
    }

    void RigGeometry::setInfluences(const std::vector<VertexWeights>& influences)
    {
        if (!mData)
            mData = new InfluenceData;

        std::unordered_map<unsigned short, BoneWeights> vertexToInfluences;
        size_t index = 0;
        for (const auto& influence : influences)
        {
            for (const auto& [vertex, weight] : influence)
                vertexToInfluences[vertex].emplace_back(index, weight);
            index++;
        }

        std::map<BoneWeights, VertexList> influencesToVertices;
        for (const auto& [vertex, weights] : vertexToInfluences)
            influencesToVertices[weights].emplace_back(vertex);

        mData->mInfluences.reserve(influencesToVertices.size());
        mData->mInfluences.assign(influencesToVertices.begin(), influencesToVertices.end());
    }

    void RigGeometry::setInfluences(const std::vector<BoneWeights>& influences)
    {
        if (!mData)
            mData = new InfluenceData;

        std::map<BoneWeights, VertexList> influencesToVertices;
        for (size_t i = 0; i < influences.size(); i++)
            influencesToVertices[influences[i]].emplace_back(static_cast<VertexList::value_type>(i));

        mData->mInfluences.reserve(influencesToVertices.size());
        mData->mInfluences.assign(influencesToVertices.begin(), influencesToVertices.end());
    }

    void RigGeometry::setTransform(osg::Matrixf&& transform)
    {
        if (!mData)
            mData = new InfluenceData;
        mData->mTransform = transform;
    }

    void RigGeometry::setRootBone(std::string_view name)
    {
        if (!mData)
            mData = new InfluenceData;
        mData->mRootBone = name;
    }

    std::string_view RigGeometry::getRootBone() const
    {
        if (!mData)
            return {};
        return mData->mRootBone;
    }

    std::size_t RigGeometry::getBoneCount() const
    {
        if (!mData)
            return 0;
        return mData->mBones.size();
    }

    std::string_view RigGeometry::getBoneName(std::size_t index) const
    {
        if (!mData || index >= mData->mBones.size())
            return {};
        return mData->mBones[index].mName;
    }

    bool RigGeometry::getSkinningDebugData(std::vector<BoneInfo>& bones, std::vector<BoneWeights>& vertexInfluences,
        std::vector<osg::Matrixf>& localBoneMatrices, std::vector<osg::Matrixf>& skeletonBoneMatrices,
        osg::Matrixf& transform, osg::Matrixf& skinToSkelMatrix) const
    {
        bones.clear();
        vertexInfluences.clear();
        localBoneMatrices.clear();
        skeletonBoneMatrices.clear();
        transform.makeIdentity();
        skinToSkelMatrix.makeIdentity();

        if (!mData || !mSourceGeometry || mSourceGeometry->getVertexArray() == nullptr)
            return false;

        const std::size_t vertexCount = mSourceGeometry->getVertexArray()->getNumElements();
        if (vertexCount == 0)
            return false;

        bones = mData->mBones;
        vertexInfluences.resize(vertexCount);
        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            for (unsigned short vertex : vertices)
            {
                if (vertex < vertexInfluences.size())
                    vertexInfluences[vertex] = influences;
            }
        }

        localBoneMatrices.resize(bones.size());
        skeletonBoneMatrices.resize(bones.size());
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            localBoneMatrices[i].makeIdentity();
            skeletonBoneMatrices[i].makeIdentity();
            if (i >= mNodes.size() || mNodes[i] == nullptr)
                continue;

            if (mNodes[i]->mNode != nullptr)
                localBoneMatrices[i] = mNodes[i]->mNode->getMatrix();
            skeletonBoneMatrices[i] = mNodes[i]->mMatrixInSkeletonSpace;
        }

        transform = mData->mTransform;
        if (mSkinToSkelMatrix)
            skinToSkelMatrix = *mSkinToSkelMatrix;
        return true;
    }

    bool RigGeometry::getFalloutFingerVertexWeights(
        std::vector<float>& thumb, std::vector<float>& index, std::vector<float>& grip) const
    {
        thumb.clear();
        index.clear();
        grip.clear();

        if (!mData || !mSourceGeometry || mSourceGeometry->getVertexArray() == nullptr)
            return false;

        const std::size_t vertexCount = mSourceGeometry->getVertexArray()->getNumElements();
        if (vertexCount == 0)
            return false;

        thumb.assign(vertexCount, 0.f);
        index.assign(vertexCount, 0.f);
        grip.assign(vertexCount, 0.f);

        auto clampUnit = [](float value) {
            return std::max(0.f, std::min(1.f, value));
        };

        bool found = false;
        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            float thumbWeight = 0.f;
            float indexWeight = 0.f;
            float gripWeight = 0.f;

            for (const auto& [boneIndex, weight] : influences)
            {
                if (boneIndex >= mData->mBones.size() || weight <= 0.f)
                    continue;

                const std::string boneName = Misc::StringUtils::lowerCase(mData->mBones[boneIndex].mName);
                if (boneName.find("finger0") != std::string::npos || boneName.find("thumb") != std::string::npos)
                    thumbWeight += weight;
                else if (boneName.find("finger1") != std::string::npos || boneName.find("index") != std::string::npos)
                    indexWeight += weight;
                else if (boneName.find("finger2") != std::string::npos || boneName.find("finger3") != std::string::npos
                    || boneName.find("finger4") != std::string::npos || boneName.find("middle") != std::string::npos
                    || boneName.find("ring") != std::string::npos || boneName.find("pinky") != std::string::npos
                    || boneName.find("little") != std::string::npos)
                    gripWeight += weight;
            }

            if (thumbWeight <= 0.f && indexWeight <= 0.f && gripWeight <= 0.f)
                continue;

            found = true;
            for (unsigned short vertex : vertices)
            {
                if (vertex >= vertexCount)
                    continue;
                thumb[vertex] = clampUnit(thumb[vertex] + thumbWeight);
                index[vertex] = clampUnit(index[vertex] + indexWeight);
                grip[vertex] = clampUnit(grip[vertex] + gripWeight);
            }
        }

        return found;
    }

    namespace
    {
        int getFalloutFingerBoneSlot(const std::string& boneName)
        {
            const auto chainSlot = [&](std::string_view base, int group) -> int {
                const std::string chain2 = std::string(base) + "2";
                const std::string chain1 = std::string(base) + "1";
                if (boneName.find(chain2) != std::string::npos)
                    return group * 3 + 2;
                if (boneName.find(chain1) != std::string::npos)
                    return group * 3 + 1;
                if (boneName.find(base) != std::string::npos)
                    return group * 3;
                return -1;
            };

            int slot = chainSlot("thumb1", 0);
            if (slot >= 0)
                return slot;
            slot = chainSlot("finger1", 1);
            if (slot >= 0)
                return slot;
            slot = chainSlot("finger2", 2);
            if (slot >= 0)
                return slot;
            slot = chainSlot("finger3", 3);
            if (slot >= 0)
                return slot;
            slot = chainSlot("finger4", 4);
            if (slot >= 0)
                return slot;

            if (boneName.find("index") != std::string::npos)
                return 3;
            if (boneName.find("middle") != std::string::npos)
                return 6;
            if (boneName.find("ring") != std::string::npos)
                return 9;
            if (boneName.find("pinky") != std::string::npos || boneName.find("little") != std::string::npos)
                return 12;
            if (boneName.find("thumb") != std::string::npos)
                return 0;

            return -1;
        }
    }

    bool RigGeometry::getFalloutFingerBoneVertexWeights(std::array<std::vector<float>, 15>& fingerBones) const
    {
        for (auto& weights : fingerBones)
            weights.clear();

        if (!mData || !mSourceGeometry || mSourceGeometry->getVertexArray() == nullptr)
            return false;

        const std::size_t vertexCount = mSourceGeometry->getVertexArray()->getNumElements();
        if (vertexCount == 0)
            return false;

        for (auto& weights : fingerBones)
            weights.assign(vertexCount, 0.f);

        auto clampUnit = [](float value) {
            return std::max(0.f, std::min(1.f, value));
        };

        bool found = false;
        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            std::array<float, 15> fingerWeights{};
            for (const auto& [boneIndex, weight] : influences)
            {
                if (boneIndex >= mData->mBones.size() || weight <= 0.f)
                    continue;

                const std::string boneName = Misc::StringUtils::lowerCase(mData->mBones[boneIndex].mName);
                const int slot = getFalloutFingerBoneSlot(boneName);
                if (slot < 0 || slot >= static_cast<int>(fingerWeights.size()))
                    continue;

                fingerWeights[slot] += weight;
            }

            bool hasFingerWeight = false;
            for (float weight : fingerWeights)
            {
                if (weight > 0.f)
                {
                    hasFingerWeight = true;
                    break;
                }
            }
            if (!hasFingerWeight)
                continue;

            found = true;
            for (unsigned short vertex : vertices)
            {
                if (vertex >= vertexCount)
                    continue;

                for (std::size_t slot = 0; slot < fingerWeights.size(); ++slot)
                    fingerBones[slot][vertex] = clampUnit(fingerBones[slot][vertex] + fingerWeights[slot]);
            }
        }

        return found;
    }

    void RigGeometry::accept(osg::NodeVisitor& nv)
    {
        if (!nv.validNodeMask(*this))
            return;

        nv.pushOntoNodePath(this);

        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            // The cull visitor won't be applied to the node itself,
            // but we want to use its state to render the child geometry.
            osg::StateSet* stateset = getStateSet();
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);
            if (stateset)
                cv->pushStateSet(stateset);

            cull(&nv);
            if (stateset)
                cv->popStateSet();
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
            updateBounds(&nv);
        else
            nv.apply(*this);

        nv.popFromNodePath();
    }

    void RigGeometry::accept(osg::PrimitiveFunctor& func) const
    {
        getGeometry(mLastFrameNumber)->accept(func);
    }

    osg::Geometry* RigGeometry::getGeometry(unsigned int frame) const
    {
        return mGeometry[frame % 2].get();
    }

}
