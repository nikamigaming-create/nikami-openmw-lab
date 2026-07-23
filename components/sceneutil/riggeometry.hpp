#ifndef OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H
#define OPENMW_COMPONENTS_NIFOSG_RIGGEOMETRY_H

#include <osg/Geometry>
#include <osg/Matrixf>
#include <osg/NodeVisitor>

#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace SceneUtil
{
    class Skeleton;
    class Bone;

    // TODO: This class has a lot of issues.
    // - We require too many workarounds to ensure safety.
    // - mSourceGeometry should be const, but can not be const because of a use case in shadervisitor.cpp.
    // - We create useless mGeometry clones in template RigGeometries.
    // - We do not support compileGLObjects.
    // - We duplicate some code in MorphGeometry.

    /// @brief Mesh skinning implementation.
    /// @note A RigGeometry may be attached directly to a Skeleton, or somewhere below a Skeleton.
    /// Note though that the RigGeometry ignores any transforms below the Skeleton, so the attachment point is not that
    /// important.
    /// @note The internal Geometry used for rendering is double buffered, this allows updates to be done in a thread
    /// safe way while not compromising rendering performance. This is crucial when using osg's default threading model
    /// of DrawThreadPerContext.
    class RigGeometry : public osg::Drawable
    {
    public:
        RigGeometry();
        RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop);

        META_Object(SceneUtil, RigGeometry)

        // Currently empty as this is difficult to implement. Technically we would need to compile both internal
        // geometries in separate frames but this method is only called once. Alternatively we could compile just the
        // static parts of the model.
        void compileGLObjects(osg::RenderInfo& renderInfo) const override {}

        struct BoneInfo
        {
            std::string mName;
            osg::BoundingSpheref mBoundSphere;
            osg::Matrixf mInvBindMatrix;
        };

        using VertexWeight = std::pair<unsigned short, float>;
        using VertexWeights = std::vector<VertexWeight>;
        using BoneWeight = std::pair<size_t, float>;
        using BoneWeights = std::vector<BoneWeight>;

        void setBoneInfo(std::vector<BoneInfo>&& bones);
        // Convert influences in vertex and weight list per bone format
        void setInfluences(const std::vector<VertexWeights>& influences);
        // Convert influences in bone and weight list per vertex format
        void setInfluences(const std::vector<BoneWeights>& influences);

        /// Initialize this geometry from the source geometry.
        /// @note The source geometry will not be modified.
        void setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeom);

        void setTransform(osg::Matrixf&& transform);

        void setRootBone(std::string_view name);

        std::string_view getRootBone() const;
        std::size_t getBoneCount() const;
        std::string_view getBoneName(std::size_t index) const;
        bool getSkinningDebugData(std::vector<BoneInfo>& bones, std::vector<BoneWeights>& vertexInfluences,
            std::vector<osg::Matrixf>& localBoneMatrices, std::vector<osg::Matrixf>& skeletonBoneMatrices,
            osg::Matrixf& transform, osg::Matrixf& skinToSkelMatrix) const;
        bool isFalloutCharacterRig() const;
        void setFalloutFlagSkinning(bool enabled);
        void setFalloutCharacterSkinning(bool enabled);
        void setSourceFrameSkinning(bool enabled);
        void setInverseSkinToSkeletonMatrix(bool enabled);
        bool getFalloutFingerVertexWeights(
            std::vector<float>& thumb, std::vector<float>& index, std::vector<float>& grip) const;
        bool getFalloutFingerBoneVertexWeights(std::array<std::vector<float>, 15>& fingerBones) const;

        /// Apply the retail-style VATS wireframe to this skinned body mesh. Vertex colors are derived from the
        /// authored skin weights, so rigid attachments under target bones are deliberately unaffected.
        bool setFalloutVatsHighlight(
            std::span<const std::string_view> targetBones, std::string_view selectedBone, bool enabled);

        osg::ref_ptr<osg::Geometry> getSourceGeometry() const;
        osg::Geometry* getRenderGeometry(unsigned int index) const;
        osg::Geometry* getLastFrameGeometry() const;
        unsigned int getLastCullTraversalNumber() const
        {
            return mLastCullTraversalNumber.load(std::memory_order_acquire);
        }
        bool hasResolvedParentSkeleton() const { return mSkeleton != nullptr; }
        bool computeCurrentFalloutSkinningBounds(osg::NodeVisitor* nv, osg::BoundingBox& box);
        void forceNextUpdate();
        bool refreshFalloutSkinningForCurrentPose();

        void accept(osg::NodeVisitor& nv) override;
        bool supports(const osg::PrimitiveFunctor&) const override { return true; }
        void accept(osg::PrimitiveFunctor&) const override;

        struct CopyBoundingBoxCallback : osg::Drawable::ComputeBoundingBoxCallback
        {
            osg::BoundingBox boundingBox;

            osg::BoundingBox computeBound(const osg::Drawable&) const override { return boundingBox; }
        };

        struct CopyBoundingSphereCallback : osg::Node::ComputeBoundingSphereCallback
        {
            osg::BoundingSphere boundingSphere;

            osg::BoundingSphere computeBound(const osg::Node&) const override { return boundingSphere; }
        };

    private:
        void cull(osg::NodeVisitor* nv);
        void updateBounds(osg::NodeVisitor* nv);

        osg::ref_ptr<osg::Geometry> mGeometry[2];
        osg::Geometry* getGeometry(unsigned int frame) const;

        osg::ref_ptr<osg::Geometry> mSourceGeometry;
        osg::ref_ptr<const osg::Vec4Array> mSourceTangents;
        Skeleton* mSkeleton{ nullptr };

        osg::ref_ptr<osg::RefMatrix> mSkinToSkelMatrix;

        using VertexList = std::vector<unsigned short>;
        struct InfluenceData : public osg::Referenced
        {
            std::vector<BoneInfo> mBones;
            std::vector<std::pair<BoneWeights, VertexList>> mInfluences;
            osg::Matrixf mTransform;
            std::string mRootBone;
        };
        osg::ref_ptr<InfluenceData> mData;
        std::vector<Bone*> mNodes;

        unsigned int mLastFrameNumber{ 0 };
        // mLastFrameNumber is also reset by forceNextUpdate() so the double-buffered
        // geometry is refreshed after an animation/IK change.  Keep actual cull
        // evidence separate; otherwise a proof gate sampled during the following
        // update traversal observes zero even though this drawable was rendered.
        std::atomic_uint mLastCullTraversalNumber{ 0 };
        bool mBoundsFirstFrame{ true };
        bool mLoggedFalloutRigInit{ false };
        bool mLoggedFalloutCullTraversal{ false };
        bool mHaveFalloutMatrixBaseline{ false };
        bool mLoggedFalloutMatrixChange{ false };
        bool mLoggedFalloutVertexSkinning{ false };
        bool mLoggedFalloutInfluenceSummary{ false };
        bool mLoggedFalloutSkinningModes{ false };
        bool mLoggedFalloutPoseSanity{ false };
        bool mLoggedFalloutCullInitRecovery{ false };
        bool mFalloutDerivedInvBindComputed{ false };
        bool mLoggedFalloutDerivedInvBind{ false };
        bool mFalloutFallbackDecided{ false };
        bool mFalloutUseSourceFallback{ false };
        bool mFalloutFlagSkinning{ false };
        bool mFalloutCharacterSkinning{ false };
        bool mSourceFrameSkinning{ false };
        bool mInverseSkinToSkeletonMatrix{ false };
        bool mLoggedFalloutFlagSkinning{ false };
        bool mLoggedInvalidSkinningData{ false };
        mutable bool mFalloutCharacterRigComputed{ false };
        mutable bool mFalloutCharacterRig{ false };
        std::vector<osg::Matrixf> mFalloutMatrixBaseline;
        std::vector<osg::Matrixf> mFalloutDerivedInvBindMatrices;
        std::array<osg::ref_ptr<osg::Array>, 2> mFalloutVatsOriginalColorArrays;
        osg::ref_ptr<osg::StateSet> mFalloutVatsOriginalStateSet;
        bool mFalloutVatsHighlightActive{ false };

        bool initFromParentSkeleton(osg::NodeVisitor* nv);

        void updateSkinToSkelMatrix(const osg::NodePath& nodePath);
    };

}

#endif
