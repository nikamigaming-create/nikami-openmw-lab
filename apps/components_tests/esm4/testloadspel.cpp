#include <components/esm4/common.hpp>
#include <components/esm4/loadspel.hpp>
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
        appendRecord(plugin, "SPEL", 0x92c48, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    constexpr std::array<std::uint8_t, 16> sPlasmaSpellData{
        // FalloutNV.esm 00092C48 PlasmaEffect SPEL.SPIT.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x6a, 0xcd, 0xcd, 0xcd,
    };
    constexpr std::array<std::uint8_t, 20> sPlasmaEffectData{
        // One GooificationEffect for four seconds, self range, no actor value.
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    };

    TEST(Esm4SpellTest, parsesRetailPlasmaCriticalSpellByteExactly)
    {
        ESM4::Spell::Data data;
        ASSERT_TRUE(ESM4::loadFalloutSpellData(sPlasmaSpellData, data));
        EXPECT_EQ(data.type, ESM4::Spell::Type::ActorEffect);
        EXPECT_EQ(data.flags, 0x6au);

        ESM4::Spell::Effect effect;
        effect.baseEffect = ESM::FormId::fromUint32(0x4dfda);
        ASSERT_TRUE(ESM4::loadFalloutSpellEffectData(sPlasmaEffectData, effect));
        EXPECT_EQ(effect.baseEffect.toUint32(), 0x4dfdau);
        EXPECT_EQ(effect.magnitude, 0u);
        EXPECT_EQ(effect.area, 0u);
        EXPECT_EQ(effect.duration, 4u);
        EXPECT_EQ(effect.range, ESM4::Spell::Range::Self);
        EXPECT_EQ(effect.actorValue, -1);
    }

    TEST(Esm4SpellTest, loadsAndAdjustsRetailCriticalSpellReferences)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("PlasmaEffect"));
        appendSubRecord(payload, "FULL", zString("Gooification"));
        appendSubRecord(payload, "SPIT", std::string_view(
            reinterpret_cast<const char*>(sPlasmaSpellData.data()), sPlasmaSpellData.size()));
        const std::uint32_t baseEffect = 0x4dfda;
        appendSubRecord(payload, "EFID", std::string_view(
            reinterpret_cast<const char*>(&baseEffect), sizeof(baseEffect)));
        appendSubRecord(payload, "EFIT", std::string_view(
            reinterpret_cast<const char*>(sPlasmaEffectData.data()), sPlasmaEffectData.size()));
        auto reader = makeReader(payload);

        ESM4::Spell spell;
        spell.load(*reader);
        EXPECT_EQ(spell.mId, ESM::FormId::fromUint32(0x02092c48));
        EXPECT_EQ(spell.mEditorId, "PlasmaEffect");
        EXPECT_EQ(spell.mFullName, "Gooification");
        ASSERT_EQ(spell.mEffects.size(), 1u);
        EXPECT_EQ(spell.mEffects.front().baseEffect, ESM::FormId::fromUint32(0x0204dfda));
        EXPECT_EQ(spell.mEffects.front().duration, 4u);
    }

    TEST(Esm4SpellTest, rejectsMalformedLayoutsAndMissingEffectData)
    {
        ESM4::Spell::Data data;
        data.cost = 42;
        const std::array<std::uint8_t, 12> shortSpell{};
        EXPECT_FALSE(ESM4::loadFalloutSpellData(shortSpell, data));
        EXPECT_EQ(data.cost, 42u);

        ESM4::Spell::Effect effect;
        effect.duration = 42;
        const std::array<std::uint8_t, 16> shortEffect{};
        EXPECT_FALSE(ESM4::loadFalloutSpellEffectData(shortEffect, effect));
        EXPECT_EQ(effect.duration, 42u);

        std::string payload;
        appendSubRecord(payload, "EDID", zString("Incomplete"));
        appendSubRecord(payload, "SPIT", std::string_view(
            reinterpret_cast<const char*>(sPlasmaSpellData.data()), sPlasmaSpellData.size()));
        const std::uint32_t baseEffect = 0x4dfda;
        appendSubRecord(payload, "EFID", std::string_view(
            reinterpret_cast<const char*>(&baseEffect), sizeof(baseEffect)));
        auto reader = makeReader(payload);
        ESM4::Spell spell;
        EXPECT_THROW(spell.load(*reader), std::runtime_error);
    }
}
