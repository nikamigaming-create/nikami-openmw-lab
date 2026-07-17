#include "falloutcombat.hpp"

#include <cmath>

#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadweap.hpp>

namespace MWMechanics
{
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

    std::optional<FalloutShotContract> buildFalloutHitscanContract(const ESM4::Weapon& weapon,
        const ESM4::Projectile& projectile, ESM::FormId ammo, FalloutShotFailure& failure)
    {
        failure = FalloutShotFailure::None;
        if (ammo.isZeroOrUnset())
            failure = FalloutShotFailure::MissingAmmo;
        else if (!weapon.mData.hasBallistics)
            failure = FalloutShotFailure::MissingBallistics;
        else if (weapon.mData.projectile != projectile.mId)
            failure = FalloutShotFailure::ProjectileMismatch;
        else if (!projectile.mData.present)
            failure = FalloutShotFailure::MissingProjectileData;
        else if ((projectile.mData.flags & ESM4::Projectile::Hitscan) == 0)
            failure = FalloutShotFailure::UnsupportedProjectile;
        else if (weapon.mData.ammoUse == 0)
            failure = FalloutShotFailure::InvalidAmmoUse;
        else if (weapon.mData.numProjectiles == 0)
            failure = FalloutShotFailure::InvalidProjectileCount;
        else if (weapon.mData.numProjectiles != 1)
            failure = FalloutShotFailure::UnsupportedProjectileCount;
        else if (!std::isfinite(projectile.mData.range) || projectile.mData.range <= 0.f)
            failure = FalloutShotFailure::InvalidRange;

        if (failure != FalloutShotFailure::None)
            return std::nullopt;

        return FalloutShotContract{ ammo, weapon.mData.projectile, weapon.mData.ammoUse,
            weapon.mData.numProjectiles, static_cast<float>(weapon.mData.damage), weapon.mData.minRange,
            weapon.mData.maxRange, projectile.mData.range };
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
            case FalloutShotFailure::UnsupportedProjectile:
                return "unsupported-projectile";
            case FalloutShotFailure::InvalidAmmoUse:
                return "invalid-ammo-use";
            case FalloutShotFailure::InvalidProjectileCount:
                return "invalid-projectile-count";
            case FalloutShotFailure::UnsupportedProjectileCount:
                return "unsupported-projectile-count";
            case FalloutShotFailure::InvalidRange:
                return "invalid-range";
        }
        return "unknown";
    }
}
