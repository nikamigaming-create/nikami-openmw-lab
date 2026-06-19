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
#include "loadpack.hpp"

#include <cstring>
#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

void ESM4::AIPackage::load(ESM4::Reader& reader)
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
            case ESM::fourCC("PKDT"):
            {
                if (subHdr.dataSize == 4)
                {
                    reader.get(mData.flags);
                    mData.type = 0; // FIXME
                }
                else if (subHdr.dataSize == 8)
                {
                    reader.get(mData);
                    mFo3PackageFlags = mData.flags;
                }
                else if (subHdr.dataSize == 12)
                {
                    struct RawFo3Pkdt
                    {
                        std::uint32_t flags;
                        std::uint8_t type;
                        std::uint8_t unused;
                        std::uint16_t procedureFlags;
                        std::uint16_t typeSpecificFlags;
                        std::uint16_t unused2;
                    };

                    RawFo3Pkdt data{};
                    reader.get(data);
                    mData.flags = data.flags;
                    mData.type = data.type;
                    mFo3PackageFlags = data.flags;
                    mFo3ProcedureFlags = data.procedureFlags;
                    mFo3TypeSpecificFlags = data.typeSpecificFlags;
                }
                else if (subHdr.dataSize != sizeof(mData))
                    reader.skipSubRecordData(); // FIXME: FO3
                else
                {
                    reader.get(mData);
                    mFo3PackageFlags = mData.flags;
                }

                break;
            }
            case ESM::fourCC("PSDT"): // reader.get(mSchedule); break;
            {
                if (subHdr.dataSize != sizeof(mSchedule))
                    reader.skipSubRecordData(); // FIXME:
                else
                    reader.get(mSchedule); // TES4

                break;
            }
            case ESM::fourCC("PLDT"):
            {
                if (subHdr.dataSize != sizeof(mLocation))
                    reader.skipSubRecordData(); // FIXME:
                else
                {
                    reader.get(mLocation); // TES4
                    if (mLocation.type != 5)
                        reader.adjustFormId(mLocation.location);
                }

                break;
            }
            case ESM::fourCC("PLD2"):
            {
                PLDT location{};
                if (subHdr.dataSize != sizeof(location))
                    reader.skipSubRecordData();
                else
                {
                    reader.get(location);
                    if (location.type != 5)
                        reader.adjustFormId(location.location);
                    mExtraLocations.push_back(location);
                }

                break;
            }
            case ESM::fourCC("PTDT"):
            {
                if (subHdr.dataSize == sizeof(mTarget))
                {
                    reader.get(mTarget); // TES4
                    if (mTarget.type != 2)
                        reader.adjustFormId(mTarget.target);
                }
                else if (subHdr.dataSize == 16)
                {
                    struct RawFo3Ptdt
                    {
                        std::int32_t type;
                        ESM::FormId32 target;
                        std::int32_t distance;
                        float unknown;
                    };

                    RawFo3Ptdt data{};
                    reader.get(data);
                    mTarget.type = data.type;
                    mTarget.target = data.target;
                    mTarget.distance = data.distance;
                    mFo3TargetUnknown = data.unknown;
                    if (mTarget.type != 2)
                        reader.adjustFormId(mTarget.target);
                }
                else
                    reader.skipSubRecordData(); // FIXME: FO3

                break;
            }
            case ESM::fourCC("PTD2"):
            {
                PTDT target{};
                float unknown = 0.f;
                if (subHdr.dataSize == sizeof(target))
                {
                    reader.get(target);
                    if (target.type != 2)
                        reader.adjustFormId(target.target);
                    mExtraTargets.push_back(target);
                    mExtraTargetUnknowns.push_back(unknown);
                }
                else if (subHdr.dataSize == 16)
                {
                    struct RawFo3Ptdt
                    {
                        std::int32_t type;
                        ESM::FormId32 target;
                        std::int32_t distance;
                        float unknown;
                    };

                    RawFo3Ptdt data{};
                    reader.get(data);
                    target.type = data.type;
                    target.target = data.target;
                    target.distance = data.distance;
                    unknown = data.unknown;
                    if (target.type != 2)
                        reader.adjustFormId(target.target);
                    mExtraTargets.push_back(target);
                    mExtraTargetUnknowns.push_back(unknown);
                }
                else
                    reader.skipSubRecordData();

                break;
            }
            case ESM::fourCC("CTDA"):
            {
                if (subHdr.dataSize != sizeof(CTDA))
                {
                    reader.skipSubRecordData(); // FIXME: FO3
                    break;
                }

                CTDA condition;
                reader.get(condition);
                // FIXME: how to "unadjust" if not FormId?
                // adjustFormId(condition.param1);
                // adjustFormId(condition.param2);
                mConditions.push_back(condition);

                break;
            }
            case ESM::fourCC("CTDT"): // always 20 for TES4
            case ESM::fourCC("TNAM"): // FO3
            case ESM::fourCC("INAM"): // FO3
            case ESM::fourCC("CNAM"): // FO3
            case ESM::fourCC("SCHR"): // FO3
            case ESM::fourCC("POBA"): // FO3
            case ESM::fourCC("POCA"): // FO3
            case ESM::fourCC("POEA"): // FO3
            case ESM::fourCC("SCTX"): // FO3
            case ESM::fourCC("SCDA"): // FO3
            case ESM::fourCC("SCRO"): // FO3
            case ESM::fourCC("PKDD"): // FO3
            case ESM::fourCC("PKD2"): // FO3
            case ESM::fourCC("PKPT"): // FO3
            case ESM::fourCC("PKED"): // FO3
            case ESM::fourCC("PKE2"): // FO3
            case ESM::fourCC("PKAM"): // FO3
            case ESM::fourCC("PUID"): // FO3
            case ESM::fourCC("PKW3"): // FO3
            case ESM::fourCC("PKFD"): // FO3
            case ESM::fourCC("SLSD"): // FO3
            case ESM::fourCC("SCVR"): // FO3
            case ESM::fourCC("SCRV"): // FO3
            case ESM::fourCC("IDLB"): // FO3
            case ESM::fourCC("ANAM"): // TES5
            case ESM::fourCC("BNAM"): // TES5
            case ESM::fourCC("FNAM"): // TES5
            case ESM::fourCC("PNAM"): // TES5
            case ESM::fourCC("QNAM"): // TES5
            case ESM::fourCC("UNAM"): // TES5
            case ESM::fourCC("XNAM"): // TES5
            case ESM::fourCC("PDTO"): // TES5
            case ESM::fourCC("PTDA"): // TES5
            case ESM::fourCC("PFOR"): // TES5
            case ESM::fourCC("PFO2"): // TES5
            case ESM::fourCC("PRCB"): // TES5
            case ESM::fourCC("PKCU"): // TES5
            case ESM::fourCC("PKC2"): // TES5
            case ESM::fourCC("CITC"): // TES5
            case ESM::fourCC("CIS1"): // TES5
            case ESM::fourCC("CIS2"): // TES5
            case ESM::fourCC("VMAD"): // TES5
            case ESM::fourCC("TPIC"): // TES5
                reader.skipSubRecordData();
                break;
            case ESM::fourCC("IDLF"): // FO3/FONV
                if (subHdr.dataSize == 1)
                    reader.get(mIdleFlags);
                else if (subHdr.dataSize == 4)
                {
                    std::uint32_t flags = 0;
                    reader.get(flags);
                    mIdleFlags = static_cast<std::uint8_t>(flags & 0xff);
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("IDLC"): // FO3/FONV
                if (subHdr.dataSize == 1)
                {
                    std::uint8_t count = 0;
                    reader.get(count);
                    mIdleCount = count;
                }
                else if (subHdr.dataSize == 4)
                    reader.get(mIdleCount);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("IDLT"): // FO3/FONV
                if (subHdr.dataSize == sizeof(mIdleTimer))
                    reader.get(mIdleTimer);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("IDLA"): // FO3/FONV
            {
                if (subHdr.dataSize % sizeof(ESM::FormId32) != 0)
                {
                    reader.skipSubRecordData();
                    break;
                }

                const std::size_t idleCount = subHdr.dataSize / sizeof(ESM::FormId32);
                mIdleAnim.resize(idleCount);
                for (ESM::FormId& value : mIdleAnim)
                    reader.getFormId(value);
                break;
            }
            default:
            {
                const bool isFo3OrFonv = reader.esmVersion() == ESM::VER_094 || reader.esmVersion() == ESM::VER_132
                    || reader.esmVersion() == ESM::VER_133 || reader.esmVersion() == ESM::VER_134;
                if (isFo3OrFonv)
                {
                    reader.skipSubRecordData();
                    break;
                }
                throw std::runtime_error("ESM4::PACK::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
            }
        }
    }
}

// void ESM4::AIPackage::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::AIPackage::blank()
//{
// }
