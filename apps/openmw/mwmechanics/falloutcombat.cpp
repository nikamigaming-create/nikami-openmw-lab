#include "falloutcombat.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <osg/Math>

#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadweap.hpp>

namespace MWMechanics
{
    std::optional<ESM4::Faction::GroupCombatReaction> resolveFalloutFactionReaction(
        std::span<const ESM4::ActorFaction> actorFactions, std::span<const ESM4::ActorFaction> targetFactions,
        const FalloutFactionLookup& findFaction)
    {
        std::vector<ESM::FormId> targetIds;
        targetIds.reserve(targetFactions.size());
        for (const ESM4::ActorFaction& membership : targetFactions)
        {
            const ESM::FormId id = ESM::FormId::fromUint32(membership.faction);
            if (!id.isZeroOrUnset())
                targetIds.push_back(id);
        }
        if (targetIds.empty())
            return std::nullopt;

        auto result = ESM4::Faction::GroupCombatReaction::Neutral;
        for (const ESM4::ActorFaction& membership : actorFactions)
        {
            const ESM::FormId actorFactionId = ESM::FormId::fromUint32(membership.faction);
            if (actorFactionId.isZeroOrUnset())
                continue;

            const ESM4::Faction* faction = findFaction(actorFactionId);
            if (faction == nullptr)
                return std::nullopt;

            for (const ESM4::Faction::Relation& relation : faction->mRelations)
            {
                if (std::find(targetIds.begin(), targetIds.end(), relation.mFaction) == targetIds.end())
                    continue;

                // Native StartCombat treats any authored Ally/Friend pairing as non-hostile even when another
                // membership is Enemy. Preserve that contract across all directional faction pairs.
                if (relation.mGroupCombatReaction == ESM4::Faction::GroupCombatReaction::Friend)
                    result = ESM4::Faction::GroupCombatReaction::Friend;
                else if (relation.mGroupCombatReaction == ESM4::Faction::GroupCombatReaction::Ally
                    && result != ESM4::Faction::GroupCombatReaction::Friend)
                    result = ESM4::Faction::GroupCombatReaction::Ally;
                else if (relation.mGroupCombatReaction == ESM4::Faction::GroupCombatReaction::Enemy
                    && result == ESM4::Faction::GroupCombatReaction::Neutral)
                    result = ESM4::Faction::GroupCombatReaction::Enemy;
            }
        }

        return result;
    }

    bool shouldFalloutActorInitiateCombat(
        std::uint8_t aggression, std::optional<ESM4::Faction::GroupCombatReaction> reaction)
    {
        switch (aggression)
        {
            case 0:
                return false;
            case 1:
                return reaction == ESM4::Faction::GroupCombatReaction::Enemy;
            case 2:
                return reaction == ESM4::Faction::GroupCombatReaction::Enemy
                    || reaction == ESM4::Faction::GroupCombatReaction::Neutral;
            case 3:
                return true;
            default:
                return false;
        }
    }

    bool shouldFalloutActorFlee(std::uint8_t confidence) noexcept
    {
        return confidence == 0;
    }

    std::optional<ESM::FormId> selectAuthoredFalloutAmmo(std::span<const ESM::FormId> candidates,
        std::uint8_t rounds, const FalloutAmmoTypePredicate& isAmmo, const FalloutAmmoCount& countAmmo)
    {
        if (rounds == 0)
            return std::nullopt;

        for (ESM::FormId candidate : candidates)
        {
            if (candidate.isZeroOrUnset() || !isAmmo(candidate))
                continue;
            if (countAmmo(candidate) >= static_cast<int>(rounds))
                return candidate;
        }
        return std::nullopt;
    }

    std::optional<FalloutShotContract> buildFalloutRayShotContract(const ESM4::Weapon& weapon,
        const ESM4::Projectile& projectile, ESM::FormId ammo, FalloutShotFailure& failure)
    {
        failure = FalloutShotFailure::None;
        const bool consumesWeapon = isFalloutThrownWeapon(weapon);
        if ((!consumesWeapon && ammo.isZeroOrUnset()) || (consumesWeapon && ammo != weapon.mId))
            failure = FalloutShotFailure::MissingAmmo;
        else if (!weapon.mData.hasBallistics)
            failure = FalloutShotFailure::MissingBallistics;
        else if (weapon.mData.projectile != projectile.mId)
            failure = FalloutShotFailure::ProjectileMismatch;
        else if (!projectile.mData.present)
            failure = FalloutShotFailure::MissingProjectileData;
        else if (!consumesWeapon && weapon.mData.ammoUse == 0)
            failure = FalloutShotFailure::InvalidAmmoUse;
        else if (weapon.mData.numProjectiles == 0)
            failure = FalloutShotFailure::InvalidProjectileCount;
        else if (!std::isfinite(projectile.mData.range) || projectile.mData.range <= 0.f)
            failure = FalloutShotFailure::InvalidRange;
        else if (!std::isfinite(weapon.mData.minSpread) || weapon.mData.minSpread < 0.f
            || weapon.mData.minSpread >= 90.f || !std::isfinite(weapon.mData.spread) || weapon.mData.spread < 0.f
            || weapon.mData.spread >= 90.f)
            failure = FalloutShotFailure::InvalidSpread;

        if (failure != FalloutShotFailure::None)
            return std::nullopt;

        return FalloutShotContract{ ammo, weapon.mData.projectile,
            static_cast<std::uint8_t>(consumesWeapon ? 1 : weapon.mData.ammoUse),
            weapon.mData.numProjectiles, static_cast<float>(weapon.mData.damage), weapon.mData.minRange,
            weapon.mData.maxRange, projectile.mData.range, weapon.mData.minSpread, weapon.mData.spread,
            (projectile.mData.flags & ESM4::Projectile::Hitscan) != 0, consumesWeapon };
    }

    std::optional<FalloutFireCadence> buildFalloutFireCadence(
        const ESM4::Weapon& weapon, FalloutFireCadenceFailure& failure)
    {
        failure = FalloutFireCadenceFailure::None;
        if (!weapon.mData.isAutomatic())
            return FalloutFireCadence{};

        if (!std::isfinite(weapon.mData.animationMultiplier) || weapon.mData.animationMultiplier <= 0.f)
            failure = FalloutFireCadenceFailure::InvalidAnimationMultiplier;
        else if (!std::isfinite(weapon.mData.animAttackMult) || weapon.mData.animAttackMult <= 0.f)
            failure = FalloutFireCadenceFailure::InvalidAttackMultiplier;
        else if (!std::isfinite(weapon.mData.fireRate) || weapon.mData.fireRate <= 0.f)
            failure = FalloutFireCadenceFailure::InvalidFireRate;

        if (failure != FalloutFireCadenceFailure::None)
            return std::nullopt;

        const float shotsPerSecond = weapon.mData.animationMultiplier * weapon.mData.animAttackMult
            * weapon.mData.animAttackMult * weapon.mData.fireRate;
        if (!std::isfinite(shotsPerSecond) || shotsPerSecond <= 0.f)
        {
            failure = FalloutFireCadenceFailure::InvalidShotsPerSecond;
            return std::nullopt;
        }
        return FalloutFireCadence{ true, 1.f / shotsPerSecond };
    }

    bool advanceFalloutTrigger(FalloutTriggerState& state, bool triggerDown, bool ready,
        const FalloutFireCadence& cadence, float duration) noexcept
    {
        if (state.mCooldown > 0.f && std::isfinite(duration) && duration > 0.f)
            state.mCooldown -= duration;

        const bool pressed = triggerDown && !state.mWasDown;
        state.mWasDown = triggerDown;
        if (!ready)
            return false;
        if (!cadence.mAutomatic)
            return pressed;
        if (!triggerDown || !std::isfinite(cadence.mSecondsPerShot) || cadence.mSecondsPerShot <= 0.f
            || state.mCooldown > 0.f)
            return false;

        state.mCooldown += cadence.mSecondsPerShot;
        return true;
    }

    bool isFalloutThrownWeapon(const ESM4::Weapon& weapon) noexcept
    {
        return weapon.mData.animationType == 13;
    }

    std::optional<osg::Vec3f> buildFalloutRayDirection(
        const osg::Vec3f& direction, float spreadDegrees, const osg::Vec2f& diskSample)
    {
        if (!std::isfinite(direction.x()) || !std::isfinite(direction.y()) || !std::isfinite(direction.z())
            || !std::isfinite(spreadDegrees) || spreadDegrees < 0.f || spreadDegrees >= 90.f
            || !std::isfinite(diskSample.x()) || !std::isfinite(diskSample.y()) || diskSample.length2() > 1.00001f)
            return std::nullopt;

        osg::Vec3f forward = direction;
        if (forward.normalize() == 0.f)
            return std::nullopt;

        const osg::Vec3f reference
            = std::abs(forward.z()) < 0.999f ? osg::Vec3f(0.f, 0.f, 1.f) : osg::Vec3f(1.f, 0.f, 0.f);
        osg::Vec3f right = forward ^ reference;
        if (right.normalize() == 0.f)
            return std::nullopt;
        osg::Vec3f up = right ^ forward;
        up.normalize();

        const float coneRadius = std::tan(osg::DegreesToRadians(spreadDegrees));
        osg::Vec3f result
            = forward + right * (diskSample.x() * coneRadius) + up * (diskSample.y() * coneRadius);
        if (result.normalize() == 0.f)
            return std::nullopt;
        return result;
    }

    bool isFalloutMeleeAnimationType(std::uint8_t animationType) noexcept
    {
        return animationType <= 2;
    }

    std::optional<FalloutMeleeContract> buildFalloutMeleeContract(const ESM4::Weapon* weapon,
        std::uint8_t animationType, float skill, float strength, const FalloutMeleeTuning& tuning,
        FalloutMeleeFailure& failure)
    {
        failure = FalloutMeleeFailure::None;
        const bool unarmedFamily = animationType == 0;
        const bool bareHanded = weapon == nullptr;
        if (!isFalloutMeleeAnimationType(animationType))
            failure = FalloutMeleeFailure::NotMelee;
        else if (bareHanded && !unarmedFamily)
            failure = FalloutMeleeFailure::MissingWeapon;
        else if (!std::isfinite(skill) || skill < 0.f || !std::isfinite(strength) || strength < 0.f)
            failure = FalloutMeleeFailure::InvalidActorValues;
        else if (!std::isfinite(tuning.mDamageSkillBase) || !std::isfinite(tuning.mDamageSkillMult)
            || !std::isfinite(tuning.mUnarmedDamageBase) || !std::isfinite(tuning.mUnarmedDamageMult)
            || !std::isfinite(tuning.mMeleeStrengthMult) || !std::isfinite(tuning.mMeleeStrengthOffset)
            || !std::isfinite(tuning.mCombatDistance) || !std::isfinite(tuning.mUnarmedReach))
            failure = FalloutMeleeFailure::InvalidTuning;

        if (failure != FalloutMeleeFailure::None)
            return std::nullopt;

        const float skillMultiplier = tuning.mDamageSkillBase + tuning.mDamageSkillMult * (skill / 100.f);
        const float familyDamage = unarmedFamily
            ? tuning.mUnarmedDamageBase + tuning.mUnarmedDamageMult * skill
            : tuning.mMeleeStrengthOffset + tuning.mMeleeStrengthMult * strength;
        const float weaponDamage = bareHanded ? 0.f : static_cast<float>(weapon->mData.damage);
        const float damage = weaponDamage * skillMultiplier + familyDamage;
        if (!std::isfinite(skillMultiplier) || skillMultiplier < 0.f || !std::isfinite(damage) || damage <= 0.f)
        {
            failure = FalloutMeleeFailure::InvalidDamage;
            return std::nullopt;
        }

        const float reachMultiplier = bareHanded ? tuning.mUnarmedReach : weapon->mData.reach;
        const float reach = tuning.mCombatDistance * reachMultiplier;
        if (!std::isfinite(reachMultiplier) || reachMultiplier <= 0.f || !std::isfinite(reach) || reach <= 0.f)
        {
            failure = FalloutMeleeFailure::InvalidReach;
            return std::nullopt;
        }

        return FalloutMeleeContract{ damage, reach, unarmedFamily, bareHanded };
    }

    std::optional<FalloutMeleeContract> buildFalloutCreatureMeleeContract(float authoredDamage,
        float combatSkill, float strength, const FalloutMeleeTuning& tuning, FalloutMeleeFailure& failure)
    {
        failure = FalloutMeleeFailure::None;
        if (!std::isfinite(authoredDamage) || authoredDamage <= 0.f || !std::isfinite(combatSkill)
            || combatSkill < 0.f || !std::isfinite(strength) || strength < 0.f)
            failure = FalloutMeleeFailure::InvalidActorValues;
        else if (!std::isfinite(tuning.mDamageSkillBase) || !std::isfinite(tuning.mDamageSkillMult)
            || !std::isfinite(tuning.mMeleeStrengthMult) || !std::isfinite(tuning.mMeleeStrengthOffset)
            || !std::isfinite(tuning.mCombatDistance) || !std::isfinite(tuning.mUnarmedReach))
            failure = FalloutMeleeFailure::InvalidTuning;
        if (failure != FalloutMeleeFailure::None)
            return std::nullopt;

        const float skillMultiplier
            = tuning.mDamageSkillBase + tuning.mDamageSkillMult * (combatSkill / 100.f);
        const float strengthDamage = tuning.mMeleeStrengthOffset + tuning.mMeleeStrengthMult * strength;
        const float damage = authoredDamage * skillMultiplier + strengthDamage;
        if (!std::isfinite(skillMultiplier) || skillMultiplier < 0.f || !std::isfinite(damage) || damage <= 0.f)
        {
            failure = FalloutMeleeFailure::InvalidDamage;
            return std::nullopt;
        }

        const float reach = tuning.mCombatDistance * tuning.mUnarmedReach;
        if (!std::isfinite(reach) || reach <= 0.f)
        {
            failure = FalloutMeleeFailure::InvalidReach;
            return std::nullopt;
        }
        return FalloutMeleeContract{ damage, reach, true, true };
    }

    std::string_view getFalloutShotFailureName(FalloutShotFailure failure)
    {
        switch (failure)
        {
            case FalloutShotFailure::None:
                return "none";
            case FalloutShotFailure::MissingAmmo:
                return "missing-ammo";
            case FalloutShotFailure::MissingBallistics:
                return "missing-ballistics";
            case FalloutShotFailure::ProjectileMismatch:
                return "projectile-mismatch";
            case FalloutShotFailure::MissingProjectileData:
                return "missing-projectile-data";
            case FalloutShotFailure::InvalidAmmoUse:
                return "invalid-ammo-use";
            case FalloutShotFailure::InvalidProjectileCount:
                return "invalid-projectile-count";
            case FalloutShotFailure::InvalidRange:
                return "invalid-range";
            case FalloutShotFailure::InvalidSpread:
                return "invalid-spread";
        }
        return "unknown";
    }

    std::string_view getFalloutFireCadenceFailureName(FalloutFireCadenceFailure failure)
    {
        switch (failure)
        {
            case FalloutFireCadenceFailure::None:
                return "none";
            case FalloutFireCadenceFailure::InvalidAnimationMultiplier:
                return "invalid-animation-multiplier";
            case FalloutFireCadenceFailure::InvalidAttackMultiplier:
                return "invalid-attack-multiplier";
            case FalloutFireCadenceFailure::InvalidFireRate:
                return "invalid-fire-rate";
            case FalloutFireCadenceFailure::InvalidShotsPerSecond:
                return "invalid-shots-per-second";
        }
        return "unknown";
    }

    std::string_view getFalloutMeleeFailureName(FalloutMeleeFailure failure)
    {
        switch (failure)
        {
            case FalloutMeleeFailure::None:
                return "none";
            case FalloutMeleeFailure::NotMelee:
                return "not-melee";
            case FalloutMeleeFailure::MissingWeapon:
                return "missing-weapon";
            case FalloutMeleeFailure::InvalidActorValues:
                return "invalid-actor-values";
            case FalloutMeleeFailure::InvalidTuning:
                return "invalid-tuning";
            case FalloutMeleeFailure::InvalidDamage:
                return "invalid-damage";
            case FalloutMeleeFailure::InvalidReach:
                return "invalid-reach";
        }
        return "unknown";
    }
}
