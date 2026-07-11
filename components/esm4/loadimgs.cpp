#include "loadimgs.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"

void ESM4::ImageSpace::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("DNAM"):
            {
                std::vector<std::uint8_t> raw(subHdr.dataSize);
                if (!raw.empty())
                    reader.get(raw.data(), raw.size());
                const std::size_t traitBytes = std::min(raw.size(), sizeof(mTraits));
                if (traitBytes != 0)
                    std::memcpy(mTraits.data(), raw.data(), traitBytes);
                break;
            }
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadimgs"))
                    break;
                throw std::runtime_error("ESM4::IMGS::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
