#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "testutil.hpp"

namespace
{
    using namespace std::literals;

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    TEST(Esm4WeaponTest, loadsHeldWorldAndFirstPersonModelFields)
    {
        std::string recordData;
        ESM4Test::appendSubRecord(recordData, "MODL", zString("weapons/held.nif"));
        ESM4Test::appendSubRecord(recordData, "MOD4", zString("weapons/world.nif"));
        ESM4Test::appendSubRecord(recordData, "MO4T", std::string(7, '\x11'));
        ESM4Test::appendSubRecord(recordData, "MO4S", std::string(13, '\x22'));
        ESM4Test::appendSubRecord(recordData, "MO4C", std::string(4, '\x33'));
        ESM4Test::appendSubRecord(recordData, "MO4F", std::string(1, '\x44'));
        std::string firstPersonModel;
        ESM4Test::appendPod(firstPersonModel, std::uint32_t{ 0x5678 });
        ESM4Test::appendSubRecord(recordData, "WNAM", firstPersonModel);
        ESM4Test::appendSubRecord(recordData, "ICON", "icons/weapon.dds\0"sv);

        auto reader = ESM4Test::makeReader("WEAP", std::move(recordData));
        ESM4::Weapon weapon;
        weapon.load(*reader);

        EXPECT_EQ(weapon.mModel.getOriginal(), "weapons/held.nif");
        EXPECT_EQ(weapon.mWorldModel.getOriginal(), "weapons/world.nif");
        EXPECT_EQ(weapon.mFirstPersonModel, ESM::FormId::fromUint32(0x02005678));
        EXPECT_EQ(weapon.mIcon, "icons/weapon.dds");
    }
}
