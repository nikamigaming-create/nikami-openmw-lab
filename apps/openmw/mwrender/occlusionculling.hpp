#ifndef OPENMW_MWRENDER_OCCLUSIONCULLING_H
#define OPENMW_MWRENDER_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Camera>
#include <osg/Image>
#include <osg/Object>
#include <osg/Texture2D>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <unordered_map>
#include <vector>

#include <components/occlusionculling/occludermesh.hpp>
#include <components/occlusionculling/occlusionstorage.hpp>
#include <components/sceneutil/nodecallback.hpp>

namespace MWRender
{
    // Bring component types into MWRender namespace for backward compatibility.
    using OccluderMesh = OcclusionCulling::OccluderMesh;
    using OcclusionStorage = OcclusionCulling::OcclusionStorage;
    using OcclusionCulling::buildSimplifiedMesh;

    /// Stored on paged chunk nodes (UserDataContainer) to provide sub-object
    /// occluder meshes for MOC rasterization during cull traversal.
    class PagedOccluderData : public osg::Object
    {
    public:
        PagedOccluderData() = default;
        PagedOccluderData(const PagedOccluderData& copy, const osg::CopyOp& = {})
            : osg::Object(copy)
            , mOccluderMeshes(copy.mOccluderMeshes)
        {
        }
        META_Object(MWRender, PagedOccluderData)

        std::vector<OccluderMesh> mOccluderMeshes;
    };
}

namespace osgUtil
{
    class CullVisitor;
}

namespace osg
{
    class Group;
    class Node;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace Terrain
{
    class TerrainOccluder;
}

namespace MWRender
{
    /// Installed on the SceneRoot (LightManager). At the start of each main-camera cull,
    /// rasterizes terrain into the software occlusion buffer. Skips RTT cameras (shadows,
    /// water reflection) and interiors (no terrain data).
    class SceneOcclusionCallback
        : public SceneUtil::NodeCallback<SceneOcclusionCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        SceneOcclusionCallback(SceneUtil::OcclusionCuller* culler, Terrain::TerrainOccluder* occluder, int radiusCells,
            bool enableTerrainOccluder, bool enableDebugOverlay, bool enableDebugMessages, bool enableInteriors,
            OcclusionStorage* storage = nullptr);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

        /// Update cell type flags. Call when the player transitions cells.
        void setCellType(bool isInterior, bool isQuasiExterior);

        void setStorage(OcclusionStorage* storage) { mStorage = storage; }

    private:
        void setupDebugOverlay();
        void updateDebugOverlay(osgUtil::CullVisitor* cv);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        Terrain::TerrainOccluder* mTerrainOccluder;
        int mRadiusCells;
        bool mEnableTerrainOccluder;
        bool mEnableDebugOverlay;
        bool mEnableDebugMessages;
        bool mEnableInteriors;
        bool mIsInterior = false;
        bool mIsQuasiExterior = false;
        unsigned int mLastFrameNumber = 0;
        OcclusionStorage* mStorage = nullptr;

        // Scratch buffers reused across frames
        std::vector<osg::Vec3f> mPositions;
        std::vector<unsigned int> mIndices;

        // Debug overlay
        osg::ref_ptr<osg::Camera> mDebugCamera;
        osg::ref_ptr<osg::Image> mDebugImage;
        osg::ref_ptr<osg::Texture2D> mDebugTexture;
        std::vector<float> mDepthPixels;
    };

    /// Installed on paged chunk nodes (from ObjectPaging). Rasterizes the chunk's
    /// pre-built occluder meshes into MOC during the terrain traversal, before cell
    /// objects are tested.
    class PagedOccluderCallback
        : public SceneUtil::NodeCallback<PagedOccluderCallback, osg::Node*, osgUtil::CullVisitor*>
    {
    public:
        PagedOccluderCallback(SceneUtil::OcclusionCuller* culler, float maxDistance, unsigned int maxTriangles);

        void operator()(osg::Node* node, osgUtil::CullVisitor* cv);

    private:
        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mMaxDistanceSq;
        unsigned int mMaxTriangles;
    };

    /// Installed on each Cell Root group. Two-pass approach:
    /// Pass 1: Large objects (radius >= threshold) are tested against terrain depth, and if
    ///         visible, their shrunken AABB is rasterized as an occluder, then traversed.
    /// Pass 2: Small objects are tested against the enriched depth buffer (terrain + buildings).
    class CellOcclusionCallback
        : public SceneUtil::NodeCallback<CellOcclusionCallback, osg::Group*, osgUtil::CullVisitor*>
    {
    public:
        CellOcclusionCallback(SceneUtil::OcclusionCuller* culler, float occluderMinRadius, float occluderMaxRadius,
            float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            unsigned int maxTriangles, OcclusionStorage* storage = nullptr);

        void operator()(osg::Group* node, osgUtil::CullVisitor* cv);

    private:
        /// Get cached occluder mesh (actual triangles + AABB) for a node.
        const OccluderMesh& getOccluderMesh(osg::Node* node);

        osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        float mOccluderMinRadius;
        float mOccluderMaxRadius;
        float mOccluderShrinkFactor;
        int mOccluderMeshResolution;
        int mOccluderMaxMeshResolution;
        float mOccluderInsideThreshold;
        float mOccluderMaxDistanceSq;
        bool mEnableStaticOccluders;
        unsigned int mMaxTriangles;

        std::unordered_map<osg::Node*, OccluderMesh> mMeshCache;
        OcclusionStorage* mStorage = nullptr;
    };
}

#endif
