#include "loadfact.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sRelation = ESM::fourCC("XNAM");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");
    constexpr std::uint32_t sCrimeGoldMultiplier = ESM::fourCC("CNAM");
    constexpr std::uint32_t sRank = ESM::fourCC("RNAM");
    constexpr std::uint32_t sMaleTitle = ESM::fourCC("MNAM");
    constexpr std::uint32_t sFemaleTitle = ESM::fourCC("FNAM");
    constexpr std::uint32_t sReputation = ESM::fourCC("WMI1");

    [[noreturn]] void fail(std::string_view field)
    {
        throw std::runtime_error("ESM4::Faction::load - invalid or incomplete " + std::string(field));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail(ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize) + ", expected "
                + std::to_string(expected));
        }
    }

    void requireZString(const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize == ESM4::Fallout::kEmptySubrecordBytes)
            fail(ESM::printName(header.typeId) + " string");
    }

    template <class T>
    void readExact(ESM4::Reader& reader, T& value, std::string_view field)
    {
        if (!reader.getExact(value))
            fail(field);
    }
}

void ESM4::Faction::load(Reader& reader)
{
    if (!Fallout::isNewVegasVersion(reader.esmVersion()))
        fail("unsupported ESM4 version");

    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasFullName = false;
    bool hasData = false;
    bool hasCrimeGoldMultiplier = false;
    bool hasReputation = false;
    bool relationsStarted = false;
    bool rankHasMaleTitle = false;
    bool rankHasFemaleTitle = false;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case sEditorId:
                if (hasEditorId || hasFullName || relationsStarted || hasData)
                    fail("EDID order");
                requireZString(header);
                if (!reader.getZString(mEditorId))
                    fail("EDID");
                hasEditorId = true;
                break;
            case sFullName:
                if (!hasEditorId || hasFullName || relationsStarted || hasData)
                    fail("FULL order");
                requireZString(header);
                reader.getLocalizedString(mFullName);
                hasFullName = true;
                break;
            case sRelation:
            {
                if (!hasEditorId || hasData)
                    fail("XNAM order");
                requireSize(header, Fallout::kFactionRelationBytes);
                Relation relation;
                if (!reader.getFormId(relation.mFaction))
                    fail("XNAM faction");
                readExact(reader, relation.mModifier, "XNAM modifier");
                std::uint32_t reaction = 0;
                readExact(reader, reaction, "XNAM reaction");
                if (reaction > static_cast<std::uint32_t>(GroupCombatReaction::Friend))
                    fail("XNAM reaction");
                relation.mGroupCombatReaction = static_cast<GroupCombatReaction>(reaction);
                mRelations.push_back(std::move(relation));
                relationsStarted = true;
                break;
            }
            case sData:
                if (!hasEditorId || hasData)
                    fail("DATA order");
                if (header.dataSize != Fallout::kFactionDataShortBytes
                    && header.dataSize != Fallout::kFactionDataLongBytes)
                    fail("DATA size");
                readExact(reader, mData.mFlags1, "DATA flags 1");
                if (header.dataSize == Fallout::kFactionDataLongBytes)
                {
                    readExact(reader, mData.mFlags2, "DATA flags 2");
                    readExact(reader, mData.mUnused[Fallout::kFactionDataUnusedFirstIndex], "DATA unused byte 0");
                    readExact(reader, mData.mUnused[Fallout::kFactionDataUnusedSecondIndex], "DATA unused byte 1");
                }
                mData.mSerializedSize = static_cast<std::uint8_t>(header.dataSize);
                hasData = true;
                break;
            case sCrimeGoldMultiplier:
            {
                if (!hasData || hasCrimeGoldMultiplier || !mRanks.empty() || hasReputation)
                    fail("CNAM order");
                requireSize(header, Fallout::kFactionCrimeGoldMultiplierBytes);
                float value = 0.f;
                readExact(reader, value, "CNAM value");
                mCrimeGoldMultiplier = value;
                hasCrimeGoldMultiplier = true;
                break;
            }
            case sRank:
            {
                if (!hasData || hasReputation)
                    fail("RNAM order");
                requireSize(header, Fallout::kFactionRankBytes);
                Rank rank;
                readExact(reader, rank.mRank, "RNAM rank");
                mRanks.push_back(std::move(rank));
                rankHasMaleTitle = false;
                rankHasFemaleTitle = false;
                break;
            }
            case sMaleTitle:
                if (mRanks.empty() || hasReputation || rankHasMaleTitle || rankHasFemaleTitle)
                    fail("MNAM order");
                requireZString(header);
                if (!reader.getZString(mRanks.back().mMaleTitle))
                    fail("MNAM");
                rankHasMaleTitle = true;
                break;
            case sFemaleTitle:
                if (mRanks.empty() || hasReputation || rankHasFemaleTitle)
                    fail("FNAM order");
                requireZString(header);
                if (!reader.getZString(mRanks.back().mFemaleTitle))
                    fail("FNAM");
                rankHasFemaleTitle = true;
                break;
            case sReputation:
                if (!hasData || hasReputation)
                    fail("WMI1 order");
                requireSize(header, Fallout::kFactionReputationBytes);
                if (!reader.getFormId(mReputation))
                    fail("WMI1 reputation");
                hasReputation = true;
                break;
            default:
                fail(ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasData)
        fail("required fields");
}
