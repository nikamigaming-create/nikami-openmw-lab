#ifndef GAME_MWCLASS_FNVACTORSTATE_H
#define GAME_MWCLASS_FNVACTORSTATE_H

#include <string>
#include <string_view>

namespace ESM
{
    struct CreatureState;
    struct ObjectState;
}

namespace MWWorld
{
    class ConstPtr;
    class ESMStore;
    class Ptr;
}

namespace MWClass
{
    /// Validate mutable fields common to a placed FNV ObjectState before applying any outer reference mutation.
    bool validateFnvPlacedObjectState(
        const ESM::ObjectState& state, std::string_view objectKind, std::string& error);

    /// Validate the complete compatibility payload before applying any enclosing reference or runtime state.
    bool validateFnvActorState(bool isFnv, std::string_view actorKind, const ESM::CreatureState& state,
        const MWWorld::ESMStore& store, std::string& error);

    /// FNV NPC state hooks live outside ESM4Npc so the protected class header remains source-compatible.
    void readFnvNpcState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state);
    void writeFnvNpcState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state);
}

#endif
