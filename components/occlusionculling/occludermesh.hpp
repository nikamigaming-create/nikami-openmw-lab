#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUDERMESH_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUDERMESH_H

#include <osg/BoundingBox>
#include <osg/Vec3f>

#include <vector>

namespace osg
{
    class Node;
}

namespace OcclusionCulling
{
    /// Simplified occluder mesh used for CPU-side software occlusion culling.
    /// Stored in local (model) space; callers apply a world transform before rasterizing.
    struct OccluderMesh
    {
        osg::BoundingBox aabb;
        std::vector<osg::Vec3f> vertices; // shrunk toward centroid for conservative occlusion
        std::vector<unsigned int> indices; // triangle indices
    };

    /// Build a simplified occluder mesh from an OSG node's geometry.
    /// Collects triangles from the node, applies vertex clustering on a coarse 3D grid,
    /// and shrinks the result toward the centroid for conservative occlusion.
    /// Alpha-tested geometry (e.g. foliage billboards) is excluded.
    /// @param node        Source node to collect geometry from
    /// @param gridRes     Grid resolution for vertex clustering (higher = more detail)
    /// @param shrinkFactor How much to shrink toward centroid (0..1, 1 = no shrink)
    /// @return Simplified mesh with AABB, or empty mesh if no geometry found
    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor);
}

#endif
