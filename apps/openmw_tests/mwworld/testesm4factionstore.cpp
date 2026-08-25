#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include <components/esm4/loadfact.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    constexpr std::uint32_t kFactionFormId = 0x000003e8;
    constexpr std::uint32_t kExpectedFactionFormId = 0x020003e8;
    constexpr std::uint32_t kTes4FormId = 0;
    constexpr std::uint32_t kUnusedRecordWord = 0;
    constexpr std::uint16_t kUnusedRecordVersion = 0;
    constexpr std::uint32_t kRecordFlags = ESM4::Rec_Constant | ESM4::Rec_Ignored;
    constexpr std::uint32_t kSyntheticModIndex = 2;
    constexpr std::int32_t kHeaderRecordCount = 2;
    constexpr std::uint32_t kHeaderFlags = 0x800;
    constexpr std::size_t kExpectedFactionCount = 1;
    constexpr float kFalloutPluginVersion
        = std::bit_cast<float>(static_cast<std::uint32_t>(ESM::VER_134));

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), ESM4::Fallout::kOnDiskFormIdBytes);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view data,
        std::uint32_t flags)
    {
        ASSERT_EQ(type.size(), ESM4::Fallout::kOnDiskFormIdBytes);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(data.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, kUnusedRecordWord);
        appendPod(output, kUnusedRecordVersion);
        appendPod(output, kUnusedRecordVersion);
        output.append(data);
    }

    std::string makePlugin(std::string_view factionPayload)
    {
        std::string hedr;
        appendPod(hedr, kFalloutPluginVersion);
        appendPod(hedr, kHeaderRecordCount);
        appendPod(hedr, kHeaderFlags);

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", kTes4FormId, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "FACT", kFactionFormId, factionPayload, kRecordFlags);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename)
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(kSyntheticModIndex);
        return reader;
    }

    TEST(Esm4FactionStoreTest, LoadsFalloutFactionIntoTypedStore)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestFaction"));
        appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes,
            static_cast<char>(ESM4::Faction::HiddenFromPlayer)));

        auto reader = makeReader(makePlugin(payload), "FalloutNV.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);
        store.setUp();

        const auto& factions = store.get<ESM4::Faction>();
        ASSERT_EQ(factions.getSize(), kExpectedFactionCount);
        const ESM4::Faction* faction = factions.search(ESM::RefId(ESM::FormId::fromUint32(kExpectedFactionFormId)));
        ASSERT_NE(faction, nullptr);
        EXPECT_EQ(faction->mEditorId, "TestFaction");
        EXPECT_EQ(faction->mData.mFlags1, ESM4::Faction::HiddenFromPlayer);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4FactionStoreTest, DoesNotApplyFactionSchemaToNonFalloutContent)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("OtherFaction"));
        appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes, '\0'));

        auto reader = makeReader(makePlugin(payload), "Skyrim.esm");
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::Faction>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }
}
