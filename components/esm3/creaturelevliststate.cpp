#include "creaturelevliststate.hpp"

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{

    void CreatureLevListState::load(ESMReader& esm)
    {
        ObjectState::load(esm);

<<<<<<< HEAD
        if (esm.getFormatVersion() <= MaxActorIdSaveGameFormatVersion)
        {
            mSpawnedActor.mIndex = static_cast<uint32_t>(-1);
            esm.getHNOT(mSpawnedActor.mIndex, "SPAW");
        }
        else if (esm.peekNextSub("SPAW"))
            mSpawnedActor = esm.getFormId(true, "SPAW");
=======
        mSpawnActorId = -1;
        esm.getHNOT(mSpawnActorId, "SPAW");
>>>>>>> origin/main

        mSpawn = false;
        esm.getHNOT(mSpawn, "RESP");
    }

    void CreatureLevListState::save(ESMWriter& esm, bool inInventory) const
    {
        ObjectState::save(esm, inInventory);

<<<<<<< HEAD
        if (mSpawnedActor.isSet())
            esm.writeFormId(mSpawnedActor, true, "SPAW");
=======
        if (mSpawnActorId != -1)
            esm.writeHNT("SPAW", mSpawnActorId);
>>>>>>> origin/main

        if (mSpawn)
            esm.writeHNT("RESP", mSpawn);
    }

}
