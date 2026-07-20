#include "imagespacecomposition.hpp"

#include "loadcell.hpp"
#include "loadwrld.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace
{
    constexpr std::array<std::size_t, ESM4::ImageSpaceModifier::sMultiplyAddChannelCount> sTraitForChannel = {
        ESM4::ImageSpace::Trait_EyeAdaptSpeed,
        ESM4::ImageSpace::Trait_HdrBlurRadius,
        ESM4::ImageSpace::Trait_SkinDimmer,
        ESM4::ImageSpace::Trait_EmissiveMultiplier,
        ESM4::ImageSpace::Trait_TargetLuminance,
        ESM4::ImageSpace::Trait_UpperLuminanceClamp,
        ESM4::ImageSpace::Trait_BrightScale,
        ESM4::ImageSpace::Trait_BrightClamp,
        ESM4::ImageSpace::Trait_LuminanceRampNoTexture,
        ESM4::ImageSpace::Trait_LuminanceRampMin,
        ESM4::ImageSpace::Trait_LuminanceRampMax,
        ESM4::ImageSpace::Trait_SunlightDimmer,
        ESM4::ImageSpace::Trait_GrassDimmer,
        ESM4::ImageSpace::Trait_TreeDimmer,
        ESM4::ImageSpace::Trait_BloomBlurRadius,
        ESM4::ImageSpace::Trait_BloomAlphaInterior,
        ESM4::ImageSpace::Trait_BloomAlphaExterior,
        ESM4::ImageSpace::Trait_CinematicSaturation,
        ESM4::ImageSpace::Trait_CinematicContrast,
        ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance,
        ESM4::ImageSpace::Trait_CinematicBrightness,
    };

    float evaluate(const std::vector<ESM4::ImageSpaceModifier::FloatKey>& keys, float time, float neutral)
    {
        if (keys.empty())
            return neutral;
        if (keys.size() == 1 || time <= keys.front().time)
            return keys.front().value;
        for (std::size_t i = 1; i < keys.size(); ++i)
        {
            if (time <= keys[i].time)
            {
                const float duration = keys[i].time - keys[i - 1].time;
                if (duration <= 0.f)
                    return keys[i].value;
                const float factor = std::clamp((time - keys[i - 1].time) / duration, 0.f, 1.f);
                return std::lerp(keys[i - 1].value, keys[i].value, factor);
            }
        }
        return keys.back().value;
    }

    std::array<float, 4> evaluate(
        const std::vector<ESM4::ImageSpaceModifier::ColorKey>& keys, float time, std::array<float, 4> neutral)
    {
        if (keys.empty())
            return neutral;
        if (keys.size() == 1 || time <= keys.front().time)
            return keys.front().value;
        for (std::size_t i = 1; i < keys.size(); ++i)
        {
            if (time <= keys[i].time)
            {
                const float duration = keys[i].time - keys[i - 1].time;
                if (duration <= 0.f)
                    return keys[i].value;
                const float factor = std::clamp((time - keys[i - 1].time) / duration, 0.f, 1.f);
                std::array<float, 4> result{};
                for (std::size_t component = 0; component < result.size(); ++component)
                    result[component] = std::lerp(keys[i - 1].value[component], keys[i].value[component], factor);
                return result;
            }
        }
        return keys.back().value;
    }
}

ESM4::ComposedImageSpace ESM4::composeImageSpace(
    const ImageSpace& base, const std::vector<ImageSpaceModifierContribution>& modifiers)
{
    ComposedImageSpace result;
    result.mTraits = base.mTraits;

    std::array<float, ImageSpaceModifier::sMultiplyAddChannelCount> multiplierDelta{};
    std::array<float, ImageSpaceModifier::sMultiplyAddChannelCount> additive{};

    const float baseTintStrength = std::max(0.f, base.mTraits[ImageSpace::Trait_CinematicTintStrength]);
    std::array<float, 3> tintNumerator = {
        base.mTraits[ImageSpace::Trait_CinematicTintRed] * baseTintStrength,
        base.mTraits[ImageSpace::Trait_CinematicTintGreen] * baseTintStrength,
        base.mTraits[ImageSpace::Trait_CinematicTintBlue] * baseTintStrength,
    };
    float tintWeight = baseTintStrength;
    float strongestTint = baseTintStrength;
    std::unordered_map<const ImageSpaceModifier*, float> tintStrengthByModifier;

    std::array<float, 3> fadeNumerator{};
    float fadeWeight = 0.f;
    float strongestFade = 0.f;
    std::unordered_map<const ImageSpaceModifier*, float> fadeStrengthByModifier;

    for (const ImageSpaceModifierContribution& contribution : modifiers)
    {
        if (contribution.mModifier == nullptr || !std::isfinite(contribution.mStrength))
            continue;
        const float strength = std::max(0.f, contribution.mStrength);
        const ImageSpaceModifier& modifier = *contribution.mModifier;
        for (std::size_t channel = 0; channel < ImageSpaceModifier::sMultiplyAddChannelCount; ++channel)
        {
            multiplierDelta[channel]
                += (evaluate(modifier.mMultiply[channel], contribution.mTime, 1.f) - 1.f) * strength;
            additive[channel] += evaluate(modifier.mAdd[channel], contribution.mTime, 0.f) * strength;
        }

        const std::array<float, 4> tint = evaluate(modifier.mTint, contribution.mTime, { 1.f, 1.f, 1.f, 0.f });
        const float tintStrength = std::max(0.f, tint[3]) * strength;
        for (std::size_t component = 0; component < 3; ++component)
            tintNumerator[component] += tint[component] * tintStrength;
        tintWeight += tintStrength;
        tintStrengthByModifier[&modifier] += tintStrength;

        const std::array<float, 4> fade = evaluate(modifier.mFade, contribution.mTime, { 0.f, 0.f, 0.f, 0.f });
        const float fadeStrength = std::max(0.f, fade[3]) * strength;
        for (std::size_t component = 0; component < 3; ++component)
            fadeNumerator[component] += fade[component] * fadeStrength;
        fadeWeight += fadeStrength;
        fadeStrengthByModifier[&modifier] += fadeStrength;
    }

    for (const auto& entry : tintStrengthByModifier)
        strongestTint = std::max(strongestTint, entry.second);
    for (const auto& entry : fadeStrengthByModifier)
        strongestFade = std::max(strongestFade, entry.second);

    for (std::size_t channel = 0; channel < ImageSpaceModifier::sMultiplyAddChannelCount; ++channel)
    {
        const std::size_t trait = sTraitForChannel[channel];
        result.mTraits[trait] = base.mTraits[trait] * (1.f + multiplierDelta[channel]) + additive[channel];
    }

    if (tintWeight > 0.f)
    {
        for (std::size_t component = 0; component < 3; ++component)
            result.mTint[component] = tintNumerator[component] / tintWeight;
        result.mTint[3] = strongestTint;
    }
    if (fadeWeight > 0.f)
    {
        for (std::size_t component = 0; component < 3; ++component)
            result.mFade[component] = fadeNumerator[component] / fadeWeight;
        result.mFade[3] = strongestFade;
    }

    result.mTraits[ImageSpace::Trait_CinematicTintRed] = result.mTint[0];
    result.mTraits[ImageSpace::Trait_CinematicTintGreen] = result.mTint[1];
    result.mTraits[ImageSpace::Trait_CinematicTintBlue] = result.mTint[2];
    result.mTraits[ImageSpace::Trait_CinematicTintStrength] = result.mTint[3];
    return result;
}

ESM::FormId ESM4::resolveCellImageSpace(const Cell& cell, const World* parentWorld)
{
    if (!cell.mImageSpace.isZeroOrUnset())
        return cell.mImageSpace;
    if (cell.isExterior() && parentWorld != nullptr)
        return parentWorld->mImageSpace;
    return {};
}
