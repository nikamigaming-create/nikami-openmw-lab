#include <components/esm4/common.hpp>
#include <components/esm4/loadmgef.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
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
        appendRecord(plugin, "MGEF", 0x4dfda, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    constexpr std::array<std::uint8_t, 72> sGooificationData{
        // FalloutNV.esm 0004DFDA GooificationEffect MGEF.DATA.
        0x75, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x91, 0x76, 0x0c, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0xcd, 0xcd,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x24, 0xce, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    };

    TEST(Esm4MagicEffectTest, parsesRetailGooificationDataByteExactly)
    {
        ESM4::MagicEffect::Data data;
        ASSERT_TRUE(ESM4::loadFalloutMagicEffectData(sGooificationData, data));
        ASSERT_TRUE(data.present);
        EXPECT_EQ(data.flags, 0x10000075u);
        EXPECT_EQ(data.associatedItem.toUint32(), 0x0c7691u);
        EXPECT_EQ(data.school, -1);
        EXPECT_EQ(data.resistanceActorValue, -1);
        EXPECT_EQ(data.counterEffectCount, 0);
        EXPECT_FLOAT_EQ(data.projectileSpeed, 1.f);
        EXPECT_EQ(data.hitSound.toUint32(), 0x0bce24u);
        EXPECT_EQ(data.archetype, ESM4::MagicEffect::Archetype::Script);
        EXPECT_EQ(data.actorValue, -1);
    }

    TEST(Esm4MagicEffectTest, loadsAndAdjustsRetailCriticalEffectReferences)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("GooificationEffect"));
        appendSubRecord(payload, "FULL", zString("Gooification"));
        appendSubRecord(payload, "DESC", zString(""));
        appendSubRecord(payload, "DATA", std::string_view(
            reinterpret_cast<const char*>(sGooificationData.data()), sGooificationData.size()));
        auto reader = makeReader(payload);

        ESM4::MagicEffect effect;
        effect.load(*reader);
        EXPECT_EQ(effect.mId, ESM::FormId::fromUint32(0x0204dfda));
        EXPECT_EQ(effect.mEditorId, "GooificationEffect");
        EXPECT_EQ(effect.mFullName, "Gooification");
        EXPECT_EQ(effect.mData.associatedItem, ESM::FormId::fromUint32(0x020c7691));
        EXPECT_EQ(effect.mData.hitSound, ESM::FormId::fromUint32(0x020bce24));
    }

    TEST(Esm4MagicEffectTest, rejectsEveryNonRetailDataSizeAndInvalidArchetypeWithoutMutation)
    {
        ESM4::MagicEffect::Data data;
        data.flags = 42;
        const std::array<std::uint8_t, 68> shortData{};
        const std::array<std::uint8_t, 76> longData{};
        EXPECT_FALSE(ESM4::loadFalloutMagicEffectData(shortData, data));
        EXPECT_FALSE(ESM4::loadFalloutMagicEffectData(longData, data));
        EXPECT_EQ(data.flags, 42u);

        auto invalid = sGooificationData;
        const std::uint32_t invalidArchetype = 37;
        std::memcpy(invalid.data() + 64, &invalidArchetype, sizeof(invalidArchetype));
        EXPECT_FALSE(ESM4::loadFalloutMagicEffectData(invalid, data));
        EXPECT_EQ(data.flags, 42u);
    }
}
