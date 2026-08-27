#ifndef OPENMW_MWMECHANICS_FALLOUTAMMOEFFECTS_H
#define OPENMW_MWMECHANICS_FALLOUTAMMOEFFECTS_H

#include <cstddef>
#include <optional>
#include <span>

#include <components/esm4/loadamef.hpp>

namespace MWMechanics
{
    enum class FalloutAmmoEffectFailure
    {
        None,
        InvalidBaseValue,
        MissingEffect,
        InvalidEffectValue,
        InvalidOperation,
        InvalidResult,
    };

    /// Apply the authored AMMO.RCIL effects of one type in list order.
    ///
    /// The effect records and their order are supplied by the caller. This
    /// keeps record lookup, inventory ownership, and combat state outside the
    /// arithmetic contract while making malformed data fail closed.
    [[nodiscard]] std::optional<float> applyFalloutAmmoEffects(float baseValue, ESM4::AmmoEffect::Type type,
        std::span<const ESM4::AmmoEffect* const> effects, FalloutAmmoEffectFailure& failure) noexcept;
}

#endif
