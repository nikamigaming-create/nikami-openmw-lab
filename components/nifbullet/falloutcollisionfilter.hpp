#ifndef OPENMW_COMPONENTS_NIFBULLET_FALLOUTCOLLISIONFILTER_H
#define OPENMW_COMPONENTS_NIFBULLET_FALLOUTCOLLISIONFILTER_H

#include <cstdint>
#include <optional>
#include <vector>

namespace NifBullet
{
    // All Fallout collision-filter policy is supplied by the caller. The
    // loader can obtain this from a reviewed Fallout data/config section, and
    // tests can inject a deterministic fixture. Keeping the table out of the
    // evaluator prevents retail policy from becoming anonymous engine state.
    struct FalloutCollisionFilterConfig
    {
        std::vector<std::uint64_t> mPrimaryLayerMasks;
        std::vector<std::uint32_t> mBipedSubfieldMasks;
        std::uint32_t mLayerMask = 0;
        std::uint32_t mSubfieldMask = 0;
        std::uint32_t mDisableBit = 0;
        std::uint32_t mAlternateRuleBit = 0;
        std::uint32_t mBipedLayer = 0;
        std::uint32_t mDeadBipedLayer = 0;
        std::uint32_t mOrderedDisableExceptionLayer = 0;
        std::uint32_t mGroupShift = 0;
        std::uint32_t mSubfieldShift = 0;
        std::uint32_t mAdjacentSubfieldDistance = 0;
    };

    // Evaluates two initialized Fallout 3 / New Vegas collision-filter words. A missing result identifies a layer
    // outside the injected retail table and must be handled as unsupported by the caller.
    std::optional<bool> evaluateFalloutCollisionFilter(
        std::uint32_t first, std::uint32_t second, const FalloutCollisionFilterConfig& config);
}

#endif
