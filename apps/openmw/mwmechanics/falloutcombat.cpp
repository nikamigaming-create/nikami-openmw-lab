#include "falloutcombat.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <osg/Math>

#include <components/esm4/loadamef.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4creature.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

namespace MWMechanics
{
    namespace
    {
        constexpr std::int8_t sFirstFalloutLimbActorValue = 25;
        constexpr std::int8_t sLastFalloutLimbActorValue = 31;

        bool isFalloutLimbActorValue(std::int8_t actorValue) noexcept
        {
            return actorValue >= sFirstFalloutLimbActorValue && actorValue <= sLastFalloutLimbActorValue;
        }

        const ESM4::BodyPartData* findFalloutBodyPartData(
            const MWWorld::ESMStore& store, std::string_view editorId)
        {
            const auto& bodyPartData = store.get<ESM4::BodyPartData>();
            const auto found = std::find_if(bodyPartData.begin(), bodyPartData.end(), [&](const auto& candidate) {
                return Misc::StringUtils::ciEqual(candidate.mEditorId, editorId);
            });
            return found != bodyPartData.end() ? std::addressof(*found) : nullptr;
        }

        bool matchesFalloutBodyPartNode(
            const ESM4::BodyPartData::BodyPart& bodyPart, std::string_view node) noexcept
        {
            if (node.empty())
                return false;
            const auto matches = [&](std::string_view authored) {
                return !authored.empty() && Misc::StringUtils::ciEqual(authored, node);
            };
            return matches(bodyPart.mNodeName) || matches(bodyPart.mVATSTarget)
                || matches(bodyPart.mIKStartNode) || matches(bodyPart.mGoreEffectsTarget);
        }
    }

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

    std::optional<FalloutAiCombatRange> buildFalloutAiCombatRange(const ESM4::Weapon* weapon,
        float combatDistance, float unarmedReach, FalloutAiCombatRangeFailure& failure)
    {
        failure = FalloutAiCombatRangeFailure::None;
        if (!std::isfinite(combatDistance) || combatDistance <= 0.f || !std::isfinite(unarmedReach)
            || unarmedReach <= 0.f)
            failure = FalloutAiCombatRangeFailure::InvalidTuning;
        else if (weapon == nullptr)
            return FalloutAiCombatRange{ combatDistance * unarmedReach, false };
        else if (isFalloutMeleeAnimationType(weapon->mData.animationType))
        {
            if (!std::isfinite(weapon->mData.reach) || weapon->mData.reach <= 0.f)
                failure = FalloutAiCombatRangeFailure::InvalidWeaponReach;
            else
                return FalloutAiCombatRange{ combatDistance * weapon->mData.reach, false };
        }
        else if (!weapon->mData.hasBallistics)
            failure = FalloutAiCombatRangeFailure::MissingBallistics;
        else if (!std::isfinite(weapon->mData.maxRange) || weapon->mData.maxRange <= 0.f)
            failure = FalloutAiCombatRangeFailure::InvalidWeaponRange;
        else
            return FalloutAiCombatRange{ weapon->mData.maxRange, true };

        return std::nullopt;
    }

    std::optional<FalloutDamageMitigation> resolveFalloutDamageMitigation(float incomingDamage,
        float damageResistance, float damageThreshold, float minimumDamageMultiplier,
        float maximumDamageResistance, FalloutDamageMitigationFailure& failure)
    {
        failure = FalloutDamageMitigationFailure::None;
        if (!std::isfinite(incomingDamage) || incomingDamage < 0.f)
            failure = FalloutDamageMitigationFailure::InvalidDamage;
        else if (!std::isfinite(damageResistance))
            failure = FalloutDamageMitigationFailure::InvalidResistance;
        else if (!std::isfinite(damageThreshold))
            failure = FalloutDamageMitigationFailure::InvalidThreshold;
        else if (!std::isfinite(minimumDamageMultiplier) || minimumDamageMultiplier < 0.f
            || minimumDamageMultiplier > 1.f)
            failure = FalloutDamageMitigationFailure::InvalidMinimumMultiplier;
        else if (!std::isfinite(maximumDamageResistance) || maximumDamageResistance < 0.f
            || maximumDamageResistance > 100.f)
            failure = FalloutDamageMitigationFailure::InvalidResistanceCap;

        if (failure != FalloutDamageMitigationFailure::None)
            return std::nullopt;

        const float effectiveResistance = std::min(damageResistance, maximumDamageResistance);
        const float effectiveThreshold = std::max(0.f, damageThreshold);
        const float afterResistance = incomingDamage * (1.f - effectiveResistance / 100.f);
        const float minimumDamage = incomingDamage * minimumDamageMultiplier;
        const float reducedDamage = afterResistance - effectiveThreshold;
        const float healthDamage = std::max(reducedDamage, minimumDamage);
        if (!std::isfinite(afterResistance) || !std::isfinite(minimumDamage) || !std::isfinite(healthDamage))
        {
            failure = FalloutDamageMitigationFailure::InvalidDamage;
            return std::nullopt;
        }

        return FalloutDamageMitigation{ incomingDamage, effectiveResistance, effectiveThreshold,
            afterResistance, minimumDamage, healthDamage, reducedDamage <= minimumDamage };
    }

    std::optional<FalloutRangedDamage> buildFalloutRangedDamage(float authoredDamage, float skill,
        float normalizedCondition, const FalloutRangedDamageTuning& tuning, FalloutRangedDamageFailure& failure)
    {
        failure = FalloutRangedDamageFailure::None;
        if (!std::isfinite(authoredDamage) || authoredDamage < 0.f)
            failure = FalloutRangedDamageFailure::InvalidWeaponDamage;
        else if (!std::isfinite(skill) || skill < 0.f)
            failure = FalloutRangedDamageFailure::InvalidSkill;
        else if (!std::isfinite(normalizedCondition) || normalizedCondition < 0.f || normalizedCondition > 1.f)
            failure = FalloutRangedDamageFailure::InvalidCondition;
        else if (!std::isfinite(tuning.mWeaponDamageMultiplier) || tuning.mWeaponDamageMultiplier < 0.f
            || !std::isfinite(tuning.mSkillBase) || tuning.mSkillBase < 0.f
            || !std::isfinite(tuning.mSkillMultiplier) || tuning.mSkillMultiplier < 0.f
            || !std::isfinite(tuning.mConditionThreshold) || tuning.mConditionThreshold < 0.f
            || tuning.mConditionThreshold > 1.f || !std::isfinite(tuning.mConditionPenaltyRate)
            || tuning.mConditionPenaltyRate < 0.f)
            failure = FalloutRangedDamageFailure::InvalidTuning;

        if (failure != FalloutRangedDamageFailure::None)
            return std::nullopt;

        const float skillMultiplier = tuning.mSkillBase + tuning.mSkillMultiplier * (skill / 100.f);
        const float conditionMultiplier = normalizedCondition >= tuning.mConditionThreshold
            ? 1.f
            : std::max(0.f,
                1.f - (tuning.mConditionThreshold - normalizedCondition) * tuning.mConditionPenaltyRate);
        const float damage
            = authoredDamage * tuning.mWeaponDamageMultiplier * skillMultiplier * conditionMultiplier;
        if (!std::isfinite(skillMultiplier) || skillMultiplier < 0.f || !std::isfinite(conditionMultiplier)
            || !std::isfinite(damage) || damage < 0.f)
        {
            failure = FalloutRangedDamageFailure::InvalidDamage;
            return std::nullopt;
        }

        return FalloutRangedDamage{ authoredDamage, skill, skillMultiplier, normalizedCondition,
            conditionMultiplier, tuning.mWeaponDamageMultiplier, damage };
    }

    std::optional<FalloutExplosionDamage> resolveFalloutExplosionDamage(float authoredDamage,
        float damageMultiplier, float radius, float distance, FalloutExplosionDamageFailure& failure)
    {
        failure = FalloutExplosionDamageFailure::None;
        if (!std::isfinite(authoredDamage) || authoredDamage < 0.f)
            failure = FalloutExplosionDamageFailure::InvalidDamage;
        else if (!std::isfinite(damageMultiplier) || damageMultiplier < 0.f)
            failure = FalloutExplosionDamageFailure::InvalidMultiplier;
        else if (!std::isfinite(radius) || radius <= 0.f)
            failure = FalloutExplosionDamageFailure::InvalidRadius;
        else if (!std::isfinite(distance) || distance < 0.f)
            failure = FalloutExplosionDamageFailure::InvalidDistance;

        if (failure != FalloutExplosionDamageFailure::None)
            return std::nullopt;

        const float falloff = std::clamp(1.f - distance / radius, 0.f, 1.f);
        const float damage = authoredDamage * damageMultiplier * falloff;
        if (!std::isfinite(damage) || damage < 0.f)
        {
            failure = FalloutExplosionDamageFailure::InvalidResult;
            return std::nullopt;
        }
        return FalloutExplosionDamage{ authoredDamage, damageMultiplier, radius, distance, falloff, damage };
    }

    std::optional<osg::Vec3f> resolveFalloutProjectileBounce(const osg::Vec3f& velocity,
        const osg::Vec3f& collisionNormal, float bounciness, FalloutProjectileBounceFailure& failure)
    {
        failure = FalloutProjectileBounceFailure::None;
        const auto finiteVector = [](const osg::Vec3f& value) {
            return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
        };
        if (!finiteVector(velocity))
            failure = FalloutProjectileBounceFailure::InvalidVelocity;
        else if (!finiteVector(collisionNormal) || collisionNormal.length2() <= 0.f)
            failure = FalloutProjectileBounceFailure::InvalidNormal;
        else if (!std::isfinite(bounciness) || bounciness < 0.f)
            failure = FalloutProjectileBounceFailure::InvalidBounciness;
        if (failure != FalloutProjectileBounceFailure::None)
            return std::nullopt;

        osg::Vec3f normal = collisionNormal;
        normal.normalize();
        const float normalSpeed = velocity * normal;
        const osg::Vec3f result
            = normalSpeed < 0.f ? velocity - normal * ((1.f + bounciness) * normalSpeed) : velocity;
        if (!finiteVector(result))
        {
            failure = FalloutProjectileBounceFailure::InvalidResult;
            return std::nullopt;
        }
        return result;
    }

    std::optional<FalloutProjectileTrigger> buildFalloutProjectileTrigger(
        const ESM4::Projectile& projectile, float projectileSkill, float minesDelayMin,
        float exteriorRadiusMultiplier, bool exterior, FalloutProjectileTriggerFailure& failure)
    {
        failure = FalloutProjectileTriggerFailure::None;
        if (!projectile.mData.present)
            failure = FalloutProjectileTriggerFailure::MissingData;
        else if ((projectile.mData.flags & ESM4::Projectile::Explosion) == 0
            || projectile.mData.explosion.isZeroOrUnset())
            failure = FalloutProjectileTriggerFailure::MissingExplosion;
        else if (!std::isfinite(projectileSkill) || projectileSkill < 0.f)
            failure = FalloutProjectileTriggerFailure::InvalidSkill;
        else if (!std::isfinite(minesDelayMin) || minesDelayMin < 0.f
            || !std::isfinite(exteriorRadiusMultiplier) || exteriorRadiusMultiplier < 0.f)
            failure = FalloutProjectileTriggerFailure::InvalidTuning;
        else if (!std::isfinite(projectile.mData.alternateTimer)
            || projectile.mData.alternateTimer < 0.f)
            failure = FalloutProjectileTriggerFailure::InvalidTimer;
        else if (!std::isfinite(projectile.mData.alternateProximity)
            || projectile.mData.alternateProximity < 0.f)
            failure = FalloutProjectileTriggerFailure::InvalidProximity;
        if (failure != FalloutProjectileTriggerFailure::None)
            return std::nullopt;

        FalloutProjectileTrigger result;
        if ((projectile.mData.flags & ESM4::Projectile::Detonates) != 0)
            result.mMode = FalloutProjectileTriggerMode::Remote;
        else if ((projectile.mData.flags & ESM4::Projectile::AlternateTrigger) == 0)
            result.mMode = FalloutProjectileTriggerMode::Impact;
        else if (projectile.mData.alternateProximity > 0.f)
        {
            result.mMode = FalloutProjectileTriggerMode::Proximity;
            result.mDelay = minesDelayMin
                + projectile.mData.alternateTimer * (std::min(projectileSkill, 100.f) / 100.f);
            result.mProximityRadius = projectile.mData.alternateProximity
                * (exterior ? exteriorRadiusMultiplier : 1.f);
        }
        else
        {
            result.mMode = FalloutProjectileTriggerMode::Timed;
            result.mDelay = projectile.mData.alternateTimer;
            if (result.mDelay <= 0.f)
            {
                failure = FalloutProjectileTriggerFailure::InvalidTimer;
                return std::nullopt;
            }
        }

        if (!std::isfinite(result.mDelay) || result.mDelay < 0.f
            || !std::isfinite(result.mProximityRadius) || result.mProximityRadius < 0.f)
        {
            failure = FalloutProjectileTriggerFailure::InvalidResult;
            return std::nullopt;
        }
        return result;
    }

    std::optional<FalloutCriticalContract> buildFalloutCriticalContract(const ESM4::Weapon& weapon,
        float actorCriticalChance, bool vats, float vatsCriticalChanceBonus, FalloutCriticalFailure& failure)
    {
        failure = FalloutCriticalFailure::None;
        if (!weapon.mCriticalData.present)
            failure = FalloutCriticalFailure::MissingCriticalData;
        else if (!std::isfinite(actorCriticalChance) || actorCriticalChance < 0.f)
            failure = FalloutCriticalFailure::InvalidActorChance;
        else if (!std::isfinite(weapon.mCriticalData.chanceMultiplier)
            || weapon.mCriticalData.chanceMultiplier < 0.f)
            failure = FalloutCriticalFailure::InvalidWeaponMultiplier;
        else if (weapon.mData.isAutomatic()
            && (!std::isfinite(weapon.mData.fireRate) || weapon.mData.fireRate <= 0.f))
            failure = FalloutCriticalFailure::InvalidAutomaticFireRate;
        else if (!std::isfinite(vatsCriticalChanceBonus) || vatsCriticalChanceBonus < 0.f)
            failure = FalloutCriticalFailure::InvalidVatsBonus;

        if (failure != FalloutCriticalFailure::None)
            return std::nullopt;

        const float weaponMultiplier = weapon.mData.isAutomatic()
            ? weapon.mCriticalData.chanceMultiplier / weapon.mData.fireRate
            : weapon.mCriticalData.chanceMultiplier;
        const float chance = weapon.mCriticalData.chanceMultiplier == 0.f
            ? 0.f
            : actorCriticalChance * weaponMultiplier + (vats ? vatsCriticalChanceBonus : 0.f);
        if (!std::isfinite(chance) || chance < 0.f)
        {
            failure = FalloutCriticalFailure::InvalidChance;
            return std::nullopt;
        }

        return FalloutCriticalContract{ std::min(chance, 100.f),
            static_cast<float>(weapon.mCriticalData.damage), weapon.mCriticalData.effect,
            (weapon.mCriticalData.flags & ESM4::Weapon::CriticalData::OnDeath) != 0 };
    }

    bool doesFalloutCriticalHit(float chancePercent, float roll) noexcept
    {
        if (!std::isfinite(chancePercent) || chancePercent <= 0.f || !std::isfinite(roll) || roll < 0.f
            || roll >= 1.f)
            return false;
        return roll * 100.f < std::min(chancePercent, 100.f);
    }

    std::optional<float> applyFalloutAmmoEffects(float baseValue, ESM4::AmmoEffect::Type type,
        std::span<const ESM4::AmmoEffect* const> effects, FalloutAmmoEffectFailure& failure)
    {
        failure = FalloutAmmoEffectFailure::None;
        if (!std::isfinite(baseValue))
        {
            failure = FalloutAmmoEffectFailure::InvalidBaseValue;
            return std::nullopt;
        }

        float result = baseValue;
        for (const ESM4::AmmoEffect* effect : effects)
        {
            if (effect == nullptr)
            {
                failure = FalloutAmmoEffectFailure::MissingEffect;
                return std::nullopt;
            }
            if (effect->mType != type)
                continue;
            if (!std::isfinite(effect->mValue))
            {
                failure = FalloutAmmoEffectFailure::InvalidEffectValue;
                return std::nullopt;
            }

            switch (effect->mOperation)
            {
                case ESM4::AmmoEffect::Operation::Add:
                    result += effect->mValue;
                    break;
                case ESM4::AmmoEffect::Operation::Multiply:
                    result *= effect->mValue;
                    break;
                case ESM4::AmmoEffect::Operation::Subtract:
                    result -= effect->mValue;
                    break;
                default:
                    failure = FalloutAmmoEffectFailure::InvalidOperation;
                    return std::nullopt;
            }
            if (!std::isfinite(result))
            {
                failure = FalloutAmmoEffectFailure::InvalidResult;
                return std::nullopt;
            }
        }
        return result;
    }

    std::optional<FalloutWeaponDegradation> buildFalloutWeaponDegradation(
        const ESM4::Weapon& weapon, std::span<const ESM4::AmmoEffect* const> effects,
        float damageToWeaponGameSetting, bool vats, float vatsDamageToWeaponMultiplier,
        FalloutWeaponDegradationFailure& failure)
    {
        failure = FalloutWeaponDegradationFailure::None;
        const bool override = (weapon.mData.flags2 & ESM4::Weapon::Data::OverrideDamageToWeapon) != 0;
        const float baseLoss = override ? weapon.mData.damageToWeaponMult : damageToWeaponGameSetting;
        if (!override && (!std::isfinite(damageToWeaponGameSetting) || damageToWeaponGameSetting < 0.f))
            failure = FalloutWeaponDegradationFailure::InvalidGameSetting;
        else if (override && (!std::isfinite(baseLoss) || baseLoss < 0.f))
            failure = FalloutWeaponDegradationFailure::InvalidWeaponOverride;
        else if (vats && (!std::isfinite(vatsDamageToWeaponMultiplier) || vatsDamageToWeaponMultiplier < 0.f))
            failure = FalloutWeaponDegradationFailure::InvalidVatsMultiplier;
        if (failure != FalloutWeaponDegradationFailure::None)
            return std::nullopt;

        FalloutAmmoEffectFailure effectFailure = FalloutAmmoEffectFailure::None;
        const std::optional<float> ammoAdjusted = applyFalloutAmmoEffects(
            baseLoss, ESM4::AmmoEffect::Type::WeaponCondition, effects, effectFailure);
        if (!ammoAdjusted)
        {
            failure = FalloutWeaponDegradationFailure::InvalidAmmoEffect;
            return std::nullopt;
        }

        const float vatsMultiplier = vats ? vatsDamageToWeaponMultiplier : 1.f;
        const float conditionLoss = *ammoAdjusted * vatsMultiplier;
        if (!std::isfinite(conditionLoss) || conditionLoss < 0.f)
        {
            failure = FalloutWeaponDegradationFailure::InvalidResult;
            return std::nullopt;
        }
        return FalloutWeaponDegradation{ baseLoss, *ammoAdjusted, vatsMultiplier, conditionLoss, override };
    }

    std::optional<float> resolveFalloutArmorConditionMultiplier(
        float normalizedCondition, float penaltyRate) noexcept
    {
        if (!std::isfinite(normalizedCondition) || normalizedCondition < 0.f || normalizedCondition > 1.f
            || !std::isfinite(penaltyRate) || penaltyRate < 0.f)
            return std::nullopt;
        if (normalizedCondition >= 0.5f)
            return 1.f;
        const float multiplier = 1.f - (0.5f - normalizedCondition) * penaltyRate;
        return std::isfinite(multiplier) ? std::optional<float>(std::max(0.f, multiplier)) : std::nullopt;
    }

    std::optional<FalloutVatsWeaponContract> buildFalloutVatsWeaponContract(
        const ESM4::Weapon& weapon, FalloutVatsWeaponFailure& failure)
    {
        failure = FalloutVatsWeaponFailure::None;
        constexpr std::uint32_t overrideActionPointsFlag = 0x00000008;
        if (!weapon.mData.hasBallistics)
            failure = FalloutVatsWeaponFailure::MissingBallistics;
        else if ((weapon.mData.flags2 & overrideActionPointsFlag) == 0)
            failure = FalloutVatsWeaponFailure::MissingAuthoredActionPointOverride;
        else if (!std::isfinite(weapon.mData.overrideActionPoints) || weapon.mData.overrideActionPoints <= 0.f)
            failure = FalloutVatsWeaponFailure::InvalidActionPointCost;
        else if (weapon.mData.baseVatsChance > 100)
            failure = FalloutVatsWeaponFailure::InvalidBaseHitChance;
        else if (!std::isfinite(weapon.mData.limbDamageMult) || weapon.mData.limbDamageMult < 0.f)
            failure = FalloutVatsWeaponFailure::InvalidLimbDamageMultiplier;
        else if (weapon.mData.skillActorValue < 0)
            failure = FalloutVatsWeaponFailure::InvalidSkillActorValue;

        if (failure != FalloutVatsWeaponFailure::None)
            return std::nullopt;
        return FalloutVatsWeaponContract{ weapon.mData.overrideActionPoints, weapon.mData.baseVatsChance,
            weapon.mData.limbDamageMult, weapon.mData.skillActorValue };
    }

    float getFalloutVatsReservedActionPoints(std::span<const FalloutVatsQueuedAction> queued) noexcept
    {
        float result = 0.f;
        for (const FalloutVatsQueuedAction& action : queued)
        {
            if (!std::isfinite(action.mActionPointCost) || action.mActionPointCost <= 0.f)
                return std::numeric_limits<float>::quiet_NaN();
            result += action.mActionPointCost;
        }
        return result;
    }

    bool doesFalloutVatsAttackHit(std::uint8_t displayedHitChance, float roll) noexcept
    {
        if (!std::isfinite(roll) || roll < 0.f || roll >= 1.f)
            return false;
        return displayedHitChance == 100
            || (displayedHitChance != 0 && roll * 100.f < static_cast<float>(displayedHitChance));
    }

    std::optional<FalloutVatsQueuedAction> queueFalloutVatsAction(
        std::span<const FalloutVatsQueuedAction> queued, ESM::FormId target, std::uint8_t bodyPart,
        unsigned int displayedHitChance, float bodyPartDamageMultiplier, std::uint8_t bodyPartHealthPercent,
        float currentActionPoints,
        std::size_t availableShots, const FalloutVatsWeaponContract& weapon, FalloutVatsQueueFailure& failure)
    {
        failure = FalloutVatsQueueFailure::None;
        if (target.isZeroOrUnset())
            failure = FalloutVatsQueueFailure::MissingTarget;
        else if (bodyPart > 14)
            failure = FalloutVatsQueueFailure::InvalidBodyPart;
        else if (displayedHitChance > 100)
            failure = FalloutVatsQueueFailure::InvalidDisplayedHitChance;
        else if (!std::isfinite(currentActionPoints) || currentActionPoints < 0.f
            || !std::isfinite(bodyPartDamageMultiplier) || bodyPartDamageMultiplier <= 0.f)
            failure = FalloutVatsQueueFailure::InvalidActionPointState;
        else if (queued.size() >= 16)
            failure = FalloutVatsQueueFailure::QueueLimitReached;
        else if (queued.size() >= availableShots)
            failure = FalloutVatsQueueFailure::InsufficientAmmunition;

        const float reserved = failure == FalloutVatsQueueFailure::None
            ? getFalloutVatsReservedActionPoints(queued) : 0.f;
        if (failure == FalloutVatsQueueFailure::None
            && (!std::isfinite(reserved) || !std::isfinite(weapon.mActionPointCost)
                || weapon.mActionPointCost <= 0.f))
            failure = FalloutVatsQueueFailure::InvalidActionPointState;
        else if (failure == FalloutVatsQueueFailure::None
            && reserved + weapon.mActionPointCost > currentActionPoints)
            failure = FalloutVatsQueueFailure::InsufficientActionPoints;

        if (failure != FalloutVatsQueueFailure::None)
            return std::nullopt;
        return FalloutVatsQueuedAction{ target, bodyPart, static_cast<std::uint8_t>(displayedHitChance),
            weapon.mActionPointCost, bodyPartDamageMultiplier, weapon.mLimbDamageMultiplier,
            bodyPartHealthPercent };
    }

    std::optional<FalloutVatsBodyPartContract> buildFalloutVatsBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index, FalloutVatsBodyPartFailure& failure)
    {
        failure = FalloutVatsBodyPartFailure::None;
        if (bodyPart.mPartName.empty())
            failure = FalloutVatsBodyPartFailure::MissingName;
        else if (bodyPart.mVATSTarget.empty())
            failure = FalloutVatsBodyPartFailure::MissingTargetNode;
        else if (!isFalloutLimbActorValue(bodyPart.mData.actorValue))
            failure = FalloutVatsBodyPartFailure::InvalidActorValue;
        else if (bodyPart.mData.toHitChance > 100)
            failure = FalloutVatsBodyPartFailure::InvalidHitChance;
        else if (!std::isfinite(bodyPart.mData.damageMult) || bodyPart.mData.damageMult <= 0.f)
            failure = FalloutVatsBodyPartFailure::InvalidDamageMultiplier;
        if (failure != FalloutVatsBodyPartFailure::None)
            return std::nullopt;
        return FalloutVatsBodyPartContract{ index, bodyPart.mPartName, bodyPart.mVATSTarget,
            bodyPart.mData.actorValue, bodyPart.mData.toHitChance, bodyPart.mData.healthPercent,
            bodyPart.mData.damageMult, (bodyPart.mData.flags & 0x40) != 0 };
    }

    const ESM4::BodyPartData* getFalloutActorBodyPartData(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
            return nullptr;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (world == nullptr || store == nullptr)
            return nullptr;

        if (actor.getType() == ESM4::Npc::sRecordId)
        {
            if (actor == world->getPlayerPtr())
                return findFalloutBodyPartData(*store, "PlayerBodyPartData");

            const ESM4::Npc* npc = actor.get<ESM4::Npc>()->mBase;
            const ESM4::Race* race = store->get<ESM4::Race>().search(npc->mRace);
            if (race == nullptr)
                return nullptr;
            if (!race->mBodyPartData.isZeroOrUnset())
                return store->get<ESM4::BodyPartData>().search(race->mBodyPartData);
            return findFalloutBodyPartData(*store, "DefaultBodyPartData");
        }
        if (actor.getType() == ESM4::Creature::sRecordId)
            return MWClass::ESM4Creature::getBodyPartData(actor);
        return nullptr;
    }

    std::optional<FalloutBodyPartContract> buildFalloutBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index) noexcept
    {
        if (index > 14 || bodyPart.mPartName.empty() || !isFalloutLimbActorValue(bodyPart.mData.actorValue)
            || !std::isfinite(bodyPart.mData.damageMult) || bodyPart.mData.damageMult <= 0.f)
            return std::nullopt;
        return FalloutBodyPartContract{ index, bodyPart.mPartName, bodyPart.mNodeName, bodyPart.mVATSTarget,
            bodyPart.mIKStartNode, bodyPart.mGoreEffectsTarget, bodyPart.mData.actorValue,
            bodyPart.mData.healthPercent, bodyPart.mData.damageMult };
    }

    std::optional<FalloutBodyPartContract> resolveFalloutBodyPartFromNodePath(
        const ESM4::BodyPartData& bodyPartData, std::span<const std::string> nodePath) noexcept
    {
        for (auto node = nodePath.rbegin(); node != nodePath.rend(); ++node)
        {
            for (std::size_t index = 0; index < bodyPartData.mBodyParts.size() && index <= 14; ++index)
            {
                const auto& bodyPart = bodyPartData.mBodyParts[index];
                if (!matchesFalloutBodyPartNode(bodyPart, *node))
                    continue;
                if (const auto contract
                    = buildFalloutBodyPartContract(bodyPart, static_cast<std::uint8_t>(index)))
                    return contract;
            }
        }
        return std::nullopt;
    }

    std::optional<FalloutLimbImpact> resolveFalloutLimbImpact(float actorMaximumHealth,
        std::uint8_t bodyPartHealthPercent, float damageTakenBefore, float rawHitDamage,
        float weaponLimbDamageMultiplier, float targetLimbDamageMultiplier) noexcept
    {
        if (!std::isfinite(actorMaximumHealth) || actorMaximumHealth <= 0.f || bodyPartHealthPercent == 0
            || !std::isfinite(damageTakenBefore) || damageTakenBefore < 0.f
            || !std::isfinite(rawHitDamage) || rawHitDamage < 0.f
            || !std::isfinite(weaponLimbDamageMultiplier) || weaponLimbDamageMultiplier < 0.f
            || !std::isfinite(targetLimbDamageMultiplier) || targetLimbDamageMultiplier < 0.f)
            return std::nullopt;

        const float maximumCondition
            = actorMaximumHealth * static_cast<float>(bodyPartHealthPercent) * 0.01f;
        const float damageApplied = rawHitDamage * weaponLimbDamageMultiplier * targetLimbDamageMultiplier;
        const float damageTakenAfter = damageTakenBefore + damageApplied;
        if (!std::isfinite(maximumCondition) || maximumCondition <= 0.f || !std::isfinite(damageApplied)
            || !std::isfinite(damageTakenAfter))
            return std::nullopt;

        const float conditionBefore = std::max(0.f, maximumCondition - damageTakenBefore);
        const float conditionAfter = std::max(0.f, maximumCondition - damageTakenAfter);
        return FalloutLimbImpact{ maximumCondition, conditionBefore, conditionAfter, damageApplied,
            damageTakenAfter, damageApplied > 0.f && conditionBefore > 0.f && conditionAfter == 0.f };
    }

    bool FalloutVatsRuntime::enter(float currentActionPoints) noexcept
    {
        cancel();
        if (!std::isfinite(currentActionPoints) || currentActionPoints < 0.f)
            return false;
        mActionPointsBefore = currentActionPoints;
        mPhase = FalloutVatsPhase::Targeting;
        return true;
    }

    void FalloutVatsRuntime::cancel() noexcept
    {
        mPhase = FalloutVatsPhase::Inactive;
        mActionPointsBefore = 0.f;
        mSelectedTarget = {};
        mSelectedBodyPart = 0;
        mDisplayedHitChance = 0;
        mSelectedHealthPercent = 0;
        mSelectedHealthDamageMultiplier = 1.f;
        mSelectedBodyPartName.clear();
        mSelectedTargetNode.clear();
        mSelectedActorValue = -1;
        mQueue.clear();
        mExecutionIndex = 0;
    }

    bool FalloutVatsRuntime::select(ESM::FormId target, const FalloutVatsBodyPartContract& bodyPart,
        unsigned int displayedHitChance) noexcept
    {
        if (mPhase != FalloutVatsPhase::Targeting || target.isZeroOrUnset() || displayedHitChance > 100)
            return false;
        mSelectedTarget = target;
        mSelectedBodyPart = bodyPart.mIndex;
        mDisplayedHitChance = displayedHitChance;
        mSelectedHealthPercent = bodyPart.mHealthPercent;
        mSelectedHealthDamageMultiplier = bodyPart.mHealthDamageMultiplier;
        mSelectedBodyPartName = bodyPart.mName;
        mSelectedTargetNode = bodyPart.mTargetNode;
        mSelectedActorValue = bodyPart.mActorValue;
        return true;
    }

    bool FalloutVatsRuntime::queueSelected(
        const FalloutVatsWeaponContract& weapon, std::size_t availableShots, FalloutVatsQueueFailure& failure)
    {
        if (mPhase != FalloutVatsPhase::Targeting)
        {
            failure = FalloutVatsQueueFailure::MissingTarget;
            return false;
        }
        const std::optional<FalloutVatsQueuedAction> action = queueFalloutVatsAction(mQueue, mSelectedTarget,
            mSelectedBodyPart, mDisplayedHitChance, mSelectedHealthDamageMultiplier, mSelectedHealthPercent,
            mActionPointsBefore, availableShots, weapon, failure);
        if (!action)
            return false;
        mQueue.push_back(*action);
        mQueue.back().mBodyPartName = mSelectedBodyPartName;
        mQueue.back().mTargetNode = mSelectedTargetNode;
        mQueue.back().mActorValue = mSelectedActorValue;
        return true;
    }

    std::optional<float> FalloutVatsRuntime::beginExecution() noexcept
    {
        if (mPhase != FalloutVatsPhase::Targeting || mQueue.empty())
            return std::nullopt;
        const float after = getActionPointsAfter();
        if (!std::isfinite(after) || after < 0.f)
            return std::nullopt;
        mPhase = FalloutVatsPhase::Executing;
        mExecutionIndex = 0;
        return after;
    }

    const FalloutVatsQueuedAction* FalloutVatsRuntime::getExecutingAction() const noexcept
    {
        if (mPhase != FalloutVatsPhase::Executing || mExecutionIndex >= mQueue.size())
            return nullptr;
        return &mQueue[mExecutionIndex];
    }

    bool FalloutVatsRuntime::advanceExecution() noexcept
    {
        if (getExecutingAction() == nullptr)
            return false;
        ++mExecutionIndex;
        return true;
    }

    bool FalloutVatsRuntime::isExecutionComplete() const noexcept
    {
        return mPhase == FalloutVatsPhase::Executing && mExecutionIndex >= mQueue.size();
    }

    bool FalloutVatsRuntime::finishExecution() noexcept
    {
        if (!isExecutionComplete())
            return false;
        cancel();
        return true;
    }

    float FalloutVatsRuntime::getReservedActionPoints() const noexcept
    {
        return MWMechanics::getFalloutVatsReservedActionPoints(mQueue);
    }

    float FalloutVatsRuntime::getActionPointsAfter() const noexcept
    {
        return mActionPointsBefore - getReservedActionPoints();
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

    std::string_view getFalloutAiCombatRangeFailureName(FalloutAiCombatRangeFailure failure)
    {
        switch (failure)
        {
            case FalloutAiCombatRangeFailure::None:
                return "none";
            case FalloutAiCombatRangeFailure::InvalidTuning:
                return "invalid-tuning";
            case FalloutAiCombatRangeFailure::MissingBallistics:
                return "missing-ballistics";
            case FalloutAiCombatRangeFailure::InvalidWeaponRange:
                return "invalid-weapon-range";
            case FalloutAiCombatRangeFailure::InvalidWeaponReach:
                return "invalid-weapon-reach";
        }
        return "unknown";
    }

    std::string_view getFalloutDamageMitigationFailureName(FalloutDamageMitigationFailure failure)
    {
        switch (failure)
        {
            case FalloutDamageMitigationFailure::None:
                return "none";
            case FalloutDamageMitigationFailure::InvalidDamage:
                return "invalid-damage";
            case FalloutDamageMitigationFailure::InvalidResistance:
                return "invalid-resistance";
            case FalloutDamageMitigationFailure::InvalidThreshold:
                return "invalid-threshold";
            case FalloutDamageMitigationFailure::InvalidMinimumMultiplier:
                return "invalid-minimum-multiplier";
            case FalloutDamageMitigationFailure::InvalidResistanceCap:
                return "invalid-resistance-cap";
        }
        return "unknown";
    }

    std::string_view getFalloutRangedDamageFailureName(FalloutRangedDamageFailure failure)
    {
        switch (failure)
        {
            case FalloutRangedDamageFailure::None:
                return "none";
            case FalloutRangedDamageFailure::InvalidWeaponDamage:
                return "invalid-weapon-damage";
            case FalloutRangedDamageFailure::InvalidSkill:
                return "invalid-skill";
            case FalloutRangedDamageFailure::InvalidCondition:
                return "invalid-condition";
            case FalloutRangedDamageFailure::InvalidTuning:
                return "invalid-tuning";
            case FalloutRangedDamageFailure::InvalidDamage:
                return "invalid-damage";
        }
        return "unknown";
    }

    std::string_view getFalloutExplosionDamageFailureName(FalloutExplosionDamageFailure failure)
    {
        switch (failure)
        {
            case FalloutExplosionDamageFailure::None:
                return "none";
            case FalloutExplosionDamageFailure::InvalidDamage:
                return "invalid-damage";
            case FalloutExplosionDamageFailure::InvalidMultiplier:
                return "invalid-multiplier";
            case FalloutExplosionDamageFailure::InvalidRadius:
                return "invalid-radius";
            case FalloutExplosionDamageFailure::InvalidDistance:
                return "invalid-distance";
            case FalloutExplosionDamageFailure::InvalidResult:
                return "invalid-result";
        }
        return "unknown";
    }

    std::string_view getFalloutProjectileBounceFailureName(FalloutProjectileBounceFailure failure)
    {
        switch (failure)
        {
            case FalloutProjectileBounceFailure::None:
                return "none";
            case FalloutProjectileBounceFailure::InvalidVelocity:
                return "invalid-velocity";
            case FalloutProjectileBounceFailure::InvalidNormal:
                return "invalid-normal";
            case FalloutProjectileBounceFailure::InvalidBounciness:
                return "invalid-bounciness";
            case FalloutProjectileBounceFailure::InvalidResult:
                return "invalid-result";
        }
        return "unknown";
    }

    std::string_view getFalloutProjectileTriggerFailureName(FalloutProjectileTriggerFailure failure)
    {
        switch (failure)
        {
            case FalloutProjectileTriggerFailure::None:
                return "none";
            case FalloutProjectileTriggerFailure::MissingData:
                return "missing-data";
            case FalloutProjectileTriggerFailure::MissingExplosion:
                return "missing-explosion";
            case FalloutProjectileTriggerFailure::InvalidSkill:
                return "invalid-skill";
            case FalloutProjectileTriggerFailure::InvalidTuning:
                return "invalid-tuning";
            case FalloutProjectileTriggerFailure::InvalidTimer:
                return "invalid-timer";
            case FalloutProjectileTriggerFailure::InvalidProximity:
                return "invalid-proximity";
            case FalloutProjectileTriggerFailure::InvalidResult:
                return "invalid-result";
        }
        return "unknown";
    }

    std::string_view getFalloutCriticalFailureName(FalloutCriticalFailure failure)
    {
        switch (failure)
        {
            case FalloutCriticalFailure::None:
                return "none";
            case FalloutCriticalFailure::MissingCriticalData:
                return "missing-critical-data";
            case FalloutCriticalFailure::InvalidActorChance:
                return "invalid-actor-chance";
            case FalloutCriticalFailure::InvalidWeaponMultiplier:
                return "invalid-weapon-multiplier";
            case FalloutCriticalFailure::InvalidAutomaticFireRate:
                return "invalid-automatic-fire-rate";
            case FalloutCriticalFailure::InvalidVatsBonus:
                return "invalid-vats-bonus";
            case FalloutCriticalFailure::InvalidChance:
                return "invalid-chance";
        }
        return "unknown";
    }

    std::string_view getFalloutAmmoEffectFailureName(FalloutAmmoEffectFailure failure)
    {
        switch (failure)
        {
            case FalloutAmmoEffectFailure::None:
                return "none";
            case FalloutAmmoEffectFailure::InvalidBaseValue:
                return "invalid-base-value";
            case FalloutAmmoEffectFailure::MissingEffect:
                return "missing-effect";
            case FalloutAmmoEffectFailure::InvalidEffectValue:
                return "invalid-effect-value";
            case FalloutAmmoEffectFailure::InvalidOperation:
                return "invalid-operation";
            case FalloutAmmoEffectFailure::InvalidResult:
                return "invalid-result";
        }
        return "unknown";
    }

    std::string_view getFalloutWeaponDegradationFailureName(FalloutWeaponDegradationFailure failure)
    {
        switch (failure)
        {
            case FalloutWeaponDegradationFailure::None:
                return "none";
            case FalloutWeaponDegradationFailure::InvalidGameSetting:
                return "invalid-game-setting";
            case FalloutWeaponDegradationFailure::InvalidWeaponOverride:
                return "invalid-weapon-override";
            case FalloutWeaponDegradationFailure::InvalidVatsMultiplier:
                return "invalid-vats-multiplier";
            case FalloutWeaponDegradationFailure::InvalidAmmoEffect:
                return "invalid-ammo-effect";
            case FalloutWeaponDegradationFailure::InvalidResult:
                return "invalid-result";
        }
        return "unknown";
    }

    std::string_view getFalloutVatsWeaponFailureName(FalloutVatsWeaponFailure failure)
    {
        switch (failure)
        {
            case FalloutVatsWeaponFailure::None:
                return "none";
            case FalloutVatsWeaponFailure::MissingBallistics:
                return "missing-ballistics";
            case FalloutVatsWeaponFailure::MissingAuthoredActionPointOverride:
                return "missing-authored-action-point-override";
            case FalloutVatsWeaponFailure::InvalidActionPointCost:
                return "invalid-action-point-cost";
            case FalloutVatsWeaponFailure::InvalidBaseHitChance:
                return "invalid-base-hit-chance";
            case FalloutVatsWeaponFailure::InvalidLimbDamageMultiplier:
                return "invalid-limb-damage-multiplier";
            case FalloutVatsWeaponFailure::InvalidSkillActorValue:
                return "invalid-skill-actor-value";
        }
        return "unknown";
    }

    std::string_view getFalloutVatsQueueFailureName(FalloutVatsQueueFailure failure)
    {
        switch (failure)
        {
            case FalloutVatsQueueFailure::None:
                return "none";
            case FalloutVatsQueueFailure::MissingTarget:
                return "missing-target";
            case FalloutVatsQueueFailure::InvalidBodyPart:
                return "invalid-body-part";
            case FalloutVatsQueueFailure::InvalidDisplayedHitChance:
                return "invalid-displayed-hit-chance";
            case FalloutVatsQueueFailure::InvalidActionPointState:
                return "invalid-action-point-state";
            case FalloutVatsQueueFailure::InsufficientActionPoints:
                return "insufficient-action-points";
            case FalloutVatsQueueFailure::InsufficientAmmunition:
                return "insufficient-ammunition";
            case FalloutVatsQueueFailure::QueueLimitReached:
                return "queue-limit-reached";
        }
        return "unknown";
    }

    std::string_view getFalloutVatsBodyPartFailureName(FalloutVatsBodyPartFailure failure)
    {
        switch (failure)
        {
            case FalloutVatsBodyPartFailure::None:
                return "none";
            case FalloutVatsBodyPartFailure::MissingName:
                return "missing-name";
            case FalloutVatsBodyPartFailure::MissingTargetNode:
                return "missing-target-node";
            case FalloutVatsBodyPartFailure::InvalidActorValue:
                return "invalid-actor-value";
            case FalloutVatsBodyPartFailure::InvalidHitChance:
                return "invalid-hit-chance";
            case FalloutVatsBodyPartFailure::InvalidDamageMultiplier:
                return "invalid-damage-multiplier";
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
