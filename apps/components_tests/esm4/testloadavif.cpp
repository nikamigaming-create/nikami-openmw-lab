#include <components/esm4/loadavif.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#include "testutil.hpp"

namespace
{
    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    ESM4::ActorValueInformation loadActorValueInformation(std::string payload)
    {
        auto reader = ESM4Test::makeReader("AVIF", std::move(payload));
        ESM4::ActorValueInformation value;
        value.load(*reader);
        return value;
    }

    TEST(Esm4ActorValueInformationTest, loadsFalloutMetadata)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("Health"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Health"));
        ESM4Test::appendSubRecord(payload, "DESC", zString("Maximum health"));
        ESM4Test::appendSubRecord(payload, "ICON", zString("Interface\\Icons\\Health.dds"));
        ESM4Test::appendSubRecord(payload, "MICO", zString("Interface\\Icons\\HealthSmall.dds"));
        ESM4Test::appendSubRecord(payload, "ANAM", zString("HP"));

        const ESM4::ActorValueInformation value = loadActorValueInformation(std::move(payload));

        EXPECT_EQ(value.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(value.mEditorId, "Health");
        EXPECT_EQ(value.mFullName, "Health");
        EXPECT_EQ(value.mDescription, "Maximum health");
        EXPECT_EQ(value.mLargeIcon, "Interface\\Icons\\Health.dds");
        EXPECT_EQ(value.mSmallIcon, "Interface\\Icons\\HealthSmall.dds");
        EXPECT_EQ(value.mShortName, "HP");
    }

    TEST(Esm4ActorValueInformationTest, rejectsUnknownSubrecord)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("Health"));
        ESM4Test::appendSubRecord(payload, "XXXX", std::string(1, '\0'));
        EXPECT_THROW(loadActorValueInformation(std::move(payload)), std::runtime_error);
    }
}
