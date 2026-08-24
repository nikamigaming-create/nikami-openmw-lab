#include "loadamef.hpp"

#include <cmath>
#include <stdexcept>
#include <string_view>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");

    [[noreturn]] void fail(std::string_view field)
    {
        throw std::runtime_error("ESM4::AMEF::load - invalid or incomplete " + std::string(field));
    }

    void requireZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0 || !reader.getZString(value))
            fail(field);
    }
}

void ESM4::AmmoEffect::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasFullName = false;
    bool hasData = false;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case sEditorId:
                if (hasEditorId || hasFullName || hasData)
                    fail("EDID order");
                requireZString(reader, mEditorId, "EDID");
                hasEditorId = true;
                break;
            case sFullName:
                if (!hasEditorId || hasFullName || hasData)
                    fail("FULL order");
                requireZString(reader, mFullName, "FULL");
                hasFullName = true;
                break;
            case sData:
            {
                if (!hasFullName || hasData || header.dataSize != sDataSerializedSize)
                    fail("DATA size or order");

                std::uint32_t type = 0;
                std::uint32_t operation = 0;
                if (!reader.getExact(type) || !reader.getExact(operation) || !reader.getExact(mValue))
                    fail("DATA bytes");
                if (type > static_cast<std::uint32_t>(Type::Fatigue))
                    fail("DATA effect type");
                if (operation > static_cast<std::uint32_t>(Operation::Subtract))
                    fail("DATA operation");
                if (!std::isfinite(mValue))
                    fail("DATA value");

                mType = static_cast<Type>(type);
                mOperation = static_cast<Operation>(operation);
                hasData = true;
                break;
            }
            default:
                fail(ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasData)
        fail("required fields");
}
