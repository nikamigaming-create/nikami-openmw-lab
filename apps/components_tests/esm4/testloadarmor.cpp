#include <components/esm4/loadarma.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
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

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    template <class T>
    void appendSubRecord(std::string& output, std::string_view type, const T& data)
    {
        appendSubRecord(output, type, std::string_view(reinterpret_cast<const char*>(&data), sizeof(data)));
    }

    void appendSubRecord(std::string& output, std::string_view type, const std::string& data)
    {
        appendSubRecord(output, type, std::string_view(data));
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::unique_ptr<ESM4::Reader> makeArmorReader(
        float version, std::string_view recordType, std::string payload)
    {
        std::string hedr;
        appendPod(hedr, version);
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
        appendRecord(plugin, recordType, 0x123456, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "armor.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    std::unique_ptr<ESM4::Reader> makeFnvReader(std::string_view recordType, std::string payload)
    {
        return makeArmorReader(1.34f, recordType, std::move(payload));
    }

    TEST(Esm4ArmorTest, shouldDecodeFnvArmorAddonBipedAndWorldModelsWithoutOrderHeuristics)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestGlovesAddon"));
        appendSubRecord(payload, "MODL", zString("meshes/armor/gloves_m.nif"));
        appendSubRecord(payload, "MOD2", zString("meshes/armor/gloves_world.nif"));
        appendSubRecord(payload, "MOD3", zString("meshes/armor/gloves_f.nif"));
        appendSubRecord(payload, "MOD4", zString("meshes/armor/gloves_world_f.nif"));
        appendSubRecord(payload, "RNAM", std::uint32_t{ 0x42 });
        appendSubRecord(payload, "BMDT",
            std::array<std::uint32_t, 2>{ ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand, 0 });

        auto reader = makeFnvReader("ARMA", std::move(payload));
        ESM4::ArmorAddon addon;
        addon.load(*reader);

        EXPECT_EQ(addon.mModelMale, "meshes/armor/gloves_m.nif");
        EXPECT_EQ(addon.mModelMaleWorld, "meshes/armor/gloves_world.nif");
        EXPECT_EQ(addon.mModelFemale, "meshes/armor/gloves_f.nif");
        EXPECT_EQ(addon.mModelFemaleWorld, "meshes/armor/gloves_world_f.nif");
        EXPECT_EQ(addon.mRacePrimary, ESM::FormId::fromUint32(0x02000042));
        EXPECT_EQ(addon.mBodyTemplate.bodyPart,
            ESM4::Armor::FO3_LeftHand | ESM4::Armor::FO3_RightHand);
    }

    TEST(Esm4ArmorTest, shouldDecodeFnvBipedModelListAndZeroOmittedAddonSlots)
    {
        std::string armorPayload;
        appendSubRecord(armorPayload, "EDID", zString("TestArmor"));
        appendSubRecord(armorPayload, "BIPL", std::uint32_t{ 0x123 });
        auto armorReader = makeFnvReader("ARMO", std::move(armorPayload));
        ESM4::Armor armor;
        armor.load(*armorReader);
        EXPECT_EQ(armor.mBipedModelList, ESM::FormId::fromUint32(0x02000123));

        ESM4::ArmorAddon addon;
        EXPECT_EQ(addon.mBodyTemplate.bodyPart, 0u);
        EXPECT_EQ(addon.mBodyTemplate.flags, 0u);
    }

    TEST(Esm4ArmorTest, shouldKeepFo4BipedModelWhenModlContainsAdditionalRace)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestFo4BodyAddon"));
        appendSubRecord(payload, "MOD2", zString("Actors/Character/CharacterAssets/MaleBody.nif"));
        appendSubRecord(payload, "MODL", std::uint32_t{ 0x13746 });

        auto reader = makeArmorReader(1.0f, "ARMA", std::move(payload));
        ESM4::ArmorAddon addon;
        addon.load(*reader);

        EXPECT_EQ(addon.mModelMale, "Actors/Character/CharacterAssets/MaleBody.nif");
        ASSERT_EQ(addon.mRaces.size(), 1u);
        EXPECT_EQ(addon.mRaces.front(), ESM::FormId::fromUint32(0x02013746));
    }
}
