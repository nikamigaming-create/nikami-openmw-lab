#ifndef OPENMW_MWMECHANICS_FALLOUTCOMBAT_H
#define OPENMW_MWMECHANICS_FALLOUTCOMBAT_H

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include <components/esm/formid.hpp>

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
