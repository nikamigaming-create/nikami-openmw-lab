#include "ownership.hpp"

#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadfact.hpp>

#include "../mwworld/cell.hpp"
#include "../mwworld/cellref.hpp"
#include "../mwworld/esmstore.hpp"

namespace
{
    bool isEsm4Faction(const ESM::RefId& id, const MWWorld::ESMStore& store)
    {
        return !id.empty() && store.get<ESM4::Faction>().search(id) != nullptr;
    }
}

namespace MWMechanics
{
    ObjectOwnership resolveObjectOwnership(
        const MWWorld::CellRef& cellRef, const MWWorld::Cell* cell, const MWWorld::ESMStore& store)
    {
        ObjectOwnership result{ cellRef.getOwner(), cellRef.getFaction(), cellRef.getFactionRank() };

        // ESM3 records already distinguish owner and faction. ESM4 serializes either kind through XOWN, so a
        // matching FACT record is authoritative even when the reference itself is not presently attached to a cell.
        if (!result.mOwner.empty())
        {
            if (result.mFaction.empty() && isEsm4Faction(result.mOwner, store))
            {
                result.mFaction = result.mOwner;
                result.mOwner = {};
            }
            return result;
        }
        if (!result.mFaction.empty() || cell == nullptr || !cell->isEsm4())
            return result;

        const ESM::RefId cellOwner(cell->getEsm4().mOwner);
        if (isEsm4Faction(cellOwner, store))
            result.mFaction = cellOwner;
        else
            result.mOwner = cellOwner;

        // CELL has no parsed XRNK in the current ESM4 model. Any authored reference XRNK was already retained above;
        // inherited ownership therefore intentionally keeps the default "any member" rank.
        result.mFactionRank = -1;
        return result;
    }

    bool isFactionOwnershipAllowed(
        const ObjectOwnership& ownership, std::span<const ESM4::ActorFaction> factions)
    {
        if (ownership.mFaction.empty())
            return true;

        for (const ESM4::ActorFaction& membership : factions)
        {
            const ESM::RefId faction(ESM::FormId::fromUint32(membership.faction));
            if (faction == ownership.mFaction)
                return ownership.mFactionRank < 0 || membership.rank >= ownership.mFactionRank;
        }
        return false;
    }
}
