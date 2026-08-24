#include <components/esm4/loadrepu.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
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

    ESM4::Reputation loadReputation(std::string payload)
    {
        auto reader = ESM4Test::makeReader("REPU", std::move(payload));
        ESM4::Reputation value;
        value.load(*reader);
        return value;
    }

    TEST(Esm4ReputationTest, loadsFalloutReputationScale)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("RepFollowers"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Followers"));
        ESM4Test::appendSubRecord(payload, "ICON", zString("Interface\\Reputation\\Followers.dds"));
        std::string data;
        ESM4Test::appendPod(data, 100.f);
        ESM4Test::appendSubRecord(payload, "DATA", data);

        const ESM4::Reputation value = loadReputation(std::move(payload));

        EXPECT_EQ(value.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(value.mEditorId, "RepFollowers");
        EXPECT_EQ(value.mFullName, "Followers");
        EXPECT_EQ(value.mIcon, "Interface\\Reputation\\Followers.dds");
        EXPECT_FLOAT_EQ(value.mMaximum, 100.f);
    }

    TEST(Esm4ReputationTest, rejectsInvalidScale)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("RepFollowers"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Followers"));
        ESM4Test::appendSubRecord(payload, "ICON", zString("Followers.dds"));
        std::string data;
        ESM4Test::appendPod(data, 0.f);
        ESM4Test::appendSubRecord(payload, "DATA", data);
        EXPECT_THROW(loadReputation(std::move(payload)), std::runtime_error);
    }
}
