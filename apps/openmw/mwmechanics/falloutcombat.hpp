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
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadfact.hpp>

namespace ESM4
{
    struct Projectile;
    struct Weapon;
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
        float mDamageMultiplier = 1.f;
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
        float mDamageMultiplier = 1.f;
        bool mAbsoluteHitChance = false;
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
        float mSelectedDamageMultiplier = 1.f;
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
        float mSpread = 0.f;
        bool mAuthoredHitscan = false;
        bool mConsumesWeapon = false;

        /// Fallout WEAP damage is authored per trigger pull. Divide it evenly between the authored rays so a
        /// multi-projectile weapon preserves the same total damage when every pellet connects.
        [[nodiscard]] float damagePerProjectile() const noexcept
        {
            return mProjectileCount == 0 ? 0.f : mDamage / static_cast<float>(mProjectileCount);
        }
    };

    using FalloutAmmoTypePredicate = std::function<bool(ESM::FormId)>;
    using FalloutAmmoCount = std::function<int(ESM::FormId)>;
    using FalloutFactionLookup = std::function<const ESM4::Faction*(ESM::FormId)>;

    /// Resolve the actor's directional authored Creation Engine group-combat reaction to the target. An actor with
    /// no effective faction is neutral to a target with known membership. A missing target identity or an unresolved
    /// nonzero authored faction returns no result so callers can fail closed.
    [[nodiscard]] std::optional<ESM4::Faction::GroupCombatReaction> resolveFalloutFactionReaction(
        std::span<const ESM4::ActorFaction> actorFactions, std::span<const ESM4::ActorFaction> targetFactions,
        const FalloutFactionLookup& findFaction);

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

    /// Validate and preserve the serialized WEAP -> PROJ contract used by the immediate-ray production path.
    /// Authored non-hitscan projectiles deliberately use the same ray fallback until moving ESM4 projectiles exist.
    [[nodiscard]] std::optional<FalloutShotContract> buildFalloutRayShotContract(const ESM4::Weapon& weapon,
        const ESM4::Projectile& projectile, ESM::FormId ammo, FalloutShotFailure& failure);

    /// Build the trigger cadence authored by Fallout's WEAP DNAM. Semi-automatic weapons are edge-triggered and do
    /// not need a cycle time. Automatic weapons use the same serialized multiplier product exposed by xNVSE.
    [[nodiscard]] std::optional<FalloutFireCadence> buildFalloutFireCadence(
        const ESM4::Weapon& weapon, FalloutFireCadenceFailure& failure);

    /// Preserve the weapon-authored VATS inputs without supplying an inferred fallback. DNAM flag 3 makes the
    /// serialized AP override authoritative; weapons that require retail GMST/perk calculation remain uncovered.
    [[nodiscard]] std::optional<FalloutVatsWeaponContract> buildFalloutVatsWeaponContract(
        const ESM4::Weapon& weapon, FalloutVatsWeaponFailure& failure);

    /// Queue one VATS attack using the hit chance already observed from the targeting calculation. This boundary
    /// deliberately does not invent the still-uncovered retail distance/visibility/perk formula. It enforces the
    /// retail-visible AP reservation atomically and preserves the selected target, limb and displayed percentage.
    [[nodiscard]] std::optional<FalloutVatsQueuedAction> queueFalloutVatsAction(
        std::span<const FalloutVatsQueuedAction> queued, ESM::FormId target, std::uint8_t bodyPart,
        unsigned int displayedHitChance, float bodyPartDamageMultiplier, float currentActionPoints,
        std::size_t availableShots, const FalloutVatsWeaponContract& weapon, FalloutVatsQueueFailure& failure);

    [[nodiscard]] float getFalloutVatsReservedActionPoints(
        std::span<const FalloutVatsQueuedAction> queued) noexcept;

    /// Resolve the displayed VATS percentage against a caller-supplied [0, 1) PRNG sample. Keeping this decision
    /// independent from weapon delivery makes misses consume the same ammo/AP and use the same authored fire path.
    [[nodiscard]] bool doesFalloutVatsAttackHit(std::uint8_t displayedHitChance, float roll) noexcept;

    [[nodiscard]] std::optional<FalloutVatsBodyPartContract> buildFalloutVatsBodyPartContract(
        const ESM4::BodyPartData::BodyPart& bodyPart, std::uint8_t index, FalloutVatsBodyPartFailure& failure);

    /// Advance one weapon trigger using elapsed simulation time. A semi-automatic trigger fires once per press;
    /// an automatic trigger repeats while held when its authored cooldown expires. Visual animation state is only an
    /// eligibility input and never supplies the cadence.
    [[nodiscard]] bool advanceFalloutTrigger(FalloutTriggerState& state, bool triggerDown, bool ready,
        const FalloutFireCadence& cadence, float duration) noexcept;

    [[nodiscard]] bool isFalloutThrownWeapon(const ESM4::Weapon& weapon) noexcept;

    /// Resolve one unit ray inside the authored circular spread cone. diskSample is a caller-provided point in the
    /// unit disk, allowing production to use the world PRNG while keeping the geometry deterministic and testable.
    [[nodiscard]] std::optional<osg::Vec3f> buildFalloutRayDirection(
        const osg::Vec3f& forward, float spreadDegrees, const osg::Vec2f& diskSample);

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
    [[nodiscard]] std::string_view getFalloutVatsWeaponFailureName(FalloutVatsWeaponFailure failure);
    [[nodiscard]] std::string_view getFalloutVatsQueueFailureName(FalloutVatsQueueFailure failure);
    [[nodiscard]] std::string_view getFalloutVatsBodyPartFailureName(FalloutVatsBodyPartFailure failure);
    [[nodiscard]] std::string_view getFalloutMeleeFailureName(FalloutMeleeFailure failure);
}

#endif
