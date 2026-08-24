#include "bulletshape.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <BulletCollision/CollisionShapes/btConvexHullShape.h>
#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
#include <BulletCollision/CollisionShapes/btScaledBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>

namespace Resource
{
    namespace
    {
        CollisionShapePtr duplicateCollisionShape(const btCollisionShape* shape)
        {
            if (shape == nullptr)
                return nullptr;

            if (shape->isCompound())
            {
                const btCompoundShape* comp = static_cast<const btCompoundShape*>(shape);
                std::unique_ptr<btCompoundShape, DeleteCollisionShape> newShape(new btCompoundShape);

                for (int i = 0, n = comp->getNumChildShapes(); i < n; ++i)
                {
                    auto child = duplicateCollisionShape(comp->getChildShape(i));
                    const btTransform& trans = comp->getChildTransform(i);
                    newShape->addChildShape(trans, child.release());
                }

                return newShape;
            }

            if (shape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE)
            {
                const btScaledBvhTriangleMeshShape* trishape = static_cast<const btScaledBvhTriangleMeshShape*>(shape);
                return CollisionShapePtr(new btScaledBvhTriangleMeshShape(
                    const_cast<btBvhTriangleMeshShape*>(trishape->getChildShape()), trishape->getLocalScaling()));
            }

            if (shape->getShapeType() == TRIANGLE_MESH_SHAPE_PROXYTYPE)
            {
                const btBvhTriangleMeshShape* trishape = static_cast<const btBvhTriangleMeshShape*>(shape);
                return CollisionShapePtr(new btScaledBvhTriangleMeshShape(
                    const_cast<btBvhTriangleMeshShape*>(trishape), btVector3(1.f, 1.f, 1.f)));
            }

            if (shape->getShapeType() == SPHERE_SHAPE_PROXYTYPE)
                return CollisionShapePtr(new btSphereShape(static_cast<const btSphereShape&>(*shape)));

            if (shape->getShapeType() == CAPSULE_SHAPE_PROXYTYPE)
                return CollisionShapePtr(new btCapsuleShape(static_cast<const btCapsuleShape&>(*shape)));

            if (shape->getShapeType() == CONVEX_HULL_SHAPE_PROXYTYPE)
            {
                const auto& hull = static_cast<const btConvexHullShape&>(*shape);
                auto copy = std::make_unique<btConvexHullShape>();
                for (int i = 0; i < hull.getNumPoints(); ++i)
                    copy->addPoint(hull.getUnscaledPoints()[i], false);
                copy->setMargin(hull.getMargin());
                copy->setLocalScaling(hull.getLocalScaling());
                copy->recalcLocalAabb();
                return CollisionShapePtr(copy.release());
            }

            if (shape->getShapeType() == BOX_SHAPE_PROXYTYPE)
            {
                const btBoxShape* boxshape = static_cast<const btBoxShape*>(shape);
                return CollisionShapePtr(new btBoxShape(*boxshape));
            }

            if (shape->getShapeType() == TERRAIN_SHAPE_PROXYTYPE)
                return CollisionShapePtr(
                    new btHeightfieldTerrainShape(static_cast<const btHeightfieldTerrainShape&>(*shape)));

            throw std::logic_error(std::string("Unhandled Bullet shape duplication: ") + shape->getName());
        }

        CollisionBody duplicateCollisionBody(const CollisionBody& body)
        {
            return { duplicateCollisionShape(body.mCollisionShape.get()), body.mCollisionShapeMaterials,
                body.mBethesdaCollisionFilter };
        }

        void deleteShape(btCollisionShape* shape)
        {
            if (shape->isCompound())
            {
                btCompoundShape* compound = static_cast<btCompoundShape*>(shape);
                for (int i = 0, n = compound->getNumChildShapes(); i < n; i++)
                    if (btCollisionShape* child = compound->getChildShape(i))
                        deleteShape(child);
            }

            delete shape;
        }
    }

    void DeleteCollisionShape::operator()(btCollisionShape* shape) const
    {
        deleteShape(shape);
    }

    CollisionBody::CollisionBody(
        CollisionShapePtr shape, CollisionShapeMaterialTable materials, std::optional<BethesdaCollisionFilter> filter)
        : mCollisionShape(std::move(shape))
        , mCollisionShapeMaterials(std::move(materials))
        , mBethesdaCollisionFilter(std::move(filter))
    {
    }

    bool CollisionShapeMaterialTable::addMaterial(int shapePart, int triangleIndex, std::uint32_t material)
    {
        const auto [it, inserted] = mEntries.emplace(std::pair{ shapePart, triangleIndex }, material);
        return inserted || it->second == material;
    }

    bool CollisionShapeMaterialTable::addUniformMaterial(std::uint32_t material)
    {
        return addMaterial(kInvalidCollisionIndex, kInvalidCollisionIndex, material);
    }

    bool CollisionShapeMaterialTable::addShapePartMaterial(int shapePart, std::uint32_t material)
    {
        return shapePart >= kFirstValidCollisionIndex && addMaterial(shapePart, kInvalidCollisionIndex, material);
    }

    bool CollisionShapeMaterialTable::addTriangleMaterial(int shapePart, int triangleIndex, std::uint32_t material)
    {
        return shapePart >= kFirstValidCollisionIndex && triangleIndex >= kFirstValidCollisionIndex
            && addMaterial(shapePart, triangleIndex, material);
    }

    bool CollisionShapeMaterialTable::addCompoundChildMaterial(int childIndex, std::uint32_t material)
    {
        return childIndex >= kFirstValidCollisionIndex && addMaterial(kInvalidCollisionIndex, childIndex, material);
    }

    std::optional<std::uint32_t> CollisionShapeMaterialTable::findMaterial(int shapePart, int triangleIndex) const
    {
        const auto found = mEntries.find({ shapePart, triangleIndex });
        return found != mEntries.end() ? std::optional(found->second) : std::nullopt;
    }

    std::optional<std::uint32_t> CollisionShapeMaterialTable::getMaterial(int shapePart, int triangleIndex) const
    {
        if (const auto exact = findMaterial(shapePart, triangleIndex))
            return exact;
        if (shapePart >= kFirstValidCollisionIndex && triangleIndex >= kFirstValidCollisionIndex)
            if (const auto shapePartDefault = findMaterial(shapePart, kInvalidCollisionIndex))
                return shapePartDefault;
        if (shapePart >= kFirstValidCollisionIndex || triangleIndex >= kFirstValidCollisionIndex)
            return findMaterial(kInvalidCollisionIndex, kInvalidCollisionIndex);
        return std::nullopt;
    }

    BulletShape::BulletShape(const BulletShape& other, const osg::CopyOp& copyOp)
        : Object(other, copyOp)
        , CollisionBody(duplicateCollisionBody(other))
        , mAvoidCollisionShape(duplicateCollisionShape(other.mAvoidCollisionShape.get()))
        , mCollisionBox(other.mCollisionBox)
        , mAnimatedShapes(other.mAnimatedShapes)
        , mFileName(other.mFileName)
        , mFileHash(other.mFileHash)
        , mVisualCollisionType(other.mVisualCollisionType)
    {
        mAdditionalCollisionBodies.reserve(other.mAdditionalCollisionBodies.size());
        for (const CollisionBody& body : other.mAdditionalCollisionBodies)
            mAdditionalCollisionBodies.emplace_back(duplicateCollisionBody(body));
    }

    void BulletShape::setLocalScaling(const btVector3& scale)
    {
        if (mCollisionShape)
            mCollisionShape->setLocalScaling(scale);
        if (mAvoidCollisionShape)
            mAvoidCollisionShape->setLocalScaling(scale);
        for (CollisionBody& body : mAdditionalCollisionBodies)
            if (body.mCollisionShape)
                body.mCollisionShape->setLocalScaling(scale);
    }

    bool BulletShape::getCollisionAabb(const btTransform& transform, btVector3& min, btVector3& max) const
    {
        bool found = false;
        for (std::size_t i = 0; i < getCollisionBodyCount(); ++i)
        {
            const CollisionBody* body = getCollisionBody(i);
            if (body == nullptr || body->mCollisionShape == nullptr)
                continue;
            btVector3 bodyMin;
            btVector3 bodyMax;
            body->mCollisionShape->getAabb(transform, bodyMin, bodyMax);
            if (!found)
            {
                min = bodyMin;
                max = bodyMax;
                found = true;
            }
            else
            {
                min.setMin(bodyMin);
                max.setMax(bodyMax);
            }
        }
        return found;
    }

    std::optional<std::uint32_t> BulletShape::getHavokMaterial(int shapePart, int triangleIndex) const
    {
        return mCollisionShapeMaterials.getMaterial(shapePart, triangleIndex);
    }

    std::optional<std::uint32_t> BulletShape::getHavokMaterial(
        std::size_t bodyIndex, int shapePart, int triangleIndex) const
    {
        const CollisionBody* body = getCollisionBody(bodyIndex);
        return body != nullptr ? body->mCollisionShapeMaterials.getMaterial(shapePart, triangleIndex) : std::nullopt;
    }

    std::size_t BulletShape::getCollisionBodyCount() const
    {
        return (mCollisionShape != nullptr ? kFirstCollisionBodyCount : kNoCollisionBodyCount)
            + mAdditionalCollisionBodies.size();
    }

    CollisionBody* BulletShape::getCollisionBody(std::size_t index)
    {
        return const_cast<CollisionBody*>(std::as_const(*this).getCollisionBody(index));
    }

    const CollisionBody* BulletShape::getCollisionBody(std::size_t index) const
    {
        if (mCollisionShape != nullptr)
        {
            if (index == kPrimaryCollisionBodyIndex)
                return this;
            --index;
        }
        return index < mAdditionalCollisionBodies.size() ? &mAdditionalCollisionBodies[index] : nullptr;
    }

    osg::ref_ptr<BulletShapeInstance> makeInstance(osg::ref_ptr<const BulletShape> source)
    {
        return { new BulletShapeInstance(std::move(source)) };
    }

    BulletShapeInstance::BulletShapeInstance(osg::ref_ptr<const BulletShape> source)
        : BulletShape(*source)
        , mSource(std::move(source))
    {
    }
}
