#include "falloutammoeffects.hpp"

#include <cmath>

std::optional<float> MWMechanics::applyFalloutAmmoEffects(float baseValue, ESM4::AmmoEffect::Type type,
    std::span<const ESM4::AmmoEffect* const> effects, FalloutAmmoEffectFailure& failure) noexcept
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
