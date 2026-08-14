#include "loadmesg.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "reader.hpp"

namespace
{
    [[noreturn]] void fail(const std::string& message)
    {
        throw std::runtime_error("ESM4::Message::load - " + message);
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize));
    }
}

void ESM4::Message::load(Reader& reader)
{
    Message value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;
    if (value.mFlags != 0)
        fail("unsupported nonzero record flags " + std::to_string(value.mFlags));

    bool sawEditorId = false;
    bool sawDescription = false;
    bool sawFullName = false;
    bool sawIcon = false;
    bool sawMessageFlags = false;
    bool sawDisplayTime = false;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (sawEditorId || !reader.getZString(value.mEditorId))
                    fail("invalid or duplicate EDID");
                sawEditorId = true;
                break;
            case ESM::fourCC("DESC"):
                if (sawDescription)
                    fail("duplicate DESC");
                reader.getLocalizedString(value.mDescription);
                sawDescription = true;
                break;
            case ESM::fourCC("FULL"):
                if (sawFullName)
                    fail("duplicate FULL");
                reader.getLocalizedString(value.mFullName);
                sawFullName = true;
                break;
            case ESM::fourCC("INAM"):
                requireSize(header, 4);
                if (sawIcon || !reader.getFormId(value.mIcon))
                    fail("invalid or duplicate INAM");
                sawIcon = true;
                break;
            case ESM::fourCC("DNAM"):
                requireSize(header, 4);
                if (sawMessageFlags || !reader.getExact(value.mMessageFlags))
                    fail("invalid or duplicate DNAM");
                sawMessageFlags = true;
                break;
            case ESM::fourCC("TNAM"):
                requireSize(header, 4);
                if (sawDisplayTime || !reader.getExact(value.mDisplayTime))
                    fail("invalid or duplicate TNAM");
                sawDisplayTime = true;
                break;
            case ESM::fourCC("ITXT"):
            {
                MessageButton button;
                reader.getLocalizedString(button.mText);
                value.mButtons.push_back(std::move(button));
                break;
            }
            case ESM::fourCC("CTDA"):
            {
                if (value.mButtons.empty())
                    fail("CTDA appears before ITXT");
                TargetCondition condition;
                if (!loadTargetCondition(reader, condition))
                    fail("unsupported CTDA");
                value.mButtons.back().mConditions.push_back(condition);
                break;
            }
            case ESM::fourCC("NAM0"):
            case ESM::fourCC("NAM1"):
            case ESM::fourCC("NAM2"):
            case ESM::fourCC("NAM3"):
            case ESM::fourCC("NAM4"):
            case ESM::fourCC("NAM5"):
            case ESM::fourCC("NAM6"):
            case ESM::fourCC("NAM7"):
            case ESM::fourCC("NAM8"):
            case ESM::fourCC("NAM9"):
                requireSize(header, 1);
                reader.skipSubRecordData();
                break;
            default:
                fail("unknown subrecord " + ESM::printName(header.typeId));
        }
    }

    if (!sawEditorId || !sawDescription || !sawIcon)
        fail("missing required EDID, DESC, or INAM");
    *this = std::move(value);
}
