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
#include "loadinfo.hpp"

#include <cstring>
#include <stdexcept>

#include <components/esm/refid.hpp>

#include "reader.hpp"
<<<<<<< HEAD
//#include "writer.hpp"
=======
#include "grouptype.hpp"
// #include "writer.hpp"
>>>>>>> origin/main

void ESM4::DialogInfo::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

<<<<<<< HEAD
    mEditorId = ESM::RefId(mId).serializeText(); // FIXME: quick workaround to use existing code

    bool ignore = false;
=======
    // FO3/FONV INFO records normally inherit their topic from the surrounding
    // topic-child GRUP rather than carrying a TPIC subrecord of their own.
    // Retaining a zero topic here made every loaded response unreachable.
    if (reader.stackSize() != 0 && reader.grp().type == ESM4::Grp_TopicChild)
        mTopic = ESM::FormId::fromUint32(reader.grp().label.value);

    mEditorId = ESM::RefId(mId).serializeText(); // FIXME: quick workaround to use existing code

    bool readingEndScript = false;
    DialogResponse* currentResponse = nullptr;
>>>>>>> origin/main

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("QSTI"):
                reader.getFormId(mQuest);
                break; // FormId quest id
<<<<<<< HEAD
=======
            case ESM::fourCC("TPIC"):
                reader.getFormId(mTopic);
                break;
            case ESM::fourCC("PNAM"):
                reader.getFormId(mPreviousInfo);
                break;
>>>>>>> origin/main
            case ESM::fourCC("SNDD"):
                reader.getFormId(mSound);
                break; // FO3 (not used in FONV?)
            case ESM::fourCC("TRDT"):
            {
<<<<<<< HEAD
                if (subHdr.dataSize == 16) // TES4
                    reader.get(&mResponseData, 16);
                else if (subHdr.dataSize == 20) // FO3
                    reader.get(&mResponseData, 20);
                else // FO3/FONV
                {
                    reader.get(mResponseData);
                    if (mResponseData.sound)
                        reader.adjustFormId(mResponseData.sound);
                }
=======
                currentResponse = &mResponses.emplace_back();
                if (subHdr.dataSize == 16) // TES4
                    reader.get(&currentResponse->mData, 16);
                else if (subHdr.dataSize == 20) // FO3
                    reader.get(&currentResponse->mData, 20);
                else if (subHdr.dataSize == sizeof(TargetResponseData)) // FO3/FONV
                {
                    reader.get(currentResponse->mData);
                    if (currentResponse->mData.sound)
                        reader.adjustFormId(currentResponse->mData.sound);
                }
                else
                    reader.skipSubRecordData();

                mResponseData = currentResponse->mData;
>>>>>>> origin/main

                break;
            }
            case ESM::fourCC("NAM1"):
<<<<<<< HEAD
                reader.getLocalizedString(mResponse);
                break; // response text
            case ESM::fourCC("NAM2"):
                reader.getZString(mNotes);
                break; // actor notes
            case ESM::fourCC("NAM3"):
                reader.getZString(mEdits);
                break; // not in TES4
            case ESM::fourCC("CTDA"): // FIXME: how to detect if 1st/2nd param is a formid?
            {
                if (subHdr.dataSize == 24) // TES4
                    reader.get(&mTargetCondition, 24);
                else if (subHdr.dataSize == 20) // FO3
                    reader.get(&mTargetCondition, 20);
                else if (subHdr.dataSize == 28)
                {
                    reader.get(mTargetCondition); // FO3/FONV
                    if (mTargetCondition.reference)
                        reader.adjustFormId(mTargetCondition.reference);
                }
                else // TES5
                {
                    reader.get(&mTargetCondition, 20);
                    if (subHdr.dataSize == 36)
                        reader.getFormId(mParam3);
                    reader.get(mTargetCondition.runOn);
                    reader.get(mTargetCondition.reference);
                    if (mTargetCondition.reference)
                        reader.adjustFormId(mTargetCondition.reference);
                    reader.skipSubRecordData(4); // unknown
=======
                if (currentResponse != nullptr)
                {
                    reader.getLocalizedString(currentResponse->mResponse);
                    mResponse = currentResponse->mResponse;
                }
                else
                    reader.getLocalizedString(mResponse);
                break; // response text
            case ESM::fourCC("NAM2"):
                if (currentResponse != nullptr)
                {
                    reader.getZString(currentResponse->mNotes);
                    mNotes = currentResponse->mNotes;
                }
                else
                    reader.getZString(mNotes);
                break; // actor notes
            case ESM::fourCC("NAM3"):
                if (currentResponse != nullptr)
                {
                    reader.getZString(currentResponse->mEdits);
                    mEdits = currentResponse->mEdits;
                }
                else
                    reader.getZString(mEdits);
                break; // not in TES4
            case ESM::fourCC("SNAM"):
                if (currentResponse != nullptr)
                    reader.getFormId(currentResponse->mSpeakerAnimation);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("LNAM"):
                if (currentResponse != nullptr)
                    reader.getFormId(currentResponse->mListenerAnimation);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("CTDA"):
            case ESM::fourCC("CTDT"):
            {
                TargetCondition condition;
                if (loadTargetCondition(reader, condition, &mParam3))
                {
                    mTargetCondition = condition;
                    mTargetConditions.push_back(condition);
>>>>>>> origin/main
                }

                break;
            }
            case ESM::fourCC("SCHR"):
<<<<<<< HEAD
            {
                if (!ignore)
                    reader.get(mScript.scriptHeader);
                else
                    reader.skipSubRecordData(); // TODO: does the second one ever used?

                break;
            }
            case ESM::fourCC("SCDA"):
                reader.skipSubRecordData();
                break; // compiled script data
            case ESM::fourCC("SCTX"):
                reader.getString(mScript.scriptSource);
                break;
            case ESM::fourCC("SCRO"):
                reader.getFormId(mScript.globReference);
                break;
            case ESM::fourCC("SLSD"):
            {
                ScriptLocalVariableData localVar;
                reader.get(localVar.index);
                reader.get(localVar.unknown1);
                reader.get(localVar.unknown2);
                reader.get(localVar.unknown3);
                reader.get(localVar.type);
                reader.get(localVar.unknown4);
                mScript.localVarData.push_back(std::move(localVar));
                // WARN: assumes SCVR will follow immediately

                break;
            }
            case ESM::fourCC("SCVR"): // assumed always pair with SLSD
            {
                if (!mScript.localVarData.empty())
                    reader.getZString(mScript.localVarData.back().variableName);
                else
                    reader.skipSubRecordData();

                break;
            }
            case ESM::fourCC("SCRV"):
            {
                std::uint32_t index;
                reader.get(index);

                mScript.localRefVarIndex.push_back(index);

                break;
            }
            case ESM::fourCC("NEXT"): // FO3/FONV marker for next script header
            {
                ignore = true;

                break;
            }
=======
            case ESM::fourCC("SCDA"):
            case ESM::fourCC("SCTX"):
            case ESM::fourCC("SCRO"):
            case ESM::fourCC("SLSD"):
            case ESM::fourCC("SCVR"): // assumed always pair with SLSD
            case ESM::fourCC("SCRV"):
                loadScriptSubRecord(reader, readingEndScript ? mEndScript : mScript);
                break;
            case ESM::fourCC("NEXT"): // FO3/FONV marker for next script header
                readingEndScript = true;
                break;
>>>>>>> origin/main
            case ESM::fourCC("DATA"): // always 3 for TES4 ?
            {
                if (subHdr.dataSize == 4) // FO3/FONV
                {
                    reader.get(mDialType);
                    reader.get(mNextSpeaker);
                    reader.get(mInfoFlags);
                }
                else
                    reader.skipSubRecordData(); // FIXME
                break;
            }
<<<<<<< HEAD
            case ESM::fourCC("NAME"): // FormId add topic (not always present)
            case ESM::fourCC("CTDT"): // older version of CTDA? 20 bytes
            case ESM::fourCC("SCHD"): // 28 bytes
            case ESM::fourCC("TCLT"): // FormId choice
            case ESM::fourCC("TCLF"): // FormId
            case ESM::fourCC("PNAM"): // TES4 DLC
            case ESM::fourCC("TPIC"): // TES4 DLC
            case ESM::fourCC("ANAM"): // FO3 speaker formid
            case ESM::fourCC("DNAM"): // FO3 speech challenge
            case ESM::fourCC("KNAM"): // FO3 formid
            case ESM::fourCC("LNAM"): // FONV
            case ESM::fourCC("TCFU"): // FONV
=======
            case ESM::fourCC("NAME"):
                reader.getFormId(mAddTopics.emplace_back());
                break;
            case ESM::fourCC("TCLT"):
                reader.getFormId(mChoices.emplace_back());
                break;
            case ESM::fourCC("TCLF"):
                reader.getFormId(mLinkFrom.emplace_back());
                break;
            case ESM::fourCC("TCFU"):
                reader.getFormId(mFollowUps.emplace_back());
                break;
            case ESM::fourCC("RNAM"):
                reader.getLocalizedString(mPrompt);
                break;
            case ESM::fourCC("ANAM"):
                reader.getFormId(mSpeaker);
                break;
            case ESM::fourCC("KNAM"):
                reader.getFormId(mActorValueOrPerk);
                break;
            case ESM::fourCC("DNAM"):
                if (subHdr.dataSize == sizeof(mSpeechChallenge))
                    reader.get(mSpeechChallenge);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("SCHD"): // 28 bytes
>>>>>>> origin/main
            case ESM::fourCC("TIFC"): // TES5
            case ESM::fourCC("TWAT"): // TES5
            case ESM::fourCC("CIS1"): // TES5
            case ESM::fourCC("CIS2"): // TES5
            case ESM::fourCC("CNAM"): // TES5
            case ESM::fourCC("ENAM"): // TES5
            case ESM::fourCC("EDID"): // TES5
            case ESM::fourCC("VMAD"): // TES5
            case ESM::fourCC("BNAM"): // TES5
<<<<<<< HEAD
            case ESM::fourCC("SNAM"): // TES5
            case ESM::fourCC("ONAM"): // TES5
            case ESM::fourCC("QNAM"): // TES5 for mScript
            case ESM::fourCC("RNAM"): // TES5
=======
            case ESM::fourCC("ONAM"): // TES5
            case ESM::fourCC("QNAM"): // TES5 for mScript
>>>>>>> origin/main
            case ESM::fourCC("ALFA"): // FO4
            case ESM::fourCC("GNAM"): // FO4
            case ESM::fourCC("GREE"): // FO4
            case ESM::fourCC("INAM"): // FO4
            case ESM::fourCC("INCC"): // FO4
            case ESM::fourCC("INTV"): // FO4
            case ESM::fourCC("IOVR"): // FO4
            case ESM::fourCC("MODQ"): // FO4
            case ESM::fourCC("NAM0"): // FO4
            case ESM::fourCC("NAM4"): // FO4
            case ESM::fourCC("NAM9"): // FO4
            case ESM::fourCC("SRAF"): // FO4
            case ESM::fourCC("TIQS"): // FO4
            case ESM::fourCC("TNAM"): // FO4
            case ESM::fourCC("TRDA"): // FO4
            case ESM::fourCC("TSCE"): // FO4
            case ESM::fourCC("WZMD"): // FO4
                reader.skipSubRecordData();
                break;
            default:
<<<<<<< HEAD
=======
                if (reader.skipUnknownStarfieldSubRecordData("loadinfo"))
                    break;
>>>>>>> origin/main
                throw std::runtime_error("ESM4::INFO::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}

// void ESM4::DialogInfo::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::DialogInfo::blank()
//{
// }
