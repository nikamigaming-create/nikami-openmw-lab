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
            || hasModifier(mDamageModifiers) || hasModifier(mTemporaryModifiers) || !mPerks.empty());
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
        writer.endRecord(ESM::REC_FPLR);
    }

    void FalloutPlayerRuntimeState::readRecord(ESM::ESMReader& reader)
    {
        if (!mBase)
            invalidSave("record encountered without an initialized native Player base state");

        std::uint32_t version = 0;
        reader.getHNT(version, "VERS");
        if (version != 1 && version != 2 && version != SaveVersion)
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
    }
}
