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
        unsigned int displayedHitChance, float bodyPartDamageMultiplier, float currentActionPoints,
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
            weapon.mActionPointCost, bodyPartDamageMultiplier * weapon.mLimbDamageMultiplier };
    }

    std::optional<FalloutVatsBodyPartContract> buildFalloutVatsBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index, FalloutVatsBodyPartFailure& failure)
    {
        failure = FalloutVatsBodyPartFailure::None;
        if (bodyPart.mPartName.empty())
            failure = FalloutVatsBodyPartFailure::MissingName;
        else if (bodyPart.mVATSTarget.empty())
            failure = FalloutVatsBodyPartFailure::MissingTargetNode;
        else if (bodyPart.mData.actorValue < 0)
            failure = FalloutVatsBodyPartFailure::InvalidActorValue;
        else if (bodyPart.mData.toHitChance > 100)
            failure = FalloutVatsBodyPartFailure::InvalidHitChance;
        else if (!std::isfinite(bodyPart.mData.damageMult) || bodyPart.mData.damageMult <= 0.f)
            failure = FalloutVatsBodyPartFailure::InvalidDamageMultiplier;
        if (failure != FalloutVatsBodyPartFailure::None)
            return std::nullopt;
        return FalloutVatsBodyPartContract{ index, bodyPart.mPartName, bodyPart.mVATSTarget,
            bodyPart.mData.actorValue, bodyPart.mData.toHitChance, bodyPart.mData.damageMult,
            (bodyPart.mData.flags & 0x40) != 0 };
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
        mSelectedDamageMultiplier = 1.f;
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
        mSelectedDamageMultiplier = bodyPart.mDamageMultiplier;
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
            mSelectedBodyPart, mDisplayedHitChance, mSelectedDamageMultiplier, mActionPointsBefore,
            availableShots, weapon, failure);
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
