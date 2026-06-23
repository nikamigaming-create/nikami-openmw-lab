#ifndef OPENMW_ESM_QUESTOBJECTIVESTATE_H
#define OPENMW_ESM_QUESTOBJECTIVESTATE_H

#include <components/esm/refid.hpp>

#include <cstdint>

namespace ESM
{
    class ESMReader;
    class ESMWriter;

    // format 0, saved games only

    struct QuestObjectiveState
    {
        ESM::RefId mTopic;
        int32_t mObjective = 0;
        uint8_t mDisplayed = 0;
        uint8_t mCompleted = 0;

        void load(ESMReader& esm);
        void save(ESMWriter& esm) const;
    };
}

#endif
