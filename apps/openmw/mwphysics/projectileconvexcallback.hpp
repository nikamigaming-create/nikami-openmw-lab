#ifndef OPENMW_MWPHYSICS_PROJECTILECONVEXCALLBACK_H
#define OPENMW_MWPHYSICS_PROJECTILECONVEXCALLBACK_H

#include <span>

#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>

class btCollisionObject;

namespace MWPhysics
{
    class Projectile;

    class ProjectileConvexCallback : public btCollisionWorld::ClosestConvexResultCallback
    {
    public:
        explicit ProjectileConvexCallback(std::span<const btCollisionObject* const> casters,
            const btCollisionObject* me, const btVector3& from, const btVector3& to, Projectile& projectile)
            : btCollisionWorld::ClosestConvexResultCallback(from, to)
            , mCasters(casters)
            , mMe(me)
            , mProjectile(projectile)
        {
        }

        btScalar addSingleResult(btCollisionWorld::LocalConvexResult& result, bool normalInWorldSpace) override;

    private:
        std::span<const btCollisionObject* const> mCasters;
        const btCollisionObject* mMe;
        Projectile& mProjectile;
    };
}

#endif
