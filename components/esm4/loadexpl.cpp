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
#include "loadexpl.hpp"

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

void ESM4::Explosion::load(ESM4::Reader& reader)
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
            case ESM::fourCC("EITM"):
                reader.getFormId(mMagicEffect);
                break;
            case ESM::fourCC("MNAM"):
                reader.getFormId(mImpactDataSet);
                break;
            case ESM::fourCC("DATA"):
                mData = readRawSubrecord(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("OBND"):
                mObjectBounds = readRawSubrecord(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("MODT"):
                mModelData = readRawSubrecord(reader, subHdr.dataSize);
                break;
            default:
                throw std::runtime_error("ESM4::EXPL::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
