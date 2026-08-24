#include "loadipds.hpp"

#include <stdexcept>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    void loadFalloutData(ESM4::Reader& reader, ESM4::ImpactDataSet& data, std::uint32_t size, bool isFONV)
    {
        if (!isFONV || size < ESM4::Fallout::kImpactDataSetMinBytes || size > ESM4::Fallout::kImpactDataSetMaxBytes
            || size % ESM4::Fallout::kOnDiskFormIdBytes != 0)
        {
            reader.skipSubRecordData();
            return;
        }

        data.mImpacts = {};
        const std::size_t count = size / ESM4::Fallout::kOnDiskFormIdBytes;
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
    const bool isFONV = ESM4::Fallout::isNewVegasVersion(esmVer);

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
