#ifndef OPENMW_MWMECHANICS_OWNERSHIP_H
#define OPENMW_MWMECHANICS_OWNERSHIP_H

#include <span>

#include <components/esm/refid.hpp>
#include <components/esm4/actor.hpp>

namespace MWWorld
{
    class Cell;
    class CellRef;
    class ESMStore;
}

namespace MWMechanics
{
    struct ObjectOwnership
    {
        ESM::RefId mOwner;
        ESM::RefId mFaction;
        int mFactionRank = -1;
    };

    /// Resolve authored ownership without losing ESM4's polymorphic XOWN field. Reference ownership takes
    /// precedence; otherwise an ESM4 reference inherits its CELL XOWN. An XOWN that names a FACT is returned as
    /// faction ownership rather than actor ownership.
    [[nodiscard]] ObjectOwnership resolveObjectOwnership(
        const MWWorld::CellRef& cellRef, const MWWorld::Cell* cell, const MWWorld::ESMStore& store);

    /// Return whether an FNV actor's authored faction membership satisfies the ownership rank requirement.
    [[nodiscard]] bool isFactionOwnershipAllowed(
        const ObjectOwnership& ownership, std::span<const ESM4::ActorFaction> factions);
}

#endif
