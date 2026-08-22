#ifndef OPENMW_MWPHYSICS_CLOSESTCONVEXRESULTCALLBACK_H
#define OPENMW_MWPHYSICS_CLOSESTCONVEXRESULTCALLBACK_H

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

namespace MWPhysics
{
    class ClosestConvexResultCallback : public btCollisionWorld::ClosestConvexResultCallback
    {
    public:
        ClosestConvexResultCallback(const btVector3& from, const btVector3& to)
            : btCollisionWorld::ClosestConvexResultCallback(from, to)
        {
        }

        btScalar addSingleResult(btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace) override;

        int getHitShapePart() const { return mHitShapePart; }
        int getHitTriangleIndex() const { return mHitTriangleIndex; }

    private:
        int mHitShapePart = -1;
        int mHitTriangleIndex = -1;
    };
}

#endif
