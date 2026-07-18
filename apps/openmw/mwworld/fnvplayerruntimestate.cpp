#include "fnvplayerruntimestate.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

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
        return actorValue == HealthActorValue
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
        mCurrent = makeBaseCurrent();
    }

    void FalloutPlayerRuntimeState::clear()
    {
        mBase.reset();
        mCurrent = {};
    }

    void FalloutPlayerRuntimeState::resetCurrent()
    {
        mCurrent = makeBaseCurrent();
    }

    bool FalloutPlayerRuntimeState::isDirty() const
    {
        return mBase && mCurrent != makeBaseCurrent();
    }

    std::optional<FalloutRuntimeActorValue> FalloutPlayerRuntimeState::getBaseActorValue(
        std::uint32_t actorValue) const
    {
        if (!mBase)
            return std::nullopt;
        if (actorValue == HealthActorValue)
            return FalloutRuntimeActorValue{ static_cast<float>(mBase->mHealth), std::nullopt };
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
        if (const auto index = specialIndex(actorValue))
            return FalloutRuntimeActorValue{ mCurrent.mSpecial[*index], std::nullopt };
        if (const auto index = skillIndex(actorValue))
            return FalloutRuntimeActorValue{ mCurrent.mSkills[*index], mBase->mSkillOffsets[*index] };
        return std::nullopt;
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
        for (const float value : mCurrent.mSpecial)
            writer.writeHNT("SPEC", value);
        for (const float value : mCurrent.mSkills)
            writer.writeHNT("SKIL", value);
        for (const std::uint8_t value : mBase->mSkillOffsets)
            writer.writeHNT("SOFF", value);
        writer.endRecord(ESM::REC_FPLR);
    }

    void FalloutPlayerRuntimeState::readRecord(ESM::ESMReader& reader)
    {
        if (!mBase)
            invalidSave("record encountered without an initialized native Player base state");

        std::uint32_t version = 0;
        reader.getHNT(version, "VERS");
        if (version != SaveVersion)
            invalidSave("unsupported version " + std::to_string(version));

        ESM::FormId player = reader.getFormId(true, "FORM");
        const bool contentAvailable = reader.applyContentFileMapping(player);

        CurrentState restored;
        reader.getHNT(restored.mHealth, "HLTH");
        for (float& value : restored.mSpecial)
            reader.getHNT(value, "SPEC");
        for (float& value : restored.mSkills)
            reader.getHNT(value, "SKIL");
        std::array<std::uint8_t, FalloutPlayerState::SkillCount> skillOffsets{};
        for (std::uint8_t& value : skillOffsets)
            reader.getHNT(value, "SOFF");
        if (reader.hasMoreSubs())
            invalidSave("unexpected trailing subrecord");

        if (!std::isfinite(restored.mHealth)
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
    }
}
