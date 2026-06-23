/*
  Copyright (C) 2026 Nikami

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software in
     a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "loadperk.hpp"

#include <stdexcept>

#include "reader.hpp"

namespace
{
    std::vector<std::uint8_t> readRawSubrecord(ESM4::Reader& reader, std::uint16_t size)
    {
        std::vector<std::uint8_t> value(size);
        if (size != 0)
            reader.get(value.data(), value.size());
        return value;
    }
}

void ESM4::Perk::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mFullName);
                break;
            case ESM::fourCC("DESC"):
                reader.getLocalizedString(mDescription);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mIcon);
                break;
            case ESM::fourCC("DATA"):
                mData.push_back(readRawSubrecord(reader, subHdr.dataSize));
                break;
            case ESM::fourCC("CTDA"):
            {
                if (subHdr.dataSize == sizeof(TargetCondition))
                {
                    TargetCondition condition;
                    reader.get(condition);
                    if (condition.reference)
                        reader.adjustFormId(condition.reference);
                    mConditions.push_back(condition);
                }
                else
                    reader.skipSubRecordData();
                break;
            }
            case ESM::fourCC("EPFD"):
                mEffectData.push_back(readRawSubrecord(reader, subHdr.dataSize));
                break;
            case ESM::fourCC("EPFT"):
            {
                std::uint8_t value = 0;
                reader.get(value);
                mEffectTypes.push_back(value);
                break;
            }
            case ESM::fourCC("PRKC"):
            {
                std::uint8_t value = 0;
                reader.get(value);
                mEffectRanks.push_back(value);
                break;
            }
            case ESM::fourCC("PRKE"):
                mEffectPriorities.push_back(readRawSubrecord(reader, subHdr.dataSize));
                break;
            case ESM::fourCC("EPF2"):
            case ESM::fourCC("EPF3"):
            {
                std::string value;
                reader.getString(value);
                mEffectStrings.push_back(value);
                break;
            }
            case ESM::fourCC("SCHR"):
                reader.get(mScript.scriptHeader);
                break;
            case ESM::fourCC("SCDA"):
                reader.skipSubRecordData();
                break;
            case ESM::fourCC("SCTX"):
                reader.getString(mScript.scriptSource);
                break;
            case ESM::fourCC("SCRO"):
            {
                ESM::FormId reference;
                reader.getFormId(reference);
                mScriptReferences.push_back(reference);
                break;
            }
            case ESM::fourCC("PRKF"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::PERK::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
