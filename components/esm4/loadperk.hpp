#ifndef OPENMW_COMPONENTS_ESM4_LOADPERK_H
#define OPENMW_COMPONENTS_ESM4_LOADPERK_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "script.hpp"

namespace ESM4
{
    class Reader;

    // Fallout: New Vegas PERK records. The same record signature is reused by
    // other Bethesda games with incompatible layouts, so ESMStore dispatches
    // this type only after a Fallout: New Vegas master has selected the session.
    struct Perk
    {
        struct Data
        {
            std::uint8_t mTrait = 0;
            std::uint8_t mMinimumLevel = 0;
            std::uint8_t mRankCount = 0;
            std::uint8_t mPlayable = 0;
            std::optional<std::uint8_t> mHidden;
            std::uint8_t mSerializedSize = 0;
        };

        enum class EntryType : std::uint8_t
        {
            Quest = 0,
            Ability = 1,
            EntryPoint = 2,
        };

        struct QuestEntry
        {
            ESM::FormId mQuest;
            std::uint8_t mStage = 0;
            // These bytes are not stable padding in the retail corpus. Preserve
            // their authored values rather than normalizing or interpreting them.
            std::array<std::uint8_t, 3> mUnused{};
        };

        struct AbilityEntry
        {
            // Two official entries intentionally omit DATA.
            std::optional<ESM::FormId> mAbility;
        };

        struct EntryPointData
        {
            std::uint8_t mEntryPoint = 0;
            std::uint8_t mFunction = 0;
            std::uint8_t mConditionTabCount = 0;
        };

        struct ConditionGroup
        {
            std::uint8_t mTab = 0;
            std::vector<TargetCondition> mConditions;
        };

        enum class EntryPointFunctionType : std::uint8_t
        {
            Float = 1,
            FormId = 3,
            EmbeddedScript = 4,
        };

        struct EntryPointEntry
        {
            EntryPointData mData;
            std::vector<ConditionGroup> mConditionGroups;
            EntryPointFunctionType mFunctionType = EntryPointFunctionType::Float;
            std::optional<float> mFloat;
            ESM::FormId mFormId;
            std::string mButtonLabel;
            std::uint16_t mScriptFlags = 0;
            ScriptDefinition mScript;
        };

        using EntryData = std::variant<QuestEntry, AbilityEntry, EntryPointEntry>;

        struct Entry
        {
            EntryType mType = EntryType::Quest;
            std::uint8_t mRank = 0;
            std::uint8_t mPriority = 0;
            EntryData mData = QuestEntry{};
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mDescription;
        std::string mIcon;
        std::vector<TargetCondition> mConditions;
        Data mData;
        std::vector<Entry> mEntries;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_PERK4;
    };
}

#endif
