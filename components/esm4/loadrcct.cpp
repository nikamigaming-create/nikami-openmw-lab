#include "loadrcct.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");

    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::RecipeCategory::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail(ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize) + ", expected "
                + std::to_string(expected));
        }
    }

    void requireZString(const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize == ESM4::Fallout::kEmptySubrecordBytes)
            fail(ESM::printName(header.typeId) + " string");
    }
}

void ESM4::RecipeCategory::load(Reader& reader)
{
    if (!Fallout::isNewVegasVersion(reader.esmVersion()))
        fail("unsupported ESM4 version");

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
            case sEditorId:
                if (hasEditorId || hasFullName || hasData)
                    fail("EDID order");
                requireZString(header);
                if (!reader.getZString(value.mEditorId))
                    fail("EDID");
                hasEditorId = true;
                break;
            case sFullName:
                if (!hasEditorId || hasFullName || hasData)
                    fail("FULL order");
                requireZString(header);
                if (!reader.getZString(value.mFullName))
                    fail("FULL");
                hasFullName = true;
                break;
            case sData:
                if (!hasFullName || hasData)
                    fail("DATA order");
                requireSize(header, Fallout::kRecipeCategoryDataBytes);
                if (!reader.getExact(value.mData))
                    fail("DATA");
                hasData = true;
                break;
            default:
                fail(ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasFullName || !hasData)
        fail("required fields");

    *this = std::move(value);
}
