#include "questobjectivestate.hpp"

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{
    void QuestObjectiveState::load(ESMReader& esm)
    {
        mTopic = esm.getHNRefId("YETO");
        esm.getHNT(mObjective, "QOID");
        esm.getHNT(mDisplayed, "QDIS");
        esm.getHNT(mCompleted, "QDON");
    }

    void QuestObjectiveState::save(ESMWriter& esm) const
    {
        esm.writeHNRefId("YETO", mTopic);
        esm.writeHNT("QOID", mObjective);
        esm.writeHNT("QDIS", mDisplayed);
        esm.writeHNT("QDON", mCompleted);
    }
}
