#ifndef OPENMW_COMPONENTS_ESM4_LOADFACT_H
#define OPENMW_COMPONENTS_ESM4_LOADFACT_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    // Fallout: New Vegas FACT data. Other Bethesda games reuse FACT with
    // different schemas, so ESMStore dispatches this type only for FNV.
    struct Faction
    {
        enum DataFlags1 : std::uint8_t
        {
            HiddenFromPlayer = 1u << 0,
            Evil = 1u << 1,
            SpecialCombat = 1u << 2,
        };

        enum DataFlags2 : std::uint8_t
        {
            TrackCrime = 1u << 0,
            AllowSell = 1u << 1,
        };

        enum class GroupCombatReaction : std::uint32_t
        {
            Neutral = 0,
            Enemy = 1,
            Ally = 2,
            Friend = 3,
        };

        struct Relation
        {
            ESM::FormId mFaction;
            std::int32_t mModifier = 0;
            GroupCombatReaction mGroupCombatReaction = GroupCombatReaction::Neutral;
        };

        struct Data
        {
            std::uint8_t mFlags1 = 0;
            std::uint8_t mFlags2 = 0;
            std::array<std::uint8_t, 2> mUnused{};
            std::uint8_t mSerializedSize = 0;
        };

        struct Rank
        {
            std::int32_t mRank = 0;
            std::string mMaleTitle;
            std::string mFemaleTitle;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::vector<Relation> mRelations;
        Data mData;
        std::optional<float> mCrimeGoldMultiplier;
        std::vector<Rank> mRanks;
        ESM::FormId mReputation;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_FACT4;
    };
}

#endif
