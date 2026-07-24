#include "loadipds.hpp"

#include <stdexcept>

#include "reader.hpp"

void ESM4::ImpactDataSet::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHeader = reader.subRecordHeader();
        switch (subHeader.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("DATA"):
                if (subHeader.dataSize == 0 || subHeader.dataSize % sizeof(std::uint32_t) != 0
                    || subHeader.dataSize > mImpacts.size() * sizeof(std::uint32_t))
                    throw std::runtime_error("ESM4::ImpactDataSet::load - unsupported Fallout New Vegas DATA layout");
                // FNV omits unused trailing material slots in some IPDS
                // records instead of always serializing all twelve.
                for (std::size_t i = 0; i < subHeader.dataSize / sizeof(std::uint32_t); ++i)
                    reader.getFormId(mImpacts[i]);
                break;
            default:
                throw std::runtime_error(
                    "ESM4::ImpactDataSet::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(subHeader.typeId));
        }
    }
}
