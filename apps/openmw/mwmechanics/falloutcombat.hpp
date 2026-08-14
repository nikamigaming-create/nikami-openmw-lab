#ifndef OPENMW_MWMECHANICS_FALLOUTCOMBAT_H
#define OPENMW_MWMECHANICS_FALLOUTCOMBAT_H

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <components/esm/formid.hpp>
#include <components/esm4/actor.hpp>
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
        UnsupportedProjectile,
        InvalidAmmoUse,
        InvalidProjectileCount,
        UnsupportedProjectileCount,
        InvalidRange,
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

    /// Select the first authored AMMO entry that has enough rounds. Candidate order is authoritative; this function
    /// never guesses a replacement or matches by editor-id/name.
    [[nodiscard]] std::optional<ESM::FormId> selectAuthoredFalloutAmmo(std::span<const ESM::FormId> candidates,
        std::uint8_t rounds, const FalloutAmmoTypePredicate& isAmmo, const FalloutAmmoCount& countAmmo);

    /// Validate and preserve the exact serialized WEAP -> PROJ contract used by the first production hitscan path.
    [[nodiscard]] std::optional<FalloutShotContract> buildFalloutHitscanContract(const ESM4::Weapon& weapon,
        const ESM4::Projectile& projectile, ESM::FormId ammo, FalloutShotFailure& failure);

    [[nodiscard]] std::string_view getFalloutShotFailureName(FalloutShotFailure failure);
}

#endif
