#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

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

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags)
    {
        ASSERT_EQ(type.size(), 4);
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    std::string relation(
        std::uint32_t faction, std::int32_t modifier, ESM4::Faction::GroupCombatReaction reaction)
    {
        std::string result;
        appendPod(result, faction);
        appendPod(result, modifier);
        appendPod(result, static_cast<std::uint32_t>(reaction));
        return result;
    }

    std::string longFactionData(std::uint8_t flags1, std::uint8_t flags2)
    {
        std::string result;
        appendPod(result, flags1);
        appendPod(result, flags2);
        appendPod(result, std::uint8_t{ 0 });
        appendPod(result, std::uint8_t{ 0 });
        return result;
    }

    std::string makePlugin(
        const std::vector<std::pair<std::uint32_t, std::string>>& factionRecords, bool withMaster = false,
        std::uint32_t factionFlags = 0)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, static_cast<std::int32_t>(factionRecords.size() + 1));
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        if (withMaster)
        {
            appendSubRecord(headerPayload, "MAST", zString("Master.esm"));
            appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));
        }

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        for (const auto& [formId, payload] : factionRecords)
            appendRecord(plugin, "FACT", formId, payload, factionFlags);
        return plugin;
    }

    std::string makeSingleRecordPlugin(
        std::string_view type, std::uint32_t formId, std::string_view payload)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, type, formId, payload, 0);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename,
        std::uint32_t modIndex, bool withMaster = false)
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (withMaster)
            reader->updateModIndices({ { "master.esm", 2 } });
        return reader;
    }

    std::string makeCompleteFactionPayload()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("TestFaction"));
        appendSubRecord(payload, "FULL", zString("Test Faction"));
        appendSubRecord(payload, "XNAM",
            relation(0x00001234, -100, ESM4::Faction::GroupCombatReaction::Enemy));
        appendSubRecord(payload, "XNAM",
            relation(0x01005678, 100, ESM4::Faction::GroupCombatReaction::Friend));
        appendSubRecord(payload, "DATA",
            longFactionData(ESM4::Faction::HiddenFromPlayer | ESM4::Faction::Evil,
                ESM4::Faction::TrackCrime | ESM4::Faction::AllowSell));
        appendSubRecord(payload, "CNAM", pod(1.f));
        appendSubRecord(payload, "RNAM", pod(std::int32_t{ 0 }));
        appendSubRecord(payload, "MNAM", zString("Initiate"));
        appendSubRecord(payload, "FNAM", zString("Initiate"));
        appendSubRecord(payload, "RNAM", pod(std::int32_t{ 1 }));
        appendSubRecord(payload, "FNAM", zString("Senior Initiate"));
        appendSubRecord(payload, "WMI1", pod(std::uint32_t{ 0x010043de }));
        return payload;
    }

    std::string makeMinimalFactionPayload()
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString("MinimalFaction"));
        appendSubRecord(payload, "DATA", pod(std::uint8_t{ ESM4::Faction::HiddenFromPlayer }));
        return payload;
    }

    TEST(Esm4FactionStoreTest, shouldLoadEveryOfficialFnvFieldAndBothDataLayoutsIntoTheTypedStore)
    {
        const std::string plugin
            = makePlugin({ { 0x010003e8, makeCompleteFactionPayload() }, { 0x010003e9, makeMinimalFactionPayload() } },
                true, ESM4::Rec_Constant | ESM4::Rec_Ignored);
        auto reader = makeReader(plugin, "FalloutNV.esm", 5, true);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const auto& factions = store.get<ESM4::Faction>();
        ASSERT_EQ(factions.getSize(), 2);
        const ESM4::Faction* faction = factions.search(ESM::RefId(ESM::FormId::fromUint32(0x050003e8)));
        ASSERT_NE(faction, nullptr);
        EXPECT_EQ(faction->mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(faction->mFlags, ESM4::Rec_Constant | ESM4::Rec_Ignored);
        EXPECT_EQ(faction->mEditorId, "TestFaction");
        EXPECT_EQ(faction->mFullName, "Test Faction");
        ASSERT_EQ(faction->mRelations.size(), 2);
        EXPECT_EQ(faction->mRelations[0].mFaction, ESM::FormId::fromUint32(0x02001234));
        EXPECT_EQ(faction->mRelations[0].mModifier, -100);
        EXPECT_EQ(faction->mRelations[0].mGroupCombatReaction, ESM4::Faction::GroupCombatReaction::Enemy);
        EXPECT_EQ(faction->mRelations[1].mFaction, ESM::FormId::fromUint32(0x05005678));
        EXPECT_EQ(faction->mRelations[1].mModifier, 100);
        EXPECT_EQ(faction->mRelations[1].mGroupCombatReaction, ESM4::Faction::GroupCombatReaction::Friend);
        EXPECT_EQ(faction->mData.mFlags1, ESM4::Faction::HiddenFromPlayer | ESM4::Faction::Evil);
        EXPECT_EQ(faction->mData.mFlags2, ESM4::Faction::TrackCrime | ESM4::Faction::AllowSell);
        EXPECT_EQ(faction->mData.mSerializedSize, 4);
        ASSERT_TRUE(faction->mCrimeGoldMultiplier.has_value());
        EXPECT_FLOAT_EQ(*faction->mCrimeGoldMultiplier, 1.f);
        ASSERT_EQ(faction->mRanks.size(), 2);
        EXPECT_EQ(faction->mRanks[0].mRank, 0);
        EXPECT_EQ(faction->mRanks[0].mMaleTitle, "Initiate");
        EXPECT_EQ(faction->mRanks[0].mFemaleTitle, "Initiate");
        EXPECT_EQ(faction->mRanks[1].mRank, 1);
        EXPECT_TRUE(faction->mRanks[1].mMaleTitle.empty());
        EXPECT_EQ(faction->mRanks[1].mFemaleTitle, "Senior Initiate");
        EXPECT_EQ(faction->mReputation, ESM::FormId::fromUint32(0x050043de));

        const ESM4::Faction* minimal = factions.search(ESM::RefId(ESM::FormId::fromUint32(0x050003e9)));
        ASSERT_NE(minimal, nullptr);
        EXPECT_EQ(minimal->mData.mFlags1, ESM4::Faction::HiddenFromPlayer);
        EXPECT_EQ(minimal->mData.mFlags2, 0);
        EXPECT_EQ(minimal->mData.mSerializedSize, 1);
        EXPECT_FALSE(minimal->mCrimeGoldMultiplier.has_value());
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4FactionStoreTest, shouldNotApplyTheFnvSchemaToAnotherGamesFactRecords)
    {
        const std::string plugin = makePlugin({ { 0x3e8, makeCompleteFactionPayload() } });
        auto reader = makeReader(plugin, "Skyrim.esm", 0);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::Faction>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4FactionStoreTest, shouldRetainEveryFnvCreatureFactionMembershipInAuthoredOrder)
    {
        ESM4::ActorFaction mantis{ 0x000e60e2, 0, 0x49, 0x46, 0x5a };
        ESM4::ActorFaction creature{ 0x00000013, 0, 0x49, 0x46, 0x5a };
        std::string payload;
        appendSubRecord(payload, "EDID", zString("GSGiantMantisNymph"));
        appendSubRecord(payload, "SNAM", pod(mantis));
        appendSubRecord(payload, "SNAM", pod(creature));

        const std::string plugin = makeSingleRecordPlugin("CREA", 0x0011d584, payload);
        auto reader = makeReader(plugin, "FalloutNV.esm", 1);
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const ESM4::Creature* loaded
            = store.get<ESM4::Creature>().search(ESM::RefId(ESM::FormId::fromUint32(0x0111d584)));
        ASSERT_NE(loaded, nullptr);
        ASSERT_EQ(loaded->mFactions.size(), 2);
        EXPECT_EQ(loaded->mFactions[0].faction, 0x010e60e2);
        EXPECT_EQ(loaded->mFactions[1].faction, 0x01000013);
        EXPECT_EQ(loaded->mFaction.faction, 0x01000013);
    }

    TEST(Esm4FactionStoreTest, shouldRejectUnobservedFnvSubrecordSizes)
    {
        std::vector<std::pair<std::string, std::string>> invalidSubrecords{
            { "XNAM", std::string(8, '\0') },
            { "DATA", std::string(2, '\0') },
            { "CNAM", std::string(8, '\0') },
            { "RNAM", std::string(3, '\0') },
            { "WMI1", std::string(8, '\0') },
        };

        for (const auto& [type, bytes] : invalidSubrecords)
        {
            SCOPED_TRACE(type);
            std::string payload;
            appendSubRecord(payload, "EDID", zString("InvalidFaction"));
            if (type != "DATA" && type != "XNAM")
                appendSubRecord(payload, "DATA", pod(std::uint8_t{ 0 }));
            appendSubRecord(payload, type, bytes);
            if (type == "XNAM")
                appendSubRecord(payload, "DATA", pod(std::uint8_t{ 0 }));
            const std::string plugin = makePlugin({ { 0x3e8, payload } });
            auto reader = makeReader(plugin, "FalloutNV.esm", 0);
            MWWorld::ESMStore store;
            EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        }
    }

    TEST(Esm4FactionStoreTest, shouldRejectUnprovenFnvSubrecordsAndOrdering)
    {
        std::vector<std::string> invalidPayloads;

        std::string unknown = makeMinimalFactionPayload();
        appendSubRecord(unknown, "INAM", zString("UnusedInsignia"));
        invalidPayloads.push_back(std::move(unknown));

        std::string missingData;
        appendSubRecord(missingData, "EDID", zString("MissingData"));
        invalidPayloads.push_back(std::move(missingData));

        std::string relationAfterData = makeMinimalFactionPayload();
        appendSubRecord(relationAfterData, "XNAM",
            relation(0x1234, 0, ESM4::Faction::GroupCombatReaction::Neutral));
        invalidPayloads.push_back(std::move(relationAfterData));

        std::string rankTitleWithoutRank = makeMinimalFactionPayload();
        appendSubRecord(rankTitleWithoutRank, "MNAM", zString("No Rank"));
        invalidPayloads.push_back(std::move(rankTitleWithoutRank));

        std::string crimeAfterRank = makeMinimalFactionPayload();
        appendSubRecord(crimeAfterRank, "RNAM", pod(std::int32_t{ 0 }));
        appendSubRecord(crimeAfterRank, "CNAM", pod(1.f));
        invalidPayloads.push_back(std::move(crimeAfterRank));

        std::string rankAfterReputation = makeMinimalFactionPayload();
        appendSubRecord(rankAfterReputation, "WMI1", pod(std::uint32_t{ 0x1234 }));
        appendSubRecord(rankAfterReputation, "RNAM", pod(std::int32_t{ 0 }));
        invalidPayloads.push_back(std::move(rankAfterReputation));

        for (std::size_t index = 0; index < invalidPayloads.size(); ++index)
        {
            SCOPED_TRACE(index);
            const std::string plugin = makePlugin({ { 0x3e8, invalidPayloads[index] } });
            auto reader = makeReader(plugin, "FalloutNV.esm", 0);
            MWWorld::ESMStore store;
            EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        }
    }
}
