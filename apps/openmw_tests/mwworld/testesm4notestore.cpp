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

#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/reader.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    struct Record
    {
        std::uint32_t mFormId = 0;
        std::string mPayload;
        std::uint32_t mFlags = 0;
    };

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

    std::string notePayload(std::string_view editorId, std::uint8_t data, std::string_view content = "Text",
        std::uint32_t quest = 0, std::uint32_t speaker = 0)
    {
        std::string payload;
        appendSubRecord(payload, "EDID", zString(editorId));
        appendSubRecord(payload, "OBND", std::string(12, '\0'));
        appendSubRecord(payload, "FULL", zString("Stored Note"));
        appendSubRecord(payload, "DATA", pod(data));
        if (quest != 0)
            appendSubRecord(payload, "ONAM", pod(quest));
        switch (data)
        {
            case 0:
                break;
            case 1:
                appendSubRecord(payload, "TNAM", zString(content));
                break;
            case 2:
                appendSubRecord(payload, "XNAM", zString(content));
                break;
            case 3:
                appendSubRecord(payload, "TNAM", pod(std::uint32_t{ 0x00000014 }));
                if (speaker != 0)
                    appendSubRecord(payload, "SNAM", pod(speaker));
                break;
        }
        return payload;
    }

    std::string makePlugin(const std::vector<Record>& records, std::string_view master = {})
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, static_cast<std::int32_t>(records.size() + 1));
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerPayload;
        appendSubRecord(headerPayload, "HEDR", hedr);
        if (!master.empty())
        {
            appendSubRecord(headerPayload, "MAST", zString(master));
            appendSubRecord(headerPayload, "DATA", pod(std::uint64_t{ 0 }));
        }

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerPayload, ESM4::Rec_ESM);
        for (const Record& record : records)
            appendRecord(plugin, "NOTE", record.mFormId, record.mPayload, record.mFlags);
        return plugin;
    }

    std::unique_ptr<ESM4::Reader> makeReader(const std::string& plugin, std::string_view filename,
        std::uint32_t modIndex, const std::map<std::string, int>& masters = {})
    {
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!masters.empty())
            reader->updateModIndices(masters);
        return reader;
    }

    TEST(Esm4NoteStoreTest, loadsFnvNotesAndAdjustsEveryStoredFormId)
    {
        const std::string plugin = makePlugin(
            {
                { 0x00000500, notePayload("StoredText", 1, "Stored terminal text", 0x00000100) },
                { 0x01000600, notePayload("StoredVoice", 3, {}, 0x01000200, 0x01000300) },
            },
            "Master.esm");
        auto reader = makeReader(plugin, "FalloutNV.esm", 5, { { "master.esm", 2 } });

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const auto& notes = store.get<ESM4::Note>();
        ASSERT_EQ(notes.getSize(), 2);
        const ESM4::Note* text = notes.search(ESM::RefId(ESM::FormId::fromUint32(0x02000500)));
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->mEditorId, "StoredText");
        EXPECT_EQ(text->mText, "Stored terminal text");
        ASSERT_EQ(text->mQuests.size(), 1);
        EXPECT_EQ(text->mQuests[0], ESM::FormId::fromUint32(0x02000100));

        const ESM4::Note* voice = notes.search(ESM::RefId(ESM::FormId::fromUint32(0x05000600)));
        ASSERT_NE(voice, nullptr);
        EXPECT_EQ(voice->mVoiceTopic, ESM::FormId::fromUint32(0x02000014));
        EXPECT_EQ(voice->mVoiceSpeaker, ESM::FormId::fromUint32(0x05000300));
        ASSERT_EQ(voice->mQuests.size(), 1);
        EXPECT_EQ(voice->mQuests[0], ESM::FormId::fromUint32(0x05000200));
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(Esm4NoteStoreTest, reproducesTheFrozenOfficialWinningLiveCensus)
    {
        constexpr std::array<std::size_t, 4> frozenByData = { 2, 1166, 7, 50 };
        constexpr std::size_t frozenNotes = 1225;
        static_assert(frozenByData[0] + frozenByData[1] + frozenByData[2] + frozenByData[3] == frozenNotes);

        std::vector<Record> records;
        records.reserve(frozenNotes);
        std::uint32_t formId = 0x00010000;
        for (std::uint8_t data = 0; data < frozenByData.size(); ++data)
        {
            for (std::size_t index = 0; index < frozenByData[data]; ++index)
            {
                records.push_back({ formId++,
                    notePayload("Note" + std::to_string(data) + "_" + std::to_string(index), data,
                        data == 2 ? "image.dds" : "text") });
            }
        }
        ASSERT_EQ(records.size(), frozenNotes);

        auto reader = makeReader(makePlugin(records), "FalloutNV.esm", 0);
        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        const auto& notes = store.get<ESM4::Note>();
        ASSERT_EQ(notes.getSize(), frozenNotes);
        std::array<std::size_t, 4> actualByData{};
        for (const ESM4::Note& note : notes)
        {
            ASSERT_LT(note.mData, actualByData.size());
            ++actualByData[note.mData];
        }
        EXPECT_EQ(actualByData, frozenByData);
    }

    TEST(Esm4NoteStoreTest, laterPluginReplacesAnEarlierNote)
    {
        MWWorld::ESMStore store;
        auto base = makeReader(makePlugin({ { 0x00000500, notePayload("Note", 1, "Base text") } }), "FalloutNV.esm", 0);
        store.loadESM4(*base, nullptr);
        auto replacement
            = makeReader(makePlugin({ { 0x00000500, notePayload("Note", 1, "Winning text") } }, "FalloutNV.esm"),
                "DeadMoney.esm", 1, { { "falloutnv.esm", 0 } });
        store.loadESM4(*replacement, nullptr);

        const auto& notes = store.get<ESM4::Note>();
        ASSERT_EQ(notes.getSize(), 1);
        const auto* note = notes.search(ESM::RefId(ESM::FormId::fromUint32(0x00000500)));
        ASSERT_NE(note, nullptr);
        EXPECT_EQ(note->mText, "Winning text");
    }

    TEST(Esm4NoteStoreTest, doesNotApplyTheFnvSchemaToAnotherGame)
    {
        auto reader
            = makeReader(makePlugin({ { 0x00000500, notePayload("WrongGame", 1, "Skipped") } }), "Skyrim.esm", 0);

        MWWorld::ESMStore store;
        store.loadESM4(*reader, nullptr);

        EXPECT_EQ(store.get<ESM4::Note>().getSize(), 0);
        EXPECT_EQ(store.getESM4Game(), MWWorld::ESM4Game::Skyrim);
    }

    TEST(Esm4NoteStoreTest, rejectsMalformedFnvNoteBeforeTypedStoreInsertion)
    {
        std::string invalid = notePayload("Invalid", 1, "Text");
        appendSubRecord(invalid, "DESC", zString("not authored"));
        auto reader = makeReader(makePlugin({ { 0x00000500, std::move(invalid) } }), "FalloutNV.esm", 0);

        MWWorld::ESMStore store;
        EXPECT_THROW(store.loadESM4(*reader, nullptr), std::runtime_error);
        EXPECT_EQ(store.get<ESM4::Note>().getSize(), 0);
    }
}
