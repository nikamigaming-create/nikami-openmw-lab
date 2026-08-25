#include <components/esm4/falloutformat.hpp>
#include <components/esm4/loadfact.hpp>

#include "testutil.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    constexpr std::uint32_t kFirstFactionFormId = 0x00001234;
    constexpr std::uint32_t kSecondFactionFormId = 0x01005678;
    constexpr std::uint32_t kReputationFormId = 0x010043de;
    constexpr std::uint32_t kExpectedFactionFormId = 0x02123456;
    constexpr std::uint32_t kExpectedFirstFactionFormId = 0x02001234;
    constexpr std::uint32_t kExpectedSecondFactionFormId = 0x02005678;
    constexpr std::uint32_t kExpectedReputationFormId = 0x020043de;
    constexpr std::int32_t kEnemyModifier = -100;
    constexpr std::int32_t kFriendModifier = 100;
    constexpr std::int32_t kFirstRank = 0;
    constexpr std::int32_t kSecondRank = 1;
    constexpr std::size_t kFirstRelationIndex = 0;
    constexpr std::size_t kSecondRelationIndex = 1;
    constexpr std::size_t kFirstRankIndex = 0;
    constexpr std::size_t kSecondRankIndex = 1;
    constexpr std::size_t kExpectedRelationCount = 2;
    constexpr std::size_t kExpectedRankCount = 2;
    constexpr std::size_t kMalformedExtraBytes = sizeof(std::uint32_t);
    constexpr std::size_t kMalformedMissingByte = 1;

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        ESM4Test::appendPod(result, value);
        return result;
    }

    std::string relation(
        std::uint32_t faction, std::int32_t modifier, ESM4::Faction::GroupCombatReaction reaction)
    {
        std::string result;
        ESM4Test::appendPod(result, faction);
        ESM4Test::appendPod(result, modifier);
        ESM4Test::appendPod(result, static_cast<std::uint32_t>(reaction));
        return result;
    }

    std::string longData(std::uint8_t flags1, std::uint8_t flags2)
    {
        std::string result;
        ESM4Test::appendPod(result, flags1);
        ESM4Test::appendPod(result, flags2);
        result.append(ESM4::Fallout::kFactionDataUnusedBytes, '\0');
        return result;
    }

    ESM4::Faction loadFaction(std::string payload, float version = ESM4Test::kFalloutPluginVersion)
    {
        auto reader = ESM4Test::makeReader("FACT", std::move(payload), ESM4Test::kSyntheticModIndex, version);
        ESM4::Faction faction;
        faction.load(*reader);
        return faction;
    }

    TEST(Esm4FactionTest, LoadsCompleteFalloutContract)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("TestFaction"));
        ESM4Test::appendSubRecord(payload, "FULL", zString("Test Faction"));
        ESM4Test::appendSubRecord(payload, "XNAM",
            relation(kFirstFactionFormId, kEnemyModifier, ESM4::Faction::GroupCombatReaction::Enemy));
        ESM4Test::appendSubRecord(payload, "XNAM",
            relation(kSecondFactionFormId, kFriendModifier, ESM4::Faction::GroupCombatReaction::Friend));
        ESM4Test::appendSubRecord(payload, "DATA",
            longData(ESM4::Faction::HiddenFromPlayer | ESM4::Faction::Evil,
                ESM4::Faction::TrackCrime | ESM4::Faction::AllowSell));
        ESM4Test::appendSubRecord(payload, "CNAM", pod(0.f));
        ESM4Test::appendSubRecord(payload, "RNAM", pod(kFirstRank));
        ESM4Test::appendSubRecord(payload, "MNAM", zString("Initiate"));
        ESM4Test::appendSubRecord(payload, "FNAM", zString("Initiate"));
        ESM4Test::appendSubRecord(payload, "RNAM", pod(kSecondRank));
        ESM4Test::appendSubRecord(payload, "FNAM", zString("Senior Initiate"));
        ESM4Test::appendSubRecord(payload, "WMI1", pod(kReputationFormId));

        const ESM4::Faction faction = loadFaction(std::move(payload));

        EXPECT_EQ(faction.mId, ESM::FormId::fromUint32(kExpectedFactionFormId));
        EXPECT_EQ(faction.mEditorId, "TestFaction");
        EXPECT_EQ(faction.mFullName, "Test Faction");
        ASSERT_EQ(faction.mRelations.size(), kExpectedRelationCount);
        EXPECT_EQ(faction.mRelations[kFirstRelationIndex].mFaction, ESM::FormId::fromUint32(kExpectedFirstFactionFormId));
        EXPECT_EQ(faction.mRelations[kFirstRelationIndex].mModifier, kEnemyModifier);
        EXPECT_EQ(faction.mRelations[kFirstRelationIndex].mGroupCombatReaction, ESM4::Faction::GroupCombatReaction::Enemy);
        EXPECT_EQ(faction.mRelations[kSecondRelationIndex].mFaction, ESM::FormId::fromUint32(kExpectedSecondFactionFormId));
        EXPECT_EQ(faction.mRelations[kSecondRelationIndex].mModifier, kFriendModifier);
        EXPECT_EQ(faction.mRelations[kSecondRelationIndex].mGroupCombatReaction, ESM4::Faction::GroupCombatReaction::Friend);
        EXPECT_EQ(faction.mData.mFlags1, ESM4::Faction::HiddenFromPlayer | ESM4::Faction::Evil);
        EXPECT_EQ(faction.mData.mFlags2, ESM4::Faction::TrackCrime | ESM4::Faction::AllowSell);
        EXPECT_EQ(faction.mData.mSerializedSize, ESM4::Fallout::kFactionDataLongBytes);
        ASSERT_TRUE(faction.mCrimeGoldMultiplier.has_value());
        EXPECT_FLOAT_EQ(*faction.mCrimeGoldMultiplier, 0.f);
        ASSERT_EQ(faction.mRanks.size(), kExpectedRankCount);
        EXPECT_EQ(faction.mRanks[kFirstRankIndex].mRank, kFirstRank);
        EXPECT_EQ(faction.mRanks[kFirstRankIndex].mMaleTitle, "Initiate");
        EXPECT_EQ(faction.mRanks[kFirstRankIndex].mFemaleTitle, "Initiate");
        EXPECT_EQ(faction.mRanks[kSecondRankIndex].mRank, kSecondRank);
        EXPECT_TRUE(faction.mRanks[kSecondRankIndex].mMaleTitle.empty());
        EXPECT_EQ(faction.mRanks[kSecondRankIndex].mFemaleTitle, "Senior Initiate");
        EXPECT_EQ(faction.mReputation, ESM::FormId::fromUint32(kExpectedReputationFormId));
    }

    TEST(Esm4FactionTest, LoadsShortDataLayout)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("MinimalFaction"));
        ESM4Test::appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes, '\0'));

        const ESM4::Faction faction = loadFaction(std::move(payload));

        EXPECT_EQ(faction.mData.mSerializedSize, ESM4::Fallout::kFactionDataShortBytes);
        EXPECT_EQ(faction.mData.mFlags2, 0);
        EXPECT_FALSE(faction.mCrimeGoldMultiplier.has_value());
    }

    TEST(Esm4FactionTest, RejectsMalformedFixedPayloads)
    {
        const std::pair<std::string_view, std::size_t> malformed[] = {
            { "XNAM", ESM4::Fallout::kFactionRelationBytes - kMalformedExtraBytes },
            { "DATA", ESM4::Fallout::kFactionDataShortBytes + kMalformedMissingByte },
            { "CNAM", ESM4::Fallout::kFactionCrimeGoldMultiplierBytes + kMalformedExtraBytes },
            { "RNAM", ESM4::Fallout::kFactionRankBytes - kMalformedMissingByte },
            { "WMI1", ESM4::Fallout::kFactionReputationBytes + kMalformedExtraBytes },
        };

        for (const auto& [type, size] : malformed)
        {
            SCOPED_TRACE(type);
            std::string payload;
            ESM4Test::appendSubRecord(payload, "EDID", zString("InvalidFaction"));
            if (type != "DATA" && type != "XNAM")
                ESM4Test::appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes, '\0'));
            ESM4Test::appendSubRecord(payload, type, std::string(size, '\0'));
            if (type == "XNAM")
                ESM4Test::appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes, '\0'));
            EXPECT_THROW(loadFaction(std::move(payload)), std::runtime_error);
        }
    }

    TEST(Esm4FactionTest, RejectsNonFalloutVersion)
    {
        std::string payload;
        ESM4Test::appendSubRecord(payload, "EDID", zString("SkyrimFaction"));
        ESM4Test::appendSubRecord(payload, "DATA", std::string(ESM4::Fallout::kFactionDataShortBytes, '\0'));

        EXPECT_THROW(loadFaction(std::move(payload), ESM4Test::kOtherPluginVersion), std::runtime_error);
    }
}
