#ifndef OPENMW_MWWORLD_FNVPLAYERRUNTIMESTATE_H
#define OPENMW_MWWORLD_FNVPLAYERRUNTIMESTATE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "fnvplayerstate.hpp"

namespace ESM
{
    class ESMReader;
    class ESMWriter;
}

namespace MWWorld
{
    struct FalloutRuntimeActorValue
    {
        float mValue = 0.f;

        // Exact second DNAM byte. Its retail signedness/application remains unresolved, so it is retained as
        // provenance and is never projected into mValue.
        std::optional<std::uint8_t> mRawSkillOffset;
    };

    enum class FalloutActorValueMutationResult
    {
        Applied,
        Uninitialized,
        Unsupported,
        NonFinite,
    };

    struct FalloutReputationValue
    {
        float mInfamy = 0.f;
        float mFame = 0.f;

        bool operator==(const FalloutReputationValue&) const = default;
    };

    /// Mutable Player actor values kept deliberately separate from the immutable NPC_ base record state.
    ///
    /// This slice covers exact authored/current health, SPECIAL, the fourteen FNV skills, and runtime fame/infamy
    /// reputation values. It does not project actor values into Morrowind attributes, skills, or formulas.
    /// Mutations are bounded to finite float values; retail modifier-stack and UI/allocation clamps are not
    /// inferred here.
    class FalloutPlayerRuntimeState
    {
    public:
        static constexpr std::uint32_t HealthActorValue = 16;
        static constexpr std::uint32_t ActionPointsActorValue = 12;
        static constexpr std::uint32_t ExperienceActorValue = 24;
        static constexpr std::uint32_t SpecialActorValueBegin = 5;
        static constexpr std::uint32_t SpecialActorValueEnd = 11;
        static constexpr std::uint32_t SkillActorValueBegin = 32;
        static constexpr std::uint32_t SkillActorValueEnd = 45;
        static constexpr std::size_t ActorValueCount = 96;
        static constexpr std::uint32_t SaveVersion = 4;

    private:
        struct CurrentState
        {
            float mHealth = 0.f;
            float mActionPoints = 0.f;
            float mExperience = 0.f;
            std::array<float, FalloutPlayerState::SpecialCount> mSpecial{};
            std::array<float, FalloutPlayerState::SkillCount> mSkills{};

            bool operator==(const CurrentState&) const = default;
        };

        std::optional<FalloutPlayerState> mBase;
        CurrentState mCurrent;
        std::array<float, ActorValueCount> mPermanentModifiers{};
        std::array<float, ActorValueCount> mDamageModifiers{};
        std::array<float, ActorValueCount> mTemporaryModifiers{};
        std::vector<FalloutSavePlayerHeaderState::PerkRank> mPerks;
        std::map<ESM::FormId, FalloutReputationValue> mReputations;
        // Transient input/mechanics coordination only. V.A.T.S. owns its queue in ActionManager and this flag keeps
        // the ordinary held-attack path from firing an additional unqueued shot while targeting or executing.
        bool mVatsActive = false;

        static bool isSupported(std::uint32_t actorValue);
        static std::optional<std::size_t> specialIndex(std::uint32_t actorValue);
        static std::optional<std::size_t> skillIndex(std::uint32_t actorValue);
        CurrentState makeBaseCurrent() const;

    public:
        void initialize(const std::optional<FalloutPlayerState>& base);
        void initialize(const FalloutPlayerState& base);
        void applyNativeSaveState(std::span<const FalloutSavePlayerHeaderState::ActorValueModifier> modifiers,
            std::span<const FalloutSavePlayerHeaderState::PerkRank> perks);
        void clear();
        void resetCurrent();

        bool isInitialized() const { return mBase.has_value(); }
        bool isDirty() const;
        const std::optional<FalloutPlayerState>& getBaseState() const { return mBase; }

        std::optional<FalloutRuntimeActorValue> getBaseActorValue(std::uint32_t actorValue) const;
        std::optional<FalloutRuntimeActorValue> getCurrentActorValue(std::uint32_t actorValue) const;
        [[nodiscard]] std::optional<float> getCarryCapacity() const;
        [[nodiscard]] std::optional<float> getMaxActionPoints() const;
        [[nodiscard]] float getSavedDamageModifier(std::uint32_t actorValue) const;
        [[nodiscard]] bool hasPerk(ESM::FormId perk, bool alternate = false) const;
        [[nodiscard]] std::optional<std::uint8_t> getPerkRankByte(
            ESM::FormId perk, bool alternate = false) const;
        [[nodiscard]] const std::vector<FalloutSavePlayerHeaderState::PerkRank>& getPerks() const { return mPerks; }
        [[nodiscard]] std::optional<FalloutReputationValue> getReputation(ESM::FormId reputation) const;
        [[nodiscard]] std::optional<int> getReputationThreshold(
            ESM::FormId reputation, float maximum, std::uint32_t axis) const;
        bool addReputationBump(ESM::FormId reputation, bool fame, float maximum, int bump);
        bool isVatsActive() const { return mVatsActive; }
        void setVatsActive(bool active) { mVatsActive = active; }
        FalloutActorValueMutationResult setCurrentActorValue(std::uint32_t actorValue, float value);
        FalloutActorValueMutationResult modCurrentActorValue(std::uint32_t actorValue, float delta);

        int countSavedGameRecords() const;
        void write(ESM::ESMWriter& writer) const;
        void readRecord(ESM::ESMReader& reader);
    };
}

#endif
