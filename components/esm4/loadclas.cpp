/*
  Copyright (C) 2016, 2018, 2021 cc9cii

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
#include "loadclas.hpp"

#include <cstring>
#include <stdexcept>

#include "reader.hpp"
//#include "writer.hpp"

ESM4::Class::Data ESM4::Class::decodeFalloutData(std::span<const std::uint8_t> payload)
{
    if (payload.size() != sizeof(Data))
        throw std::runtime_error("ESM4::CLAS Fallout DATA must be exactly 28 bytes");

    Data result{};
    std::memcpy(&result, payload.data(), sizeof(result));
    return result;
}

void ESM4::Class::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    const std::uint32_t esmVer = reader.esmVersion();
    const bool isFONV = esmVer == ESM::VER_132 || esmVer == ESM::VER_133 || esmVer == ESM::VER_134;

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
                reader.getLocalizedString(mDesc);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mIcon);
                break;
            case ESM::fourCC("DATA"):
            {
                if (!isFONV)
                {
                    reader.skipSubRecordData();
                    break;
                }
                if (mHasFalloutData)
                    throw std::runtime_error("ESM4::CLAS contains duplicate Fallout DATA");
                std::array<std::uint8_t, sizeof(Data)> payload{};
                if (subHdr.dataSize != payload.size() || !reader.get(payload.data(), payload.size()))
                    throw std::runtime_error("ESM4::CLAS Fallout DATA size/read mismatch");
                mData = decodeFalloutData(payload);
                mHasFalloutData = true;
                break;
            }
            case ESM::fourCC("ATTR"):
            {
                if (!isFONV)
                {
                    reader.skipSubRecordData();
                    break;
                }
                if (mHasFalloutAttributes || subHdr.dataSize != mAttributes.size()
                    || !reader.get(mAttributes.data(), mAttributes.size()))
                    throw std::runtime_error("ESM4::CLAS Fallout ATTR duplicate/size/read mismatch");
                mHasFalloutAttributes = true;
                break;
            }
            case ESM::fourCC("PRPS"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::CLAS::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}

// void ESM4::Class::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Class::blank()
//{
// }
