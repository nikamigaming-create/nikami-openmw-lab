#ifndef OPENMW_COMPONENTS_NIFBULLET_FALLOUTCOLLISIONFILTER_H
#define OPENMW_COMPONENTS_NIFBULLET_FALLOUTCOLLISIONFILTER_H

#include <cstdint>
#include <optional>

namespace NifBullet
{
    // Evaluates two initialized Fallout 3 / New Vegas collision-filter words. A missing result identifies a layer
    // outside the retail table and must be handled as unsupported by the caller.
    std::optional<bool> evaluateFalloutCollisionFilter(std::uint32_t first, std::uint32_t second);
}

#endif
