#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_TYPES_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_TYPES_H

#include <components/misc/strongtypedef.hpp>

#include <cstdint>

namespace OcclusionCulling
{
    // Strong typedefs mirroring the navmeshdb pattern
    using CellId      = Misc::StrongTypedef<std::int64_t, struct CellIdTag>;
    using CellVersion = Misc::StrongTypedef<std::int64_t, struct CellVersionTag>;

    // A bitmask-encoded PVS entry.
    // We store for each source cell a blob of visible-cell IDs (sorted int64 list,
    // LZ4-compressed) keyed by (worldspace, cell_x, cell_y, content_hash).
    // content_hash is an XOR-folded hash of the sorted ref-IDs + positions in the
    // cell, so the entry is invalidated automatically when mods change cell contents.
}

#endif
