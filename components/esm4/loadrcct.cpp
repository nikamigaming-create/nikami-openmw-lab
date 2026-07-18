#include "loadrcct.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"

namespace
{
    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::RecipeCategory::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    void readZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0)
            fail("zero-sized " + std::string(field));
        if (!reader.getZString(value))
            fail("could not read " + std::string(field));
    }
}

void ESM4::RecipeCategory::load(Reader& reader)
{
    // Frozen English Ultimate Edition corpus (10 official masters):
    // 11 physical/winning-live RCCT records (FalloutNV 10, Dead Money 1);
    // EDID 11; FULL 11; DATA 11x1 with values 00,01,3c,6b,fe,ff;
    // no deleted, compressed, override, ICON/MICO, or alternate-layout record.
    RecipeCategory value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasFullName = false;
    bool hasData = false;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (hasEditorId || hasFullName || hasData)
                    fail("EDID is duplicated or out of order");
                readZString(reader, value.mEditorId, "EDID");
                hasEditorId = true;
                break;
            case ESM::fourCC("FULL"):
                if (!hasEditorId || hasFullName || hasData)
                    fail("FULL is missing, duplicated, or out of order");
                readZString(reader, value.mFullName, "FULL");
                hasFullName = true;
                break;
            case ESM::fourCC("DATA"):
                if (!hasFullName || hasData)
                    fail("DATA is missing, duplicated, or out of order");
                requireSize(header, 1);
                if (!reader.getExact(value.mData))
                    fail("could not read DATA");
                hasData = true;
                break;
            default:
                fail("unknown or out-of-order Fallout New Vegas subrecord " + ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasData)
        fail("record is missing required EDID, FULL, or DATA");

    *this = std::move(value);
}
