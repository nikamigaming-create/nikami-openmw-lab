#ifndef OPENMW_COMPONENTS_RESOURCE_BULLETSHAPE_H
#define OPENMW_COMPONENTS_RESOURCE_BULLETSHAPE_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <osg/Object>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btScaledBvhTriangleMeshShape.h>

#include <components/vfs/pathutil.hpp>

class btCollisionShape;

namespace NifBullet
{
    class BulletNifLoader;
}

namespace Resource
{
    struct DeleteCollisionShape
    {
        void operator()(btCollisionShape* shape) const;
    };

    using CollisionShapePtr = std::unique_ptr<btCollisionShape, DeleteCollisionShape>;

    struct CollisionBox
    {
        osg::Vec3f mExtents;
        osg::Vec3f mCenter;
    };

    enum class VisualCollisionType
    {
        None,
        Default,
        Camera
    };

    struct BethesdaHavokFilter
    {
        std::uint8_t mLayer = 0;
        std::uint8_t mFlags = 0;
        std::uint16_t mGroup = 0;

        auto operator<=>(const BethesdaHavokFilter&) const = default;
    };

    struct BethesdaCollisionFilter
    {
        BethesdaHavokFilter mWorldObjectFilter;
        BethesdaHavokFilter mRigidBodyFilter;
        std::vector<BethesdaHavokFilter> mSubshapeFilters;

        bool operator==(const BethesdaCollisionFilter&) const = default;
    };

    class CollisionShapeMaterialTable
    {
    public:
        bool addUniformMaterial(std::uint32_t material);
        bool addShapePartMaterial(int shapePart, std::uint32_t material);
        bool addTriangleMaterial(int shapePart, int triangleIndex, std::uint32_t material);
        bool addCompoundChildMaterial(int childIndex, std::uint32_t material);

        std::optional<std::uint32_t> getMaterial(int shapePart, int triangleIndex) const;

        bool empty() const { return mEntries.empty(); }
        std::size_t size() const { return mEntries.size(); }

        bool operator==(const CollisionShapeMaterialTable&) const = default;

    private:
        static constexpr int sWildcard = -1;

        bool addMaterial(int shapePart, int triangleIndex, std::uint32_t material);
        std::optional<std::uint32_t> findMaterial(int shapePart, int triangleIndex) const;

        std::map<std::pair<int, int>, std::uint32_t> mEntries;
    };

    struct CollisionBody
    {
        CollisionShapePtr mCollisionShape;
        CollisionShapeMaterialTable mCollisionShapeMaterials;
        std::optional<BethesdaCollisionFilter> mBethesdaCollisionFilter;

        CollisionBody() = default;
        CollisionBody(CollisionShapePtr shape, CollisionShapeMaterialTable materials,
            std::optional<BethesdaCollisionFilter> filter);
        CollisionBody(CollisionBody&&) = default;
        CollisionBody& operator=(CollisionBody&&) = default;
    };

    struct BulletShape : public osg::Object, CollisionBody
    {
        CollisionShapePtr mAvoidCollisionShape;

        // Used for actors and projectiles. mCollisionShape is used for actors only when we need to autogenerate
        // collision box for creatures. For now, use one file <-> one resource for simplicity.
        CollisionBox mCollisionBox;

        // Stores animated collision shapes.
        // mCollisionShape is a btCompoundShape (which consists of one or more child shapes).
        // In this map, for each animated collision shape,
        // we store the node's record index mapped to the child index of the shape in the btCompoundShape.
        std::map<int, int> mAnimatedShapes;

        VFS::Path::Normalized mFileName;
        std::string mFileHash;

        // Bodies after the compatibility primary body inherited above, in deterministic active-tree traversal order.
        // The current loader does not populate this collection until runtime multi-body ownership is available.
        std::vector<CollisionBody> mAdditionalCollisionBodies;

        VisualCollisionType mVisualCollisionType = VisualCollisionType::None;

        BulletShape() = default;
        // Note this is always a shallow copy and the copy will not autodelete underlying vertex data
        BulletShape(const BulletShape& other, const osg::CopyOp& copyOp = osg::CopyOp());

        META_Object(Resource, BulletShape)

        void setLocalScaling(const btVector3& scale);
        bool getCollisionAabb(const btTransform& transform, btVector3& min, btVector3& max) const;

        std::optional<std::uint32_t> getHavokMaterial(int shapePart, int triangleIndex) const;
        std::optional<std::uint32_t> getHavokMaterial(std::size_t bodyIndex, int shapePart, int triangleIndex) const;

        std::size_t getCollisionBodyCount() const;
        CollisionBody* getCollisionBody(std::size_t index);
        const CollisionBody* getCollisionBody(std::size_t index) const;

        bool isAnimated() const { return !mAnimatedShapes.empty(); }
    };

    // An instance of a BulletShape that may have its own unique scaling set on collision shapes.
    // Vertex data is shallow-copied where possible. A ref_ptr to the original shape is held to keep vertex pointers
    // intact.
    class BulletShapeInstance : public BulletShape
    {
    public:
        explicit BulletShapeInstance(osg::ref_ptr<const BulletShape> source);

        const osg::ref_ptr<const BulletShape>& getSource() const { return mSource; }

    private:
        osg::ref_ptr<const BulletShape> mSource;
    };

    osg::ref_ptr<BulletShapeInstance> makeInstance(osg::ref_ptr<const BulletShape> source);

    // Subclass btBhvTriangleMeshShape to auto-delete the meshInterface
    struct TriangleMeshShape : public btBvhTriangleMeshShape
    {
        TriangleMeshShape(
            btStridingMeshInterface* meshInterface, bool useQuantizedAabbCompression, bool buildBvh = true)
            : btBvhTriangleMeshShape(meshInterface, useQuantizedAabbCompression, buildBvh)
        {
        }

        virtual ~TriangleMeshShape()
        {
            delete getTriangleInfoMap();
            delete m_meshInterface;
        }
    };

    // btScaledBvhTriangleMeshShape that auto-deletes the child shape
    struct ScaledTriangleMeshShape : public btScaledBvhTriangleMeshShape
    {
        ScaledTriangleMeshShape(btBvhTriangleMeshShape* childShape, const btVector3& localScaling)
            : btScaledBvhTriangleMeshShape(childShape, localScaling)
        {
        }

        ~ScaledTriangleMeshShape() override { delete getChildShape(); }
    };

}

#endif
