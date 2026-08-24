#include "object.hpp"
#include "mtphysics.hpp"

#include <components/bullethelpers/collisionobject.hpp>
#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/nifosg/particle.hpp>
#include <components/resource/bulletshape.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include <BulletCollision/CollisionShapes/btCompoundShape.h>

#include <LinearMath/btTransform.h>

#include <limits>
#include <stdexcept>

namespace MWPhysics
{
    Object::Object(const MWWorld::Ptr& ptr, osg::ref_ptr<Resource::BulletShapeInstance> shapeInstance,
        osg::Quat rotation, int collisionType, PhysicsTaskScheduler* scheduler)
        : PtrHolder(ptr, osg::Vec3f())
        , mShapeInstance(std::move(shapeInstance))
        , mSolid(true)
        , mScale(ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale())
        , mPosition(ptr.getRefData().getPosition().asVec3())
        , mRotation(rotation)
        , mTaskScheduler(scheduler)
        , mCollidedWith(ScriptedCollisionType_None)
    {
        mShapeInstance->setLocalScaling(mScale);
        const std::size_t bodyCount = mShapeInstance->getCollisionBodyCount();
        if (bodyCount == sNoCollisionBodyCount || bodyCount > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::logic_error("Physics object requires a representable collision body");

        mAdditionalCollisionObjects.reserve(bodyCount - sFirstCollisionBodyCount);
        mCollisionObjects.reserve(bodyCount);
        for (std::size_t bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex)
        {
            const Resource::CollisionBody* body = mShapeInstance->getCollisionBody(bodyIndex);
            if (body == nullptr || body->mCollisionShape == nullptr)
                throw std::logic_error("Physics object collision body has no shape");

            auto object = BulletHelpers::makeCollisionObject(
                body->mCollisionShape.get(), Misc::Convert::toBullet(mPosition), Misc::Convert::toBullet(rotation));
            object->setUserPointer(this);
            object->setUserIndex(static_cast<int>(bodyIndex));
            btCollisionObject* const view = object.get();
            if (bodyIndex == sPrimaryCollisionBodyIndex)
                mCollisionObject = std::move(object);
            else
                mAdditionalCollisionObjects.push_back(std::move(object));
            mCollisionObjects.push_back(view);
        }

        for (btCollisionObject* object : mCollisionObjects)
            mTaskScheduler->addCollisionObject(
                object, collisionType, CollisionType_Actor | CollisionType_HeightMap | CollisionType_Projectile);
    }

    Object::~Object()
    {
        for (btCollisionObject* object : mCollisionObjects)
            mTaskScheduler->removeCollisionObject(object);
    }

    const Resource::BulletShapeInstance* Object::getShapeInstance() const
    {
        return mShapeInstance.get();
    }

    std::optional<std::uint32_t> Object::getHavokMaterial(int shapePart, int triangleIndex) const
    {
        return getHavokMaterial(sPrimaryCollisionBodyIndex, shapePart, triangleIndex);
    }

    std::optional<std::uint32_t> Object::getHavokMaterial(std::size_t bodyIndex, int shapePart, int triangleIndex) const
    {
        return mShapeInstance != nullptr ? mShapeInstance->getHavokMaterial(bodyIndex, shapePart, triangleIndex)
                                         : std::nullopt;
    }

    void Object::setScale(float scale)
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        mScale = { scale, scale, scale };
        mScaleUpdatePending = true;
    }

    void Object::setRotation(osg::Quat quat)
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        mRotation = quat;
        mTransformUpdatePending = true;
    }

    void Object::updatePosition()
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        mPosition = mPtr.getRefData().getPosition().asVec3();
        mTransformUpdatePending = true;
    }

    void Object::commitPositionChange()
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        if (mScaleUpdatePending)
        {
            mShapeInstance->setLocalScaling(mScale);
            mScaleUpdatePending = false;
        }
        if (mTransformUpdatePending)
        {
            btTransform trans;
            trans.setOrigin(Misc::Convert::toBullet(mPosition));
            trans.setRotation(Misc::Convert::toBullet(mRotation));
            for (btCollisionObject* object : mCollisionObjects)
                object->setWorldTransform(trans);
            mTransformUpdatePending = false;
        }
    }

    btTransform Object::getTransform() const
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        btTransform trans;
        trans.setOrigin(Misc::Convert::toBullet(mPosition));
        trans.setRotation(Misc::Convert::toBullet(mRotation));
        return trans;
    }

    bool Object::isSolid() const
    {
        return mSolid;
    }

    void Object::setSolid(bool solid)
    {
        mSolid = solid;
    }

    bool Object::isAnimated() const
    {
        return mShapeInstance->isAnimated();
    }

    bool Object::animateCollisionShapes()
    {
        if (mShapeInstance->mAnimatedShapes.empty())
            return false;

        if (!mPtr.getRefData().getBaseNode())
            return false;

        assert(mShapeInstance->mCollisionShape->isCompound());

        btCompoundShape* compound = static_cast<btCompoundShape*>(mShapeInstance->mCollisionShape.get());
        bool result = false;
        for (const auto& [recordIndex, shapeIndex] : mShapeInstance->mAnimatedShapes)
        {
            auto nodePathFound = mRecordIndexToNodePath.find(recordIndex);
            if (nodePathFound == mRecordIndexToNodePath.end())
            {
                NifOsg::FindGroupByRecordIndex visitor(recordIndex);
                mPtr.getRefData().getBaseNode()->accept(visitor);
                if (!visitor.mFound)
                {
                    Log(Debug::Warning) << "Warning: animateCollisionShapes can't find node " << recordIndex << " for "
                                        << mPtr.getCellRef().getRefId();

                    // Remove nonexistent nodes from animated shapes map and early out
                    mShapeInstance->mAnimatedShapes.erase(recordIndex);
                    return false;
                }
                osg::NodePath nodePath = visitor.mFoundPath;
                nodePath.erase(nodePath.begin());
                nodePathFound = mRecordIndexToNodePath.emplace(recordIndex, nodePath).first;
            }

            osg::NodePath& nodePath = nodePathFound->second;
            osg::Matrixf matrix = osg::computeLocalToWorld(nodePath);
            btVector3 scale = Misc::Convert::toBullet(matrix.getScale());
            matrix.orthoNormalize(matrix);

            btTransform transform;
            transform.setOrigin(Misc::Convert::toBullet(matrix.getTrans()) * compound->getLocalScaling());
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    transform.getBasis()[i][j] = matrix(j, i); // NB column/row major difference

            btCollisionShape* childShape = compound->getChildShape(shapeIndex);
            btVector3 newScale = compound->getLocalScaling() * scale;

            if (childShape->getLocalScaling() != newScale)
            {
                childShape->setLocalScaling(newScale);
                result = true;
            }

            if (!(transform == compound->getChildTransform(shapeIndex)))
            {
                compound->updateChildTransform(shapeIndex, transform);
                result = true;
            }
        }
        return result;
    }

    bool Object::collidedWith(ScriptedCollisionType type) const
    {
        return mCollidedWith & type;
    }

    void Object::addCollision(ScriptedCollisionType type)
    {
        std::unique_lock<std::mutex> lock(mPositionMutex);
        mCollidedWith |= type;
    }

    void Object::resetCollisions()
    {
        mCollidedWith = ScriptedCollisionType_None;
    }
}
