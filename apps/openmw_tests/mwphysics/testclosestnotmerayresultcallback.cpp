#include <gtest/gtest.h>

#include <array>

#include <BulletCollision/BroadphaseCollision/btBroadphaseProxy.h>
#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include "apps/openmw/mwphysics/closestnotmerayresultcallback.hpp"
#include "apps/openmw/mwphysics/collisiontype.hpp"

namespace
{
    struct CollisionObject
    {
        btCollisionObject mObject;
        btBroadphaseProxy mProxy;

        explicit CollisionObject(int group = MWPhysics::CollisionType_World)
        {
            mProxy.m_collisionFilterGroup = group;
            mProxy.m_collisionFilterMask = btBroadphaseProxy::AllFilter;
            mObject.setBroadphaseHandle(&mProxy);
        }
    };

    TEST(ClosestNotMeRayResultCallbackTest, PreservesClosestHitShapeIdentity)
    {
        CollisionObject target;
        MWPhysics::ClosestNotMeRayResultCallback callback({}, {}, btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo shapeInfo{ 7, 42 };
        btCollisionWorld::LocalRayResult result(&target.mObject, &shapeInfo, btVector3(0, -1, 0), 0.5f);

        EXPECT_EQ(callback.addSingleResult(result, true), btScalar(0.5f));
        EXPECT_EQ(callback.getHitShapePart(), 7);
        EXPECT_EQ(callback.getHitTriangleIndex(), 42);
    }

    TEST(ClosestNotMeRayResultCallbackTest, CloserHitWithoutShapeInfoClearsPreviousIdentity)
    {
        CollisionObject firstTarget;
        CollisionObject closerTarget;
        MWPhysics::ClosestNotMeRayResultCallback callback({}, {}, btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo shapeInfo{ 3, 9 };
        btCollisionWorld::LocalRayResult first(&firstTarget.mObject, &shapeInfo, btVector3(0, -1, 0), 0.75f);
        btCollisionWorld::LocalRayResult closer(&closerTarget.mObject, nullptr, btVector3(0, -1, 0), 0.25f);

        callback.addSingleResult(first, true);
        callback.addSingleResult(closer, true);

        EXPECT_EQ(callback.getHitShapePart(), -1);
        EXPECT_EQ(callback.getHitTriangleIndex(), -1);
    }

    TEST(ClosestNotMeRayResultCallbackTest, IgnoredHitDoesNotOverwriteAcceptedIdentity)
    {
        CollisionObject acceptedTarget;
        CollisionObject ignoredTarget;
        std::array<const btCollisionObject*, 1> ignored{ &ignoredTarget.mObject };
        MWPhysics::ClosestNotMeRayResultCallback callback(ignored, {}, btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo acceptedInfo{ 1, 2 };
        btCollisionWorld::LocalShapeInfo ignoredInfo{ 8, 13 };
        btCollisionWorld::LocalRayResult accepted(&acceptedTarget.mObject, &acceptedInfo, btVector3(0, -1, 0), 0.75f);
        btCollisionWorld::LocalRayResult ignoredResult(
            &ignoredTarget.mObject, &ignoredInfo, btVector3(0, -1, 0), 0.25f);

        callback.addSingleResult(accepted, true);
        EXPECT_EQ(callback.addSingleResult(ignoredResult, true), btScalar(1.f));

        EXPECT_EQ(callback.m_closestHitFraction, btScalar(0.75f));
        EXPECT_EQ(callback.getHitShapePart(), 1);
        EXPECT_EQ(callback.getHitTriangleIndex(), 2);
    }
}
