#include "riggeometry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <osg/MatrixTransform>
#include <osg/Transform>

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

        const char* getEsm4RuntimeEnv(const char* name, const char* legacyName)
        {
            if (const char* env = std::getenv(name))
                return env;

            return std::getenv(legacyName);
        }

        std::string_view getFalloutSkinningMode()
        {
            if (const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_SKINNING_MODE", "OPENMW_FNV_SKINNING_MODE"))
                return env;
            return "invBindThenSkeleton";
        }

        bool hasFalloutSkinningModeOverride()
        {
            const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_SKINNING_MODE", "OPENMW_FNV_SKINNING_MODE");
            return env != nullptr && env[0] != '\0';
        }

        bool isFalloutHandRig(std::string_view name, std::string_view rootBone)
        {
            return Misc::StringUtils::ciFind(name, "hand") != std::string_view::npos
                || Misc::StringUtils::ciFind(name, "glove") != std::string_view::npos
                || Misc::StringUtils::ciFind(rootBone, " hand") != std::string_view::npos;
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
                if (const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_HAND_SKINNING_MODE", "OPENMW_FNV_VR_HAND_SKINNING_MODE"))
                    return env;

                if (hasFalloutSkinningModeOverride())
                    return getFalloutSkinningMode();

                return getFalloutSkinningMode();
            }

            if (hasFalloutSkinningModeOverride())
                return getFalloutSkinningMode();

            return "current";
        }

        bool useFalloutSkinToSkelMatrix()
        {
            if (const char* env = getEsm4RuntimeEnv("OPENMW_ESM4_USE_SKIN_TO_SKEL", "OPENMW_FNV_USE_SKIN_TO_SKEL"))
                return std::string_view(env) != "0";
            return false;
        }

        osg::Matrixf composeSkinToSkeletonTransform(
            const osg::Matrixf& skinToSkeleton, const osg::Matrixf& skinTransform, bool inverse)
        {
            if (inverse)
            {
                osg::Matrixf skeletonToSkin;
                if (skeletonToSkin.invert(skinToSkeleton))
                    return skeletonToSkin * skinTransform;
            }
            return skinToSkeleton * skinTransform;
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
            if (mode == "invBindThenSkeleton")
                return invBind * skeleton;

            return invBind * skeleton;
        }


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
        , mFalloutCharacterSkinning(copy.mFalloutCharacterSkinning)
        , mSourceFrameSkinning(copy.mSourceFrameSkinning)
        , mInverseSkinToSkeletonMatrix(copy.mInverseSkinToSkeletonMatrix)
        , mFalloutCharacterRig(copy.mFalloutCharacterRig)
    {
        setSourceGeometry(copy.mSourceGeometry);
        setNumChildrenRequiringUpdateTraversal(1);
    }

    void RigGeometry::setFalloutFlagSkinning(bool enabled)
    {
        mFalloutFlagSkinning = enabled;
        mFalloutCharacterRigComputed = false;
    }

    void RigGeometry::setFalloutCharacterSkinning(bool enabled)
    {
        mFalloutCharacterSkinning = enabled;
        mFalloutCharacterRigComputed = false;
    }

    void RigGeometry::setSourceFrameSkinning(bool enabled)
    {
        mSourceFrameSkinning = enabled;
    }

    void RigGeometry::setInverseSkinToSkeletonMatrix(bool enabled)
    {
        mInverseSkinToSkeletonMatrix = enabled;
    }

    bool RigGeometry::isFalloutCharacterRig() const
    {
        if (mFalloutCharacterRigComputed)
            return mFalloutCharacterRig;

        if (mData == nullptr)
            return false;

        // Explicitly marked actor rigs have already been classified by the loader/animation
        // layer. Do not re-reject SSE partition rigs merely because they use "NPC ..." bone
        // names instead of the older Bip01 convention.
        if (mFalloutCharacterSkinning)
        {
            mFalloutCharacterRig = !mData->mBones.empty();
            mFalloutCharacterRigComputed = true;
            return mFalloutCharacterRig;
        }

        const bool markedFalloutRig = mFalloutFlagSkinning || mFalloutCharacterSkinning;
        const bool heuristicEnabled
            = getEsm4RuntimeEnv("OPENMW_ESM4_ENABLE_RIG_HEURISTIC", "OPENMW_FNV_ENABLE_RIG_HEURISTIC") != nullptr;
        if (!markedFalloutRig && !heuristicEnabled)
        {
            mFalloutCharacterRig = false;
            mFalloutCharacterRigComputed = true;
            return mFalloutCharacterRig;
        }

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
        const bool falloutSourceSkinning = mSourceFrameSkinning || falloutSkinningMode == "source" || sourceSkinOnly
            || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback);

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
                boneMat = falloutSourceSkinning ? osg::Matrixf()
                                                : composeFalloutBoneMatrix(*boneInfo, *bone, falloutSkinningMode);
            ++bone;
            ++boneInfo;
        }

        osg::Matrixf transform;
        if (mFalloutFlagSkinning || falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && (mInverseSkinToSkeletonMatrix || useFalloutSkinToSkelMatrix()))
            transform = composeSkinToSkeletonTransform(
                *mSkinToSkelMatrix, mData->mTransform, mInverseSkinToSkeletonMatrix);
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
                copySourceSkinningGeometry(positionSrc, normalSrc, tangentSrc, positionDst, normalDst, tangentDst);
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
        const bool falloutSourceSkinning = mSourceFrameSkinning || falloutSkinningMode == "source" || sourceSkinOnly
            || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback);

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
                boneMat = falloutSourceSkinning ? osg::Matrixf()
                                                : composeFalloutBoneMatrix(*boneInfo, *bone, falloutSkinningMode);
            ++bone;
            ++boneInfo;
        }

        osg::Matrixf transform;
        if (mFalloutFlagSkinning || falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix && (mInverseSkinToSkeletonMatrix || useFalloutSkinToSkelMatrix()))
            transform = composeSkinToSkeletonTransform(
                *mSkinToSkelMatrix, mData->mTransform, mInverseSkinToSkeletonMatrix);
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
        std::size_t missingOptionalClothBones = 0;
        std::ostringstream missingOptionalClothBoneSample;
        const char* starfieldExternalSkinningEnv = std::getenv("OPENMW_WORLD_VIEWER_STARFIELD_EXTERNAL_SKINNING");
        const bool starfieldExternalSkinning
            = starfieldExternalSkinningEnv != nullptr && std::string_view(starfieldExternalSkinningEnv) != "0";
        for (const BoneInfo& info : mData->mBones)
        {
            mNodes.push_back(mSkeleton->getBone(info.mName));
            if (!mNodes.back())
            {
                ++missingBones;
                // Starfield authors secondary hair/clothing simulation against Cloth_* nodes
                // supplied by runtime cloth systems, not the retail face/body skeleton. Keep
                // those influences inert for now and report a bounded summary. Missing core
                // face/body bones remain errors because they indicate a broken actor composition.
                if (starfieldExternalSkinning
                    && Misc::StringUtils::ciFind(info.mName, "Cloth_") != std::string_view::npos)
                {
                    if (missingOptionalClothBones < 6)
                    {
                        if (missingOptionalClothBones != 0)
                            missingOptionalClothBoneSample << ',';
                        missingOptionalClothBoneSample << info.mName;
                    }
                    ++missingOptionalClothBones;
                }
                else
                    Log(Debug::Error) << "Error: RigGeometry did not find bone " << info.mName;
            }
        }

        if (missingOptionalClothBones != 0)
            Log(Debug::Verbose) << "World viewer: Starfield rig '" << getName() << "' omitted "
                                << missingOptionalClothBones
                                << " optional Cloth_* simulation bone(s); static fallback sample=["
                                << missingOptionalClothBoneSample.str() << ']';

        if (!mLoggedFalloutRigInit && isFalloutCharacterRig())
        {
            mLoggedFalloutRigInit = true;
            setCullingActive(false);
            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName() << "' initialized bones="
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
                Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                 << "' initialized skeleton during cull traversal after update miss";
            }
        }

        if (isFalloutCharacterRig() && isFalloutHiddenMorphRig(getName()))
        {
            setNodeMask(0);
            return;
        }

        unsigned int traversalNumber = nv->getTraversalNumber();
        mLastCullTraversalNumber.store(traversalNumber, std::memory_order_release);
        if (mLastFrameNumber == traversalNumber || (mLastFrameNumber != 0 && !mSkeleton->getActive()))
        {
            osg::Geometry& geom = *getGeometry(mLastFrameNumber);
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
            {
                const osg::Matrixf pathToWorld = osg::computeLocalToWorld(path);
                const osg::Vec3f translation = pathToWorld.getTrans();
                const osg::Vec3f scale = pathToWorld.getScale();
                Log(Debug::Info) << "FNV/ESM4 draw audit: rig=" << key
                                 << " pathWorldT=(" << translation.x() << "," << translation.y() << ","
                                 << translation.z() << ")"
                                 << " pathWorldScale=(" << scale.x() << "," << scale.y() << "," << scale.z()
                                 << ")";
            }
        }

        if (falloutRig && !mLoggedFalloutCullTraversal)
        {
            mLoggedFalloutCullTraversal = true;
            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
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

            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
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
            && (mSourceFrameSkinning || falloutSkinningMode == "source" || sourceSkinOnly
                || (falloutAutoMode && mFalloutUseSourceFallback));
        const bool falloutNearestBindFallback = falloutRig
            && std::getenv("OPENMW_WORLD_VIEWER_FO4_MISSING_BONE_NEAREST_BIND") != nullptr;

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

        if (falloutNearestBindFallback)
        {
            std::vector<osg::Vec3f> bindPositions(mData->mBones.size());
            std::vector<bool> validBindPositions(mData->mBones.size(), false);
            for (std::size_t i = 0; i < mData->mBones.size(); ++i)
            {
                osg::Matrixf bindMatrix;
                if (bindMatrix.invert(mData->mBones[i].mInvBindMatrix))
                {
                    bindPositions[i] = bindMatrix.getTrans();
                    validBindPositions[i] = true;
                }
            }

            for (std::size_t missing = 0; missing < mNodes.size(); ++missing)
            {
                if (mNodes[missing] != nullptr || !validBindPositions[missing])
                    continue;

                std::size_t nearest = mNodes.size();
                float nearestDistance2 = std::numeric_limits<float>::max();
                for (std::size_t candidate = 0; candidate < mNodes.size(); ++candidate)
                {
                    if (mNodes[candidate] == nullptr || !validBindPositions[candidate])
                        continue;
                    const float distance2 = (bindPositions[missing] - bindPositions[candidate]).length2();
                    if (distance2 < nearestDistance2)
                    {
                        nearest = candidate;
                        nearestDistance2 = distance2;
                    }
                }

                if (nearest != mNodes.size())
                {
                    boneMatrices[missing] = boneMatrices[nearest];
                    if (!mLoggedFalloutInfluenceSummary)
                        Log(Debug::Verbose) << "FNV/ESM4 diag: data-driven missing skin bone fallback "
                                            << mData->mBones[missing].mName << " -> "
                                            << mData->mBones[nearest].mName << " bindDistance="
                                            << std::sqrt(nearestDistance2);
                }
            }
        }

        if (falloutRig)
        {
            const char* auditFrameText = std::getenv("OPENMW_FNV_RETAIL_SKIN_PALETTE_AUDIT_FRAME");
            const unsigned int auditFrame
                = auditFrameText == nullptr ? std::numeric_limits<unsigned int>::max()
                                            : static_cast<unsigned int>(std::strtoul(auditFrameText, nullptr, 10));
            if (traversalNumber == auditFrame && Misc::StringUtils::ciEqual(getName(), "HeadOld"))
            {
                const auto matrixBits = [](const osg::Matrixf& matrix) {
                    std::ostringstream stream;
                    stream << '[' << std::uppercase << std::hex << std::setfill('0');
                    for (unsigned int row = 0; row < 4; ++row)
                    {
                        for (unsigned int column = 0; column < 4; ++column)
                        {
                            if (row != 0 || column != 0)
                                stream << ',';
                            stream << "0x" << std::setw(8)
                                   << std::bit_cast<std::uint32_t>(matrix(row, column));
                        }
                    }
                    stream << ']';
                    return stream.str();
                };

                Log(Debug::Info) << "FNV/ESM4 retail skin palette audit: frame=" << traversalNumber
                                 << " rig=" << getName() << " mode=" << falloutSkinningMode
                                 << " bones=" << boneMatrices.size()
                                 << " skinTransformBits=" << matrixBits(mData->mTransform)
                                 << " skinToSkeletonBits="
                                 << (mSkinToSkelMatrix == nullptr ? std::string("none")
                                                                  : matrixBits(*mSkinToSkelMatrix));
                for (std::size_t i = 0; i < boneMatrices.size(); ++i)
                {
                    Log(Debug::Info) << "FNV/ESM4 retail skin palette bone: frame=" << traversalNumber
                                     << " rig=" << getName() << " index=" << i
                                     << " name=\"" << mData->mBones[i].mName << "\""
                                     << " paletteBits=" << matrixBits(boneMatrices[i])
                                     << " invBindBits=" << matrixBits(mData->mBones[i].mInvBindMatrix)
                                     << " skeletonBits="
                                     << (mNodes[i] == nullptr ? std::string("missing")
                                                              : matrixBits(mNodes[i]->mMatrixInSkeletonSpace));
                }
            }
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
                        Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
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
        else if (mSkinToSkelMatrix
            && (mInverseSkinToSkeletonMatrix || !falloutRig || useFalloutSkinToSkelMatrix()))
            transform = composeSkinToSkeletonTransform(
                *mSkinToSkelMatrix, mData->mTransform, mInverseSkinToSkeletonMatrix);
        else
            transform = mData->mTransform;

        if (falloutFlagRig && !mLoggedFalloutFlagSkinning)
        {
            mLoggedFalloutFlagSkinning = true;
            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout flag RigGeometry '" << getName()
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

            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
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
                             << " sourceFrame=" << mSourceFrameSkinning
                             << " hasSkinToSkel=" << static_cast<bool>(mSkinToSkelMatrix)
                             << " useSkinToSkel=" << useFalloutSkinToSkelMatrix()
                             << " inverseSkinToSkel=" << mInverseSkinToSkeletonMatrix;
        }

        float maxFalloutVertexDelta = 0.f;
        unsigned short maxFalloutVertex = 0;
        std::size_t invalidBoneInfluences = 0;
        std::size_t invalidVertexInfluences = 0;

        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            osg::Matrixf resultMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);

            for (const auto& [index, weight] : influences)
            {
                if (index >= mNodes.size() || index >= boneMatrices.size())
                {
                    ++invalidBoneInfluences;
                    continue;
                }
                if (mNodes[index] == nullptr && !falloutNearestBindFallback)
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
                if (positionSrc == nullptr || positionDst == nullptr || vertex >= positionSrc->size()
                    || vertex >= positionDst->size())
                {
                    ++invalidVertexInfluences;
                    continue;
                }
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
                if (normalSrc != nullptr && normalDst != nullptr && vertex < normalSrc->size()
                    && vertex < normalDst->size())
                    (*normalDst)[vertex] = osg::Matrixf::transform3x3((*normalSrc)[vertex], resultMat);

                if (tangentSrc != nullptr && tangentDst != nullptr && vertex < tangentSrc->size()
                    && vertex < tangentDst->size())
                {
                    const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                    osg::Vec3f transformedTangent = osg::Matrixf::transform3x3(
                        osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()), resultMat);
                    (*tangentDst)[vertex] = osg::Vec4f(transformedTangent, srcTangent.w());
                }
            }
        }

        if (!mLoggedInvalidSkinningData && (invalidBoneInfluences != 0 || invalidVertexInfluences != 0))
        {
            mLoggedInvalidSkinningData = true;
            Log(Debug::Warning) << "RigGeometry '" << getName() << "' ignored invalid skinning data bones="
                                << invalidBoneInfluences << " vertices=" << invalidVertexInfluences
                                << " sourceVertices=" << (positionSrc != nullptr ? positionSrc->size() : 0)
                                << " destinationVertices=" << (positionDst != nullptr ? positionDst->size() : 0);
        }

        if (falloutRig && !mLoggedFalloutVertexSkinning && maxFalloutVertexDelta > 0.001f)
        {
            mLoggedFalloutVertexSkinning = true;
            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                             << "' skinned vertices this frame maxVertexSkinDelta=" << maxFalloutVertexDelta
                             << " vertex=" << maxFalloutVertex;
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
            const float diagonalExpansion
                = sourceDiag > 0.001f ? skinnedDiag / sourceDiag : (skinnedDiag > 0.001f ? std::numeric_limits<float>::infinity() : 1.f);
            const float centerDelta = (skinnedCenter - sourceCenter).length();
            const float extentRatio = maxFiniteExtentRatio(skinnedExtent, sourceExtent);
            const float sourceCenterRadius = sourceCenter.length();
            // Isolated actor-space pieces such as hands and Pip-Boy switches are small meshes located far from the
            // actor origin. A legitimate articulated pose can move their center several mesh diagonals while still
            // remaining within the actor's radial envelope.
            const float outlierRadius
                = std::max({ 64.f, sourceDiag * 1.75f, sourceCenterRadius * 1.35f });
            std::size_t outlierVertices = 0;
            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                if (((*positionDst)[i] - sourceCenter).length() > outlierRadius)
                    ++outlierVertices;
            }

            const float centerTravelLimit
                = std::max({ 24.f, sourceDiag * 1.25f, sourceCenterRadius * 0.9f });
            const bool badCenter = centerDelta > centerTravelLimit;
            // A normal animation can rotate a long, thin bind-pose limb from X into Z, producing a very large
            // component-wise extent ratio even though the complete mesh becomes smaller. Only treat that as a
            // blow-up when the overall bounding diagonal expands materially as well.
            const bool badExtent = extentRatio > 3.5f && diagonalExpansion > 1.5f;
            const float vertexTravelLimit
                = std::max({ 96.f, sourceDiag * 2.0f, sourceCenterRadius * 1.5f });
            const bool badDelta = maxVertexDelta > vertexTravelLimit;
            const bool badOutliers = vertexCount > 0 && outlierVertices > std::max<std::size_t>(24, vertexCount / 5);
            const bool bad = badCenter || badExtent || badDelta || badOutliers;
            const char* reason = badCenter   ? "center"
                : badExtent                  ? "extent"
                : badDelta                   ? "vertex"
                : badOutliers                ? "outliers"
                                             : "ok";

            if (falloutAutoMode && !mFalloutFallbackDecided)
            {
                mFalloutFallbackDecided = true;
                if (bad)
                {
                    mFalloutUseSourceFallback = true;
                    copySourceSkinningGeometry(
                        positionSrc, normalSrc, tangentSrc, positionDst, normalDst, tangentDst);
                    Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
                                     << "' auto skinning fallback=source reason=" << reason;
                }
            }

            Log(Debug::Verbose) << "FNV/ESM4 diag: Fallout RigGeometry '" << getName()
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
                             << " diagonalExpansion=" << diagonalExpansion
                             << " sourceCenterRadius=" << sourceCenterRadius
                             << " centerTravelLimit=" << centerTravelLimit
                             << " vertexTravelLimit=" << vertexTravelLimit
                             << " maxVertexDelta=" << maxVertexDelta << " maxVertex=" << maxVertexDeltaIndex
                             << " outlierVertices=" << outlierVertices << " outlierRadius=" << outlierRadius
                             << " verdict=" << (bad ? "BAD" : "OK") << " reason=" << reason;
        }

        positionDst->dirty();
        if (normalDst)
            normalDst->dirty();
        if (tangentDst)
            tangentDst->dirty();

        geom.osg::Drawable::dirtyGLObjects();

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
            && (mSourceFrameSkinning || falloutSkinningMode == "source" || sourceSkinOnly
                || (falloutSkinningMode == "auto" && mFalloutUseSourceFallback));
        if (falloutFlagRig)
            transform.makeIdentity();
        else if (falloutSourceSkinning)
            transform.makeIdentity();
        else if (mSkinToSkelMatrix
            && (mInverseSkinToSkeletonMatrix || !falloutRig || useFalloutSkinToSkelMatrix()))
            transform = composeSkinToSkeletonTransform(
                *mSkinToSkelMatrix, mData->mTransform, mInverseSkinToSkeletonMatrix);
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
