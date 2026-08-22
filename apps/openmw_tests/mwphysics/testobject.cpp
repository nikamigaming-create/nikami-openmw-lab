#include "apps/openmw/mwclass/static.hpp"
#include "apps/openmw/mwphysics/collisiontype.hpp"
#include "apps/openmw/mwphysics/mtphysics.hpp"
#include "apps/openmw/mwphysics/object.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"
#include "apps/openmw/mwworld/ptr.hpp"

#include <components/esm3/loadstat.hpp>
#include <components/resource/bulletshape.hpp>

#include <gtest/gtest.h>

#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionDispatch/btCollisionDispatcher.h>
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>

#include <osg/Quat>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace
{
    TEST(PhysicsObjectTest, OwnsRegistersTransformsAndRemovesEveryCollisionBody)
    {
        MWClass::Static::registerSelf();
        ESM::Static base;
        base.blank();
        base.mId = ESM::RefId::stringRefId("multi_body_static");
        ESM::CellRef cellRef;
        cellRef.blank();
        cellRef.mRefID = base.mId;
        MWWorld::LiveCellRef<ESM::Static> liveCellRef(cellRef, &base);
        MWWorld::Ptr ptr(&liveCellRef);

        btDefaultCollisionConfiguration configuration;
        btCollisionDispatcher dispatcher(&configuration);
        btDbvtBroadphase broadphase;
        btCollisionWorld world(&dispatcher, &broadphase, &configuration);
        MWPhysics::PhysicsTaskScheduler scheduler(1.f / 60.f, &world, nullptr);

        osg::ref_ptr<Resource::BulletShape> source = new Resource::BulletShape;
        source->mCollisionShape.reset(new btBoxShape(btVector3(1.f, 2.f, 3.f)));
        ASSERT_TRUE(source->mCollisionShapeMaterials.addUniformMaterial(4));
        Resource::CollisionShapeMaterialTable materials;
        ASSERT_TRUE(materials.addUniformMaterial(9));
        auto additionalShape = std::make_unique<btCompoundShape>();
        btTransform additionalTransform = btTransform::getIdentity();
        additionalTransform.setOrigin(btVector3(10.f, 0.f, 0.f));
        additionalShape->addChildShape(additionalTransform, new btSphereShape(2.f));
        source->mAdditionalCollisionBodies.emplace_back(
            Resource::CollisionShapePtr(additionalShape.release()), std::move(materials), std::nullopt);
        osg::ref_ptr<Resource::BulletShapeInstance> instance = Resource::makeInstance(source);

        {
            MWPhysics::Object object(ptr, instance, osg::Quat(), MWPhysics::CollisionType_World, &scheduler);

            const auto collisionObjects = object.getCollisionObjects();
            ASSERT_EQ(collisionObjects.size(), 2u);
            EXPECT_EQ(world.getNumCollisionObjects(), 2);
            EXPECT_EQ(collisionObjects[0], object.getCollisionObject());
            EXPECT_EQ(collisionObjects[0]->getUserPointer(), &object);
            EXPECT_EQ(collisionObjects[1]->getUserPointer(), &object);
            EXPECT_EQ(collisionObjects[0]->getUserIndex(), 0);
            EXPECT_EQ(collisionObjects[1]->getUserIndex(), 1);
            EXPECT_EQ(object.getHavokMaterial(0, -1, -1), 4u);
            EXPECT_EQ(object.getHavokMaterial(1, -1, -1), 9u);

            const btVector3 rayFrom(10.f, 0.f, 10.f);
            const btVector3 rayTo(10.f, 0.f, -10.f);
            btCollisionWorld::ClosestRayResultCallback callback(rayFrom, rayTo);
            callback.m_collisionFilterGroup = MWPhysics::CollisionType_Actor;
            callback.m_collisionFilterMask = MWPhysics::CollisionType_World;
            world.rayTest(rayFrom, rayTo, callback);
            ASSERT_TRUE(callback.hasHit());
            ASSERT_NE(callback.m_collisionObject, nullptr);
            EXPECT_EQ(callback.m_collisionObject->getUserIndex(), 1);
            EXPECT_EQ(
                object.getHavokMaterial(static_cast<std::size_t>(callback.m_collisionObject->getUserIndex()), -1, -1),
                9u);

            object.setScale(2.f);
            object.setRotation(osg::Quat(osg::DegreesToRadians(45.f), osg::Vec3f(0.f, 0.f, 1.f)));
            object.commitPositionChange();

            EXPECT_EQ(instance->getCollisionBody(0)->mCollisionShape->getLocalScaling(), btVector3(2.f, 2.f, 2.f));
            EXPECT_EQ(instance->getCollisionBody(1)->mCollisionShape->getLocalScaling(), btVector3(2.f, 2.f, 2.f));
            EXPECT_EQ(collisionObjects[0]->getWorldTransform(), collisionObjects[1]->getWorldTransform());
        }

        EXPECT_EQ(world.getNumCollisionObjects(), 0);
    }
}
