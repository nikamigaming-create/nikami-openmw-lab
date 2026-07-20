#include <components/esm4/loadrace.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <cstdint>
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

    void appendFourCC(std::string& output, std::string_view value)
    {
        ASSERT_EQ(value.size(), 4);
        output.append(value);
    }

    void appendSubRecord(std::string& output, std::string_view type, std::uint32_t value)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint16_t>(sizeof(value)));
        appendPod(output, value);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload)
    {
        appendFourCC(output, type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        output.append(payload);
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& payload)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 1 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendFourCC(headerPayload, "HEDR");
        appendPod(headerPayload, static_cast<std::uint16_t>(hedr.size()));
        headerPayload.append(hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload);
        appendRecord(plugin, "RACE", 0x19, payload);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        EXPECT_TRUE(reader->getRecordHeader());
        reader->getRecordData();
        return reader;
    }

    TEST(Esm4RaceTest, PreservesFalloutBodyPartDataLink)
    {
        constexpr std::uint32_t bodyPartData = 0x0002d1e0;
        std::string payload;
        appendSubRecord(payload, "GNAM", bodyPartData);
        auto reader = makeReader(payload);
        ESM4::Race race;
        race.load(*reader);
        EXPECT_EQ(race.mBodyPartData, ESM::FormId::fromUint32(bodyPartData));
    }

    TEST(Esm4RaceTest, DoesNotTreatAttackDataAsFalloutBodyPartDataLink)
    {
        constexpr std::uint32_t bodyPartData = 0x0002d1e0;
        std::string payload;
        appendSubRecord(payload, "GNAM", bodyPartData);
        appendSubRecord(payload, "ATKD", std::uint32_t{ 0xdeadbeef });
        auto reader = makeReader(payload);
        ESM4::Race race;
        race.load(*reader);
        EXPECT_EQ(race.mBodyPartData, ESM::FormId::fromUint32(bodyPartData));
    }
}
