/*
  Copyright (C) 2020-2021 cc9cii

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  cc9cii cc9c@iinet.net.au

  Much of the information on the data structures are based on the information
  from Tes4Mod:Mod_File_Format and Tes5Mod:File_Formats but also refined by
  trial & error.  See http://en.uesp.net/wiki for details.

*/
#include "loadqust.hpp"

#include <cstring>
#include <stdexcept>

#include "reader.hpp"
// #include "writer.hpp"

void ESM4::Quest::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    QuestStage* currentStage = nullptr;
    QuestStageEntry* currentStageEntry = nullptr;
    QuestObjective* currentObjective = nullptr;
    QuestObjectiveTarget* currentObjectiveTarget = nullptr;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mQuestName);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mFileName);
                break; // TES4 (none in FO3/FONV)
            case ESM::fourCC("DATA"):
            {
                if (subHdr.dataSize == 2) // TES4
                {
                    reader.get(&mData, 2);
                    mData.questDelay = 0.f; // unused in TES4 but keep it clean

                    // if ((mData.flags & Flag_StartGameEnabled) != 0)
                    // std::cout << "start quest " << mEditorId << std::endl;
                }
                else
                    reader.get(mData); // FO3

                break;
            }
            case ESM::fourCC("SCRI"):
                reader.getFormId(mQuestScript);
                break;
            case ESM::fourCC("CTDA"): // FIXME: how to detect if 1st/2nd param is a formid?
            {
                TargetCondition condition;
                if (!loadTargetCondition(reader, condition))
                    break;

                if (currentObjectiveTarget != nullptr)
                    currentObjectiveTarget->mConditions.push_back(condition);
                else if (currentStageEntry != nullptr)
                    currentStageEntry->mConditions.push_back(condition);
                else
                    mTargetConditions.push_back(condition);

                break;
            }
            case ESM::fourCC("INDX"):
                if (subHdr.dataSize == sizeof(std::int16_t))
                {
                    currentStage = &mStages.emplace_back();
                    reader.get(currentStage->mIndex);
                    currentStageEntry = nullptr;
                    currentObjective = nullptr;
                    currentObjectiveTarget = nullptr;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("QSDT"):
                if (subHdr.dataSize == sizeof(std::uint8_t) && currentStage != nullptr)
                {
                    currentStageEntry = &currentStage->mEntries.emplace_back();
                    reader.get(currentStageEntry->mFlags);
                    currentObjective = nullptr;
                    currentObjectiveTarget = nullptr;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("CNAM"):
                if (currentStageEntry != nullptr)
                    reader.getLocalizedString(currentStageEntry->mLogEntry);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("NAM0"):
                if (currentStageEntry != nullptr)
                    reader.getFormId(currentStageEntry->mNextQuest);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("QOBJ"):
                if (subHdr.dataSize == sizeof(std::int32_t))
                {
                    currentObjective = &mObjectives.emplace_back();
                    reader.get(currentObjective->mIndex);
                    currentObjectiveTarget = nullptr;
                    currentStage = nullptr;
                    currentStageEntry = nullptr;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("NNAM"):
                if (currentObjective != nullptr)
                    reader.getLocalizedString(currentObjective->mDescription);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("QSTA"):
                if (subHdr.dataSize == 8 && currentObjective != nullptr)
                {
                    currentObjectiveTarget = &currentObjective->mTargets.emplace_back();
                    reader.getFormId(currentObjectiveTarget->mTarget);
                    reader.get(currentObjectiveTarget->mFlags);
                    reader.skipSubRecordData(3);
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("SCHR"):
            case ESM::fourCC("SCDA"):
            case ESM::fourCC("SCTX"):
            case ESM::fourCC("SCRO"):
            case ESM::fourCC("SLSD"):
            case ESM::fourCC("SCVR"):
            case ESM::fourCC("SCRV"):
                if (currentStageEntry != nullptr)
                    loadScriptSubRecord(reader, currentStageEntry->mScript);
                else if (!loadScriptSubRecord(reader, mScript))
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("ANAM"): // TES5
            case ESM::fourCC("DNAM"): // TES5
            case ESM::fourCC("ENAM"): // TES5
            case ESM::fourCC("FNAM"): // TES5
            case ESM::fourCC("NEXT"): // TES5
            case ESM::fourCC("ALCA"): // TES5
            case ESM::fourCC("ALCL"): // TES5
            case ESM::fourCC("ALCO"): // TES5
            case ESM::fourCC("ALDN"): // TES5
            case ESM::fourCC("ALEA"): // TES5
            case ESM::fourCC("ALED"): // TES5
            case ESM::fourCC("ALEQ"): // TES5
            case ESM::fourCC("ALFA"): // TES5
            case ESM::fourCC("ALFC"): // TES5
            case ESM::fourCC("ALFD"): // TES5
            case ESM::fourCC("ALFE"): // TES5
            case ESM::fourCC("ALFI"): // TES5
            case ESM::fourCC("ALFL"): // TES5
            case ESM::fourCC("ALFR"): // TES5
            case ESM::fourCC("ALID"): // TES5
            case ESM::fourCC("ALLS"): // TES5
            case ESM::fourCC("ALNA"): // TES5
            case ESM::fourCC("ALNT"): // TES5
            case ESM::fourCC("ALPC"): // TES5
            case ESM::fourCC("ALRT"): // TES5
            case ESM::fourCC("ALSP"): // TES5
            case ESM::fourCC("ALST"): // TES5
            case ESM::fourCC("ALUA"): // TES5
            case ESM::fourCC("CIS1"): // TES5
            case ESM::fourCC("CIS2"): // TES5
            case ESM::fourCC("CNTO"): // TES5
            case ESM::fourCC("COCT"): // TES5
            case ESM::fourCC("ECOR"): // TES5
            case ESM::fourCC("FLTR"): // TES5
            case ESM::fourCC("KNAM"): // TES5
            case ESM::fourCC("KSIZ"): // TES5
            case ESM::fourCC("KWDA"): // TES5
            case ESM::fourCC("QNAM"): // TES5
            case ESM::fourCC("QTGL"): // TES5
            case ESM::fourCC("SPOR"): // TES5
            case ESM::fourCC("VMAD"): // TES5
            case ESM::fourCC("VTCK"): // TES5
            case ESM::fourCC("ALCC"): // FO4
            case ESM::fourCC("ALCS"): // FO4
            case ESM::fourCC("ALDI"): // FO4
            case ESM::fourCC("ALFV"): // FO4
            case ESM::fourCC("ALLA"): // FO4
            case ESM::fourCC("ALMI"): // FO4
            case ESM::fourCC("GNAM"): // FO4
            case ESM::fourCC("GWOR"): // FO4
            case ESM::fourCC("LNAM"): // FO4
            case ESM::fourCC("NAM2"): // FO4
            case ESM::fourCC("OCOR"): // FO4
            case ESM::fourCC("SNAM"): // FO4
            case ESM::fourCC("XNAM"): // FO4
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadqust"))
                    break;
                throw std::runtime_error("ESM4::QUST::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
    // if (mEditorId == "DAConversations")
    // std::cout << mEditorId << std::endl;
}

// void ESM4::Quest::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Quest::blank()
//{
// }
