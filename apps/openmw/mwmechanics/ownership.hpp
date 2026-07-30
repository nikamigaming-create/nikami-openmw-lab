#ifndef OPENMW_MWMECHANICS_OWNERSHIP_H
#define OPENMW_MWMECHANICS_OWNERSHIP_H

#include <map>

#include <components/esm/formid.hpp>
#include <components/esm/refid.hpp>

namespace MWWorld
{
    class ESMStore;
    class Ptr;
}

namespace MWMechanics
{
    struct Ownership
    {
        ESM::RefId mOwner;
        ESM::RefId mFaction;
        int mRequiredFactionRank = -1;
        bool mOwnerIsFaction = false;
    };

    /// Resolve the owner which applies to a live object. An explicit
    /// reference owner takes precedence; otherwise the containing cell owner
    /// is inherited.
    Ownership resolveOwnership(const MWWorld::Ptr& target, const MWWorld::ESMStore& store);

    /// Check personal and faction ownership without performing crime or world
    /// side effects. A null faction map represents an actor without faction
    /// membership.
    bool isOwnershipAllowed(const Ownership& ownership, const ESM::RefId& actorBase,
        ESM::FormId actorReference, const std::map<ESM::RefId, int>* actorFactions);
}

#endif
