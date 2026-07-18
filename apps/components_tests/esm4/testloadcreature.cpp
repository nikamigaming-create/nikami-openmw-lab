#include <components/esm4/loadcrea.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendFourCC(std::string& output, std::string_view value)
    {
        ASSERT_EQ(value.size(), 4);
        output.append(value);
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendSubRecord(std::string& output, std::string_view type, const std::string& data)
    {
        appendSubRecord(output, type, std::string_view(data));
    }

    template <class T>
    void appendSubRecord(std::string& output, std::string_view type, const T& data)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        appendSubRecord(output, type, std::string_view(reinterpret_cast<const char*>(&data), sizeof(data)));
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, std::uint32_t{ 0 }); // flags
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 }); // revision
        appendPod(output, std::uint16_t{ 0 }); // form version
        appendPod(output, std::uint16_t{ 0 }); // unknown
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload)
    {
        std::string headerPayload;
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 1 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, "CREA", 0x10ab79, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4CreatureRuntimeStateTest, preservesExactGoodspringsBighornerPayload)
    {
        // FalloutNV.esm CREA 0x0010ab79 GSBigHorner from the frozen corpus.
        constexpr std::array<std::uint8_t, 20> aiBytes{ 0x00, 0x00, 0x32, 0x32, 0x00, 0x6f, 0x20,
            0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
        constexpr std::array<std::uint8_t, 17> dataBytes{ 0x01, 0x28, 0x32, 0x32, 0x3c, 0x00, 0x00,
            0x00, 0x28, 0x00, 0x08, 0x03, 0x08, 0x05, 0x05, 0x05, 0x05 };

        std::string payload;
        appendSubRecord(payload, "AIDT", aiBytes);
        appendSubRecord(payload, "DATA", dataBytes);

        auto reader = makeReader(payload);
        ESM4::Creature creature;
        creature.load(*reader);

        EXPECT_TRUE(creature.mIsFONV);
        EXPECT_TRUE(creature.mHasFNVAIData);
        EXPECT_TRUE(creature.mHasFNVData);
        EXPECT_EQ(std::memcmp(&creature.mFNVAIData, aiBytes.data(), aiBytes.size()), 0);
        EXPECT_EQ(std::memcmp(&creature.mFNVData, dataBytes.data(), dataBytes.size()), 0);

        EXPECT_EQ(creature.mFNVAIData.aggression, 0);
        EXPECT_EQ(creature.mFNVAIData.confidence, 0);
        EXPECT_EQ(creature.mFNVAIData.energyLevel, 50);
        EXPECT_EQ(creature.mFNVAIData.responsibility, 50);
        EXPECT_EQ(creature.mFNVAIData.mood, 0);
        EXPECT_EQ(creature.mFNVAIData.unused[0], 0x6f);
        EXPECT_EQ(creature.mFNVAIData.unused[1], 0x20);
        EXPECT_EQ(creature.mFNVAIData.unused[2], 0x64);
        EXPECT_EQ(creature.mFNVAIData.services, 0u);
        EXPECT_EQ(creature.mFNVAIData.assistance, 1);
        EXPECT_EQ(creature.mFNVAIData.aggroRadiusBehavior, 0);
        EXPECT_EQ(creature.mFNVAIData.aggroRadius, 0);

        EXPECT_EQ(creature.mFNVData.type, 1);
        EXPECT_EQ(creature.mFNVData.combatSkill, 40);
        EXPECT_EQ(creature.mFNVData.magicSkill, 50);
        EXPECT_EQ(creature.mFNVData.stealthSkill, 50);
        EXPECT_EQ(creature.mFNVData.health, 60);
        EXPECT_EQ(creature.mFNVData.unused, 0);
        EXPECT_EQ(creature.mFNVData.damage, 40);
        EXPECT_EQ(creature.mFNVData.strength, 8);
        EXPECT_EQ(creature.mFNVData.perception, 3);
        EXPECT_EQ(creature.mFNVData.endurance, 8);
        EXPECT_EQ(creature.mFNVData.charisma, 5);
        EXPECT_EQ(creature.mFNVData.intelligence, 5);
        EXPECT_EQ(creature.mFNVData.agility, 5);
        EXPECT_EQ(creature.mFNVData.luck, 5);
    }

    TEST(Esm4CreatureRuntimeStateTest, rejectsSmallerAndLargerFalloutPayloads)
    {
        constexpr std::array<std::pair<std::string_view, std::size_t>, 4> malformed{
            std::pair{ std::string_view{ "AIDT" }, std::size_t{ 19 } },
            std::pair{ std::string_view{ "AIDT" }, std::size_t{ 21 } },
            std::pair{ std::string_view{ "DATA" }, std::size_t{ 16 } },
            std::pair{ std::string_view{ "DATA" }, std::size_t{ 18 } },
        };

        for (const auto& [type, size] : malformed)
        {
            SCOPED_TRACE(type);
            std::string payload;
            appendSubRecord(payload, type, std::string(size, '\0'));

            auto reader = makeReader(payload);
            ESM4::Creature creature;
            EXPECT_THROW(creature.load(*reader), std::runtime_error);
        }
    }

    TEST(Esm4CreatureTemplateTest, resolvesPartialVisualFieldsIndependently)
    {
        ESM4::Creature concrete;
        concrete.mModel = "actors/robot/skeleton.nif";
        concrete.mBaseTemplate = ESM::FormId::fromUint32(0x100);

        ESM4::Creature middle;
        middle.mNif = { "actors/robot/screen.nif" };
        middle.mBaseTemplate = ESM::FormId::fromUint32(0x200);

        ESM4::Creature root;
        root.mBodyParts = { ESM::FormId::fromUint32(0x300) };
        root.mKf = { "actors/robot/idle.kf" };

        const ESM4::CreatureVisualTemplate result
            = ESM4::resolveCreatureVisualTemplate({ &concrete, &middle, &root });
        EXPECT_EQ(result.mModel, &concrete);
        EXPECT_EQ(result.mNif, &middle);
        EXPECT_EQ(result.mKf, &root);
        EXPECT_EQ(result.mBodyParts, &root);
    }

    TEST(Esm4CreatureTemplateTest, honorsUseModelFlagBeforeResolvingEachVisualField)
    {
        ESM4::Creature concrete;
        concrete.mModel = "wrong/local/skeleton.nif";
        concrete.mNif = { "wrong/local/screen.nif" };
        concrete.mBodyParts = { ESM::FormId::fromUint32(0x111) };
        concrete.mBaseTemplate = ESM::FormId::fromUint32(0x100);
        concrete.mBaseConfig.fo3.templateFlags = ESM4::Creature::Template_UseModel;

        ESM4::Creature templated;
        templated.mModel = "actors/robot/skeleton.nif";
        templated.mNif = { "actors/robot/screen.nif" };
        templated.mBodyParts = { ESM::FormId::fromUint32(0x222) };

        const ESM4::CreatureVisualTemplate result
            = ESM4::resolveCreatureVisualTemplate({ &concrete, &templated });
        EXPECT_EQ(result.mModel, &templated);
        EXPECT_EQ(result.mNif, &templated);
        EXPECT_EQ(result.mBodyParts, &templated);
    }
}
