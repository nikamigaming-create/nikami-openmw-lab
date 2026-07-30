#include "ownership.hpp"

#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadnpc.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"

namespace MWMechanics
{
    namespace
    {
        ESM::RefId refIdFromRuntimeFormId(ESM::FormId value)
        {
            if (value.isZeroOrUnset())
                return {};
            if (value.hasContentFile())
                return ESM::RefId::formIdRefId(value);
            return ESM::RefId::generated(value.mIndex);
        }
    }

    Ownership resolveOwnership(const MWWorld::Ptr& target, const MWWorld::ESMStore& store)
    {
        Ownership result;
        const MWWorld::CellRef& cellRef = target.getCellRef();
        result.mOwner = cellRef.getOwner();
        const bool referenceOwner = !result.mOwner.empty();
        if (!referenceOwner && target.isInCell())
            result.mOwner = target.getCell()->getOwner();

        result.mOwnerIsFaction = !result.mOwner.empty()
            && store.get<ESM4::Faction>().search(result.mOwner) != nullptr;
        if (result.mOwnerIsFaction)
        {
            result.mFaction = result.mOwner;
            // ESM4 actors have no reference faction-rank field. They are
            // returned before ordinary use/stealing checks, but keeping this
            // resolver total makes it safe for diagnostics and tests.
            if (referenceOwner && target.getType() != ESM4::Npc::sRecordId
                && target.getType() != ESM4::Creature::sRecordId)
                result.mRequiredFactionRank = cellRef.getFactionRank();
        }
        else
        {
            result.mFaction = cellRef.getFaction();
            if (!result.mFaction.empty())
                result.mRequiredFactionRank = cellRef.getFactionRank();
        }
        return result;
    }

    bool isOwnershipAllowed(const Ownership& ownership, const ESM::RefId& actorBase,
        ESM::FormId actorReference, const std::map<ESM::RefId, int>* actorFactions)
    {
        if (!ownership.mOwner.empty() && !ownership.mOwnerIsFaction
            && ownership.mOwner != refIdFromRuntimeFormId(actorReference)
            && ownership.mOwner != actorBase && ownership.mOwner != "Player")
            return false;

        if (ownership.mFaction.empty())
            return true;
        if (actorFactions == nullptr)
            return false;

        const auto found = actorFactions->find(ownership.mFaction);
        return found != actorFactions->end()
            && (ownership.mRequiredFactionRank < 0 || found->second >= ownership.mRequiredFactionRank);
    }
}
