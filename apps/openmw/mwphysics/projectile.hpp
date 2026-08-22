#ifndef OPENMW_MWPHYSICS_PROJECTILE_H
#define OPENMW_MWPHYSICS_PROJECTILE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <LinearMath/btVector3.h>

#include "ptrholder.hpp"

class btCollisionObject;
class btCollisionShape;
class btConvexShape;

namespace osg
{
    class Vec3f;
}

namespace MWPhysics
{
    class PhysicsTaskScheduler;
    class PhysicsSystem;

    class Projectile final : public PtrHolder
    {
    public:
        Projectile(const MWWorld::Ptr& caster, const osg::Vec3f& position, float radius,
            PhysicsTaskScheduler* scheduler, PhysicsSystem* physicssystem);
        ~Projectile() override;

        btConvexShape* getConvexShape() const { return mConvexShape; }

        void updateCollisionObjectPosition();

        bool isActive() const { return mActive.load(std::memory_order_acquire); }

        MWWorld::Ptr getTarget() const;
        std::optional<std::uint32_t> getHitHavokMaterial() const;

        MWWorld::Ptr getCaster() const;
        void setCaster(const MWWorld::Ptr& caster);
        std::span<const btCollisionObject* const> getCasterCollisionObjects() const { return mCasterColObjs; }

        void setHitWater() { mHitWater = true; }

        bool getHitWater() const { return mHitWater; }

        void hit(const btCollisionObject* target, btVector3 pos, btVector3 normal, int shapePart = -1,
            int triangleIndex = -1);

        void setValidTargets(const std::vector<MWWorld::Ptr>& targets);
        bool isValidTarget(const btCollisionObject* target) const;
        bool isAnyValidTarget(std::span<const btCollisionObject* const> targets) const;

        btVector3 getHitPosition() const { return mHitPosition; }

    private:
        std::unique_ptr<btCollisionShape> mShape;
        btConvexShape* mConvexShape;

        bool mHitWater;
        std::atomic<bool> mActive;
        MWWorld::Ptr mCaster;
        std::vector<const btCollisionObject*> mCasterColObjs;
        const btCollisionObject* mHitTarget;
        btVector3 mHitPosition;
        btVector3 mHitNormal;
        int mHitShapePart = -1;
        int mHitTriangleIndex = -1;

        std::vector<const btCollisionObject*> mValidTargets;

        mutable std::mutex mMutex;

        PhysicsSystem* mPhysics;
        PhysicsTaskScheduler* mTaskScheduler;

        Projectile(const Projectile&);
        Projectile& operator=(const Projectile&);
    };

}

#endif
