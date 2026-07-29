#include <components/esm4/common.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <class T>
    std::string pod(const T& value)
    {
        std::string result;
        appendPod(result, value);
        return result;
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        ASSERT_EQ(type.size(), 4);
        ASSERT_LE(data.size(), std::numeric_limits<std::uint16_t>::max());
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::string zString(std::string_view value)
    {
        std::string result(value);
        result.push_back('\0');
        return result;
    }

    void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view payload,
        std::uint32_t flags = 0)
    {
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(payload.size()));
        appendPod(output, flags);
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        output.append(payload);
    }

    std::string prefix(std::string_view editorId = "ByteExactNote")
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        return payload;
    }

    std::string textPayload(std::string_view text = "Exact note text")
    {
        std::string payload = prefix();
        appendSubRecord(payload, "OBND", std::string("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c", 12));
        appendSubRecord(payload, "FULL", zString("Byte Exact Note"));
        appendSubRecord(payload, "MODL", zString("clutter\\junk\\paper01.nif"));
        appendSubRecord(payload, "ICON", zString("interface\\icons\\note.dds"));
        appendSubRecord(payload, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(payload, "ONAM", pod(std::uint32_t{ 0x00000100 }));
        appendSubRecord(payload, "ONAM", pod(std::uint32_t{ 0x01000200 }));
        appendSubRecord(payload, "TNAM", zString(text));
        return payload;
    }

    std::unique_ptr<ESM4::Reader> makeReader(
        const std::string& payload, std::uint32_t flags = 0, bool prepareData = true)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        appendSubRecord(headerPayload, "MAST", zString("Master.esm"));
        appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        appendRecord(plugin, "NOTE", 0x010003e8, payload, flags);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "FalloutNV.esm", nullptr, nullptr, true);
        reader->setModIndex(5);
        reader->updateModIndices({ { "master.esm", 2 } });
        EXPECT_TRUE(reader->getRecordHeader());
        if (prepareData)
            reader->getRecordData();
        return reader;
    }

    void expectRejectedWithoutMutation(const std::string& payload, std::uint32_t flags = 0, bool prepareData = true)
    {
        auto reader = makeReader(payload, flags, prepareData);
        ESM4::Note note;
        note.mEditorId = "sentinel";
        note.mText = "unchanged";
        note.mData = 42;
        note.mQuests.push_back(ESM::FormId::fromUint32(0x123));
        EXPECT_THROW(note.load(*reader), std::runtime_error);
        EXPECT_EQ(note.mEditorId, "sentinel");
        EXPECT_EQ(note.mText, "unchanged");
        EXPECT_EQ(note.mData, 42);
        ASSERT_EQ(note.mQuests.size(), 1);
        EXPECT_EQ(note.mQuests[0], ESM::FormId::fromUint32(0x123));
    }

    TEST(Esm4NoteTest, loadsTextShapeAndAdjustsEveryAuthoredFormId)
    {
        auto reader = makeReader(textPayload());
        ESM4::Note note;
        note.load(*reader);

        EXPECT_EQ(note.mId, ESM::FormId::fromUint32(0x050003e8));
        EXPECT_EQ(note.mFlags, 0u);
        EXPECT_EQ(note.mEditorId, "ByteExactNote");
        EXPECT_EQ(note.mFullName, "Byte Exact Note");
        EXPECT_EQ(note.mModel, "clutter\\junk\\paper01.nif");
        EXPECT_EQ(note.mIcon, "interface\\icons\\note.dds");
        EXPECT_EQ(note.mObjectBounds[0], 1);
        EXPECT_EQ(note.mObjectBounds[11], 12);
        EXPECT_EQ(note.mData, 1);
        EXPECT_EQ(note.mText, "Exact note text");
        ASSERT_EQ(note.mQuests.size(), 2);
        EXPECT_EQ(note.mQuests[0], ESM::FormId::fromUint32(0x02000100));
        EXPECT_EQ(note.mQuests[1], ESM::FormId::fromUint32(0x05000200));
        EXPECT_TRUE(note.mImage.empty());
        EXPECT_TRUE(note.mVoiceTopic.isZeroOrUnset());
        EXPECT_TRUE(note.mVoiceSpeaker.isZeroOrUnset());
    }

    TEST(Esm4NoteTest, loadsAllFourFrozenDataShapesWithoutInventingMissingContent)
    {
        std::string noContent = prefix("NoContent");
        appendSubRecord(noContent, "OBND", std::string(12, '\0'));
        appendSubRecord(noContent, "FULL", zString("Note"));
        appendSubRecord(noContent, "MODL", zString("clutter\\junk\\paper02.nif"));
        appendSubRecord(noContent, "ICON", zString("interface\\icons\\note.dds"));
        appendSubRecord(noContent, "DATA", pod(std::uint8_t{ 0 }));
        auto noContentReader = makeReader(noContent);
        ESM4::Note noContentNote;
        noContentNote.load(*noContentReader);
        EXPECT_EQ(noContentNote.mData, 0);
        EXPECT_TRUE(noContentNote.mText.empty());
        EXPECT_TRUE(noContentNote.mImage.empty());

        std::string emptyText = prefix("EmptyText");
        appendSubRecord(emptyText, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(emptyText, "TNAM", zString(""));
        auto emptyTextReader = makeReader(emptyText);
        ESM4::Note emptyTextNote;
        emptyTextNote.load(*emptyTextReader);
        EXPECT_EQ(emptyTextNote.mData, 1);
        EXPECT_TRUE(emptyTextNote.mText.empty());

        std::string image = prefix("ImageNote");
        appendSubRecord(image, "FULL", zString("Image"));
        appendSubRecord(image, "DATA", pod(std::uint8_t{ 2 }));
        appendSubRecord(image, "ONAM", pod(std::uint32_t{ 0x00000300 }));
        appendSubRecord(image, "XNAM", zString("interface\\notes\\image.dds"));
        auto imageReader = makeReader(image);
        ESM4::Note imageNote;
        imageNote.load(*imageReader);
        EXPECT_EQ(imageNote.mData, 2);
        EXPECT_EQ(imageNote.mImage, "interface\\notes\\image.dds");
        ASSERT_EQ(imageNote.mQuests.size(), 1);
        EXPECT_EQ(imageNote.mQuests[0], ESM::FormId::fromUint32(0x02000300));

        std::string voice = prefix("VoiceNote");
        appendSubRecord(voice, "OBND", std::string(12, '\0'));
        appendSubRecord(voice, "FULL", zString("Voice"));
        appendSubRecord(voice, "DATA", pod(std::uint8_t{ 3 }));
        appendSubRecord(voice, "ONAM", pod(std::uint32_t{ 0x01000400 }));
        appendSubRecord(voice, "TNAM", pod(std::uint32_t{ 0x00000500 }));
        appendSubRecord(voice, "SNAM", pod(std::uint32_t{ 0x01000600 }));
        auto voiceReader = makeReader(voice);
        ESM4::Note voiceNote;
        voiceNote.load(*voiceReader);
        EXPECT_EQ(voiceNote.mData, 3);
        EXPECT_EQ(voiceNote.mVoiceTopic, ESM::FormId::fromUint32(0x02000500));
        EXPECT_EQ(voiceNote.mVoiceSpeaker, ESM::FormId::fromUint32(0x05000600));
    }

    TEST(Esm4NoteTest, loadsTheOneFrozenModbShape)
    {
        std::string payload = prefix("BoundedText");
        appendSubRecord(payload, "FULL", zString("Bounded Text"));
        appendSubRecord(payload, "MODL", zString("clutter\\junk\\paper01.nif"));
        appendSubRecord(payload, "MODB", pod(8.375f));
        appendSubRecord(payload, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(payload, "TNAM", zString("Bounds retained"));

        auto reader = makeReader(payload);
        ESM4::Note note;
        note.load(*reader);
        EXPECT_FLOAT_EQ(note.mBoundRadius, 8.375f);
        EXPECT_EQ(note.mText, "Bounds retained");
    }

    TEST(Esm4NoteTest, rejectsMalformedSizesShapesOrderingAndUnknownFieldsAtomically)
    {
        std::vector<std::string> malformed;
        malformed.emplace_back();

        std::string missingEditorTerminator;
        appendSubRecord(missingEditorTerminator, "EDID", "NoTerminator");
        appendSubRecord(missingEditorTerminator, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(missingEditorTerminator, "TNAM", zString("Text"));
        malformed.push_back(std::move(missingEditorTerminator));

        std::string dataFirst;
        appendSubRecord(dataFirst, "DATA", pod(std::uint8_t{ 0 }));
        malformed.push_back(std::move(dataFirst));

        std::string duplicateEditor = prefix();
        appendSubRecord(duplicateEditor, "EDID", zString("Again"));
        malformed.push_back(std::move(duplicateEditor));

        std::string shortBounds = prefix();
        appendSubRecord(shortBounds, "OBND", std::string(11, '\0'));
        malformed.push_back(std::move(shortBounds));

        std::string boundsAfterFull = prefix();
        appendSubRecord(boundsAfterFull, "FULL", zString("Full"));
        appendSubRecord(boundsAfterFull, "OBND", std::string(12, '\0'));
        malformed.push_back(std::move(boundsAfterFull));

        std::string duplicateFull = prefix();
        appendSubRecord(duplicateFull, "FULL", zString("Full"));
        appendSubRecord(duplicateFull, "FULL", zString("Again"));
        malformed.push_back(std::move(duplicateFull));

        std::string modelBeforeEditor;
        appendSubRecord(modelBeforeEditor, "MODL", zString("model.nif"));
        malformed.push_back(std::move(modelBeforeEditor));

        std::string shortModb = prefix();
        appendSubRecord(shortModb, "MODL", zString("model.nif"));
        appendSubRecord(shortModb, "MODB", std::string(3, '\0'));
        malformed.push_back(std::move(shortModb));

        std::string modbWithoutModel = prefix();
        appendSubRecord(modbWithoutModel, "MODB", pod(1.f));
        malformed.push_back(std::move(modbWithoutModel));

        std::string iconAfterModb = prefix();
        appendSubRecord(iconAfterModb, "MODL", zString("model.nif"));
        appendSubRecord(iconAfterModb, "MODB", pod(1.f));
        appendSubRecord(iconAfterModb, "ICON", zString("icon.dds"));
        malformed.push_back(std::move(iconAfterModb));

        malformed.push_back(prefix());
        for (const std::size_t size : { 0u, 2u })
        {
            std::string badData = prefix();
            appendSubRecord(badData, "DATA", std::string(size, '\0'));
            malformed.push_back(std::move(badData));
        }

        std::string unsupportedData = prefix();
        appendSubRecord(unsupportedData, "DATA", pod(std::uint8_t{ 4 }));
        malformed.push_back(std::move(unsupportedData));

        std::string duplicateData = prefix();
        appendSubRecord(duplicateData, "DATA", pod(std::uint8_t{ 0 }));
        appendSubRecord(duplicateData, "DATA", pod(std::uint8_t{ 0 }));
        malformed.push_back(std::move(duplicateData));

        std::string typeZeroQuest = prefix();
        appendSubRecord(typeZeroQuest, "DATA", pod(std::uint8_t{ 0 }));
        appendSubRecord(typeZeroQuest, "ONAM", pod(std::uint32_t{ 0x10 }));
        malformed.push_back(std::move(typeZeroQuest));

        std::string missingText = prefix();
        appendSubRecord(missingText, "DATA", pod(std::uint8_t{ 1 }));
        malformed.push_back(std::move(missingText));

        std::string imageForText = prefix();
        appendSubRecord(imageForText, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(imageForText, "XNAM", zString("image.dds"));
        malformed.push_back(std::move(imageForText));

        std::string unterminatedText = prefix();
        appendSubRecord(unterminatedText, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(unterminatedText, "TNAM", "text");
        malformed.push_back(std::move(unterminatedText));

        std::string missingImage = prefix();
        appendSubRecord(missingImage, "DATA", pod(std::uint8_t{ 2 }));
        malformed.push_back(std::move(missingImage));

        std::string textForImage = prefix();
        appendSubRecord(textForImage, "DATA", pod(std::uint8_t{ 2 }));
        appendSubRecord(textForImage, "TNAM", zString("text"));
        malformed.push_back(std::move(textForImage));

        std::string missingVoiceTopic = prefix();
        appendSubRecord(missingVoiceTopic, "DATA", pod(std::uint8_t{ 3 }));
        malformed.push_back(std::move(missingVoiceTopic));

        for (const std::size_t size : { 3u, 5u })
        {
            std::string badTopic = prefix();
            appendSubRecord(badTopic, "DATA", pod(std::uint8_t{ 3 }));
            appendSubRecord(badTopic, "TNAM", std::string(size, '\0'));
            malformed.push_back(std::move(badTopic));
        }

        std::string zeroTopic = prefix();
        appendSubRecord(zeroTopic, "DATA", pod(std::uint8_t{ 3 }));
        appendSubRecord(zeroTopic, "TNAM", pod(std::uint32_t{ 0 }));
        malformed.push_back(std::move(zeroTopic));

        std::string speakerWithoutTopic = prefix();
        appendSubRecord(speakerWithoutTopic, "DATA", pod(std::uint8_t{ 3 }));
        appendSubRecord(speakerWithoutTopic, "SNAM", pod(std::uint32_t{ 0x10 }));
        malformed.push_back(std::move(speakerWithoutTopic));

        std::string shortSpeaker = prefix();
        appendSubRecord(shortSpeaker, "DATA", pod(std::uint8_t{ 3 }));
        appendSubRecord(shortSpeaker, "TNAM", pod(std::uint32_t{ 0x10 }));
        appendSubRecord(shortSpeaker, "SNAM", std::string(3, '\0'));
        malformed.push_back(std::move(shortSpeaker));

        std::string zeroSpeaker = prefix();
        appendSubRecord(zeroSpeaker, "DATA", pod(std::uint8_t{ 3 }));
        appendSubRecord(zeroSpeaker, "TNAM", pod(std::uint32_t{ 0x10 }));
        appendSubRecord(zeroSpeaker, "SNAM", pod(std::uint32_t{ 0 }));
        malformed.push_back(std::move(zeroSpeaker));

        std::string shortQuest = prefix();
        appendSubRecord(shortQuest, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(shortQuest, "ONAM", std::string(3, '\0'));
        malformed.push_back(std::move(shortQuest));

        std::string zeroQuest = prefix();
        appendSubRecord(zeroQuest, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(zeroQuest, "ONAM", pod(std::uint32_t{ 0 }));
        malformed.push_back(std::move(zeroQuest));

        std::string fiveQuests = prefix();
        appendSubRecord(fiveQuests, "DATA", pod(std::uint8_t{ 1 }));
        for (std::uint32_t value = 1; value <= 5; ++value)
            appendSubRecord(fiveQuests, "ONAM", pod(value));
        malformed.push_back(std::move(fiveQuests));

        std::string questAfterText = prefix();
        appendSubRecord(questAfterText, "DATA", pod(std::uint8_t{ 1 }));
        appendSubRecord(questAfterText, "TNAM", zString("Text"));
        appendSubRecord(questAfterText, "ONAM", pod(std::uint32_t{ 1 }));
        malformed.push_back(std::move(questAfterText));

        std::string duplicateContent = textPayload();
        appendSubRecord(duplicateContent, "TNAM", zString("Again"));
        malformed.push_back(std::move(duplicateContent));

        std::string unknown = prefix();
        appendSubRecord(unknown, "DESC", zString("not authored"));
        malformed.push_back(std::move(unknown));

        std::string truncated = prefix();
        truncated.append("DATA");
        appendPod(truncated, std::uint16_t{ 1 });
        malformed.push_back(std::move(truncated));

        for (const std::string& payload : malformed)
            expectRejectedWithoutMutation(payload);
    }

    TEST(Esm4NoteTest, rejectsEveryRepresentativeNonzeroHeaderFlagAtomically)
    {
        const std::string payload = textPayload();
        expectRejectedWithoutMutation(payload, ESM4::Rec_Constant);
        expectRejectedWithoutMutation(payload, ESM4::Rec_Deleted);

        // The flag gate executes before subrecord access. Avoid asking the
        // synthetic Reader to inflate this deliberately uncompressed payload;
        // production readers have already inflated a legitimately compressed
        // record before dispatching its loader.
        expectRejectedWithoutMutation(payload, ESM4::Rec_Compressed, false);
        expectRejectedWithoutMutation(payload, ESM4::Rec_Constant | ESM4::Rec_Deleted);
    }
}
