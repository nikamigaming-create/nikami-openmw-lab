#include <algorithm>

#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include "collisiontype.hpp"
#include "projectile.hpp"
#include "projectileconvexcallback.hpp"

namespace MWPhysics
{
    btScalar ProjectileConvexCallback::addSingleResult(
        btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace)
    {
        const auto* hitObject = result.m_hitCollisionObject;
        // don't hit the caster
        if (std::ranges::find(mCasters, hitObject) != mCasters.end())
            return 1.f;

        // don't hit the projectile
        if (hitObject == mMe)
            return 1.f;

        bool hitsProjectile = false;
        Projectile* projectileTarget = nullptr;
        switch (hitObject->getBroadphaseHandle()->m_collisionFilterGroup)
        {
            case CollisionType_Actor:
            {
                if (!mProjectile.isValidTarget(hitObject))
                    return 1.f;
                break;
            }
            case CollisionType_Projectile:
            {
                projectileTarget = static_cast<Projectile*>(hitObject->getUserPointer());
                if (projectileTarget == nullptr
                    || !mProjectile.isAnyValidTarget(projectileTarget->getCasterCollisionObjects()))
                    return 1.f;
                hitsProjectile = true;
                break;
            }
        }

        const bool accepted = result.m_hitFraction < m_closestHitFraction;
        const btScalar hitFraction
            = btCollisionWorld::ClosestConvexResultCallback::addSingleResult(result, normalInWorldSpace);
        if (!accepted)
            return hitFraction;

        if (hitsProjectile)
            projectileTarget->hit(mMe, m_hitPointWorld, m_hitNormalWorld);
        else if (hitObject->getBroadphaseHandle()->m_collisionFilterGroup == CollisionType_Water)
            mProjectile.setHitWater();

        int shapePart = sInvalidCollisionIndex;
        int triangleIndex = sInvalidCollisionIndex;
        if (result.m_localShapeInfo != nullptr)
        {
            shapePart = result.m_localShapeInfo->m_shapePart;
            triangleIndex = result.m_localShapeInfo->m_triangleIndex;
        }
        mProjectile.hit(hitObject, m_hitPointWorld, m_hitNormalWorld, shapePart, triangleIndex);

        return hitFraction;
    }

}
