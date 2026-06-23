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
#include <utility>

#include "reader.hpp"
//#include "writer.hpp"

namespace
{
    bool readQuestCondition(ESM4::Reader& reader, std::uint16_t dataSize, ESM4::TargetCondition& cond)
    {
        if (dataSize == 20)
        {
            reader.get(&cond, 20);
            cond.runOn = 0;
            cond.reference = 0;
            return true;
        }

        if (dataSize == 24)
        {
            reader.get(&cond, 24);
            cond.reference = 0;
            return true;
        }

        if (dataSize == 28)
        {
            reader.get(cond);
            if (cond.reference)
                reader.adjustFormId(cond.reference);
            return true;
        }

        return false;
    }

    ESM4::ScriptDefinition& getActiveScript(ESM4::QuestStageEntry* currentEntry, ESM4::Quest& quest)
    {
        if (currentEntry != nullptr)
            return currentEntry->mScript;
        return quest.mScript;
    }
}

void ESM4::Quest::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    ESM4::QuestStage* currentStage = nullptr;
    ESM4::QuestStageEntry* currentEntry = nullptr;
    ESM4::QuestObjective* currentObjective = nullptr;
    ESM4::QuestObjectiveTarget* currentTarget = nullptr;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getZString(mQuestName);
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
                TargetCondition cond;
                if (readQuestCondition(reader, subHdr.dataSize, cond))
                {
                    if (currentTarget != nullptr)
                        currentTarget->mConditions.push_back(cond);
                    else if (currentEntry != nullptr)
                        currentEntry->mConditions.push_back(cond);
                    else
                        mTargetConditions.push_back(cond);
                }
                else
                {
                    // one record with size 20: EDID GenericSupMutBehemoth
                    reader.skipSubRecordData(); // FIXME
                }
                // FIXME: support TES5

                break;
            }
            case ESM::fourCC("SCHR"):
                reader.get(getActiveScript(currentEntry, *this).scriptHeader);
                break;
            case ESM::fourCC("SCDA"):
                reader.skipSubRecordData();
                break; // compiled script data
            case ESM::fourCC("SCTX"):
                reader.getString(getActiveScript(currentEntry, *this).scriptSource);
                break;
            case ESM::fourCC("SCRO"):
                reader.getFormId(getActiveScript(currentEntry, *this).globReference);
                break;
            case ESM::fourCC("SLSD"): // FO3/FONV embedded script local variable data
            {
                if (subHdr.dataSize < 24)
                {
                    reader.skipSubRecordData();
                    break;
                }
                ScriptLocalVariableData localVar;
                reader.get(localVar.index);
                reader.get(localVar.unknown1);
                reader.get(localVar.unknown2);
                reader.get(localVar.unknown3);
                reader.get(localVar.type);
                reader.get(localVar.unknown4);
                if (subHdr.dataSize > 24)
                    reader.skipSubRecordData(subHdr.dataSize - 24);
                getActiveScript(currentEntry, *this).localVarData.push_back(std::move(localVar));
                break;
            }
            case ESM::fourCC("SCVR"): // assumed paired with SLSD
            {
                ScriptDefinition& script = getActiveScript(currentEntry, *this);
                if (!script.localVarData.empty())
                    reader.getZString(script.localVarData.back().variableName);
                else
                    reader.skipSubRecordData();
                break;
            }
            case ESM::fourCC("SCRV"):
            {
                if (subHdr.dataSize < sizeof(std::uint32_t))
                {
                    reader.skipSubRecordData();
                    break;
                }
                std::uint32_t index;
                reader.get(index);
                if (subHdr.dataSize > sizeof(index))
                    reader.skipSubRecordData(subHdr.dataSize - sizeof(index));
                getActiveScript(currentEntry, *this).localRefVarIndex.push_back(index);
                break;
            }
            case ESM::fourCC("INDX"):
            {
                std::int16_t index = 0;
                if (subHdr.dataSize >= sizeof(index))
                    reader.get(index);
                else
                    reader.skipSubRecordData();
                if (subHdr.dataSize > sizeof(index))
                    reader.skipSubRecordData(subHdr.dataSize - sizeof(index));

                mStages.emplace_back();
                currentStage = &mStages.back();
                currentStage->mIndex = index;
                currentEntry = nullptr;
                currentObjective = nullptr;
                currentTarget = nullptr;
                break;
            }
            case ESM::fourCC("QSDT"):
            {
                std::uint8_t flags = 0;
                if (subHdr.dataSize >= sizeof(flags))
                    reader.get(flags);
                else
                    reader.skipSubRecordData();
                if (subHdr.dataSize > sizeof(flags))
                    reader.skipSubRecordData(subHdr.dataSize - sizeof(flags));

                if (currentStage == nullptr)
                {
                    mStages.emplace_back();
                    currentStage = &mStages.back();
                }
                currentStage->mEntries.emplace_back();
                currentEntry = &currentStage->mEntries.back();
                currentEntry->mFlags = flags;
                currentObjective = nullptr;
                currentTarget = nullptr;
                break;
            }
            case ESM::fourCC("CNAM"):
                if (currentEntry != nullptr)
                    reader.getZString(currentEntry->mLogEntry);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("NNAM"): // FO3
                if (currentObjective != nullptr)
                    reader.getZString(currentObjective->mDescription);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("QOBJ"): // FO3
            {
                std::int32_t index = 0;
                if (subHdr.dataSize >= sizeof(index))
                    reader.get(index);
                else
                    reader.skipSubRecordData();
                if (subHdr.dataSize > sizeof(index))
                    reader.skipSubRecordData(subHdr.dataSize - sizeof(index));

                mObjectives.emplace_back();
                currentObjective = &mObjectives.back();
                currentObjective->mIndex = index;
                currentStage = nullptr;
                currentEntry = nullptr;
                currentTarget = nullptr;
                break;
            }
            case ESM::fourCC("QSTA"):
            {
                if (currentObjective == nullptr)
                {
                    reader.skipSubRecordData();
                    break;
                }

                currentObjective->mTargets.emplace_back();
                currentTarget = &currentObjective->mTargets.back();
                if (subHdr.dataSize >= 4)
                    reader.getFormId(currentTarget->mTarget);
                else
                {
                    reader.skipSubRecordData();
                    break;
                }
                if (subHdr.dataSize >= 5)
                    reader.get(currentTarget->mFlags);
                if (subHdr.dataSize > 5)
                    reader.skipSubRecordData(subHdr.dataSize - 5);
                break;
            }
            case ESM::fourCC("NAM0"): // FO3
                if (currentEntry != nullptr)
                    reader.getFormId(currentEntry->mNextQuest);
                else
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
