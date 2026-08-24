#include "falloutcollisionfilter.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace NifBullet
{
    namespace
    {
        constexpr std::uint32_t kSetBit = 1U;
        constexpr std::uint32_t kNoSystemGroup = 0U;
        constexpr std::uint32_t kNoMask = 0U;

        bool isValid(const FalloutCollisionFilterConfig& config)
        {
            constexpr std::uint32_t kWordBits = std::numeric_limits<std::uint32_t>::digits;
            return !config.mPrimaryLayerMasks.empty() && !config.mBipedSubfieldMasks.empty()
                && config.mPrimaryLayerMasks.size() <= std::numeric_limits<std::uint64_t>::digits
                && config.mBipedSubfieldMasks.size() <= std::numeric_limits<std::uint32_t>::digits
                && config.mLayerMask != kNoMask && config.mSubfieldMask != kNoMask && config.mDisableBit != kNoMask
                && config.mAlternateRuleBit != kNoMask && config.mGroupShift < kWordBits
                && config.mSubfieldShift < kWordBits && config.mGroupShift + config.mSubfieldShift < kWordBits;
        }

        bool testPrimary(const FalloutCollisionFilterConfig& config, std::uint32_t firstLayer,
            std::uint32_t secondLayer)
        {
            return ((config.mPrimaryLayerMasks[firstLayer] >> secondLayer) & kSetBit) != 0;
        }

        bool testBiped(const FalloutCollisionFilterConfig& config, std::uint32_t firstSubfield,
            std::uint32_t secondSubfield)
        {
            return ((config.mBipedSubfieldMasks[firstSubfield] >> secondSubfield) & kSetBit) != 0;
        }

        bool isBipedLayer(const FalloutCollisionFilterConfig& config, std::uint32_t layer)
        {
            return layer == config.mBipedLayer || layer == config.mDeadBipedLayer;
        }
    }

    std::optional<bool> evaluateFalloutCollisionFilter(
        std::uint32_t first, std::uint32_t second, const FalloutCollisionFilterConfig& config)
    {
        if (!isValid(config))
            return std::nullopt;

        const std::uint32_t firstLayer = first & config.mLayerMask;
        const std::uint32_t secondLayer = second & config.mLayerMask;
        if (firstLayer >= config.mPrimaryLayerMasks.size() || secondLayer >= config.mPrimaryLayerMasks.size())
            return std::nullopt;

        if (firstLayer != config.mOrderedDisableExceptionLayer
            && ((first | second) & config.mDisableBit) != 0)
            return false;

        const std::uint32_t firstGroup = first >> config.mGroupShift;
        const std::uint32_t secondGroup = second >> config.mGroupShift;
        if (firstGroup == kNoSystemGroup || secondGroup == kNoSystemGroup)
            return true;

        const bool sameGroup = firstGroup == secondGroup;
        const std::uint32_t firstSubfield = (first >> config.mSubfieldShift) & config.mSubfieldMask;
        const std::uint32_t secondSubfield = (second >> config.mSubfieldShift) & config.mSubfieldMask;
        if (firstSubfield >= config.mBipedSubfieldMasks.size() || secondSubfield >= config.mBipedSubfieldMasks.size())
            return std::nullopt;
        if (sameGroup && firstLayer == config.mBipedLayer && secondLayer == config.mBipedLayer)
            return testBiped(config, firstSubfield, secondSubfield);

        if (!sameGroup)
            return testPrimary(config, firstLayer, secondLayer);

        if ((first & second & config.mAlternateRuleBit) != 0)
        {
            if (!testPrimary(config, firstLayer, secondLayer))
                return false;
            const int difference = static_cast<int>(firstSubfield) - static_cast<int>(secondSubfield);
            const int adjacentDistance = static_cast<int>(config.mAdjacentSubfieldDistance);
            return difference != adjacentDistance && difference != -adjacentDistance;
        }

        if (!isBipedLayer(config, firstLayer) || !isBipedLayer(config, secondLayer))
            return false;
        return testBiped(config, firstSubfield, secondSubfield);
    }
}
