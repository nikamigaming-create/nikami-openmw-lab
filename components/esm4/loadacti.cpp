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
#include "loadacti.hpp"

#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

void ESM4::Activator::load(ESM4::Reader& reader)
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
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("SCRI"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mScriptId);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("SNAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mLoopingSound);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("VNAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mActivationSound);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("MODB"):
                if (subHdr.dataSize == sizeof(mBoundRadius))
                    reader.get(mBoundRadius);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("INAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mRadioTemplate);
                else
                    reader.skipSubRecordData();
                break; // FONV
            case ESM::fourCC("RNAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mRadioStation);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("XATO"):
                reader.getZString(mActivationPrompt);
                break; // FONV
            case ESM::fourCC("MODT"): // Model data
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"): // Model data end
            case ESM::fourCC("DAMC"): // Destructible
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DMDC"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DMDS"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DSTF"): // Destructible end
            case ESM::fourCC("FNAM"):
            case ESM::fourCC("KNAM"):
            case ESM::fourCC("KSIZ"):
            case ESM::fourCC("KWDA"):
            case ESM::fourCC("OBND"):
            case ESM::fourCC("PNAM"):
            case ESM::fourCC("VMAD"):
            case ESM::fourCC("WNAM"):
            case ESM::fourCC("CTDA"):
            case ESM::fourCC("CIS1"):
            case ESM::fourCC("CIS2"):
            case ESM::fourCC("CITC"):
            case ESM::fourCC("NVNM"):
            case ESM::fourCC("ATTX"): // FO4
            case ESM::fourCC("ACSH"): // Starfield
            case ESM::fourCC("ANAM"): // Starfield
            case ESM::fourCC("BFCB"): // Starfield
            case ESM::fourCC("BFCE"): // Starfield
            case ESM::fourCC("BNAM"): // Starfield
            case ESM::fourCC("CNAM"): // Starfield
            case ESM::fourCC("FLLD"): // Starfield
            case ESM::fourCC("FTYP"): // FO4
            case ESM::fourCC("JNAM"): // Starfield
            case ESM::fourCC("MOLM"): // Starfield
            case ESM::fourCC("NTRM"): // FO4
            case ESM::fourCC("ODTY"): // Starfield
            case ESM::fourCC("PTCL"): // Starfield
            case ESM::fourCC("PTT2"): // Starfield
            case ESM::fourCC("PTRN"): // FO4
            case ESM::fourCC("PRPS"): // FO4
            case ESM::fourCC("RADR"): // FO4
            case ESM::fourCC("REFL"): // Starfield
            case ESM::fourCC("SNTP"): // Starfield
            case ESM::fourCC("STCP"): // FO4
            case ESM::fourCC("WTFM"): // Starfield
            case ESM::fourCC("XALG"): // Starfield
            case ESM::fourCC("XFLG"): // Starfield
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadacti"))
                    break;
                throw std::runtime_error("ESM4::ACTI::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}

// void ESM4::Activator::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Activator::blank()
//{
// }
