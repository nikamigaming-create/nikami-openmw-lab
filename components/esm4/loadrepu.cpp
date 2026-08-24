#include "loadrepu.hpp"

#include <cmath>
#include <stdexcept>
#include <string_view>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sIcon = ESM::fourCC("ICON");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");

    [[noreturn]] void fail(std::string_view field)
    {
        throw std::runtime_error("ESM4::REPU::load - invalid or incomplete " + std::string(field));
    }

    void requireZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0 || !reader.getZString(value))
            fail(field);
    }
}

void ESM4::Reputation::load(Reader& reader)
{
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
            case sEditorId:
                if (hasEditorId || hasFullName || hasIcon || hasData)
                    fail("EDID order");
                requireZString(reader, mEditorId, "EDID");
                hasEditorId = true;
                break;
            case sFullName:
                if (!hasEditorId || hasFullName || hasIcon || hasData)
                    fail("FULL order");
                reader.getLocalizedString(mFullName);
                hasFullName = true;
                break;
            case sIcon:
                if (!hasFullName || hasIcon || hasData)
                    fail("ICON order");
                requireZString(reader, mIcon, "ICON");
                hasIcon = true;
                break;
            case sData:
                if (!hasIcon || hasData || header.dataSize != sDataSerializedSize
                    || !reader.getExact(mMaximum) || !std::isfinite(mMaximum) || mMaximum <= 0.f)
                    fail("DATA");
                hasData = true;
                break;
            default:
                fail(ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasIcon || !hasData)
        fail("required fields");
}
