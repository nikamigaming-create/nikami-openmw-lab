#include "loadamef.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"

namespace
{
    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::AmmoEffect::load - " + std::string(message));
    }

    void readZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0 || !reader.getZString(value))
            fail("could not read nonempty " + std::string(field));
    }
}

void ESM4::AmmoEffect::load(Reader& reader)
{
    // Frozen English Ultimate Edition corpus (80 physical records): FalloutNV 54, Dead Money 3,
    // Honest Hearts 3, Lonesome Road 1, and Gun Runners' Arsenal 19. Every record is exactly
    // EDID,FULL,DATA; DATA is the xEdit/xNVSE 12-byte type, operation, float-value contract.
    AmmoEffect value;
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
            {
                if (!hasFullName || hasData || header.dataSize != 12)
                    fail("DATA is missing, duplicated, out of order, or not 12 bytes");
                std::uint32_t type = 0;
                std::uint32_t operation = 0;
                if (!reader.getExact(type) || !reader.getExact(operation) || !reader.getExact(value.mValue))
                    fail("could not read DATA");
                if (type > static_cast<std::uint32_t>(Type::Fatigue))
                    fail("DATA contains an invalid effect type");
                if (operation > static_cast<std::uint32_t>(Operation::Subtract))
                    fail("DATA contains an invalid operation");
                if (!std::isfinite(value.mValue))
                    fail("DATA contains a non-finite value");
                value.mType = static_cast<Type>(type);
                value.mOperation = static_cast<Operation>(operation);
                hasData = true;
                break;
            }
            default:
                fail("unknown or out-of-order Fallout New Vegas subrecord " + ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasData)
        fail("record is missing required EDID, FULL, or DATA");
    *this = std::move(value);
}
