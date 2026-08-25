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
#include <format>
#include <stdexcept>
#include <vector>

#include "falloutformat.hpp"
#include "reader.hpp"
//#include "writer.hpp"

namespace
{
    template <typename T>
    T readFalloutValue(std::span<const std::uint8_t> payload, std::size_t& offset)
    {
        if (offset > payload.size() || payload.size() - offset < sizeof(T))
            throw std::runtime_error("ESM4 Fallout fixed payload ended before its declared fields");

        T value{};
        std::memcpy(&value, payload.data() + offset, sizeof(value));
        offset += sizeof(value);
        return value;
    }
}

ESM4::Class::FalloutData ESM4::Class::decodeFalloutData(std::span<const std::uint8_t> payload)
{
    if (payload.size() != Fallout::kClassFalloutDataBytes)
        throw std::runtime_error(
            std::format("ESM4::CLAS Fallout DATA must be exactly {} bytes", Fallout::kClassFalloutDataBytes));

    FalloutData result{};
    std::size_t offset = 0;
    for (auto& actorValue : result.mTagActorValues)
        actorValue = readFalloutValue<std::int32_t>(payload, offset);
    result.mRawFlags = readFalloutValue<std::uint32_t>(payload, offset);
    result.mRawServices = readFalloutValue<std::uint32_t>(payload, offset);
    result.mRawTeaches = readFalloutValue<std::uint8_t>(payload, offset);
    result.mTrainingLevel = readFalloutValue<std::uint8_t>(payload, offset);
    for (auto& reserved : result.mReserved)
        reserved = readFalloutValue<std::uint8_t>(payload, offset);
    return result;
}

void ESM4::Class::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    const bool isFONV = Fallout::isNewVegasVersion(reader.esmVersion());

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
                if (!isFONV)
                {
                    reader.skipSubRecordData();
                    break;
                }
                if (mHasFalloutData)
                    throw std::runtime_error("ESM4::CLAS contains duplicate Fallout DATA");
                {
                    std::vector<std::uint8_t> payload(subHdr.dataSize);
                    if (!reader.get(payload.data(), payload.size()))
                        throw std::runtime_error("ESM4::CLAS Fallout DATA read failed");
                    mFalloutData = decodeFalloutData(payload);
                    mHasFalloutData = true;
                }
                break;
            case ESM::fourCC("ATTR"):
                if (!isFONV)
                {
                    reader.skipSubRecordData();
                    break;
                }
                if (mHasFalloutAttributes || subHdr.dataSize != Fallout::kClassFalloutAttributesBytes
                    || !reader.get(mFalloutAttributes.data(), mFalloutAttributes.size()))
                    throw std::runtime_error("ESM4::CLAS Fallout ATTR duplicate/size/read mismatch");
                mHasFalloutAttributes = true;
                break;
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
