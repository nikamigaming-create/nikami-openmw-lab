#include <components/esm4/common.hpp>
#include <components/esm4/loadamef.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string data(std::uint32_t type, std::uint32_t operation, float value)
    {
        std::string result;
        appendPod(result, type);
        appendPod(result, operation);
        appendPod(result, value);
        return result;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        const auto appendRecord = [](std::string& output, std::string_view type, std::uint32_t formId,
                                      std::string_view body) {
            output.append(type);
            appendPod(output, static_cast<std::uint32_t>(body.size()));
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, formId);
            appendPod(output, std::uint32_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            appendPod(output, std::uint16_t{ 0 });
            output.append(body);
        };

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, "AMEF", 0x15e8ec, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    std::string retailConditionEffect()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("AmmoEffectSpecialConditionBonus"));
        appendSubRecord(payload, "FULL", zString("COND Decay Reduction"));
        appendSubRecord(payload, "DATA", data(4, 1, 0.75f));
        return payload;
    }

    TEST(Esm4AmmoEffectTest, parsesRetailConditionMultiplierByteExactly)
    {
        auto reader = makeReader(retailConditionEffect());
        ESM4::AmmoEffect effect;
        effect.load(*reader);

        EXPECT_EQ(effect.mId, ESM::FormId::fromUint32(0x0215e8ec));
        EXPECT_EQ(effect.mEditorId, "AmmoEffectSpecialConditionBonus");
        EXPECT_EQ(effect.mFullName, "COND Decay Reduction");
        EXPECT_EQ(effect.mType, ESM4::AmmoEffect::Type::WeaponCondition);
        EXPECT_EQ(effect.mOperation, ESM4::AmmoEffect::Operation::Multiply);
        EXPECT_FLOAT_EQ(effect.mValue, 0.75f);
    }

    TEST(Esm4AmmoEffectTest, parsesEveryNativeTypeAndOperation)
    {
        for (std::uint32_t type = 0; type <= 5; ++type)
        {
            for (std::uint32_t operation = 0; operation <= 2; ++operation)
            {
                std::string payload;
                appendSubRecord(payload, "EDID", zString("Effect"));
                appendSubRecord(payload, "FULL", zString("Effect"));
                appendSubRecord(payload, "DATA", data(type, operation, 2.5f));
                auto reader = makeReader(payload);
                ESM4::AmmoEffect effect;
                effect.load(*reader);
                EXPECT_EQ(static_cast<std::uint32_t>(effect.mType), type);
                EXPECT_EQ(static_cast<std::uint32_t>(effect.mOperation), operation);
            }
        }
    }

    TEST(Esm4AmmoEffectTest, rejectsMalformedOrUnknownPayloadWithoutMutation)
    {
        const auto expectRejected = [](const std::string& payload) {
            auto reader = makeReader(payload);
            ESM4::AmmoEffect effect;
            effect.mEditorId = "sentinel";
            effect.mValue = 42.f;
            EXPECT_THROW(effect.load(*reader), std::runtime_error);
            EXPECT_EQ(effect.mEditorId, "sentinel");
            EXPECT_FLOAT_EQ(effect.mValue, 42.f);
        };

        std::string invalidType;
        appendSubRecord(invalidType, "EDID", zString("Invalid"));
        appendSubRecord(invalidType, "FULL", zString("Invalid"));
        appendSubRecord(invalidType, "DATA", data(6, 1, 1.f));
        expectRejected(invalidType);

        std::string invalidOperation;
        appendSubRecord(invalidOperation, "EDID", zString("Invalid"));
        appendSubRecord(invalidOperation, "FULL", zString("Invalid"));
        appendSubRecord(invalidOperation, "DATA", data(4, 3, 1.f));
        expectRejected(invalidOperation);

        std::string shortData;
        appendSubRecord(shortData, "EDID", zString("Invalid"));
        appendSubRecord(shortData, "FULL", zString("Invalid"));
        appendSubRecord(shortData, "DATA", pod(std::uint32_t{ 4 }));
        expectRejected(shortData);

        std::string unknown = retailConditionEffect();
        appendSubRecord(unknown, "ICON", zString("not-authored.dds"));
        expectRejected(unknown);
    }
}
