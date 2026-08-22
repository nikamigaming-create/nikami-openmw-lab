#include "loadipds.hpp"

#include <stdexcept>

#include "reader.hpp"

namespace
{
    void loadFalloutData(ESM4::Reader& reader, ESM4::ImpactDataSet& data, std::uint32_t size, bool isFONV)
    {
        if (!isFONV || size < 36 || size > 48 || size % sizeof(std::uint32_t) != 0)
        {
            reader.skipSubRecordData();
            return;
        }

        data.mImpacts = {};
        const std::size_t count = size / sizeof(std::uint32_t);
        for (std::size_t index = 0; index < count; ++index)
            reader.getFormId(data.mImpacts[index]);
        data.mPresent = true;
    }
}

void ESM4::ImpactDataSet::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    const std::uint32_t esmVer = reader.esmVersion();
    const bool isFONV = esmVer == ESM::VER_132 || esmVer == ESM::VER_133 || esmVer == ESM::VER_134;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subRecord = reader.subRecordHeader();
        switch (subRecord.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("DATA"):
                loadFalloutData(reader, *this, subRecord.dataSize, isFONV);
                break;
            default:
                throw std::runtime_error(
                    "ESM4::ImpactDataSet::load - Unknown subrecord " + ESM::printName(subRecord.typeId));
        }
    }
}
