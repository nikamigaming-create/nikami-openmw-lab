#include "loadavif.hpp"

#include <stdexcept>
#include <string_view>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sDescription = ESM::fourCC("DESC");
    constexpr std::uint32_t sLargeIcon = ESM::fourCC("ICON");
    constexpr std::uint32_t sSmallIcon = ESM::fourCC("MICO");
    constexpr std::uint32_t sShortName = ESM::fourCC("ANAM");

    [[noreturn]] void fail(std::string_view field)
    {
        throw std::runtime_error("ESM4::AVIF::load - invalid or unknown " + std::string(field));
    }

    void requireZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0 || !reader.getZString(value))
            fail(field);
    }
}

void ESM4::ActorValueInformation::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case sEditorId:
                requireZString(reader, mEditorId, "EDID");
                break;
            case sFullName:
                reader.getLocalizedString(mFullName);
                break;
            case sDescription:
                reader.getLocalizedString(mDescription);
                break;
            case sLargeIcon:
                requireZString(reader, mLargeIcon, "ICON");
                break;
            case sSmallIcon:
                requireZString(reader, mSmallIcon, "MICO");
                break;
            case sShortName:
                requireZString(reader, mShortName, "ANAM");
                break;
            default:
                fail(ESM::printName(header.typeId));
        }
    }
}
