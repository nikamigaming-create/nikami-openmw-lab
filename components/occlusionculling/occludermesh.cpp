#include "occludermesh.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <osg/AlphaFunc>
#include <osg/BoundingBox>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Transform>

namespace
{
    class CollectMeshVisitor : public osg::NodeVisitor
    {
    public:
        CollectMeshVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Transform& transform) override
        {
            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();
            transform.computeLocalToWorldMatrix(matrix, this);
            mMatrixStack.push_back(matrix);
            traverse(transform);
            mMatrixStack.pop_back();
        }

        void apply(osg::Drawable& drawable) override
        {
            auto* geom = drawable.asGeometry();
            if (!geom)
                return;

            // Skip alpha-tested drawables (e.g. tree leaf billboards) —
            // their transparent regions would cause false occlusion.
            if (hasAlphaTest(drawable))
                return;

            const auto* verts = dynamic_cast<const osg::Vec3Array*>(geom->getVertexArray());
            if (!verts || verts->empty())
                return;

            osg::Matrix matrix;
            if (!mMatrixStack.empty())
                matrix = mMatrixStack.back();

            unsigned int baseVertex = static_cast<unsigned int>(mVertices.size());
            for (const auto& v : *verts)
                mVertices.push_back(v * matrix);

            for (unsigned int p = 0; p < geom->getNumPrimitiveSets(); ++p)
                collectTriangles(geom->getPrimitiveSet(p), baseVertex);
        }

        std::vector<osg::Vec3f> mVertices;
        std::vector<unsigned int> mIndices;

    private:
        static bool hasAlphaTest(const osg::Node& node)
        {
            if (const auto* ss = node.getStateSet())
                if (ss->getAttribute(osg::StateAttribute::ALPHAFUNC))
                    return true;
            for (const auto* parent : node.getParents())
                if (const auto* ss = parent->getStateSet())
                    if (ss->getAttribute(osg::StateAttribute::ALPHAFUNC))
                        return true;
            return false;
        }

        void collectTriangles(const osg::PrimitiveSet* pset, unsigned int baseVertex)
        {
            unsigned int count = pset->getNumIndices();
            switch (pset->getMode())
            {
                case GL_TRIANGLES:
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                    {
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                        mIndices.push_back(baseVertex + pset->index(i + 2));
                    }
                    break;
                case GL_TRIANGLE_STRIP:
                    for (unsigned int i = 0; i + 2 < count; ++i)
                    {
                        if (i % 2 == 0)
                        {
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                        else
                        {
                            mIndices.push_back(baseVertex + pset->index(i + 1));
                            mIndices.push_back(baseVertex + pset->index(i));
                            mIndices.push_back(baseVertex + pset->index(i + 2));
                        }
                    }
                    break;
                case GL_TRIANGLE_FAN:
                    for (unsigned int i = 1; i + 1 < count; ++i)
                    {
                        mIndices.push_back(baseVertex + pset->index(0));
                        mIndices.push_back(baseVertex + pset->index(i));
                        mIndices.push_back(baseVertex + pset->index(i + 1));
                    }
                    break;
                default:
                    break;
            }
        }

        std::vector<osg::Matrix> mMatrixStack;
    };
}

namespace OcclusionCulling
{
    OccluderMesh buildSimplifiedMesh(osg::Node* node, int gridRes, float shrinkFactor)
    {
        OccluderMesh mesh;

        CollectMeshVisitor cmv;
        node->accept(cmv);

        if (cmv.mIndices.empty() || cmv.mVertices.size() < 3)
        {
            osg::ComputeBoundsVisitor cbv;
            node->accept(cbv);
            mesh.aabb = cbv.getBoundingBox();
            return mesh;
        }

        for (const auto& v : cmv.mVertices)
            mesh.aabb.expandBy(v);
        const unsigned int res = static_cast<unsigned int>(gridRes);
        float dx = mesh.aabb.xMax() - mesh.aabb.xMin();
        float dy = mesh.aabb.yMax() - mesh.aabb.yMin();
        float dz = mesh.aabb.zMax() - mesh.aabb.zMin();
        float maxDim = std::max({ dx, dy, dz });
        float cellSize = maxDim / res;

        if (cellSize > 0)
        {
            unsigned int resX = std::max(1u, static_cast<unsigned int>(std::ceil(dx / cellSize)));
            unsigned int resY = std::max(1u, static_cast<unsigned int>(std::ceil(dy / cellSize)));

            struct CellData
            {
                osg::Vec3f sum;
                unsigned int count = 0;
                unsigned int newIndex = 0;
            };
            std::unordered_map<unsigned int, CellData> cells;
            std::vector<unsigned int> vertexRemap(cmv.mVertices.size());

            for (size_t i = 0; i < cmv.mVertices.size(); ++i)
            {
                const osg::Vec3f& v = cmv.mVertices[i];
                float fx = (v.x() - mesh.aabb.xMin()) / cellSize;
                float fy = (v.y() - mesh.aabb.yMin()) / cellSize;
                float fz = (v.z() - mesh.aabb.zMin()) / cellSize;
                unsigned int gx = std::min(static_cast<unsigned int>(std::max(fx, 0.0f)), resX - 1);
                unsigned int gy = std::min(static_cast<unsigned int>(std::max(fy, 0.0f)), resY - 1);
                unsigned int gz = std::min(static_cast<unsigned int>(std::max(fz, 0.0f)), res - 1);
                unsigned int cellId = gx + gy * resX + gz * resX * resY;

                auto& cell = cells[cellId];
                cell.sum += v;
                cell.count++;
                vertexRemap[i] = cellId;
            }

            unsigned int nextIdx = 0;
            for (auto& [id, cell] : cells)
            {
                cell.newIndex = nextIdx++;
                mesh.vertices.push_back(cell.sum / static_cast<float>(cell.count));
            }

            std::unordered_set<uint64_t> seen;
            for (size_t i = 0; i + 2 < cmv.mIndices.size(); i += 3)
            {
                unsigned int a = cells[vertexRemap[cmv.mIndices[i]]].newIndex;
                unsigned int b = cells[vertexRemap[cmv.mIndices[i + 1]]].newIndex;
                unsigned int c = cells[vertexRemap[cmv.mIndices[i + 2]]].newIndex;

                if (a == b || b == c || a == c)
                    continue;

                unsigned int tri[3] = { a, b, c };
                if (tri[1] < tri[0] && tri[1] < tri[2])
                    std::rotate(tri, tri + 1, tri + 3);
                else if (tri[2] < tri[0] && tri[2] < tri[1])
                    std::rotate(tri, tri + 2, tri + 3);
                if (tri[1] > tri[2])
                    std::swap(tri[1], tri[2]);

                uint64_t key = (uint64_t(tri[0]) << 42) | (uint64_t(tri[1]) << 21) | uint64_t(tri[2]);
                if (seen.insert(key).second)
                {
                    mesh.indices.push_back(a);
                    mesh.indices.push_back(b);
                    mesh.indices.push_back(c);
                }
            }
        }

        if (!mesh.vertices.empty())
        {
            osg::Vec3f center(0, 0, 0);
            for (const auto& v : mesh.vertices)
                center += v;
            center /= static_cast<float>(mesh.vertices.size());
            for (auto& v : mesh.vertices)
                v = center + (v - center) * shrinkFactor;
        }

        return mesh;
    }
}
