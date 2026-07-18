#ifndef OPENMW_MWWORLD_FNVPLAYERSTATE_H
#define OPENMW_MWWORLD_FNVPLAYERSTATE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <components/esm/formid.hpp>
#include <components/esm4/actor.hpp>

namespace ESM
{
    struct NPC;
}

namespace ESM4
{
    struct Npc;
}

namespace MWWorld
{
    template <class T>
    class Store;

    enum class FalloutSpecial : std::size_t
    {
        Strength,
        Perception,
        Endurance,
        Charisma,
        Intelligence,
        Agility,
        Luck,
        Count,
    };

    enum class FalloutSkill : std::size_t
    {
        Barter,
        BigGuns,
        EnergyWeapons,
        Explosives,
        Lockpick,
        Medicine,
        MeleeWeapons,
        Repair,
        Science,
        SmallGuns,
        Sneak,
        Speech,
        SurvivalOrThrowing,
        Unarmed,
        Count,
    };

    struct FalloutActorValueComponents
    {
        // The exact authored component. For skills this is the first DNAM byte array, not a derived total.
        double mValue = 0.0;

        // FNV stores a second raw DNAM byte for every skill. Its signedness/application is deliberately not
        // inferred here; consumers that understand the retail rule can use this exact byte separately.
        std::optional<std::uint8_t> mRawSkillOffset;
    };

    struct FalloutPlayerState
    {
        static constexpr std::size_t SpecialCount = static_cast<std::size_t>(FalloutSpecial::Count);
        static constexpr std::size_t SkillCount = static_cast<std::size_t>(FalloutSkill::Count);

        ESM::FormId mBaseRecord;
        ESM::FormId mTraitsRecord;
        ESM::FormId mStatsRecord;
        ESM::FormId mAIDataRecord;
        ESM::FormId mModelRecord;
        ESM::FormId mBaseDataRecord;

        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        ESM::FormId mRace;
        ESM::FormId mClass;
        ESM::FormId mHair;
        ESM::FormId mEyes;
        ESM::FormId mVoiceType;

        std::uint32_t mRecordFlags = 0;
        ESM4::ACBS_FO3 mStatsConfig{};
        ESM4::AIDataFO3 mAIData{};
        std::int32_t mHealth = 0;
        std::array<std::uint8_t, SpecialCount> mSpecial{};
        std::array<std::uint8_t, SkillCount> mSkillValues{};
        std::array<std::uint8_t, SkillCount> mSkillOffsets{};

        std::uint8_t getSpecial(FalloutSpecial value) const;
        std::uint8_t getSkillValue(FalloutSkill value) const;
        std::uint8_t getSkillOffset(FalloutSkill value) const;

        // FNV actor-value indices supported by exact NPC_ payloads: AI data 0-4, SPECIAL 5-11, health 16,
        // and skills 32-45. Unsupported/derived actor values fail closed with std::nullopt.
        std::optional<FalloutActorValueComponents> getActorValueComponents(std::uint32_t actorValue) const;
    };

    struct FalloutPlayerStateResolution
    {
        std::optional<FalloutPlayerState> mState;
        std::string mError;

        explicit operator bool() const { return mState.has_value(); }
    };

    FalloutPlayerStateResolution resolveFalloutPlayerState(const Store<ESM4::Npc>& npcs);

    // Seed only fields that have an explicit same-unit shared representation. The ESM3 proxy remains a
    // compatibility carrier: its 0-100 attributes and 27 skills must retain their existing compatibility values.
    // FalloutPlayerState is authoritative and intentionally retains all FNV SPECIAL/skills separately.
    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state);
}

#endif
