/*
  Copyright (C) 2016, 2018, 2020-2021 cc9cii

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
#include "loadmisc.hpp"

#include <stdexcept>
#include <utility>

#include "reader.hpp"
//#include "writer.hpp"

void ESM4::MiscItem::load(ESM4::Reader& reader)
{
    MiscItem value;
    value.mOriginalRecordType = reader.hdr().record.typeId;
    const bool ordinaryMisc = value.mOriginalRecordType == ESM4::REC_MISC;
    const bool casinoChip = value.mOriginalRecordType == ESM4::REC_CHIP;
    const bool caravanCard = value.mOriginalRecordType == ESM4::REC_CCRD;
    const bool caravanMoney = value.mOriginalRecordType == ESM4::REC_CMNY;
    if (!ordinaryMisc && !casinoChip && !caravanCard && !caravanMoney)
        throw std::runtime_error("ESM4::MiscItem::load - unsupported record type "
            + ESM::printName(value.mOriginalRecordType));

    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(value.mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(value.mFullName);
                break;
            case ESM::fourCC("MODL"):
                reader.getZString(value.mModel);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(value.mIcon);
                break;
            case ESM::fourCC("MICO"):
                reader.getZString(value.mMiniIcon);
                break; // FO3
            case ESM::fourCC("SCRI"):
                reader.getFormId(value.mScriptId);
                break;
            case ESM::fourCC("DATA"):
                if (ordinaryMisc)
                    reader.get(value.mData);
                else
                    reader.get(value.mData.value);
                break;
            case ESM::fourCC("MODB"):
                reader.get(value.mBoundRadius);
                break;
            case ESM::fourCC("YNAM"):
                reader.getFormId(value.mPickUpSound);
                break;
            case ESM::fourCC("ZNAM"):
                reader.getFormId(value.mDropSound);
                break;
            case ESM::fourCC("TX00"):
                if (!caravanCard)
                    throw std::runtime_error("ESM4::MiscItem::load - TX00 outside CCRD");
                reader.getZString(value.mCardFaceTexture);
                break;
            case ESM::fourCC("TX01"):
                if (!caravanCard)
                    throw std::runtime_error("ESM4::MiscItem::load - TX01 outside CCRD");
                reader.getZString(value.mCardBackTexture);
                break;
            case ESM::fourCC("INTV"):
                if (!caravanCard)
                    throw std::runtime_error("ESM4::MiscItem::load - INTV outside CCRD");
                if (value.mCardSuit == 0)
                    reader.get(value.mCardSuit);
                else if (value.mCardRank == 0)
                    reader.get(value.mCardRank);
                else
                    throw std::runtime_error("ESM4::MiscItem::load - too many CCRD INTV fields");
                break;
            case ESM::fourCC("MODT"): // Model data
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"): // Model data end
            case ESM::fourCC("KSIZ"):
            case ESM::fourCC("KWDA"):
            case ESM::fourCC("OBND"):
            case ESM::fourCC("VMAD"):
            case ESM::fourCC("RNAM"): // FONV
            case ESM::fourCC("DAMC"): // Destructible
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DMDC"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DMDS"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DSTF"): // Destructible end
            case ESM::fourCC("CDIX"): // FO4
            case ESM::fourCC("CVPA"): // FO4
            case ESM::fourCC("FIMD"): // FO4
            case ESM::fourCC("PTRN"): // FO4
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::MISC::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }

    *this = std::move(value);
}

// void ESM4::MiscItem::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::MiscItem::blank()
//{
// }
