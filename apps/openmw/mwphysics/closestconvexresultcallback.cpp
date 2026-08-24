#include "closestconvexresultcallback.hpp"

namespace MWPhysics
{
    btScalar ClosestConvexResultCallback::addSingleResult(
        btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace)
    {
        const bool accepted = result.m_hitFraction < m_closestHitFraction;
        const btScalar hitFraction
            = btCollisionWorld::ClosestConvexResultCallback::addSingleResult(result, normalInWorldSpace);
        if (!accepted)
            return hitFraction;

        if (result.m_localShapeInfo != nullptr)
        {
            mHitShapePart = result.m_localShapeInfo->m_shapePart;
            mHitTriangleIndex = result.m_localShapeInfo->m_triangleIndex;
        }
        else
        {
            mHitShapePart = sInvalidCollisionIndex;
            mHitTriangleIndex = sInvalidCollisionIndex;
        }
        return hitFraction;
    }
}
