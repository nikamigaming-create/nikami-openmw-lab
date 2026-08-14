#ifndef OPENMW_ESM_CREATURELEVLISTSTATE_H
#define OPENMW_ESM_CREATURELEVLISTSTATE_H

#include "objectstate.hpp"

namespace ESM
{
    // format 0, saved games only

    struct CreatureLevListState final : public ObjectState
    {
<<<<<<< HEAD
        ESM::RefNum mSpawnedActor; // actor id in older saves
=======
        int32_t mSpawnActorId;
>>>>>>> origin/main
        bool mSpawn;

        void load(ESMReader& esm) override;
        void save(ESMWriter& esm, bool inInventory = false) const override;

        CreatureLevListState& asCreatureLevListState() override { return *this; }
        const CreatureLevListState& asCreatureLevListState() const override { return *this; }
    };
}

#endif
