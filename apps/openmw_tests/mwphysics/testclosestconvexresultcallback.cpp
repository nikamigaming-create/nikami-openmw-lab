#include <gtest/gtest.h>

#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include "apps/openmw/mwphysics/closestconvexresultcallback.hpp"
#include "apps/openmw/mwphysics/constants.hpp"

namespace
{
    TEST(ClosestConvexResultCallbackTest, PreservesClosestHitShapeIdentity)
    {
        btCollisionObject target;
        MWPhysics::ClosestConvexResultCallback callback(btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo shapeInfo{ 7, 42 };
        btCollisionWorld::LocalConvexResult result(
            &target, &shapeInfo, btVector3(0, -1, 0), btVector3(0, 0.5f, 0), 0.5f);

        EXPECT_EQ(callback.addSingleResult(result, true), btScalar(0.5f));
        EXPECT_EQ(callback.getHitShapePart(), 7);
        EXPECT_EQ(callback.getHitTriangleIndex(), 42);
    }

    TEST(ClosestConvexResultCallbackTest, CloserHitWithoutShapeInfoClearsPreviousIdentity)
    {
        btCollisionObject firstTarget;
        btCollisionObject closerTarget;
        MWPhysics::ClosestConvexResultCallback callback(btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo shapeInfo{ 3, 9 };
        btCollisionWorld::LocalConvexResult first(
            &firstTarget, &shapeInfo, btVector3(0, -1, 0), btVector3(0, 0.75f, 0), 0.75f);
        btCollisionWorld::LocalConvexResult closer(
            &closerTarget, nullptr, btVector3(0, -1, 0), btVector3(0, 0.25f, 0), 0.25f);

        callback.addSingleResult(first, true);
        callback.addSingleResult(closer, true);

        EXPECT_EQ(callback.getHitShapePart(), MWPhysics::sInvalidCollisionIndex);
        EXPECT_EQ(callback.getHitTriangleIndex(), MWPhysics::sInvalidCollisionIndex);
    }

    TEST(ClosestConvexResultCallbackTest, FartherHitDoesNotOverwriteClosestShapeIdentity)
    {
        btCollisionObject closestTarget;
        btCollisionObject fartherTarget;
        MWPhysics::ClosestConvexResultCallback callback(btVector3(0, 0, 0), btVector3(0, 1, 0));
        btCollisionWorld::LocalShapeInfo closestInfo{ 3, 9 };
        btCollisionWorld::LocalShapeInfo fartherInfo{ 8, 13 };
        btCollisionWorld::LocalConvexResult closest(
            &closestTarget, &closestInfo, btVector3(0, -1, 0), btVector3(0, 0.25f, 0), 0.25f);
        btCollisionWorld::LocalConvexResult farther(
            &fartherTarget, &fartherInfo, btVector3(0, -1, 0), btVector3(0, 0.75f, 0), 0.75f);

        callback.addSingleResult(closest, true);
        callback.addSingleResult(farther, true);

        EXPECT_EQ(callback.getHitShapePart(), 3);
        EXPECT_EQ(callback.getHitTriangleIndex(), 9);
    }
}
