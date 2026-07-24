#ifndef OPENMW_MWMECHANICS_FALLOUTCOMBAT_H
#define OPENMW_MWMECHANICS_FALLOUTCOMBAT_H

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/esm/formid.hpp>
#include <components/esm4/actor.hpp>
#include <components/esm4/loadamef.hpp>
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadmgef.hpp>
#include <components/esm4/loadspel.hpp>

namespace ESM4
{
    struct Projectile;
    struct Script;
    struct Weapon;
}

namespace MWWorld
{
    class Ptr;
}

namespace MWMechanics
{
    enum class FalloutShotFailure
    {
        None,
        MissingAmmo,
        MissingBallistics,
        ProjectileMismatch,
        MissingProjectileData,
        InvalidAmmoUse,
        InvalidProjectileCount,
        InvalidRange,
        InvalidSpread,
    };

    enum class FalloutMeleeFailure
    {
        None,
        NotMelee,
        MissingWeapon,
        InvalidActorValues,
        InvalidTuning,
        InvalidDamage,
        InvalidReach,
    };

    enum class FalloutFireCadenceFailure
    {
        None,
        InvalidAnimationMultiplier,
        InvalidAttackMultiplier,
        InvalidFireRate,
        InvalidShotsPerSecond,
    };

    enum class FalloutAiCombatRangeFailure
    {
        None,
        InvalidTuning,
        MissingBallistics,
        InvalidWeaponRange,
        InvalidWeaponReach,
    };

    struct FalloutAiCombatRange
    {
        float mDistance = 0.f;
        bool mRanged = false;
    };

    enum class FalloutDamageMitigationFailure
    {
        None,
        InvalidDamage,
        InvalidResistance,
        InvalidThreshold,
        InvalidMinimumMultiplier,
        InvalidResistanceCap,
    };

    enum class FalloutRangedDamageFailure
    {
        None,
        InvalidWeaponDamage,
        InvalidSkill,
        InvalidCondition,
        InvalidTuning,
        InvalidDamage,
    };

    enum class FalloutExplosionDamageFailure
    {
        None,
        InvalidDamage,
        InvalidMultiplier,
        InvalidRadius,
        InvalidDistance,
        InvalidResult,
    };

    enum class FalloutExplosionKnockdownFailure
    {
        None,
        InvalidDamage,
        InvalidHealth,
        InvalidAgility,
        InvalidTuning,
        InvalidResult,
    };

    enum class FalloutExplosionKnockdownMode
    {
        None,
        Forced,
        Chance,
    };

    enum class FalloutProjectileBounceFailure
    {
        None,
        InvalidVelocity,
        InvalidNormal,
        InvalidBounciness,
        InvalidResult,
    };

    enum class FalloutBallisticAimFailure
    {
        None,
        InvalidDisplacement,
        InvalidSpeed,
        InvalidGravity,
        Unreachable,
        InvalidResult,
    };

    enum class FalloutProjectileTriggerFailure
    {
        None,
        MissingData,
        MissingExplosion,
        InvalidSkill,
        InvalidTuning,
        InvalidTimer,
        InvalidProximity,
        InvalidResult,
    };

    enum class FalloutProjectileTriggerMode
    {
        Impact,
        Timed,
        Proximity,
        Remote,
    };

    struct FalloutProjectileTrigger
    {
        FalloutProjectileTriggerMode mMode = FalloutProjectileTriggerMode::Impact;
        float mDelay = 0.f;
        float mProximityRadius = 0.f;
    };

    enum class FalloutWeaponOnFireAction
    {
        None,
        DetonatePlacedExplosives,
    };

    enum class FalloutCriticalFailure
    {
        None,
        MissingCriticalData,
        InvalidActorChance,
        InvalidWeaponMultiplier,
        InvalidAutomaticFireRate,
        InvalidVatsBonus,
        InvalidChance,
    };

    enum class FalloutActorEffectFailure
    {
        None,
        MissingSpell,
        MissingSpellData,
        UnsupportedSpellType,
        EmptySpell,
        MissingBaseEffect,
        MissingMagicEffect,
        MissingMagicEffectData,
        InvalidRange,
        MissingScript,
    };

    enum class FalloutAmmoEffectFailure
    {
        None,
        InvalidBaseValue,
        MissingEffect,
        InvalidEffectValue,
        InvalidOperation,
        InvalidResult,
    };

    enum class FalloutWeaponDegradationFailure
    {
        None,
        InvalidGameSetting,
        InvalidWeaponOverride,
        InvalidVatsMultiplier,
        InvalidAmmoEffect,
        InvalidResult,
    };

    struct FalloutRangedDamageTuning
    {
        float mWeaponDamageMultiplier = 0.f;
        float mSkillBase = 0.f;
        float mSkillMultiplier = 0.f;
        float mConditionThreshold = 0.f;
        float mConditionPenaltyRate = 0.f;
    };

    struct FalloutRangedDamage
    {
        float mAuthoredDamage = 0.f;
        float mSkill = 0.f;
        float mSkillMultiplier = 0.f;
        float mCondition = 0.f;
        float mConditionMultiplier = 0.f;
        float mWeaponDamageMultiplier = 0.f;
        float mDamage = 0.f;
    };

    struct FalloutExplosionDamage
    {
        float mAuthoredDamage = 0.f;
        float mDamageMultiplier = 0.f;
        float mRadius = 0.f;
        float mDistance = 0.f;
        float mFalloff = 0.f;
        float mDamage = 0.f;
    };

    struct FalloutExplosionKnockdownTuning
    {
        float mDamageMultiplier = 0.f;
        float mDamageBase = 0.f;
        float mAgilityMultiplier = 0.f;
        float mAgilityBase = 0.f;
        float mMaximumChance = 0.f;
        float mCurrentHealthThresholdPercent = 0.f;
        float mBaseHealthThresholdPercent = 0.f;
    };

    struct FalloutExplosionKnockdown
    {
        FalloutExplosionKnockdownMode mMode = FalloutExplosionKnockdownMode::None;
        float mHealthDamage = 0.f;
        float mCurrentHealth = 0.f;
        float mMaximumHealth = 0.f;
        float mAgility = 0.f;
        float mChance = 0.f;
    };

    struct FalloutCriticalContract
    {
        float mChancePercent = 0.f;
        float mDamage = 0.f;
        ESM::FormId mEffect;
        bool mEffectOnDeath = false;

        [[nodiscard]] float damageForProjectile(float baseDamage, bool critical) const noexcept
        {
            return baseDamage + (critical ? mDamage : 0.f);
        }
    };

    /// Pointer-free snapshot of one native SPEL.EFID/EFIT entry and its winning MGEF record. Keeping the complete
    /// parsed data structures here preserves conditions, authored timing, resistance, shaders, sounds, model, and
    /// archetype parameters for the execution stage without retaining store pointers across a load-order rebuild.
    struct FalloutActorEffectEntry
    {
        ESM4::Spell::Effect mEffect;
        ESM4::MagicEffect::Data mMagicEffect;
        std::string mModel;
        std::vector<ESM::FormId> mCounterEffects;
        ESM::FormId mScript;
    };

    struct FalloutActorEffectContract
    {
        ESM::FormId mSpell;
        ESM4::Spell::Data mSpellData;
        std::vector<FalloutActorEffectEntry> mEffects;
    };

    struct FalloutWeaponDegradation
    {
        float mBaseLoss = 0.f;
        float mAmmoAdjustedLoss = 0.f;
        float mVatsMultiplier = 1.f;
        float mConditionLoss = 0.f;
        bool mUsesWeaponOverride = false;
    };

    struct FalloutDamageMitigation
    {
        float mIncomingDamage = 0.f;
        float mDamageResistance = 0.f;
        float mDamageThreshold = 0.f;
        float mDamageAfterResistance = 0.f;
        float mMinimumDamage = 0.f;
        float mHealthDamage = 0.f;
        bool mThresholdLimited = false;
    };

    enum class FalloutVatsWeaponFailure
    {
        None,
        MissingBallistics,
        MissingAuthoredActionPointOverride,
        InvalidActionPointCost,
        InvalidBaseHitChance,
        InvalidLimbDamageMultiplier,
        InvalidSkillActorValue,
    };

    struct FalloutVatsWeaponContract
    {
        float mActionPointCost = 0.f;
        std::uint8_t mBaseHitChance = 0;
        float mLimbDamageMultiplier = 1.f;
        std::int32_t mSkillActorValue = -1;
    };

    enum class FalloutVatsQueueFailure
    {
        None,
        MissingTarget,
        InvalidBodyPart,
        InvalidDisplayedHitChance,
        InvalidActionPointState,
        InsufficientActionPoints,
        InsufficientAmmunition,
        QueueLimitReached,
    };

    struct FalloutVatsQueuedAction
    {
        ESM::FormId mTarget;
        std::uint8_t mBodyPart = 0;
        std::uint8_t mDisplayedHitChance = 0;
        float mActionPointCost = 0.f;
        float mHealthDamageMultiplier = 1.f;
        float mLimbDamageMultiplier = 1.f;
        std::uint8_t mHealthPercent = 0;
        std::string mBodyPartName;
        std::string mTargetNode;
        std::int8_t mActorValue = -1;
    };

    enum class FalloutVatsBodyPartFailure
    {
        None,
        MissingName,
        MissingTargetNode,
        InvalidActorValue,
        InvalidHitChance,
        InvalidDamageMultiplier,
    };

    struct FalloutVatsBodyPartContract
    {
        std::uint8_t mIndex = 0;
        std::string_view mName;
        std::string_view mTargetNode;
        std::int8_t mActorValue = -1;
        std::uint8_t mBaseHitChance = 0;
        std::uint8_t mHealthPercent = 0;
        float mHealthDamageMultiplier = 1.f;
        bool mAbsoluteHitChance = false;
    };

    struct FalloutVatsCameraPose
    {
        osg::Vec3f mFocus;
        osg::Vec3f mEye;
    };

    /// The authored BPTD fields shared by ordinary ray hits and VATS. Unlike the VATS presentation contract,
    /// this keeps every authored node identity so a rendered hit can be mapped without name heuristics.
    struct FalloutBodyPartContract
    {
        std::uint8_t mIndex = 0;
        std::string_view mName;
        std::string_view mNodeName;
        std::string_view mVatsTargetNode;
        std::string_view mIkStartNode;
        std::string_view mGoreEffectsTarget;
        std::int8_t mActorValue = -1;
        std::uint8_t mHealthPercent = 0;
        float mHealthDamageMultiplier = 1.f;
    };

    struct FalloutLimbImpact
    {
        float mMaximumCondition = 0.f;
        float mConditionBefore = 0.f;
        float mConditionAfter = 0.f;
        float mDamageApplied = 0.f;
        float mDamageTakenAfter = 0.f;
        bool mNewlyCrippled = false;
    };

    enum class FalloutVatsPhase
    {
        Inactive,
        Targeting,
        Executing,
    };

    /// Owns one VATS targeting/queue/execution transaction. World lookup, UI, camera and input remain adapters;
    /// this object keeps the selected authored target data, AP reservation and execution cursor deterministic.
    class FalloutVatsRuntime
    {
        FalloutVatsPhase mPhase = FalloutVatsPhase::Inactive;
        float mActionPointsBefore = 0.f;
        ESM::FormId mSelectedTarget;
        std::uint8_t mSelectedBodyPart = 0;
        unsigned int mDisplayedHitChance = 0;
        std::uint8_t mSelectedHealthPercent = 0;
        float mSelectedHealthDamageMultiplier = 1.f;
        std::string mSelectedBodyPartName;
        std::string mSelectedTargetNode;
        std::int8_t mSelectedActorValue = -1;
        std::vector<FalloutVatsQueuedAction> mQueue;
        std::size_t mExecutionIndex = 0;

    public:
        [[nodiscard]] bool enter(float currentActionPoints) noexcept;
        void cancel() noexcept;
        [[nodiscard]] bool select(ESM::FormId target, const FalloutVatsBodyPartContract& bodyPart,
            unsigned int displayedHitChance) noexcept;
        [[nodiscard]] bool queueSelected(
            const FalloutVatsWeaponContract& weapon, std::size_t availableShots, FalloutVatsQueueFailure& failure);
        [[nodiscard]] std::optional<float> beginExecution() noexcept;
        [[nodiscard]] const FalloutVatsQueuedAction* getExecutingAction() const noexcept;
        [[nodiscard]] bool advanceExecution() noexcept;
        [[nodiscard]] bool isExecutionComplete() const noexcept;
        [[nodiscard]] bool finishExecution() noexcept;

        [[nodiscard]] FalloutVatsPhase getPhase() const noexcept { return mPhase; }
        [[nodiscard]] float getActionPointsBefore() const noexcept { return mActionPointsBefore; }
        [[nodiscard]] float getReservedActionPoints() const noexcept;
        [[nodiscard]] float getActionPointsAfter() const noexcept;
        [[nodiscard]] std::span<const FalloutVatsQueuedAction> getQueue() const noexcept { return mQueue; }
    };

    struct FalloutFireCadence
    {
        bool mAutomatic = false;
        float mSecondsPerShot = 0.f;
    };

    struct FalloutTriggerState
    {
        bool mWasDown = false;
        float mCooldown = 0.f;
    };

    enum class FalloutAttackDeliveryEvent : std::uint8_t
    {
        None,
        Hit,
        Release,
    };

    /// One native non-V.A.T.S. attack waiting for its authored KF delivery key. The animation group and equipped
    /// weapon identity are frozen with the attack so a stale key from an interrupted clip cannot damage with a new
    /// weapon. A matching key consumes this state exactly once.
    struct FalloutAttackDelivery
    {
        FalloutAttackDeliveryEvent mEvent = FalloutAttackDeliveryEvent::None;
        ESM::FormId mWeapon;
        std::uint8_t mAnimationType = 0xff;
        std::string mAnimationGroup;

        [[nodiscard]] bool isPending() const noexcept
        {
            return mEvent != FalloutAttackDeliveryEvent::None;
        }
    };

    struct FalloutMeleeTuning
    {
        float mDamageSkillBase = 0.f;
        float mDamageSkillMult = 0.f;
        float mUnarmedDamageBase = 0.f;
        float mUnarmedDamageMult = 0.f;
        float mMeleeStrengthMult = 0.f;
        float mMeleeStrengthOffset = 0.f;
        float mCombatDistance = 0.f;
        float mUnarmedReach = 0.f;
    };

    struct FalloutMeleeContract
    {
        float mDamage = 0.f;
        float mReach = 0.f;
        bool mUnarmedFamily = false;
        bool mBareHanded = false;
    };

    struct FalloutShotContract
    {
        ESM::FormId mAmmo;
        ESM::FormId mProjectile;
        std::uint8_t mAmmoUse = 0;
        std::uint8_t mProjectileCount = 0;
        float mDamage = 0.f;
        float mMinRange = 0.f;
        float mMaxRange = 0.f;
        float mProjectileRange = 0.f;
        float mMinSpread = 0.f;
        // Retained for record diagnostics only. FNV does not use WEAP.DNAM Spread when firing; Min Spread is the
        // authored weapon-spread input.
        float mLegacySpread = 0.f;
        bool mAuthoredHitscan = false;
        bool mConsumesWeapon = false;

        /// Fallout WEAP damage is authored per trigger pull. Divide it evenly between the authored rays so a
        /// multi-projectile weapon preserves the same total damage when every pellet connects.
        [[nodiscard]] float damagePerProjectile() const noexcept
        {
            return mProjectileCount == 0 ? 0.f : mDamage / static_cast<float>(mProjectileCount);
        }
    };

    /// Immutable hit payload carried by a moving native FNV projectile. Target armor and body-part data are
    /// intentionally resolved only at contact; the weapon/ammo/critical decision was fixed when the trigger fired.
    struct FalloutProjectileImpactContract
    {
        ESM::FormId mWeapon;
        ESM::FormId mExplosion;
        float mRawDamage = 0.f;
        float mLimbDamageMultiplier = 1.f;
        float mExplosionDamageMultiplier = 1.f;
        float mProjectileSkill = 0.f;
        std::vector<ESM::FormId> mAmmoEffects;
        std::optional<FalloutVatsQueuedAction> mVatsAction;
        bool mVatsTargetHit = true;
        bool mCritical = false;
    };

    /// A successful queued V.A.T.S. roll is authoritative for the queued actor even when the visual moving
    /// projectile intersects incidental scenery first. Ordinary shots and rolled misses retain physical collision.
    [[nodiscard]] std::optional<ESM::FormId> getAuthoritativeFalloutVatsProjectileTarget(
        const FalloutProjectileImpactContract& impact) noexcept;

    using FalloutAmmoTypePredicate = std::function<bool(ESM::FormId)>;
    using FalloutAmmoCount = std::function<int(ESM::FormId)>;
    using FalloutFactionLookup = std::function<const ESM4::Faction*(ESM::FormId)>;
    using FalloutSpellLookup = std::function<const ESM4::Spell*(ESM::FormId)>;
    using FalloutMagicEffectLookup = std::function<const ESM4::MagicEffect*(ESM::FormId)>;
    using FalloutScriptLookup = std::function<const ESM4::Script*(ESM::FormId)>;

    /// Resolve the actor's directional authored Creation Engine group-combat reaction to the target. An actor with
    /// no effective faction is neutral to a target with known membership. A missing target identity or an unresolved
    /// nonzero authored faction returns no result so callers can fail closed.
    [[nodiscard]] std::optional<ESM4::Faction::GroupCombatReaction> resolveFalloutFactionReaction(
        std::span<const ESM4::ActorFaction> actorFactions, std::span<const ESM4::ActorFaction> targetFactions,
        const FalloutFactionLookup& findFaction);

    /// Apply FNV SetAlly's symmetric group-combat relationship. Existing directional disposition modifiers are
    /// preserved and existing relation rows are updated rather than duplicated.
    [[nodiscard]] bool setFalloutFactionsAllied(ESM4::Faction& first, ESM4::Faction& second);

    /// Apply FNV SetEnemy's directional flags. A false flag means Enemy (the command default); a true flag means
    /// Neutral. Existing directional disposition modifiers are preserved and relation rows are not duplicated.
    [[nodiscard]] bool setFalloutFactionsEnemy(ESM4::Faction& first, ESM4::Faction& second,
        bool firstTreatsSecondAsNeutral = false, bool secondTreatsFirstAsNeutral = false);

    /// Decide whether an actor joins combat when a nearby faction ally is attacked. Shared faction membership is
    /// authoritative even when no explicit FACT relation is authored; explicit Ally/Friend relations also defend.
    [[nodiscard]] bool shouldFalloutActorDefendVictim(std::span<const ESM4::ActorFaction> actorFactions,
        std::span<const ESM4::ActorFaction> victimFactions,
        std::optional<ESM4::Faction::GroupCombatReaction> reaction) noexcept;

    /// Match an actor against the exact faction selected by SendAssaultAlarm. Rank is irrelevant, but zero/unset
    /// faction ids never match.
    [[nodiscard]] bool isFalloutActorInFaction(
        std::span<const ESM4::ActorFaction> actorFactions, ESM::FormId faction) noexcept;

    /// Apply Fallout's categorical aggression contract: 0 never initiates, 1 attacks enemies, 2 attacks enemies and
    /// neutrals, and 3 attacks anyone. Invalid aggression or an unknown required reaction fails closed.
    [[nodiscard]] bool shouldFalloutActorInitiateCombat(
        std::uint8_t aggression, std::optional<ESM4::Faction::GroupCombatReaction> reaction);

    /// Apply Fallout's categorical confidence contract. Only confidence 0 flees automatically; confidence 1..4
    /// remains in combat instead of inheriting Morrowind's continuous flee score and distance bias.
    [[nodiscard]] bool shouldFalloutActorFlee(std::uint8_t confidence) noexcept;

    /// Select the first authored AMMO entry that has enough rounds. Candidate order is authoritative; this function
    /// never guesses a replacement or matches by editor-id/name.
    [[nodiscard]] std::optional<ESM::FormId> selectAuthoredFalloutAmmo(std::span<const ESM::FormId> candidates,
        std::uint8_t rounds, const FalloutAmmoTypePredicate& isAmmo, const FalloutAmmoCount& countAmmo);

    /// Validate and preserve the serialized WEAP -> PROJ contract used by hitscan rays and moving projectiles.
    [[nodiscard]] std::optional<FalloutShotContract> buildFalloutRayShotContract(const ESM4::Weapon& weapon,
        const ESM4::Projectile& projectile, ESM::FormId ammo, FalloutShotFailure& failure);

    /// Build the trigger cadence authored by Fallout's WEAP DNAM. Semi-automatic weapons are edge-triggered and do
    /// not need a cycle time. Automatic weapons use the same serialized multiplier product exposed by xNVSE.
    [[nodiscard]] std::optional<FalloutFireCadence> buildFalloutFireCadence(
        const ESM4::Weapon& weapon, FalloutFireCadenceFailure& failure);

    /// Resolve the distance at which native AI may begin a Fallout weapon action. Ranged weapons preserve the
    /// authored WEAP maximum range; melee and unarmed actions use the winning Fallout combat-distance GMSTs.
    /// Invalid or absent source data fails closed instead of inheriting Morrowind projectile/reach constants.
    [[nodiscard]] std::optional<FalloutAiCombatRange> buildFalloutAiCombatRange(const ESM4::Weapon* weapon,
        float combatDistance, float unarmedReach, FalloutAiCombatRangeFailure& failure);

    /// Apply New Vegas damage mitigation to one impact. Damage resistance is a percentage and is applied before
    /// damage threshold; the combined reduction cannot pass below incomingDamage * minimumDamageMultiplier.
    /// Negative DT is ignored, matching the actor-value contract, while negative DR remains a vulnerability.
    [[nodiscard]] std::optional<FalloutDamageMitigation> resolveFalloutDamageMitigation(float incomingDamage,
        float damageResistance, float damageThreshold, float minimumDamageMultiplier,
        float maximumDamageResistance, FalloutDamageMitigationFailure& failure);

    /// Build New Vegas damage for one ranged trigger before critical, perk, ammo-effect, difficulty, and target
    /// mitigation stages. The skill coefficients come from the winning GMST records. The weapon multiplier,
    /// condition threshold, and condition penalty rate are explicit inputs because retail keeps them in engine state
    /// rather than WEAP data.
    [[nodiscard]] std::optional<FalloutRangedDamage> buildFalloutRangedDamage(float authoredDamage, float skill,
        float normalizedCondition, const FalloutRangedDamageTuning& tuning, FalloutRangedDamageFailure& failure);

    /// Resolve one actor's radial blast damage from an authored EXPL damage/radius pair. The weapon's frozen
    /// skill/condition multiplier is supplied separately because EXPL damage is not part of WEAP.DATA damage.
    /// Damage falls linearly from the detonation origin to zero at the authored radius.
    [[nodiscard]] std::optional<FalloutExplosionDamage> resolveFalloutExplosionDamage(float authoredDamage,
        float damageMultiplier, float radius, float distance, FalloutExplosionDamageFailure& failure);

    /// Resolve the native Fallout explosion knockdown formula after DT/DR mitigation. Damage below the current-
    /// health threshold cannot knock an actor down; damage above the base-health threshold forces knockdown;
    /// otherwise the chance is damage divided by Agility and capped by the winning GMSTs. The random roll is a
    /// separate operation so forced/rejected impacts do not consume the world PRNG.
    [[nodiscard]] std::optional<FalloutExplosionKnockdown> buildFalloutExplosionKnockdown(
        float healthDamage, float currentHealth, float maximumHealth, float agility,
        const FalloutExplosionKnockdownTuning& tuning, FalloutExplosionKnockdownFailure& failure);

    /// Apply the engine's integer [0, 999] test: chance * 1000 must be strictly greater than the roll.
    [[nodiscard]] bool doesFalloutExplosionKnockDown(
        const FalloutExplosionKnockdown& knockdown, unsigned int roll) noexcept;

    /// Reflect a lobber's incoming velocity around the collision normal using PROJ.DATA bounciness as its
    /// coefficient of restitution. Tangential velocity is preserved; friction remains the physics surface's job.
    [[nodiscard]] std::optional<osg::Vec3f> resolveFalloutProjectileBounce(const osg::Vec3f& velocity,
        const osg::Vec3f& collisionNormal, float bounciness, FalloutProjectileBounceFailure& failure);

    /// Aim a constant-speed Fallout projectile at a fixed world-space displacement while preserving the exact
    /// downward acceleration used by ProjectileManager. The low trajectory is selected so V.A.T.S. and AI lobbed
    /// shots reach their authored target instead of aiming along a gravity-oblivious straight ray.
    [[nodiscard]] std::optional<osg::Vec3f> buildFalloutBallisticAimDirection(
        const osg::Vec3f& displacement, float speed, float gravityAcceleration, FalloutBallisticAimFailure& failure);

    /// Resolve the PROJ flag/timer/proximity state machine. FNV proximity mines use the winning
    /// fMinesDelayMin plus their authored timer scaled by the placing actor's weapon skill. Exterior proximity uses
    /// fMineExteriorRadiusMult. Detonates takes precedence and remains remote-only.
    [[nodiscard]] std::optional<FalloutProjectileTrigger> buildFalloutProjectileTrigger(
        const ESM4::Projectile& projectile, float projectileSkill, float minesDelayMin,
        float exteriorRadiusMultiplier, bool exterior, FalloutProjectileTriggerFailure& failure);

    /// Whether a physical collision leaves this projectile active for a later timed, proximity, or remote trigger.
    /// Trigger flags only confer persistence on lobbers; retail throwing spears are missiles carrying the legacy
    /// AlternateTrigger bit and still terminate on their first impact.
    [[nodiscard]] bool doesFalloutProjectileRemainAfterImpact(const ESM4::Projectile& projectile) noexcept;

    /// Range expiry is a fallback for a projectile that is still flying. A physics collision in the same simulation
    /// step is authoritative and must be processed before distance expiry, including at the exact range boundary.
    /// Malformed distance state fails closed rather than deleting a potentially valid collision.
    [[nodiscard]] bool shouldResolveFalloutProjectileRangeExpiry(
        bool physicsActive, float distanceTravelled, float maximumRange) noexcept;

    /// A successful queued V.A.T.S. roll remains authoritative if a terminal moving projectile reaches its authored
    /// range without producing a physics collision. Misses, ordinary shots, malformed records, and persistent
    /// lobbers do not synthesize a range-expiry hit.
    [[nodiscard]] std::optional<ESM::FormId> getFalloutVatsProjectileRangeExpiryTarget(
        const ESM4::Projectile& projectile, const FalloutProjectileImpactContract& impact) noexcept;

    /// Resolve the native command executed by a weapon's authored SCPT OnFire block. This deliberately inspects
    /// script source rather than weapon FormIDs so overrides and mods retain the same data-driven behavior.
    [[nodiscard]] FalloutWeaponOnFireAction resolveFalloutWeaponOnFireAction(std::string_view scriptSource) noexcept;

    /// A placed charge is remotely selectable only after settling and only when its frozen explosion still matches
    /// the winning authored PROJ record. This prevents stale save state or record overrides from detonating another
    /// projectile family.
    [[nodiscard]] bool isFalloutRemoteDetonationCandidate(
        const ESM4::Projectile& projectile, bool settled, ESM::FormId frozenExplosion) noexcept;

    /// Resolve the weapon's native CRDT chance and damage before the target armor stage. New Vegas ignores weapon
    /// condition for critical chance. Automatic weapons divide their authored multiplier by fire rate, while VATS
    /// adds its winning GMST bonus after that multiplication. A zero CRDT multiplier disables criticals entirely.
    [[nodiscard]] std::optional<FalloutCriticalContract> buildFalloutCriticalContract(const ESM4::Weapon& weapon,
        float actorCriticalChance, bool vats, float vatsCriticalChanceBonus, FalloutCriticalFailure& failure);

    /// Resolve the complete native actor-effect chain used by WEAP.CRDT: SPEL -> ordered EFID/EFIT entries -> MGEF
    /// -> optional associated SCPT for Script archetypes. Every nonzero reference must resolve through the winning
    /// stores; malformed, incomplete, or non-actor spells fail closed rather than producing a partial effect.
    [[nodiscard]] std::optional<FalloutActorEffectContract> buildFalloutActorEffectContract(ESM::FormId spellId,
        const FalloutSpellLookup& findSpell, const FalloutMagicEffectLookup& findMagicEffect,
        const FalloutScriptLookup& findScript, FalloutActorEffectFailure& failure);

    /// Compare a percentage against a caller-supplied [0, 1) PRNG sample. Real-time multi-projectile weapons call
    /// this per actor-impacting projectile; VATS calls it once for the queued attack.
    [[nodiscard]] bool doesFalloutCriticalHit(float chancePercent, float roll) noexcept;

    /// Apply FNV AMEF operations of one requested type in the AMMO.RCIL order supplied by the caller. Effects of
    /// every other type are ignored. This is the same shared retail operation path used for damage, DR, DT, spread,
    /// condition loss, and fatigue.
    [[nodiscard]] std::optional<float> applyFalloutAmmoEffects(float baseValue, ESM4::AmmoEffect::Type type,
        std::span<const ESM4::AmmoEffect* const> effects, FalloutAmmoEffectFailure& failure);

    /// Resolve condition lost by one fired trigger. FNV starts with fDamageToWeaponValue unless WEAP flags2 selects
    /// the serialized damage-to-weapon override, then applies the selected AMMO's Weapon Condition AMEF entries.
    /// VATS scales that result by fVATSDamageToWeaponMult.
    [[nodiscard]] std::optional<FalloutWeaponDegradation> buildFalloutWeaponDegradation(
        const ESM4::Weapon& weapon, std::span<const ESM4::AmmoEffect* const> effects,
        float damageToWeaponGameSetting, bool vats, float vatsDamageToWeaponMultiplier,
        FalloutWeaponDegradationFailure& failure);

    /// New Vegas scales an equipped armor piece's DT/DR only below 50 percent condition. The vanilla penalty rate
    /// is 1.0, producing a 0.5 multiplier at zero condition and full protection at 50 percent or above.
    [[nodiscard]] std::optional<float> resolveFalloutArmorConditionMultiplier(
        float normalizedCondition, float penaltyRate) noexcept;

    /// Preserve the weapon-authored VATS inputs without supplying an inferred fallback. DNAM flag 3 makes the
    /// serialized AP override authoritative; weapons that require retail GMST/perk calculation remain uncovered.
    [[nodiscard]] std::optional<FalloutVatsWeaponContract> buildFalloutVatsWeaponContract(
        const ESM4::Weapon& weapon, FalloutVatsWeaponFailure& failure);

    /// Queue one VATS attack using the hit chance already observed from the targeting calculation. This boundary
    /// deliberately does not invent the still-uncovered retail distance/visibility/perk formula. It enforces the
    /// retail-visible AP reservation atomically and preserves the selected target, limb and displayed percentage.
    [[nodiscard]] std::optional<FalloutVatsQueuedAction> queueFalloutVatsAction(
        std::span<const FalloutVatsQueuedAction> queued, ESM::FormId target, std::uint8_t bodyPart,
        unsigned int displayedHitChance, float bodyPartDamageMultiplier, std::uint8_t bodyPartHealthPercent,
        float currentActionPoints,
        std::size_t availableShots, const FalloutVatsWeaponContract& weapon, FalloutVatsQueueFailure& failure);

    [[nodiscard]] float getFalloutVatsReservedActionPoints(
        std::span<const FalloutVatsQueuedAction> queued) noexcept;

    /// Resolve the displayed VATS percentage against a caller-supplied [0, 1) PRNG sample. Keeping this decision
    /// independent from weapon delivery makes misses consume the same ammo/AP and use the same authored fire path.
    [[nodiscard]] bool doesFalloutVatsAttackHit(std::uint8_t displayedHitChance, float roll) noexcept;

    [[nodiscard]] std::optional<FalloutVatsBodyPartContract> buildFalloutVatsBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index, FalloutVatsBodyPartFailure& failure);

    [[nodiscard]] unsigned int getFalloutVatsDisplayedHitChance(
        const FalloutVatsBodyPartContract& bodyPart, const FalloutVatsWeaponContract& weapon) noexcept;

    /// Frame the rendered front of an actor around a caller-supplied focus and framing radius. The caller supplies
    /// the rendered forward vector so ESM4 skeleton basis corrections remain authoritative.
    [[nodiscard]] FalloutVatsCameraPose buildFalloutVatsFrontalCameraPose(
        const osg::Vec3f& center, float radius, const osg::Vec3f& renderedForward) noexcept;

    /// Resolve the winning native BPTD used by a placed actor: player BPTD for the player, race GNAM or the exact
    /// default human BPTD for NPCs, and the resolved CREA PNAM provider for creatures.
    [[nodiscard]] const ESM4::BodyPartData* getFalloutActorBodyPartData(const MWWorld::Ptr& actor);

    [[nodiscard]] std::optional<FalloutBodyPartContract> buildFalloutBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index) noexcept;

    /// Prefer the deepest exact rendered node identity. No substring or actor-name matching is permitted.
    [[nodiscard]] std::optional<FalloutBodyPartContract> resolveFalloutBodyPartFromNodePath(
        const ESM4::BodyPartData& bodyPartData, std::span<const std::string> nodePath) noexcept;

    /// Convert the independent native hit-data limb channel into the target actor value's persistent condition.
    /// Health armor mitigation is intentionally absent: retail calculates limb damage before DR/DT.
    [[nodiscard]] std::optional<FalloutLimbImpact> resolveFalloutLimbImpact(float actorMaximumHealth,
        std::uint8_t bodyPartHealthPercent, float damageTakenBefore, float rawHitDamage,
        float weaponLimbDamageMultiplier, float targetLimbDamageMultiplier) noexcept;

    /// Advance one weapon trigger using elapsed simulation time. A semi-automatic trigger fires once per press;
    /// an automatic trigger repeats while held when its authored cooldown expires. Visual animation state is only an
    /// eligibility input and never supplies the cadence.
    [[nodiscard]] bool advanceFalloutTrigger(FalloutTriggerState& state, bool triggerDown, bool ready,
        const FalloutFireCadence& cadence, float duration) noexcept;

    /// Mirror the physical Flat "Use" binding into the native FNV weapon controller only while ordinary gameplay
    /// owns input. This keeps Pip-Boy dismissal, disabled controls, holstered weapons, and V.A.T.S. from firing.
    [[nodiscard]] bool shouldApplyFalloutPlayerUseInput(FalloutVatsPhase vatsPhase, bool controlsEnabled,
        bool fightingEnabled, bool guiMode, bool weaponDrawn, bool useDown) noexcept;

    /// Select the authored KF event that delivers a non-V.A.T.S. attack. Melee clips author Hit; hand-thrown and
    /// placed explosive clips author Release; non-hitscan gun/launcher clips author Hit. Automatic and hitscan
    /// weapons continue to use trigger cadence directly so an action clip cannot collapse a firing loop to one hit.
    [[nodiscard]] FalloutAttackDeliveryEvent getFalloutAttackDeliveryEvent(
        std::uint8_t animationType, bool authoredHitscan, bool automatic) noexcept;

    /// Gameplay is immediate for cadence-driven attacks and as a fallback when an event-routed visual action cannot
    /// play. A successfully playing event-routed action waits for its authored Hit/Release key.
    [[nodiscard]] bool shouldDeliverFalloutAttackImmediately(
        FalloutAttackDeliveryEvent event, bool visualAction) noexcept;

    /// Arm one exact animation-key delivery. None, an invalid animation family, or an empty group fails closed.
    [[nodiscard]] bool queueFalloutAttackDelivery(FalloutAttackDelivery& state,
        FalloutAttackDeliveryEvent event, ESM::FormId weapon, std::uint8_t animationType,
        std::string_view animationGroup);

    /// Consume a queued delivery only when the still-equipped weapon, active animation group, and native text key
    /// all match. Both native raw keys ("hit") and already-namespaced keys ("attack2: hit") are accepted.
    [[nodiscard]] std::optional<FalloutAttackDelivery> consumeFalloutAttackDelivery(
        FalloutAttackDelivery& state, ESM::FormId equippedWeapon, std::string_view animationGroup,
        std::string_view textKey) noexcept;

    [[nodiscard]] bool isFalloutThrownWeapon(const ESM4::Weapon& weapon) noexcept;

    /// Resolve one unit ray using FNV's authored WEAP Min Spread semantics. Min Spread is the median angular
    /// deviation: a sample radius of 0.5 produces that angle and radius 1 produces the 2x maximum. The caller
    /// supplies the polar sample in the unit disk, allowing production to use the world PRNG while keeping the
    /// geometry deterministic and testable.
    [[nodiscard]] std::optional<osg::Vec3f> buildFalloutRayDirection(
        const osg::Vec3f& forward, float medianSpreadDegrees, const osg::Vec2f& polarSample);

    /// Build the native Fallout melee contract from WEAP DATA/DNAM and the actor's current Fallout values. DNAM
    /// families 0..2 are HandToHand, OneHandMelee, and TwoHandMelee respectively. A missing WEAP is valid only for
    /// bare HandToHand. Damage tuning comes from the winning Fallout GMST records; reach preserves authored WEAP
    /// DATA.reach and uses the shared combat-distance scale used by actor contact tests.
    [[nodiscard]] std::optional<FalloutMeleeContract> buildFalloutMeleeContract(const ESM4::Weapon* weapon,
        std::uint8_t animationType, float skill, float strength, const FalloutMeleeTuning& tuning,
        FalloutMeleeFailure& failure);

    /// Build an intrinsic creature attack from the winning FNV CREA DATA damage/combatSkill/strength payload.
    [[nodiscard]] std::optional<FalloutMeleeContract> buildFalloutCreatureMeleeContract(float authoredDamage,
        float combatSkill, float strength, const FalloutMeleeTuning& tuning, FalloutMeleeFailure& failure);

    [[nodiscard]] bool isFalloutMeleeAnimationType(std::uint8_t animationType) noexcept;

    [[nodiscard]] std::string_view getFalloutShotFailureName(FalloutShotFailure failure);
    [[nodiscard]] std::string_view getFalloutFireCadenceFailureName(FalloutFireCadenceFailure failure);
    [[nodiscard]] std::string_view getFalloutAiCombatRangeFailureName(FalloutAiCombatRangeFailure failure);
    [[nodiscard]] std::string_view getFalloutDamageMitigationFailureName(FalloutDamageMitigationFailure failure);
    [[nodiscard]] std::string_view getFalloutRangedDamageFailureName(FalloutRangedDamageFailure failure);
    [[nodiscard]] std::string_view getFalloutExplosionDamageFailureName(FalloutExplosionDamageFailure failure);
    [[nodiscard]] std::string_view getFalloutExplosionKnockdownFailureName(
        FalloutExplosionKnockdownFailure failure);
    [[nodiscard]] std::string_view getFalloutProjectileBounceFailureName(FalloutProjectileBounceFailure failure);
    [[nodiscard]] std::string_view getFalloutBallisticAimFailureName(FalloutBallisticAimFailure failure);
    [[nodiscard]] std::string_view getFalloutProjectileTriggerFailureName(FalloutProjectileTriggerFailure failure);
    [[nodiscard]] std::string_view getFalloutCriticalFailureName(FalloutCriticalFailure failure);
    [[nodiscard]] std::string_view getFalloutActorEffectFailureName(FalloutActorEffectFailure failure);
    [[nodiscard]] std::string_view getFalloutAmmoEffectFailureName(FalloutAmmoEffectFailure failure);
    [[nodiscard]] std::string_view getFalloutWeaponDegradationFailureName(
        FalloutWeaponDegradationFailure failure);
    [[nodiscard]] std::string_view getFalloutVatsWeaponFailureName(FalloutVatsWeaponFailure failure);
    [[nodiscard]] std::string_view getFalloutVatsQueueFailureName(FalloutVatsQueueFailure failure);
    [[nodiscard]] std::string_view getFalloutVatsBodyPartFailureName(FalloutVatsBodyPartFailure failure);
    [[nodiscard]] std::string_view getFalloutMeleeFailureName(FalloutMeleeFailure failure);
}

#endif
