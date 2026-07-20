#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

#include "apps/openmw/mwworld/fnvplayerruntimestate.hpp"

namespace
{
    MWWorld::FalloutPlayerState makeBaseState(int contentFile)
    {
        MWWorld::FalloutPlayerState result;
        result.mBaseRecord = ESM::FormId{ .mIndex = 7, .mContentFile = contentFile };
        result.mHealth = 100;
        result.mSpecial = { 5, 5, 5, 5, 5, 5, 5 };
        result.mSkillValues = { 13, 14, 12, 15, 15, 14, 30, 13, 14, 25, 22, 14, 22, 29 };
        result.mSkillOffsets = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xfe, 12, 13 };
        return result;
    }

    std::unique_ptr<std::stringstream> writeRuntimeRecord(const MWWorld::FalloutPlayerState& base, ESM::FormId form,
        float health, const std::array<std::uint8_t, MWWorld::FalloutPlayerState::SkillCount>& offsets,
        bool trailing = false, std::uint32_t version = MWWorld::FalloutPlayerRuntimeState::SaveVersion)
    {
        auto stream = std::make_unique<std::stringstream>();
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        writer.startRecord(ESM::REC_FPLR);
        writer.writeHNT("VERS", version);
        writer.writeFormId(form, true, "FORM");
        writer.writeHNT("HLTH", health);
        for (std::size_t i = 0; i < MWWorld::FalloutPlayerState::SpecialCount; ++i)
            writer.writeHNT("SPEC", 5.f);
        for (const std::uint8_t value : base.mSkillValues)
            writer.writeHNT("SKIL", static_cast<float>(value));
        for (const std::uint8_t value : offsets)
            writer.writeHNT("SOFF", value);
        if (trailing)
            writer.writeHNT("JUNK", std::uint8_t{ 1 });
        writer.endRecord(ESM::REC_FPLR);
        return stream;
    }
}

TEST(FalloutPlayerRuntimeStateTest, KeepsImmutableAuthoredValuesSeparateFromFiniteMutableValues)
{
    MWWorld::FalloutPlayerRuntimeState uninitialized;
    EXPECT_EQ(uninitialized.setCurrentActorValue(5, 1.f),
        MWWorld::FalloutActorValueMutationResult::Uninitialized);
    EXPECT_EQ(uninitialized.modCurrentActorValue(5, 1.f),
        MWWorld::FalloutActorValueMutationResult::Uninitialized);
    EXPECT_FALSE(uninitialized.getCurrentActorValue(5));

    const MWWorld::FalloutPlayerState base = makeBaseState(0);
    MWWorld::FalloutPlayerRuntimeState runtime;
    runtime.initialize(base);

    ASSERT_TRUE(runtime.getBaseActorValue(5));
    ASSERT_TRUE(runtime.getCurrentActorValue(5));
    EXPECT_FLOAT_EQ(runtime.getBaseActorValue(5)->mValue, 5.f);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(5)->mValue, 5.f);
    EXPECT_FALSE(runtime.isDirty());

    EXPECT_EQ(runtime.setCurrentActorValue(5, 12.25f), MWWorld::FalloutActorValueMutationResult::Applied);
    EXPECT_EQ(runtime.modCurrentActorValue(16, -125.5f), MWWorld::FalloutActorValueMutationResult::Applied);
    EXPECT_EQ(runtime.setCurrentActorValue(43, 137.75f), MWWorld::FalloutActorValueMutationResult::Applied);
    EXPECT_FLOAT_EQ(runtime.getBaseActorValue(5)->mValue, 5.f);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(5)->mValue, 12.25f);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, -25.5f);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(43)->mValue, 137.75f);
    ASSERT_TRUE(runtime.getCurrentActorValue(43)->mRawSkillOffset);
    EXPECT_EQ(*runtime.getCurrentActorValue(43)->mRawSkillOffset, 0xfe);
    EXPECT_TRUE(runtime.isDirty());

    EXPECT_EQ(runtime.setCurrentActorValue(2, 1.f), MWWorld::FalloutActorValueMutationResult::Unsupported);
    EXPECT_EQ(runtime.setCurrentActorValue(5, std::numeric_limits<float>::quiet_NaN()),
        MWWorld::FalloutActorValueMutationResult::NonFinite);
    EXPECT_EQ(runtime.setCurrentActorValue(5, std::numeric_limits<float>::max()),
        MWWorld::FalloutActorValueMutationResult::Applied);
    EXPECT_EQ(runtime.modCurrentActorValue(5, std::numeric_limits<float>::max()),
        MWWorld::FalloutActorValueMutationResult::NonFinite);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(5)->mValue, std::numeric_limits<float>::max());

    runtime.resetCurrent();
    EXPECT_FALSE(runtime.isDirty());
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(5)->mValue, 5.f);
}

TEST(FalloutPlayerRuntimeStateTest, OmitsUnchangedStateFromTheSave)
{
    MWWorld::FalloutPlayerRuntimeState runtime;
    runtime.initialize(makeBaseState(0));
    EXPECT_EQ(runtime.countSavedGameRecords(), 0);

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        runtime.write(writer);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "unchanged-fallout-player-runtime");
    EXPECT_FALSE(reader.hasMoreRecs());
}

TEST(FalloutPlayerRuntimeStateTest, DerivesCarryCapacityFromCurrentStrength)
{
    MWWorld::FalloutPlayerRuntimeState runtime;
    EXPECT_FALSE(runtime.getCarryCapacity());

    runtime.initialize(makeBaseState(0));
    ASSERT_TRUE(runtime.getCarryCapacity());
    EXPECT_FLOAT_EQ(*runtime.getCarryCapacity(), 200.f);

    ASSERT_EQ(runtime.setCurrentActorValue(MWWorld::FalloutPlayerRuntimeState::SpecialActorValueBegin, 8.5f),
        MWWorld::FalloutActorValueMutationResult::Applied);
    ASSERT_TRUE(runtime.getCarryCapacity());
    EXPECT_FLOAT_EQ(*runtime.getCarryCapacity(), 235.f);

    ASSERT_EQ(runtime.setCurrentActorValue(MWWorld::FalloutPlayerRuntimeState::SpecialActorValueBegin,
                  std::numeric_limits<float>::max()),
        MWWorld::FalloutActorValueMutationResult::Applied);
    EXPECT_FALSE(runtime.getCarryCapacity());

    runtime.resetCurrent();
    ASSERT_TRUE(runtime.getCarryCapacity());
    EXPECT_FLOAT_EQ(*runtime.getCarryCapacity(), 200.f);
}

TEST(FalloutPlayerRuntimeStateTest, RoundTripsFractionalValuesAndOffsetProvenanceAcrossChangedLoadOrder)
{
    const MWWorld::FalloutPlayerState originalBase = makeBaseState(2);
    MWWorld::FalloutPlayerRuntimeState original;
    original.initialize(originalBase);
    ASSERT_EQ(original.setCurrentActorValue(16, 73.5f), MWWorld::FalloutActorValueMutationResult::Applied);
    ASSERT_EQ(original.setCurrentActorValue(5, 12.25f), MWWorld::FalloutActorValueMutationResult::Applied);
    ASSERT_EQ(original.setCurrentActorValue(43, 137.75f), MWWorld::FalloutActorValueMutationResult::Applied);
    ASSERT_EQ(original.countSavedGameRecords(), 1);

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        original.write(writer);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "fallout-player-runtime-save");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FPLR);
    reader.getRecHeader();

    MWWorld::FalloutPlayerState remappedBase = originalBase;
    remappedBase.mBaseRecord.mContentFile = 7;
    MWWorld::FalloutPlayerRuntimeState restored;
    restored.initialize(remappedBase);
    restored.readRecord(reader);

    EXPECT_FLOAT_EQ(restored.getBaseActorValue(16)->mValue, 100.f);
    EXPECT_FLOAT_EQ(restored.getCurrentActorValue(16)->mValue, 73.5f);
    EXPECT_FLOAT_EQ(restored.getBaseActorValue(5)->mValue, 5.f);
    EXPECT_FLOAT_EQ(restored.getCurrentActorValue(5)->mValue, 12.25f);
    EXPECT_FLOAT_EQ(restored.getCurrentActorValue(43)->mValue, 137.75f);
    ASSERT_TRUE(restored.getCurrentActorValue(43)->mRawSkillOffset);
    EXPECT_EQ(*restored.getCurrentActorValue(43)->mRawSkillOffset, 0xfe);
    EXPECT_TRUE(restored.isDirty());
    EXPECT_EQ(restored.countSavedGameRecords(), 1);
}

TEST(FalloutPlayerRuntimeStateTest, RejectsMalformedStateWithoutPartialMutation)
{
    const MWWorld::FalloutPlayerState base = makeBaseState(2);
    MWWorld::FalloutPlayerRuntimeState runtime;
    runtime.initialize(base);
    ASSERT_EQ(runtime.setCurrentActorValue(16, 81.5f), MWWorld::FalloutActorValueMutationResult::Applied);

    ESM::ESMReader reader;
    reader.open(writeRuntimeRecord(
                    base, base.mBaseRecord, std::numeric_limits<float>::infinity(), base.mSkillOffsets),
        "malformed-player-runtime");
    const std::map<int, int> contentMapping{ { 2, 2 } };
    reader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_FPLR);
    reader.getRecHeader();

    EXPECT_THROW(runtime.readRecord(reader), std::runtime_error);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);

    ESM::ESMReader versionReader;
    versionReader.open(writeRuntimeRecord(base, base.mBaseRecord, 40.f, base.mSkillOffsets, false,
                           MWWorld::FalloutPlayerRuntimeState::SaveVersion + 1),
        "unsupported-player-runtime-version");
    versionReader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(versionReader.hasMoreRecs());
    ASSERT_EQ(versionReader.getRecName().toInt(), ESM::REC_FPLR);
    versionReader.getRecHeader();
    EXPECT_THROW(runtime.readRecord(versionReader), std::runtime_error);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);
}

TEST(FalloutPlayerRuntimeStateTest, RejectsWrongMappedPlayerAndOffsetProvenanceWithoutPartialMutation)
{
    MWWorld::FalloutPlayerState base = makeBaseState(7);
    MWWorld::FalloutPlayerRuntimeState runtime;
    runtime.initialize(base);
    ASSERT_EQ(runtime.setCurrentActorValue(16, 81.5f), MWWorld::FalloutActorValueMutationResult::Applied);

    ESM::FormId wrongPlayer{ .mIndex = 8, .mContentFile = 2 };
    ESM::ESMReader wrongPlayerReader;
    wrongPlayerReader.open(
        writeRuntimeRecord(base, wrongPlayer, 40.f, base.mSkillOffsets), "wrong-mapped-player-runtime");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    wrongPlayerReader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(wrongPlayerReader.hasMoreRecs());
    ASSERT_EQ(wrongPlayerReader.getRecName().toInt(), ESM::REC_FPLR);
    wrongPlayerReader.getRecHeader();
    EXPECT_THROW(runtime.readRecord(wrongPlayerReader), std::runtime_error);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);

    auto wrongOffsets = base.mSkillOffsets;
    ++wrongOffsets[0];
    ESM::FormId savedPlayer = base.mBaseRecord;
    savedPlayer.mContentFile = 2;
    ESM::ESMReader wrongOffsetsReader;
    wrongOffsetsReader.open(
        writeRuntimeRecord(base, savedPlayer, 40.f, wrongOffsets), "wrong-offset-provenance-runtime");
    wrongOffsetsReader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(wrongOffsetsReader.hasMoreRecs());
    ASSERT_EQ(wrongOffsetsReader.getRecName().toInt(), ESM::REC_FPLR);
    wrongOffsetsReader.getRecHeader();
    EXPECT_THROW(runtime.readRecord(wrongOffsetsReader), std::runtime_error);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);
}

TEST(FalloutPlayerRuntimeStateTest, MissingContentAndTrailingDataNeverApplyState)
{
    const MWWorld::FalloutPlayerState base = makeBaseState(7);
    MWWorld::FalloutPlayerRuntimeState runtime;
    runtime.initialize(base);
    ASSERT_EQ(runtime.setCurrentActorValue(16, 81.5f), MWWorld::FalloutActorValueMutationResult::Applied);

    ESM::FormId unavailablePlayer = base.mBaseRecord;
    unavailablePlayer.mContentFile = 2;
    ESM::ESMReader unavailableReader;
    unavailableReader.open(writeRuntimeRecord(base, unavailablePlayer, 40.f, base.mSkillOffsets),
        "missing-content-player-runtime");
    const std::map<int, int> noContentMapping;
    unavailableReader.setContentFileMapping(&noContentMapping);
    ASSERT_TRUE(unavailableReader.hasMoreRecs());
    ASSERT_EQ(unavailableReader.getRecName().toInt(), ESM::REC_FPLR);
    unavailableReader.getRecHeader();
    EXPECT_NO_THROW(runtime.readRecord(unavailableReader));
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);

    ESM::ESMReader trailingReader;
    trailingReader.open(
        writeRuntimeRecord(base, unavailablePlayer, 40.f, base.mSkillOffsets, true), "trailing-player-runtime");
    const std::map<int, int> contentMapping{ { 2, 7 } };
    trailingReader.setContentFileMapping(&contentMapping);
    ASSERT_TRUE(trailingReader.hasMoreRecs());
    ASSERT_EQ(trailingReader.getRecName().toInt(), ESM::REC_FPLR);
    trailingReader.getRecHeader();
    EXPECT_THROW(runtime.readRecord(trailingReader), std::runtime_error);
    EXPECT_FLOAT_EQ(runtime.getCurrentActorValue(16)->mValue, 81.5f);
}
