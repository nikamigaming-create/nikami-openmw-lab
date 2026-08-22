#include "closestconvexresultcallback.hpp"

namespace MWPhysics
{
    btScalar ClosestConvexResultCallback::addSingleResult(
        btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace)
    {
        const btScalar hitFraction
            = btCollisionWorld::ClosestConvexResultCallback::addSingleResult(result, normalInWorldSpace);
        if (result.m_localShapeInfo != nullptr)
        {
            mHitShapePart = result.m_localShapeInfo->m_shapePart;
            mHitTriangleIndex = result.m_localShapeInfo->m_triangleIndex;
        }
        else
        {
            mHitShapePart = -1;
            mHitTriangleIndex = -1;
        }
        return hitFraction;
    }
}
