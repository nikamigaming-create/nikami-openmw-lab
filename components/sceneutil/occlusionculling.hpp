#ifndef OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OCCLUSIONCULLING_H

#include <osg/BoundingBox>
#include <osg/Matrixd>
#include <osg/Referenced>
#include <osg/Vec3f>

#include <vector>

class MaskedOcclusionCulling;

namespace SceneUtil
{
    /// Wraps Intel's Masked Software Occlusion Culling library.
    /// Provides a CPU-based hierarchical depth buffer for occlusion testing during cull traversal.
    class OcclusionCuller : public osg::Referenced
    {
    public:
        OcclusionCuller(unsigned int bufferWidth, unsigned int bufferHeight);
        ~OcclusionCuller();

        /// Call at the start of each frame's cull traversal.
        /// Clears the depth buffer and stores the view-projection matrix.
        void beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix);

        /// Rasterize terrain into the full buffer AND the terrain-only snapshot.
        /// Call this only for terrain, before any building occluders are added.
        void rasterizeTerrainOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize world-space triangles as occluders into the full buffer only (not the terrain snapshot).
        /// Use for buildings.
        void rasterizeOccluder(const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices);

        /// Rasterize a world-space AABB as an occluder (12 triangles for 6 faces).
        void rasterizeAABBOccluder(const osg::BoundingBox& worldBB);

        /// Test against the terrain-only depth buffer.
        /// Use for cells and buildings — prevents buildings from false-occluding each other.
        bool testVisibleAABBTerrainOnly(const osg::BoundingBox& worldBB) const;

        /// Test against the full depth buffer (terrain + buildings).
        /// Use for small objects in Pass 2.
        bool testVisibleAABB(const osg::BoundingBox& worldBB) const;

        bool isActive() const { return mMOC != nullptr; }
        bool isFrameActive() const { return mFrameActive; }
        void endFrame() { mFrameActive = false; }

        unsigned int getNumOccluded() const { return mNumOccluded; }
        unsigned int getNumTested() const { return mNumTested; }
        unsigned int getNumBuildingOccluders() const { return mNumBuildingOccluders; }
        unsigned int getNumBuildingTris() const { return mNumBuildingTris; }
        unsigned int getNumBuildingVerts() const { return mNumBuildingVerts; }
        void incrementBuildingOccluders(unsigned int tris, unsigned int verts)
        {
            ++mNumBuildingOccluders;
            mNumBuildingTris += tris;
            mNumBuildingVerts += verts;
        }

        /// Write the per-pixel depth buffer to depthData (width*height floats, bottom-to-top).
        void computePixelDepthBuffer(float* depthData) const;

        void getResolution(unsigned int& width, unsigned int& height) const;

    private:
        bool testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const;

        MaskedOcclusionCulling* mMOC;
        MaskedOcclusionCulling* mMOCTerrainOnly; // terrain-only snapshot for building visibility tests
        osg::Matrixd mViewProjection;
        float mVPFloat[16] = {};
        bool mFrameActive = false;

        mutable unsigned int mNumOccluded = 0;
        mutable unsigned int mNumTested = 0;
        unsigned int mNumBuildingOccluders = 0;
        unsigned int mNumBuildingTris = 0;
        unsigned int mNumBuildingVerts = 0;
    };
}

#endif
