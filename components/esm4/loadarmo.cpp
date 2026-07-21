/*
  Copyright (C) 2016, 2018-2021 cc9cii

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
#include "loadarmo.hpp"

#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

void ESM4::Armor::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    uint16_t currentIndex = 0xFFFF;
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
            case ESM::fourCC("DATA"):
            {
                switch (subHdr.dataSize)
                {
                    case 14: // TES4
                        reader.get(mData);
                        break;
                    case 12: // FO3, FNV, FO4
                        reader.get(mData.value);
                        reader.get(mData.health);
                        reader.get(mData.weight);
                        break;
                    case 8: // TES5
                        reader.get(mData.value);
                        reader.get(mData.weight);
                        break;
                    default:
                        reader.skipSubRecordData();
                        break;
                }
                break;
            }
            case ESM::fourCC("INDX"): // FO4
            {
                if (subHdr.dataSize == sizeof(currentIndex))
                    reader.get(currentIndex);
                else
                    reader.skipSubRecordData();
                break;
            }
            case ESM::fourCC("MODL"):
            {
                if (subHdr.dataSize == 4)
                {
                    // Assuming TES5
                    if (currentIndex == 0xFFFF)
                        reader.getFormId(mAddOns.emplace_back());
                    // FO4
                    else
                    {
                        ESM::FormId addon;
                        reader.getFormId(addon);
                        if (mAddOns.size() <= currentIndex)
                        {
                            mAddOns.resize(currentIndex + 1);
                            mAddOns[currentIndex] = addon;
                        }
                        else if (mAddOns[currentIndex].isZeroOrUnset())
                            mAddOns[currentIndex] = addon;
                        else
                            mAddOns.push_back(addon);
                    }
                }
                else
                {
                    if (!reader.getZString(mModelMale))
                        throw std::runtime_error("ARMO MODL data read error");
                }

                break;
            }
            case ESM::fourCC("MOD2"):
                reader.getZString(mModelMaleWorld);
                break;
            case ESM::fourCC("MOD3"):
                reader.getZString(mModelFemale);
                break;
            case ESM::fourCC("MOD4"):
                reader.getZString(mModelFemaleWorld);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mIconMale);
                break;
            case ESM::fourCC("MICO"):
                reader.getZString(mMiniIconMale);
                break;
            case ESM::fourCC("ICO2"):
                reader.getZString(mIconFemale);
                break;
            case ESM::fourCC("MIC2"):
                reader.getZString(mMiniIconFemale);
                break;
            case ESM::fourCC("BMDT"):
                if (subHdr.dataSize == 8) // FO3
                {
                    reader.get(mArmorFlags);
                    reader.get(mGeneralFlags);
                    mGeneralFlags &= 0x000000ff;
                    mGeneralFlags |= TYPE_FO3;
                }
                else // TES4
                {
                    reader.get(mArmorFlags);
                    mGeneralFlags = (mArmorFlags & 0x00ff0000) >> 16;
                    mGeneralFlags |= TYPE_TES4;
                }
                break;
            case ESM::fourCC("BODT"):
            {
                reader.get(mArmorFlags);
                uint32_t flags = 0;
                if (subHdr.dataSize == 12)
                    reader.get(flags);
                reader.get(mGeneralFlags); // skill
                mGeneralFlags &= 0x0000000f; // 0 (light), 1 (heavy) or 2 (none)
                if (subHdr.dataSize == 12)
                    mGeneralFlags |= (flags & 0x0000000f) << 3;
                mGeneralFlags |= TYPE_TES5;
                break;
            }
            case ESM::fourCC("BOD2"):
                // FO4, TES5
                if (subHdr.dataSize == 4 || subHdr.dataSize == 8)
                {
                    reader.get(mArmorFlags);
                    if (subHdr.dataSize == 8)
                    {
                        reader.get(mGeneralFlags);
                        mGeneralFlags &= 0x0000000f; // 0 (light), 1 (heavy) or 2 (none)
                        mGeneralFlags |= TYPE_TES5;
                    }
                }
                else
                {
                    reader.skipSubRecordData();
                }
                break;
            case ESM::fourCC("SCRI"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mScriptId);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("ANAM"):
                if (subHdr.dataSize == sizeof(mEnchantmentPoints))
                    reader.get(mEnchantmentPoints);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("ENAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mEnchantment);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("BIPL"): // FO3/FNV FLST of armor add-ons
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mBipedModelList);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("MODB"):
                if (subHdr.dataSize == sizeof(mBoundRadius))
                    reader.get(mBoundRadius);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("DESC"):
                reader.getLocalizedString(mText);
                break;
            case ESM::fourCC("DNAM"):
                if (subHdr.dataSize == sizeof(mFalloutData))
                {
                    reader.get(mFalloutData);
                    mHasFalloutData = true;
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("YNAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mPickUpSound);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("ZNAM"):
                if (subHdr.dataSize == sizeof(ESM::FormId32))
                    reader.getFormId(mDropSound);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("MODT"):
            case ESM::fourCC("MO2B"):
            case ESM::fourCC("MO3B"):
            case ESM::fourCC("MO4B"):
            case ESM::fourCC("MO2T"):
            case ESM::fourCC("MO2S"):
            case ESM::fourCC("MO3T"):
            case ESM::fourCC("MO4T"):
            case ESM::fourCC("MO4S"):
            case ESM::fourCC("OBND"):
            case ESM::fourCC("RNAM"): // race formid
            case ESM::fourCC("KSIZ"):
            case ESM::fourCC("KWDA"):
            case ESM::fourCC("TNAM"):
            case ESM::fourCC("BAMT"):
            case ESM::fourCC("BIDS"):
            case ESM::fourCC("ETYP"):
            case ESM::fourCC("BMCT"):
            case ESM::fourCC("EAMT"):
            case ESM::fourCC("EITM"):
            case ESM::fourCC("VMAD"):
            case ESM::fourCC("REPL"): // FO3
            case ESM::fourCC("MODD"): // FO3
            case ESM::fourCC("MOSD"): // FO3
            case ESM::fourCC("MODS"): // FO3
            case ESM::fourCC("MO3S"): // FO3
            case ESM::fourCC("BNAM"): // FONV
            case ESM::fourCC("SNAM"): // FONV
            case ESM::fourCC("DAMC"): // Destructible
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DMDC"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DMDS"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DSTF"): // Destructible end
            case ESM::fourCC("APPR"): // FO4
            case ESM::fourCC("BFCB"): // Starfield
            case ESM::fourCC("BFCE"): // Starfield
            case ESM::fourCC("DAMA"): // FO4
            case ESM::fourCC("FLLD"): // Starfield
            case ESM::fourCC("FNAM"): // FO4
            case ESM::fourCC("INRD"): // FO4
            case ESM::fourCC("ODTY"): // Starfield
            case ESM::fourCC("PTT2"): // Starfield
            case ESM::fourCC("PTRN"): // FO4
            case ESM::fourCC("REFL"): // Starfield
            case ESM::fourCC("OBTE"): // FO4 object template start
            case ESM::fourCC("OBTF"):
            case ESM::fourCC("OBTS"):
            case ESM::fourCC("STOP"): // FO4 object template end
            case ESM::fourCC("XFLG"): // Starfield
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadarmo"))
                    break;
                throw std::runtime_error("ESM4::ARMO::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
    // if ((mArmorFlags&0xffff) == 0x02) // only hair
    // std::cout << "only hair " << mEditorId << std::endl;
}

// void ESM4::Armor::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Armor::blank()
//{
// }
