#include <components/esm4/loadamef.hpp>

#include <gtest/gtest.h>

#include <limits>
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

    ESM4::AmmoEffect loadAmmoEffect(std::string payload)
    {
        auto reader = ESM4Test::makeReader("AMEF", std::move(payload));
        ESM4::AmmoEffect value;
        value.load(*reader);
        return value;
    }

    TEST(Esm4AmmoEffectTest, loadsTypedEffectData)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("AmmoEffectDamage"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Damage"));
        std::string data;
        ESM4Test::appendPod(data, static_cast<std::uint32_t>(ESM4::AmmoEffect::Type::DamageThreshold));
        ESM4Test::appendPod(data, static_cast<std::uint32_t>(ESM4::AmmoEffect::Operation::Multiply));
        ESM4Test::appendPod(data, 1.25f);
        ESM4Test::appendSubRecord(payload, "DATA", data);

        const ESM4::AmmoEffect value = loadAmmoEffect(std::move(payload));

        EXPECT_EQ(value.mId, ESM::FormId::fromUint32(0x02123456));
        EXPECT_EQ(value.mEditorId, "AmmoEffectDamage");
        EXPECT_EQ(value.mFullName, "Damage");
        EXPECT_EQ(value.mType, ESM4::AmmoEffect::Type::DamageThreshold);
        EXPECT_EQ(value.mOperation, ESM4::AmmoEffect::Operation::Multiply);
        EXPECT_FLOAT_EQ(value.mValue, 1.25f);
    }

    TEST(Esm4AmmoEffectTest, rejectsInvalidEffectData)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("AmmoEffectDamage"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Damage"));
        std::string data;
        ESM4Test::appendPod(data, std::uint32_t{ 99 });
        ESM4Test::appendPod(data, std::uint32_t{});
        ESM4Test::appendPod(data, std::numeric_limits<float>::quiet_NaN());
        ESM4Test::appendSubRecord(payload, "DATA", data);
        EXPECT_THROW(loadAmmoEffect(std::move(payload)), std::runtime_error);
    }
}
