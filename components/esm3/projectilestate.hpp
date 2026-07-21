#ifndef OPENMW_ESM_PROJECTILESTATE_H
#define OPENMW_ESM_PROJECTILESTATE_H

#include <cstdint>
#include <string>
#include <vector>

#include <osg/Quat>
#include <osg/Vec3f>

#include "components/esm/quaternion.hpp"
#include "components/esm/refid.hpp"
#include "components/esm/vector3.hpp"

#include "refnum.hpp"

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // format 0, savegames only

    struct BaseProjectileState
    {
        RefId mId;

        Vector3 mPosition;
        Quaternion mOrientation;

        int32_t mActorId;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };

    struct MagicBoltState : public BaseProjectileState
    {
        RefId mSpellId;
        float mSpeed;
        RefNum mItem;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };

    struct ProjectileState : public BaseProjectileState
    {
        RefId mBowId;
        Vector3 mVelocity;
        float mAttackStrength;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };

    struct FalloutProjectileVatsState
    {
        RefId mTarget;
        std::uint8_t mBodyPart = 0;
        std::uint8_t mDisplayedHitChance = 0;
        std::uint8_t mHealthPercent = 0;
        std::int8_t mActorValue = -1;
        float mActionPointCost = 0.f;
        float mHealthDamageMultiplier = 1.f;
        float mLimbDamageMultiplier = 1.f;
        std::string mBodyPartName;
        std::string mTargetNode;
    };

    /// Save-only state for an in-flight native Fallout projectile. Unlike ESM3 arrows, its immutable impact
    /// payload was fixed when the trigger fired and must survive save/load without being recalculated.
    struct FalloutProjectileState : public BaseProjectileState
    {
        enum Flags : std::uint8_t
        {
            Rotates = 1u << 0,
            Critical = 1u << 1,
            HasVatsAction = 1u << 2,
            VatsTargetHit = 1u << 3,
        };

        Vector3 mVelocity;
        Vector3 mRotationVelocity;
        Vector3 mPreviousPosition;
        float mGravity = 0.f;
        float mMaximumRange = 0.f;
        float mDistanceTravelled = 0.f;
        RefId mWeapon;
        RefId mExplosion;
        float mRawDamage = 0.f;
        float mLimbDamageMultiplier = 1.f;
        float mExplosionDamageMultiplier = 1.f;
        std::vector<RefId> mAmmoEffects;
        std::uint8_t mFlags = 0;
        FalloutProjectileVatsState mVats;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };

}

#endif
