#include "fnvplayerruntimestate.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <set>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

namespace
{
    [[noreturn]] void invalidSave(const std::string& detail)
    {
        throw std::runtime_error("Fallout Player runtime save is malformed: " + detail);
    }
}

namespace MWWorld
{
    bool FalloutPlayerRuntimeState::isSupported(std::uint32_t actorValue)
    {
        return actorValue == HealthActorValue || actorValue == ActionPointsActorValue
            || actorValue == ExperienceActorValue
            || (actorValue >= SpecialActorValueBegin && actorValue <= SpecialActorValueEnd)
            || (actorValue >= SkillActorValueBegin && actorValue <= SkillActorValueEnd);
    }

    std::optional<std::size_t> FalloutPlayerRuntimeState::specialIndex(std::uint32_t actorValue)
    {
        if (actorValue < SpecialActorValueBegin || actorValue > SpecialActorValueEnd)
            return std::nullopt;
        return static_cast<std::size_t>(actorValue - SpecialActorValueBegin);
    }

    std::optional<std::size_t> FalloutPlayerRuntimeState::skillIndex(std::uint32_t actorValue)
    {
        if (actorValue < SkillActorValueBegin || actorValue > SkillActorValueEnd)
            return std::nullopt;
        return static_cast<std::size_t>(actorValue - SkillActorValueBegin);
    }

    FalloutPlayerRuntimeState::CurrentState FalloutPlayerRuntimeState::makeBaseCurrent() const
    {
        CurrentState result;
        if (!mBase)
            return result;
        result.mHealth = static_cast<float>(mBase->mHealth);
        std::ranges::transform(
            mBase->mSpecial, result.mSpecial.begin(), [](std::uint8_t value) { return static_cast<float>(value); });
        std::ranges::transform(
            mBase->mSkillValues, result.mSkills.begin(), [](std::uint8_t value) { return static_cast<float>(value); });
        result.mActionPoints = 65.f + 3.f
            * result.mSpecial[static_cast<std::size_t>(FalloutSpecial::Agility)];
        const auto addModifiers = [&](std::uint32_t actorValue, float& value) {
            value += mPermanentModifiers[actorValue] + mDamageModifiers[actorValue]
                + mTemporaryModifiers[actorValue];
        };
        addModifiers(HealthActorValue, result.mHealth);
        addModifiers(ActionPointsActorValue, result.mActionPoints);
        addModifiers(ExperienceActorValue, result.mExperience);
        for (std::size_t index = 0; index < result.mSpecial.size(); ++index)
            addModifiers(SpecialActorValueBegin + static_cast<std::uint32_t>(index), result.mSpecial[index]);
        for (std::size_t index = 0; index < result.mSkills.size(); ++index)
            addModifiers(SkillActorValueBegin + static_cast<std::uint32_t>(index), result.mSkills[index]);
        return result;
    }

    void FalloutPlayerRuntimeState::initialize(const std::optional<FalloutPlayerState>& base)
    {
        if (!base)
        {
            clear();
            return;
        }
        initialize(*base);
    }

    void FalloutPlayerRuntimeState::initialize(const FalloutPlayerState& base)
    {
        mBase = base;
        mPermanentModifiers = {};
        mDamageModifiers = {};
        mTemporaryModifiers = {};
        mPerks.clear();
        mReputations.clear();
        mMapMarkerStates.clear();
        mFastTravelEnabled = true;
        mWaitEnabled = true;
        mFastTravelKeepOnCellChange = false;
        mCurrent = makeBaseCurrent();
        mVatsActive = false;
    }

    void FalloutPlayerRuntimeState::applyNativeSaveState(
        std::span<const FalloutSavePlayerHeaderState::ActorValueModifier> modifiers,
        std::span<const FalloutSavePlayerHeaderState::PerkRank> perks)
    {
        if (!mBase)
            throw std::logic_error("cannot apply native FNV Player state before initialization");

        std::array<float, ActorValueCount> permanent{};
        std::array<float, ActorValueCount> damage{};
        std::array<float, ActorValueCount> temporary{};
        for (const FalloutSavePlayerHeaderState::ActorValueModifier& modifier : modifiers)
        {
            if (modifier.mActorValue >= ActorValueCount || !std::isfinite(modifier.mModifier))
                throw std::invalid_argument("invalid native FNV Player actor-value modifier");
            std::array<float, ActorValueCount>* channel = nullptr;
            switch (modifier.mKind)
            {
                case FalloutSavePlayerHeaderState::ActorValueModifierKind::Permanent:
                    channel = &permanent;
                    break;
                case FalloutSavePlayerHeaderState::ActorValueModifierKind::Damage:
                    channel = &damage;
                    break;
                case FalloutSavePlayerHeaderState::ActorValueModifierKind::Temporary:
                    channel = &temporary;
                    break;
            }
            const float combined = (*channel)[modifier.mActorValue] + modifier.mModifier;
            if (!std::isfinite(combined))
                throw std::invalid_argument("overflowed native FNV Player actor-value modifier channel");
            (*channel)[modifier.mActorValue] = combined;
        }

        std::set<std::pair<bool, ESM::FormId>> identities;
        for (const FalloutSavePlayerHeaderState::PerkRank& perk : perks)
        {
            if (perk.mPerk.isZeroOrUnset() || !identities.emplace(perk.mAlternate, perk.mPerk).second)
                throw std::invalid_argument("invalid native FNV Player perk rank list");
        }

        mPermanentModifiers = permanent;
        mDamageModifiers = damage;
        mTemporaryModifiers = temporary;
        mPerks.assign(perks.begin(), perks.end());
        mReputations.clear();
        mMapMarkerStates.clear();
        mFastTravelEnabled = true;
        mWaitEnabled = true;
        mFastTravelKeepOnCellChange = false;
        mCurrent = makeBaseCurrent();
        if (const std::optional<float> maximum = getMaxActionPoints())
            mCurrent.mActionPoints = std::clamp(mCurrent.mActionPoints, 0.f, *maximum);
        mVatsActive = false;
    }

    void FalloutPlayerRuntimeState::clear()
    {
        mBase.reset();
        mCurrent = {};
        mPermanentModifiers = {};
        mDamageModifiers = {};
        mTemporaryModifiers = {};
        mPerks.clear();
        mReputations.clear();
        mMapMarkerStates.clear();
        mFastTravelEnabled = true;
        mWaitEnabled = true;
        mFastTravelKeepOnCellChange = false;
        mVatsActive = false;
    }

    void FalloutPlayerRuntimeState::resetCurrent()
    {
        mCurrent = makeBaseCurrent();
        mVatsActive = false;
    }

    bool FalloutPlayerRuntimeState::isDirty() const
    {
        const auto hasModifier = [](const auto& values) {
            return std::ranges::any_of(values, [](float value) { return value != 0.f; });
        };
        return mBase && (mCurrent != makeBaseCurrent() || hasModifier(mPermanentModifiers)
            || hasModifier(mDamageModifiers) || hasModifier(mTemporaryModifiers) || !mPerks.empty()
            || !mReputations.empty() || !mMapMarkerStates.empty() || !mFastTravelEnabled
            || !mWaitEnabled || mFastTravelKeepOnCellChange);
    }

    std::optional<FalloutRuntimeActorValue> FalloutPlayerRuntimeState::getBaseActorValue(
        std::uint32_t actorValue) const
    {
        if (!mBase)
            return std::nullopt;
        if (actorValue == HealthActorValue)
            return FalloutRuntimeActorValue{ static_cast<float>(mBase->mHealth), std::nullopt };
        if (actorValue == ActionPointsActorValue)
        {
            const float agility = static_cast<float>(mBase->getSpecial(FalloutSpecial::Agility));
            return FalloutRuntimeActorValue{ 65.f + 3.f * agility, std::nullopt };
        }
        if (actorValue == ExperienceActorValue)
            return FalloutRuntimeActorValue{ 0.f, std::nullopt };
        if (const auto index = specialIndex(actorValue))
            return FalloutRuntimeActorValue{ static_cast<float>(mBase->mSpecial[*index]), std::nullopt };
        if (const auto index = skillIndex(actorValue))
            return FalloutRuntimeActorValue{
                static_cast<float>(mBase->mSkillValues[*index]), mBase->mSkillOffsets[*index] };
        return std::nullopt;
    }

    std::optional<FalloutRuntimeActorValue> FalloutPlayerRuntimeState::getCurrentActorValue(
        std::uint32_t actorValue) const
    {
        if (!mBase)
            return std::nullopt;
        if (actorValue == HealthActorValue)
            return FalloutRuntimeActorValue{ mCurrent.mHealth, std::nullopt };
        if (actorValue == ActionPointsActorValue)
            return FalloutRuntimeActorValue{ mCurrent.mActionPoints, std::nullopt };
        if (actorValue == ExperienceActorValue)
            return FalloutRuntimeActorValue{ mCurrent.mExperience, std::nullopt };
        if (const auto index = specialIndex(actorValue))
            return FalloutRuntimeActorValue{ mCurrent.mSpecial[*index], std::nullopt };
        if (const auto index = skillIndex(actorValue))
            return FalloutRuntimeActorValue{ mCurrent.mSkills[*index], mBase->mSkillOffsets[*index] };
        return std::nullopt;
    }

    std::optional<float> FalloutPlayerRuntimeState::getCarryCapacity() const
    {
        const std::optional<FalloutRuntimeActorValue> strength
            = getCurrentActorValue(SpecialActorValueBegin + static_cast<std::uint32_t>(FalloutSpecial::Strength));
        if (!strength)
            return std::nullopt;

        // Fallout: New Vegas' baseline carry-weight rule, before perks and temporary modifiers.
        const float capacity = 150.f + 10.f * strength->mValue;
        if (!std::isfinite(capacity))
            return std::nullopt;
        return capacity;
    }

    std::optional<float> FalloutPlayerRuntimeState::getMaxActionPoints() const
    {
        if (!mBase)
            return std::nullopt;
        const std::uint32_t agilityActorValue
            = SpecialActorValueBegin + static_cast<std::uint32_t>(FalloutSpecial::Agility);
        const float agility = static_cast<float>(mBase->getSpecial(FalloutSpecial::Agility))
            + mPermanentModifiers[agilityActorValue] + mTemporaryModifiers[agilityActorValue];
        const float maximum = 65.f + 3.f * agility + mPermanentModifiers[ActionPointsActorValue]
            + mTemporaryModifiers[ActionPointsActorValue];
        return std::isfinite(maximum) ? std::optional<float>(maximum) : std::nullopt;
    }

    float FalloutPlayerRuntimeState::getSavedDamageModifier(std::uint32_t actorValue) const
    {
        return actorValue < mDamageModifiers.size() ? mDamageModifiers[actorValue] : 0.f;
    }

    bool FalloutPlayerRuntimeState::hasPerk(ESM::FormId perk, bool alternate) const
    {
        return getPerkRankByte(perk, alternate).has_value();
    }

    std::optional<std::uint8_t> FalloutPlayerRuntimeState::getPerkRankByte(
        ESM::FormId perk, bool alternate) const
    {
        const auto found = std::ranges::find_if(mPerks, [&](const FalloutSavePlayerHeaderState::PerkRank& entry) {
            return entry.mPerk == perk && entry.mAlternate == alternate;
        });
        if (found == mPerks.end())
            return std::nullopt;
        return found->mRankByte;
    }

    std::optional<FalloutReputationValue> FalloutPlayerRuntimeState::getReputation(
        ESM::FormId reputation) const
    {
        if (!mBase || reputation.isZeroOrUnset())
            return std::nullopt;
        const auto found = mReputations.find(reputation);
        return found == mReputations.end() ? FalloutReputationValue{} : found->second;
    }

    bool FalloutPlayerRuntimeState::addReputationBump(
        ESM::FormId reputation, bool fame, float maximum, int bump)
    {
        static constexpr std::array<float, 5> sBumpPoints{ 1.f, 2.f, 4.f, 7.f, 12.f };
        if (!mBase || reputation.isZeroOrUnset() || !std::isfinite(maximum) || maximum <= 0.f
            || bump < 1 || bump > static_cast<int>(sBumpPoints.size()))
            return false;

        FalloutReputationValue value = mReputations[reputation];
        float& axis = fame ? value.mFame : value.mInfamy;
        axis = std::clamp(axis + sBumpPoints[static_cast<std::size_t>(bump - 1)], 0.f, maximum);
        if (value == FalloutReputationValue{})
            mReputations.erase(reputation);
        else
            mReputations[reputation] = value;
        return true;
    }

    std::optional<std::uint8_t> FalloutPlayerRuntimeState::getMapMarkerState(ESM::FormId marker) const
    {
        if (!mBase || marker.isZeroOrUnset())
            return std::nullopt;
        const auto found = mMapMarkerStates.find(marker);
        return found == mMapMarkerStates.end() ? std::nullopt : std::optional(found->second);
    }

    bool FalloutPlayerRuntimeState::setMapMarkerState(ESM::FormId marker, std::uint8_t state)
    {
        if (!mBase || marker.isZeroOrUnset() || state > 2)
            return false;
        mMapMarkerStates[marker] = state;
        return true;
    }

    bool FalloutPlayerRuntimeState::setScriptedFastTravel(
        bool canFastTravel, bool canWait, bool keepOnCellChange)
    {
        if (!mBase)
            return false;

        // Retail ignores a transient disable while a persistent scripted block is already active. Enabling always
        // clears the block and also enables waiting, regardless of the authored canWait argument.
        if (!canFastTravel && mFastTravelKeepOnCellChange && !keepOnCellChange)
            return true;
        mFastTravelEnabled = canFastTravel;
        mWaitEnabled = canFastTravel || canWait;
        mFastTravelKeepOnCellChange = keepOnCellChange;
        return true;
    }

    void FalloutPlayerRuntimeState::notifyCellChanged()
    {
        if (!mFastTravelKeepOnCellChange)
            mFastTravelEnabled = true;
    }

    std::optional<int> FalloutPlayerRuntimeState::getReputationThreshold(
        ESM::FormId reputation, float maximum, std::uint32_t axis) const
    {
        const std::optional<FalloutReputationValue> value = getReputation(reputation);
        if (!value || !std::isfinite(maximum) || maximum <= 0.f || axis > 2)
            return std::nullopt;
        const auto level = [maximum](float points) {
            if (points >= maximum)
                return 3;
            if (points >= maximum * 0.5f)
                return 2;
            if (points >= maximum * 0.15f)
                return 1;
            return 0;
        };
        const int fame = level(value->mFame);
        const int infamy = level(value->mInfamy);
        if (fame == 0 && infamy == 0)
            return 1;

        int category = -1;
        int threshold = 0;
        if (infamy == 0)
        {
            category = 1;
            threshold = fame + 3; // Accepted, Liked, Idolized.
        }
        else if (fame == 0)
        {
            category = 2;
            threshold = infamy + 3; // Shunned, Hated, Vilified.
        }
        else if (fame == infamy)
        {
            category = 0;
            threshold = fame + 2; // Mixed, Unpredictable, Wild Child.
        }
        else if (fame == 2 && infamy == 1)
        {
            category = 1;
            threshold = 2; // Smiling Troublemaker.
        }
        else if (fame == 3 && infamy == 1)
        {
            category = 1;
            threshold = 3; // Good Natured Rascal.
        }
        else if (fame == 1 && infamy == 2)
        {
            category = 2;
            threshold = 2; // Sneering Punk.
        }
        else if (fame == 1 && infamy == 3)
        {
            category = 2;
            threshold = 3; // Merciful Thug.
        }
        else
        {
            category = 0;
            threshold = 2; // Dark Hero or Soft-Hearted Devil.
        }
        return axis == static_cast<std::uint32_t>(category) ? threshold : 0;
    }

    FalloutActorValueMutationResult FalloutPlayerRuntimeState::setCurrentActorValue(
        std::uint32_t actorValue, float value)
    {
        if (!mBase)
            return FalloutActorValueMutationResult::Uninitialized;
        if (!isSupported(actorValue))
            return FalloutActorValueMutationResult::Unsupported;
        if (!std::isfinite(value))
            return FalloutActorValueMutationResult::NonFinite;

        if (actorValue == HealthActorValue)
            mCurrent.mHealth = value;
        else if (actorValue == ActionPointsActorValue)
        {
            const float maximum = getMaxActionPoints().value_or(value);
            mCurrent.mActionPoints = std::clamp(value, 0.f, maximum);
        }
        else if (actorValue == ExperienceActorValue)
            mCurrent.mExperience = value;
        else if (const auto index = specialIndex(actorValue))
            mCurrent.mSpecial[*index] = value;
        else if (const auto index = skillIndex(actorValue))
            mCurrent.mSkills[*index] = value;
        else
            return FalloutActorValueMutationResult::Unsupported;
        return FalloutActorValueMutationResult::Applied;
    }

    FalloutActorValueMutationResult FalloutPlayerRuntimeState::modCurrentActorValue(
        std::uint32_t actorValue, float delta)
    {
        if (!mBase)
            return FalloutActorValueMutationResult::Uninitialized;
        const std::optional<FalloutRuntimeActorValue> current = getCurrentActorValue(actorValue);
        if (!current)
            return FalloutActorValueMutationResult::Unsupported;
        if (!std::isfinite(delta))
            return FalloutActorValueMutationResult::NonFinite;
        return setCurrentActorValue(actorValue, current->mValue + delta);
    }

    int FalloutPlayerRuntimeState::countSavedGameRecords() const
    {
        return isDirty() ? 1 : 0;
    }

    void FalloutPlayerRuntimeState::write(ESM::ESMWriter& writer) const
    {
        if (!isDirty())
            return;

        writer.startRecord(ESM::REC_FPLR);
        writer.writeHNT("VERS", SaveVersion);
        writer.writeFormId(mBase->mBaseRecord, true, "FORM");
        writer.writeHNT("HLTH", mCurrent.mHealth);
        writer.writeHNT("ACTP", mCurrent.mActionPoints);
        writer.writeHNT("EXPR", mCurrent.mExperience);
        for (const float value : mCurrent.mSpecial)
            writer.writeHNT("SPEC", value);
        for (const float value : mCurrent.mSkills)
            writer.writeHNT("SKIL", value);
        for (const std::uint8_t value : mBase->mSkillOffsets)
            writer.writeHNT("SOFF", value);
        writer.writeHNT("PCNT", static_cast<std::uint32_t>(mPerks.size()));
        for (const FalloutSavePlayerHeaderState::PerkRank& perk : mPerks)
        {
            writer.writeFormId(perk.mPerk, true, "PERK");
            writer.writeHNT("PRNK", perk.mRankByte);
            writer.writeHNT("PALT", static_cast<std::uint8_t>(perk.mAlternate));
        }
        writer.writeHNT("RCNT", static_cast<std::uint32_t>(mReputations.size()));
        for (const auto& [reputation, value] : mReputations)
        {
            writer.writeFormId(reputation, true, "RPID");
            writer.writeHNT("RINF", value.mInfamy);
            writer.writeHNT("RFAM", value.mFame);
        }
        writer.writeHNT("MCNT", static_cast<std::uint32_t>(mMapMarkerStates.size()));
        for (const auto& [marker, state] : mMapMarkerStates)
        {
            writer.writeFormId(marker, true, "MPID");
            writer.writeHNT("MPST", state);
        }
        writer.writeHNT("FTEN", static_cast<std::uint8_t>(mFastTravelEnabled));
        writer.writeHNT("WTEN", static_cast<std::uint8_t>(mWaitEnabled));
        writer.writeHNT("FTKP", static_cast<std::uint8_t>(mFastTravelKeepOnCellChange));
        writer.endRecord(ESM::REC_FPLR);
    }

    void FalloutPlayerRuntimeState::readRecord(ESM::ESMReader& reader)
    {
        if (!mBase)
            invalidSave("record encountered without an initialized native Player base state");

        std::uint32_t version = 0;
        reader.getHNT(version, "VERS");
        if (version < 1 || version > SaveVersion)
            invalidSave("unsupported version " + std::to_string(version));

        ESM::FormId player = reader.getFormId(true, "FORM");
        const bool contentAvailable = reader.applyContentFileMapping(player);

        CurrentState restored;
        reader.getHNT(restored.mHealth, "HLTH");
        if (version >= 2)
            reader.getHNT(restored.mActionPoints, "ACTP");
        else
            restored.mActionPoints = makeBaseCurrent().mActionPoints;
        if (version >= 3)
            reader.getHNT(restored.mExperience, "EXPR");
        for (float& value : restored.mSpecial)
            reader.getHNT(value, "SPEC");
        for (float& value : restored.mSkills)
            reader.getHNT(value, "SKIL");
        std::array<std::uint8_t, FalloutPlayerState::SkillCount> skillOffsets{};
        for (std::uint8_t& value : skillOffsets)
            reader.getHNT(value, "SOFF");
        std::vector<FalloutSavePlayerHeaderState::PerkRank> perks;
        if (version >= 3)
        {
            std::uint32_t count = 0;
            reader.getHNT(count, "PCNT");
            perks.reserve(count);
            std::set<std::pair<bool, ESM::FormId>> identities;
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ESM::FormId perk = reader.getFormId(true, "PERK");
                const bool contentAvailable = reader.applyContentFileMapping(perk);
                std::uint8_t rank = 0;
                std::uint8_t alternate = 0;
                reader.getHNT(rank, "PRNK");
                reader.getHNT(alternate, "PALT");
                if (alternate > 1)
                    invalidSave("invalid alternate perk flag");
                if (contentAvailable && !identities.emplace(alternate != 0, perk).second)
                    invalidSave("duplicate perk identity");
                if (contentAvailable)
                    perks.push_back({ perk, rank, alternate != 0, 0 });
            }
        }
        std::map<ESM::FormId, FalloutReputationValue> reputations;
        if (version >= 4)
        {
            std::uint32_t count = 0;
            reader.getHNT(count, "RCNT");
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ESM::FormId reputation = reader.getFormId(true, "RPID");
                const bool contentAvailable = reader.applyContentFileMapping(reputation);
                FalloutReputationValue value;
                reader.getHNT(value.mInfamy, "RINF");
                reader.getHNT(value.mFame, "RFAM");
                if (!std::isfinite(value.mInfamy) || !std::isfinite(value.mFame)
                    || value.mInfamy < 0.f || value.mFame < 0.f)
                    invalidSave("invalid reputation value");
                if (contentAvailable && (reputation.isZeroOrUnset()
                    || !reputations.emplace(reputation, value).second))
                    invalidSave("invalid or duplicate reputation identity");
            }
        }
        std::map<ESM::FormId, std::uint8_t> mapMarkerStates;
        if (version >= 5)
        {
            std::uint32_t count = 0;
            reader.getHNT(count, "MCNT");
            for (std::uint32_t index = 0; index < count; ++index)
            {
                ESM::FormId marker = reader.getFormId(true, "MPID");
                const bool markerContentAvailable = reader.applyContentFileMapping(marker);
                std::uint8_t state = 0;
                reader.getHNT(state, "MPST");
                if (state > 2)
                    invalidSave("invalid map marker state");
                if (markerContentAvailable && (marker.isZeroOrUnset()
                    || !mapMarkerStates.emplace(marker, state).second))
                    invalidSave("invalid or duplicate map marker identity");
            }
        }
        std::uint8_t fastTravelEnabled = 1;
        std::uint8_t waitEnabled = 1;
        std::uint8_t fastTravelKeepOnCellChange = 0;
        if (version >= 6)
        {
            reader.getHNT(fastTravelEnabled, "FTEN");
            reader.getHNT(waitEnabled, "WTEN");
            reader.getHNT(fastTravelKeepOnCellChange, "FTKP");
            if (fastTravelEnabled > 1 || waitEnabled > 1 || fastTravelKeepOnCellChange > 1)
                invalidSave("invalid EnableFastTravel state");
            if (fastTravelEnabled != 0 && waitEnabled == 0)
                invalidSave("fast travel cannot be enabled while waiting is disabled");
        }
        if (reader.hasMoreSubs())
            invalidSave("unexpected trailing subrecord");

        if (!std::isfinite(restored.mHealth) || !std::isfinite(restored.mActionPoints)
            || !std::isfinite(restored.mExperience)
            || !std::ranges::all_of(restored.mSpecial, [](float value) { return std::isfinite(value); })
            || !std::ranges::all_of(restored.mSkills, [](float value) { return std::isfinite(value); }))
            invalidSave("non-finite actor value");

        // Match the existing ESM4 quest-save contract: consume and validate the complete record, but do not apply
        // state whose source content is no longer present in the current load order.
        if (!contentAvailable)
            return;
        if (player != mBase->mBaseRecord)
            invalidSave("mapped FORM does not identify the current native Player base record");
        if (skillOffsets != mBase->mSkillOffsets)
            invalidSave("raw DNAM skill-offset provenance does not match the active Player base record");

        // Apply only after the whole record and every invariant has been validated.
        mCurrent = restored;
        mPermanentModifiers = {};
        mDamageModifiers = {};
        mTemporaryModifiers = {};
        mPerks = std::move(perks);
        mReputations = std::move(reputations);
        mMapMarkerStates = std::move(mapMarkerStates);
        mFastTravelEnabled = fastTravelEnabled != 0;
        mWaitEnabled = waitEnabled != 0;
        mFastTravelKeepOnCellChange = fastTravelKeepOnCellChange != 0;
    }
}
