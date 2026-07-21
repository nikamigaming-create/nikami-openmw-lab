#include <components/esm4/loadweap.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <bit>
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

    std::unique_ptr<ESM4::Reader> makeFnvWeaponReader(std::string payload)
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
        appendRecord(plugin, "WEAP", 0x123456, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4WeaponTest, shouldParseFalloutAnimationSelectorsFromDnamPrefix)
    {
        // Retail FNV WeapNVAssaultCarbine (0008F21E): TwoHandAutomatic, default grip, one ammo/use,
        // reload animation 5. Bytes between the selectors deliberately contain non-zero payload.
        const std::array<std::uint8_t, 16> dnam{
            6, 0x11, 0x22, 0x33, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x20, 0x41, 0x7f, 0xff, 1, 5
        };
        ESM4::Weapon::Data data;

        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_EQ(data.animationType, 6);
        EXPECT_FLOAT_EQ(data.animationMultiplier, 1.f);
        EXPECT_FLOAT_EQ(data.reach, 10.f);
        EXPECT_EQ(data.weaponFlags1, 0x7f);
        EXPECT_TRUE(data.isAutomatic());
        EXPECT_EQ(data.handGrip, 0xff);
        EXPECT_EQ(data.ammoUse, 1);
        EXPECT_EQ(data.reloadAnim, 5);
    }

    TEST(Esm4WeaponTest, shouldRejectTruncatedFalloutDnamWithoutChangingDefaults)
    {
        const std::array<std::uint8_t, 15> dnam{};
        ESM4::Weapon::Data data;

        EXPECT_FALSE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_EQ(data.animationType, 0xff);
        EXPECT_EQ(data.handGrip, 0xff);
        EXPECT_EQ(data.ammoUse, 0);
        EXPECT_EQ(data.reloadAnim, 0);
    }

    TEST(Esm4WeaponTest, shouldPreserveFalloutAutomaticCadenceFields)
    {
        std::array<std::uint8_t, 136> dnam{};
        const auto writeFloat = [&](std::size_t offset, float value) {
            std::memcpy(dnam.data() + offset, &value, sizeof(value));
        };
        dnam[0] = 6;
        dnam[12] = ESM4::Weapon::Data::Automatic;
        dnam[14] = 1;
        dnam[15] = 5;
        writeFloat(4, 1.25f);
        writeFloat(8, 1.f);
        writeFloat(60, 1.5f);
        writeFloat(64, 4.f);
        writeFloat(88, 9.f);
        writeFloat(128, 0.1f);
        writeFloat(132, 0.2f);

        ESM4::Weapon::Data data;
        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_TRUE(data.isAutomatic());
        EXPECT_FLOAT_EQ(data.animationMultiplier, 1.25f);
        EXPECT_FLOAT_EQ(data.reach, 1.f);
        EXPECT_FLOAT_EQ(data.animAttackMult, 1.5f);
        EXPECT_FLOAT_EQ(data.fireRate, 4.f);
        EXPECT_FLOAT_EQ(data.animShotsPerSec, 9.f);
        EXPECT_FLOAT_EQ(data.semiAutoFireDelayMin, 0.1f);
        EXPECT_FLOAT_EQ(data.semiAutoFireDelayMax, 0.2f);
    }

    TEST(Esm4WeaponTest, shouldPreserveFalloutVatsAndLimbContract)
    {
        std::array<std::uint8_t, 120> dnam{};
        const auto writeFloat = [&](std::size_t offset, float value) {
            std::memcpy(dnam.data() + offset, &value, sizeof(value));
        };
        const auto writeInt32 = [&](std::size_t offset, std::int32_t value) {
            std::memcpy(dnam.data() + offset, &value, sizeof(value));
        };
        dnam[40] = 17;
        writeFloat(68, 22.f);
        writeInt32(104, 32);
        writeFloat(116, 0.75f);

        ESM4::Weapon::Data data;
        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        EXPECT_EQ(data.baseVatsChance, 17);
        EXPECT_FLOAT_EQ(data.overrideActionPoints, 22.f);
        EXPECT_EQ(data.skillActorValue, 32);
        EXPECT_FLOAT_EQ(data.limbDamageMult, 0.75f);
    }

    TEST(Esm4WeaponTest, shouldParseRetailServiceRifleCriticalDataByteExactly)
    {
        // FalloutNV.esm 000E9C3B WEAP.CRDT: 18 critical damage, x1 multiplier, On Death, no effect.
        const std::array<std::uint8_t, 16> crdt{ 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        ESM4::Weapon::CriticalData data;
        ASSERT_TRUE(ESM4::loadFalloutWeaponCrdt(crdt, data));
        EXPECT_TRUE(data.present);
        EXPECT_EQ(data.damage, 18);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.chanceMultiplier), std::uint32_t{ 0x3f800000 });
        EXPECT_EQ(data.flags, ESM4::Weapon::CriticalData::OnDeath);
        EXPECT_TRUE(data.effect.isZeroOrUnset());

        EXPECT_FALSE(ESM4::loadFalloutWeaponCrdt(
            std::span<const std::uint8_t>(crdt.data(), crdt.size() - 1), data));
    }

    TEST(Esm4WeaponTest, shouldAdjustFalloutCriticalEffectThroughWinningLoadOrder)
    {
        std::string crdt(16, '\0');
        const std::uint16_t damage = 24;
        const float chanceMultiplier = 2.f;
        const std::uint32_t effect = 0x5678;
        std::memcpy(crdt.data(), &damage, sizeof(damage));
        std::memcpy(crdt.data() + 4, &chanceMultiplier, sizeof(chanceMultiplier));
        std::memcpy(crdt.data() + 12, &effect, sizeof(effect));

        std::string payload;
        appendSubRecord(payload, "CRDT", crdt);
        auto reader = makeFnvWeaponReader(std::move(payload));
        ESM4::Weapon weapon;
        weapon.load(*reader);

        EXPECT_TRUE(weapon.mCriticalData.present);
        EXPECT_EQ(weapon.mCriticalData.damage, 24);
        EXPECT_FLOAT_EQ(weapon.mCriticalData.chanceMultiplier, 2.f);
        EXPECT_EQ(weapon.mCriticalData.effect, ESM::FormId::fromUint32(0x02005678));
    }

    TEST(Esm4WeaponTest, shouldParseRetailServiceRifleBallisticContractByteExactly)
    {
        // FalloutNV.esm 000E9C3B WEAP.DNAM, through fireRate at byte 67.
        const std::array<std::uint8_t, 68> dnam{ 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
            0x00, 0x00, 0x80, 0x3f, 0x00, 0xff, 0x01, 0x05, 0xcd, 0xcc, 0x0c, 0x3f, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x42, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x42,
            0x00, 0x00, 0x2a, 0x26, 0x01, 0x00, 0x00, 0x00, 0x40, 0x44, 0x00, 0xc0, 0x5d, 0x45, 0x00,
            0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x33, 0x33, 0x33, 0x3f, 0x00, 0x00, 0x80, 0x3f };
        ESM4::Weapon::Data data;

        ASSERT_TRUE(ESM4::loadFalloutWeaponDnam(dnam, data));
        ASSERT_TRUE(data.hasBallistics);
        EXPECT_EQ(data.projectile.toUint32(), 0x426d);
        EXPECT_EQ(data.numProjectiles, 1);
        EXPECT_EQ(data.ammoUse, 1);
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.minSpread), std::uint32_t{ 0x3f0ccccd });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.minRange), std::uint32_t{ 0x44400000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.maxRange), std::uint32_t{ 0x455dc000 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.animAttackMult), std::uint32_t{ 0x3f333333 });
        EXPECT_EQ(std::bit_cast<std::uint32_t>(data.fireRate), std::uint32_t{ 0x3f800000 });
    }

    TEST(Esm4WeaponTest, shouldPreserveAuthoredFirstPersonModelAndConsumeItsModelData)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestServiceRifle"));
        appendSubRecord(payload, "MODL", zString("weapons/2handrifle/varmintrifle.nif"));
        appendSubRecord(payload, "MOD4", zString("weapons/2handrifle/1stpersonvarmintrifle.nif"));
        appendSubRecord(payload, "MO4T", std::string(7, '\x11'));
        appendSubRecord(payload, "MO4S", std::string(13, '\x22'));
        appendSubRecord(payload, "MO4C", std::string(4, '\x33'));
        appendSubRecord(payload, "MO4F", std::string(1, '\x44'));
        // This field after every MO4* payload proves the reader remains aligned.
        appendSubRecord(payload, "ICON", zString("textures/interface/icons/weapons/varmintrifle.dds"));

        auto reader = makeFnvWeaponReader(std::move(payload));
        ESM4::Weapon weapon;
        weapon.load(*reader);

        EXPECT_EQ(weapon.mEditorId, "TestServiceRifle");
        EXPECT_EQ(weapon.mModel, "weapons/2handrifle/varmintrifle.nif");
        EXPECT_EQ(weapon.mFirstPersonModel, "weapons/2handrifle/1stpersonvarmintrifle.nif");
        EXPECT_EQ(weapon.mIcon, "textures/interface/icons/weapons/varmintrifle.dds");
    }
}
