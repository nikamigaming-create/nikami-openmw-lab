#include "loadrepu.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

#include "reader.hpp"

namespace
{
    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Reputation::load - " + std::string(message));
    }

    void requireZString(const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize == 0)
            fail("zero-sized " + ESM::printName(header.typeId));
    }
}

void ESM4::Reputation::load(Reader& reader)
{
    // Frozen FalloutNV.esm corpus: 13 REPU records, each exactly
    // EDID, FULL, ICON, DATA(4).
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasFullName = false;
    bool hasIcon = false;
    bool hasData = false;
    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (hasEditorId || hasFullName || hasIcon || hasData)
                    fail("EDID is missing, duplicated, or out of order");
                requireZString(header);
                if (!reader.getZString(mEditorId))
                    fail("could not read EDID");
                hasEditorId = true;
                break;
            case ESM::fourCC("FULL"):
                if (!hasEditorId || hasFullName || hasIcon || hasData)
                    fail("FULL is missing, duplicated, or out of order");
                requireZString(header);
                reader.getLocalizedString(mFullName);
                hasFullName = true;
                break;
            case ESM::fourCC("ICON"):
                if (!hasFullName || hasIcon || hasData)
                    fail("ICON is missing, duplicated, or out of order");
                requireZString(header);
                if (!reader.getZString(mIcon))
                    fail("could not read ICON");
                hasIcon = true;
                break;
            case ESM::fourCC("DATA"):
                if (!hasIcon || hasData || header.dataSize != 4)
                    fail("DATA is missing, duplicated, out of order, or not four bytes");
                if (!reader.getExact(mMaximum))
                    fail("could not read DATA maximum");
                if (!std::isfinite(mMaximum) || mMaximum <= 0.f)
                    fail("DATA maximum is not finite and positive");
                hasData = true;
                break;
            default:
                fail("unknown Fallout New Vegas subrecord " + ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasIcon || !hasData)
        fail("record is missing required EDID, FULL, ICON, or DATA");
}
