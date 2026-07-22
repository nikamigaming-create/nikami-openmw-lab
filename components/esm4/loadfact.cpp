#include "loadfact.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"

namespace
{
    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Faction::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    void requireZString(const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize == 0)
            fail("zero-sized " + ESM::printName(header.typeId));
    }

    template <class T>
    void readExact(ESM4::Reader& reader, T& value, std::string_view field)
    {
        if (!reader.getExact(value))
            fail("could not read " + std::string(field));
    }
}

void ESM4::Faction::load(Reader& reader)
{
    // Frozen English Ultimate Edition corpus (10 official masters):
    // 772 FACT records; EDID 772; FULL 489; XNAM 1547x12; DATA
    // 738x4 + 34x1; CNAM 34x4; RNAM 112x4; MNAM 95; FNAM 54;
    // WMI1 48x4. No other FACT subrecords or sizes occur in that corpus.
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
        const SubRecordHeader& subHeader = reader.subRecordHeader();
        switch (subHeader.typeId)
        {
            case ESM::fourCC("EDID"):
                if (hasEditorId || hasFullName || relationsStarted || hasData)
                    fail("EDID is missing, duplicated, or out of order");
                requireZString(subHeader);
                if (!reader.getZString(mEditorId))
                    fail("could not read EDID");
                hasEditorId = true;
                break;
            case ESM::fourCC("FULL"):
                if (!hasEditorId || hasFullName || relationsStarted || hasData)
                    fail("FULL is duplicated or out of order");
                requireZString(subHeader);
                reader.getLocalizedString(mFullName);
                hasFullName = true;
                break;
            case ESM::fourCC("XNAM"):
            {
                if (!hasEditorId || hasData)
                    fail("XNAM appears outside the relation section");
                requireSize(subHeader, 12);
                Relation relation;
                if (!reader.getFormId(relation.mFaction))
                    fail("could not read XNAM faction FormID");
                readExact(reader, relation.mModifier, "XNAM modifier");
                std::uint32_t reaction = 0;
                readExact(reader, reaction, "XNAM group combat reaction");
                if (reaction > static_cast<std::uint32_t>(GroupCombatReaction::Friend))
                    fail("unsupported XNAM group combat reaction " + std::to_string(reaction));
                relation.mGroupCombatReaction = static_cast<GroupCombatReaction>(reaction);
                mRelations.push_back(relation);
                relationsStarted = true;
                break;
            }
            case ESM::fourCC("DATA"):
                if (!hasEditorId || hasData)
                    fail("DATA is missing, duplicated, or out of order");
                if (subHeader.dataSize != 1 && subHeader.dataSize != 4)
                    fail("unsupported DATA size " + std::to_string(subHeader.dataSize) + ", expected 1 or 4");
                readExact(reader, mData.mFlags1, "DATA flags 1");
                if (subHeader.dataSize == 4)
                {
                    readExact(reader, mData.mFlags2, "DATA flags 2");
                    readExact(reader, mData.mUnused[0], "DATA unused byte 0");
                    readExact(reader, mData.mUnused[1], "DATA unused byte 1");
                }
                mData.mSerializedSize = static_cast<std::uint8_t>(subHeader.dataSize);
                hasData = true;
                break;
            case ESM::fourCC("CNAM"):
            {
                if (!hasData || hasCrimeGoldMultiplier || !mRanks.empty() || hasReputation)
                    fail("CNAM is duplicated or out of order");
                requireSize(subHeader, 4);
                float value = 0.f;
                readExact(reader, value, "CNAM value");
                mCrimeGoldMultiplier = value;
                hasCrimeGoldMultiplier = true;
                break;
            }
            case ESM::fourCC("RNAM"):
            {
                if (!hasData || hasReputation)
                    fail("RNAM appears outside the rank section");
                requireSize(subHeader, 4);
                Rank rank;
                readExact(reader, rank.mRank, "RNAM rank");
                mRanks.push_back(std::move(rank));
                rankHasMaleTitle = false;
                rankHasFemaleTitle = false;
                break;
            }
            case ESM::fourCC("MNAM"):
                if (mRanks.empty() || hasReputation || rankHasMaleTitle || rankHasFemaleTitle)
                    fail("MNAM appears outside its rank or is duplicated/out of order");
                requireZString(subHeader);
                if (!reader.getZString(mRanks.back().mMaleTitle))
                    fail("could not read MNAM");
                rankHasMaleTitle = true;
                break;
            case ESM::fourCC("FNAM"):
                if (mRanks.empty() || hasReputation || rankHasFemaleTitle)
                    fail("FNAM appears outside its rank or is duplicated");
                requireZString(subHeader);
                if (!reader.getZString(mRanks.back().mFemaleTitle))
                    fail("could not read FNAM");
                rankHasFemaleTitle = true;
                break;
            case ESM::fourCC("WMI1"):
                if (!hasData || hasReputation)
                    fail("WMI1 is duplicated or out of order");
                requireSize(subHeader, 4);
                if (!reader.getFormId(mReputation))
                    fail("could not read WMI1 reputation FormID");
                hasReputation = true;
                break;
            default:
                fail("unknown Fallout New Vegas subrecord " + ESM::printName(subHeader.typeId));
        }
    }

    if (!hasEditorId || !hasData)
        fail("record is missing required EDID or DATA");
}
