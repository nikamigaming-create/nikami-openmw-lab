#include <components/esm4/loadammo.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace
{
    using namespace std::literals;

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        if (type.size() != 4 || data.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::logic_error("invalid synthetic ESM4 subrecord");
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view data)
    {
        if (type.size() != 4)
            throw std::logic_error("invalid synthetic ESM4 record");
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(data.size()));
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        output.append(data);
    }

    std::unique_ptr<ESM4::Reader> makeAmmunitionReader(std::string recordData)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerData;
        appendSubRecord(headerData, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerData);
        appendRecord(plugin, "AMMO", 0x123456, recordData);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        reader->setModIndex(2);
        if (!reader->getRecordHeader())
            throw std::logic_error("synthetic ESM4 AMMO record is missing");
        reader->getRecordData();
        return reader;
    }

    std::string makeData2(std::uint32_t projectilesPerShot, std::uint32_t projectile, float weight,
        std::uint32_t consumedAmmo = 0, float consumedPercentage = 0.f, bool includeConsumedAmmo = false)
    {
        std::string result;
        appendPod(result, projectilesPerShot);
        appendPod(result, projectile);
        appendPod(result, weight);
        if (includeConsumedAmmo)
        {
            appendPod(result, consumedAmmo);
            appendPod(result, consumedPercentage);
        }
        return result;
    }

    ESM4::Ammunition loadAmmunition(std::string data2)
    {
        std::string recordData;
        appendSubRecord(recordData, "DAT2", data2);
        appendSubRecord(recordData, "ICON", "icons/ammo.dds\0"sv);
        auto reader = makeAmmunitionReader(std::move(recordData));
        ESM4::Ammunition ammunition;
        ammunition.load(*reader);
        return ammunition;
    }

    TEST(Esm4AmmunitionTest, loadsTwelveByteData2WithoutConsumedAmmo)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(makeData2(5, 0x5678, 0.25f));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 5u);
        EXPECT_EQ(ammunition.mData.mProjectile, ESM::FormId::fromUint32(0x02005678));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(ammunition.mData.mWeight), 0x3e800000u);
        EXPECT_TRUE(ammunition.mData.mConsumedAmmo.isZeroOrUnset());
        EXPECT_FLOAT_EQ(ammunition.mData.mConsumedPercentage, 0.f);
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }

    TEST(Esm4AmmunitionTest, loadsTwentyByteData2WithConsumedAmmo)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(makeData2(1, 0x5678, 0.5f, 0xabcd, 35.f, true));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 1u);
        EXPECT_EQ(ammunition.mData.mProjectile, ESM::FormId::fromUint32(0x02005678));
        EXPECT_FLOAT_EQ(ammunition.mData.mWeight, 0.5f);
        EXPECT_EQ(ammunition.mData.mConsumedAmmo, ESM::FormId::fromUint32(0x0200abcd));
        EXPECT_FLOAT_EQ(ammunition.mData.mConsumedPercentage, 35.f);
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }

    TEST(Esm4AmmunitionTest, skipsUnknownData2SizeWithoutLosingSubrecordAlignment)
    {
        const ESM4::Ammunition ammunition = loadAmmunition(std::string(16, '\0'));

        EXPECT_EQ(ammunition.mData.mProjPerShot, 0u);
        EXPECT_TRUE(ammunition.mData.mProjectile.isZeroOrUnset());
        EXPECT_EQ(ammunition.mIcon, "icons/ammo.dds");
    }
}
