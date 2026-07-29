#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/loadterm.hpp>

#include "apps/openmw/mwworld/actionesm4terminal.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvterminalruntime.hpp"

namespace
{
    ESM4::Terminal::MenuItem makeStrictMenuItem(std::string_view text, std::string_view result, std::uint8_t flags)
    {
        ESM4::Terminal::MenuItem item;
        item.mText = text;
        item.mResultText = result;
        item.mFlags = flags;
        item.mScript.scriptHeader = { 0, 0, 0, 0, 0, 1 };
        return item;
    }

    ESM4::Terminal makeStrictTerminal(ESM::FormId id, std::string_view editorId, std::string_view description,
        const std::array<std::uint8_t, 4>& data, std::string_view itemText, std::string_view resultText,
        std::uint8_t itemFlags, bool deadMoney = false)
    {
        ESM4::Terminal terminal;
        terminal.mId = id;
        terminal.mEditorId = editorId;
        terminal.mFullName = deadMoney ? "Hologram Control" : "Terminal";
        terminal.mText = description;
        terminal.mObjectBounds = deadMoney
            ? std::array<std::uint8_t, 12>{ 0xf4, 0xff, 0xee, 0xff, 0xe9, 0xff, 0x34, 0x00, 0x16, 0x00, 0x13, 0x00 }
            : std::array<std::uint8_t, 12>{ 0xee, 0xff, 0xec, 0xff, 0x00, 0x00, 0x18, 0x00, 0x14, 0x00, 0x25, 0x00 };
        terminal.mModel = deadMoney ? "terminals\\terminal01.nif" : "terminals\\terminaldesk01.nif";
        terminal.mData.mSerializedSize = 4;
        terminal.mData.mBytes = data;
        terminal.mMenuItems.push_back(makeStrictMenuItem(itemText, resultText, itemFlags));
        terminal.mResultText = resultText;
        return terminal;
    }

    ESM4::Terminal makeBaseRetailTerminal()
    {
        // FalloutNV.esm TERM 000FD76F, placed by REFR 000F515D.
        // Exact payload SHA-256:
        // 932652d3782ae88e4f35d8f83bb7766ea343cd4bd2d12fe9476ae8e23fc90417
        return makeStrictTerminal(ESM::FormId::fromUint32(0x000fd76f), "V03WilliamTerminal", "Hello Billy boy!",
            { 0x00, 0x02, 0x04, 0x00 }, "Compose Automated Blast Message", "System Offline...", 0);
    }

    ESM4::Note makeTextDisplayNote(ESM::FormId id, std::string text)
    {
        ESM4::Note note;
        note.mId = id;
        note.mEditorId = "FrozenTerminalDisplayNote";
        note.mData = 1;
        note.mText = std::move(text);
        return note;
    }

    struct FrozenMenuItem
    {
        std::string_view mText;
        std::string_view mResultText;
        std::uint8_t mFlags;
        std::optional<ESM::FormId> mDisplayNote;
    };

    ESM4::Terminal makeDisplayNoteTerminal(ESM::FormId id, std::string_view editorId, std::string_view description,
        bool deadMoney, const std::vector<FrozenMenuItem>& menuItems)
    {
        ESM4::Terminal terminal = makeStrictTerminal(id, editorId, description,
            deadMoney ? std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x08, 0x00 }
                      : std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x04, 0x00 },
            "placeholder", "placeholder result", 0, deadMoney);
        terminal.mMenuItems.clear();
        for (const FrozenMenuItem& frozenItem : menuItems)
        {
            ESM4::Terminal::MenuItem item
                = makeStrictMenuItem(frozenItem.mText, frozenItem.mResultText, frozenItem.mFlags);
            item.mDisplayNote = frozenItem.mDisplayNote;
            terminal.mMenuItems.push_back(std::move(item));
        }
        terminal.mResultText = terminal.mMenuItems.back().mResultText;
        return terminal;
    }

    ESM4::Terminal makeSingleDisplayNoteTerminal(
        ESM::FormId id, bool deadMoney, ESM::FormId displayNote, std::string_view itemText = "Read entry")
    {
        return makeDisplayNoteTerminal(id, "SyntheticSingleDisplayNoteTerminal", "Read-only terminal", deadMoney,
            { { itemText, deadMoney ? "Displaying log..." : "Opening Message...",
                static_cast<std::uint8_t>(deadMoney ? 2 : 0), displayNote } });
    }

    ESM4::Terminal makeDeadMoneyRetailTerminalC()
    {
        // DeadMoney.esm TERM 0100F2CB, placed by REFR 0100F2D6.
        // Exact payload SHA-256:
        // fa6af8709c47409a2271187170a1b30b4c5ed768e7aaeabe34c5f727cde7e45f
        return makeStrictTerminal(ESM::FormId::fromUint32(0x0100f2cb), "NVDLC01HoloCasinoTerminalCHost",
            "Sierra Madre Host Services Network\r\n", { 0x00, 0x02, 0x08, 0x00 }, "Check Hologram Status",
            "Dealer 1 Servicing Roulette Table.", 2, true);
    }

    ESM4::Terminal makeDeadMoneyRetailTerminalA()
    {
        // DeadMoney.esm TERM 0100F2CC, placed by REFR 0100F2D4.
        // Exact payload SHA-256:
        // 3527af47b0a842ddc5cd5f59adf6ac425b7180ee2efb2922355a73e9c5e437c6
        return makeStrictTerminal(ESM::FormId::fromUint32(0x0100f2cc), "NVDLC01HoloCasinoTerminalAHost",
            "Sierra Madre Host Services Protocol\r\n", { 0x00, 0x02, 0x08, 0x00 }, "Check Hologram Status",
            "Dealer 2 Servicing Blackjack Table A.", 0, true);
    }

    MWWorld::FnvTerminalSessionSource sourceFor(
        const ESM4::Terminal& terminal, const MWWorld::ESMStore* store = nullptr)
    {
        return { MWWorld::ESM4Game::FalloutNewVegas, ESM::REC_TERM4, false, &terminal, store };
    }

    std::optional<MWWorld::PreparedTerminalSession> prepare(const ESM4::Terminal& terminal,
        MWWorld::FnvTerminalPreparationError* error = nullptr, const MWWorld::ESMStore* store = nullptr)
    {
        return MWWorld::prepareFnvTerminalSession(sourceFor(terminal, store), error);
    }

    class RecordingPresenter final : public MWWorld::TerminalSessionPresenter
    {
    public:
        struct Call
        {
            std::string mMessage;
            std::vector<std::string> mButtons;
        };

        std::vector<int> mResponses;
        std::vector<Call> mCalls;
        std::size_t mNextResponse = 0;

        int show(std::string_view message, const std::vector<std::string>& buttons) override
        {
            mCalls.push_back({ std::string(message), buttons });
            if (mNextResponse < mResponses.size())
                return mResponses[mNextResponse++];
            return 0;
        }
    };
}

static_assert(!std::is_assignable_v<MWWorld::PreparedTerminalMenuItem&, const MWWorld::PreparedTerminalMenuItem&>);
static_assert(!std::is_assignable_v<MWWorld::PreparedTerminalSession&, const MWWorld::PreparedTerminalSession&>);
static_assert(!std::is_constructible_v<MWWorld::PreparedTerminalMenuItem, std::string, std::string, bool>);
static_assert(!std::is_constructible_v<MWWorld::PreparedTerminalSession, ESM::FormId, std::string,
    std::vector<MWWorld::PreparedTerminalMenuItem>>);

TEST(FnvTerminalRuntimeTest, PreparesExactlyDecodedThreeItemOfficialSubset)
{
    struct Expected
    {
        ESM4::Terminal mTerminal;
        std::string_view mDescription;
        std::string_view mItem;
        std::string_view mResult;
        bool mRedraw;
    };
    std::vector<Expected> fixtures;
    fixtures.push_back({ makeBaseRetailTerminal(), "Hello Billy boy!", "Compose Automated Blast Message",
        "System Offline...", false });
    fixtures.push_back({ makeDeadMoneyRetailTerminalC(), "Sierra Madre Host Services Network\r\n",
        "Check Hologram Status", "Dealer 1 Servicing Roulette Table.", true });
    fixtures.push_back({ makeDeadMoneyRetailTerminalA(), "Sierra Madre Host Services Protocol\r\n",
        "Check Hologram Status", "Dealer 2 Servicing Blackjack Table A.", false });

    for (const Expected& expected : fixtures)
    {
        SCOPED_TRACE(expected.mTerminal.mEditorId);
        MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::UnsupportedMenuItem;
        const std::optional<MWWorld::PreparedTerminalSession> session = prepare(expected.mTerminal, &error);
        ASSERT_TRUE(session.has_value());
        EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::None);
        EXPECT_EQ(session->getTerminal(), expected.mTerminal.mId);
        EXPECT_EQ(session->getDescription(), expected.mDescription);
        ASSERT_EQ(session->getMenuItems().size(), 1);
        EXPECT_EQ(session->getMenuItems()[0].getText(), expected.mItem);
        EXPECT_EQ(session->getMenuItems()[0].getResultText(), expected.mResult);
        EXPECT_EQ(session->getMenuItems()[0].redrawsMenu(), expected.mRedraw);
    }
}

TEST(FnvTerminalRuntimeTest, PreparesExactFrozenTextNoteOnlyNumerator)
{
    struct FrozenNote
    {
        ESM::FormId mId;
        std::string_view mPayloadSha256;
        std::string_view mTextSha256;
        std::size_t mTextBytes;
    };
    struct FrozenTerminal
    {
        ESM::FormId mId;
        ESM::FormId mPlacement;
        std::string_view mEditorId;
        std::string_view mDescription;
        std::string_view mPayloadSha256;
        bool mDeadMoney;
        std::vector<FrozenMenuItem> mMenuItems;
        std::string_view mFinalResultText;
    };

    // These raw-record and text hashes are provenance anchors from the
    // hash-verified frozen English Ultimate Edition corpus. Synthetic text is
    // used below so this unit test does not embed the copyrighted transcripts.
    const std::vector<FrozenNote> notes{
        { ESM::FormId::fromUint32(0x000fc9c1), "2a9369098b4f006059f9de8fffb19295f6d0508de8f130514a397452d1a56dcd",
            "3dfde03741c9688aca3be1e92c229a768c43fe4952cef59ea41d36cdf80d4618", 253 },
        { ESM::FormId::fromUint32(0x000fc9c2), "92da0a3e2efb9b88884f51a8cf5b97376d635ad56340ce99610d686d1fb2144e",
            "96e32566f0320639f80bb422a8d7d037599c1b3509b2914b9f74bbcf5c720e97", 362 },
        { ESM::FormId::fromUint32(0x000fc9c3), "f8adcc9ad91a77d19f5d26ffbbe8ac4f085305280aedc91517cb9349b4d7dc28",
            "0bb7d5dbeecfa2553efacf3aed0b5d0ac7c672d9018f99549262c2f6c5841408", 577 },
        { ESM::FormId::fromUint32(0x000fc9c4), "e8cfed7d487e2916d60484fd25e23cba58c54af201a2df93117d567f9f4c016b",
            "de2f5089689002df9319f4b751be7d71e114510414c38b4881ee14a70fa1cae3", 385 },
        { ESM::FormId::fromUint32(0x000fd774), "f9f0c12a42063df7764ac6388eb19475bf95715abcc6d8965798f9d771e29eb5",
            "1328e0005a2e717cce3f41e4452dd631edc409b4ebe52224a7f5bc65483d454d", 774 },
        { ESM::FormId::fromUint32(0x000fd775), "bdb1e07b65b07389bf9c033e1f10342bb842f7a27b557350fd7b1ee50e8624f2",
            "0a1625b3f5029876d25cf4bfe87783f63fcd97f6dc8ae0f24ec183974bfa7c3c", 245 },
        { ESM::FormId::fromUint32(0x000fd776), "8566d32bda84b8c52b51200082f0f8e055ef7e8d5a936f2f9ac1138334cfd673",
            "05c517fd2f4bada1e27e2d2a29267dc6b3b4ab9257a0fd5574d1b4a5e133dd23", 519 },
        { ESM::FormId::fromUint32(0x01011b02), "4b9af0dc72a44437c8782fcba83cd0b3259c0ea1322306a121c3b3c4a42db462",
            "47bf97dca884a24bdb78aa1ef0d715086bee99971bbe9d9b2353d1a4a547d113", 453 },
        { ESM::FormId::fromUint32(0x01011b03), "eb2bba61018a0203e8aa9ab321f6bed1db29136a34c4f0b414e70788d30fc135",
            "b8dd5884e7164b7a5b79d155c81aaa3227c7a459d9a627c635226208c8c84328", 1096 },
        { ESM::FormId::fromUint32(0x010121fe), "d329c9256c45ed3e677046371d4bdd105f11829f27bfe3b40c125dc9ac3c3f0b",
            "1021ce755ddfc9687c503f5f8097e2b02ea82ceeb323a4ce726bfa6aeb09e8cd", 542 },
    };

    // Exact ITXT/RNAM/ANAM/INAM order from the four official TERM payloads.
    // The last RNAM is also loadterm's compatibility mResultText mirror.
    const std::vector<FrozenTerminal> terminals{
        { ESM::FormId::fromUint32(0x000fd76b), ESM::FormId::fromUint32(0x000f5165), "V03LincolnTerminal",
            "Lincoln Davis", "77238c6a2c1019e4267b5a5b6ca7d5283d77ac0bb0421bbbf6d930cfa172333b", false,
            { { "The Water Situation", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fd774) },
                { "External Relations", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fd775) },
                { "Your Endorsement", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fd776) } },
            "Opening Message..." },
        { ESM::FormId::fromUint32(0x000fc9b9), ESM::FormId::fromUint32(0x000f5131), "V03VincentTerminal",
            "Vincent Vanmiller\r\nMaintenance Chief",
            "4219ceef79251850e93f337435fcc4a8cd9a45cf0269e73a1d9b054550119b31", false,
            { { "Water Leak", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fc9c3) },
                { "Dinner?", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fc9c4) },
                { "Re: Dinner?", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fc9c1) },
                { "Thank You!", "Opening Message...", 0, ESM::FormId::fromUint32(0x000fc9c2) } },
            "Opening Message..." },
        { ESM::FormId::fromUint32(0x010121fb), ESM::FormId::fromUint32(0x010121ff), "NVDLC01CasinoOfficeTerminal",
            "Sierra Madre Security Network\r\n", "234dc706162327391156824cb0a0a62cbb228f509a5bdfa7204c1c25453cb989",
            true, { { "Security Measure Meeting", "Displaying log...", 2, ESM::FormId::fromUint32(0x010121fe) } },
            "Displaying log..." },
        { ESM::FormId::fromUint32(0x0100f2ca), ESM::FormId::fromUint32(0x0100f2d5), "NVDLC01HoloCasinoTerminalBHost",
            "Sierra Madre Host Services Network\r\n",
            "0df91c1612c1a9025b983d3aab2dcae6656d549204c73b1b21336afd3e745537", true,
            { { "Check Hologram Status", "Cashier is currently active.", 2, std::nullopt },
                { "Security Log 1", "Displaying log...", 2, ESM::FormId::fromUint32(0x01011b03) },
                { "Security Log 2", "Displaying log...", 2, ESM::FormId::fromUint32(0x01011b02) } },
            "Displaying log..." },
    };

    MWWorld::ESMStore store;
    const auto syntheticText = [](ESM::FormId id) { return "Frozen DATA=1 text " + std::to_string(id.toUint32()); };
    for (const FrozenNote& frozen : notes)
    {
        SCOPED_TRACE(frozen.mPayloadSha256);
        EXPECT_EQ(frozen.mPayloadSha256.size(), 64);
        EXPECT_EQ(frozen.mTextSha256.size(), 64);
        EXPECT_GT(frozen.mTextBytes, 0);
        ESM4::Note note = makeTextDisplayNote(frozen.mId, syntheticText(frozen.mId));
        EXPECT_EQ(note.mData, 1);
        EXPECT_TRUE(note.mImage.empty());
        EXPECT_TRUE(note.mVoiceTopic.isZeroOrUnset());
        EXPECT_TRUE(note.mVoiceSpeaker.isZeroOrUnset());
        EXPECT_TRUE(note.mQuests.empty());
        store.overrideRecord(note);
    }

    std::size_t linkedNotes = 0;
    std::size_t preparedItems = 0;
    for (const FrozenTerminal& frozen : terminals)
    {
        SCOPED_TRACE(frozen.mPayloadSha256);
        const ESM4::Terminal terminal = makeDisplayNoteTerminal(
            frozen.mId, frozen.mEditorId, frozen.mDescription, frozen.mDeadMoney, frozen.mMenuItems);
        EXPECT_FALSE(frozen.mPlacement.isZeroOrUnset());
        EXPECT_EQ(frozen.mPayloadSha256.size(), 64);
        EXPECT_EQ(terminal.mResultText, frozen.mFinalResultText);
        MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::UnsupportedDisplayNote;
        const std::optional<MWWorld::PreparedTerminalSession> session = prepare(terminal, &error, &store);
        ASSERT_TRUE(session.has_value());
        EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::None);
        EXPECT_EQ(session->getTerminal(), frozen.mId);
        EXPECT_EQ(session->getDescription(), frozen.mDescription);
        ASSERT_EQ(session->getMenuItems().size(), frozen.mMenuItems.size());
        preparedItems += session->getMenuItems().size();

        for (std::size_t i = 0; i < frozen.mMenuItems.size(); ++i)
        {
            const FrozenMenuItem& expected = frozen.mMenuItems[i];
            const MWWorld::PreparedTerminalMenuItem& actual = session->getMenuItems()[i];
            EXPECT_EQ(actual.getText(), expected.mText);
            EXPECT_EQ(actual.redrawsMenu(), expected.mFlags == 2);
            EXPECT_EQ(actual.getDisplayNote(), expected.mDisplayNote);
            if (expected.mDisplayNote.has_value())
            {
                EXPECT_EQ(actual.getResultText(), syntheticText(*expected.mDisplayNote));
                ++linkedNotes;
            }
            else
                EXPECT_EQ(actual.getResultText(), expected.mResultText);
        }
    }

    EXPECT_EQ(notes.size(), 10);
    EXPECT_EQ(terminals.size(), 4);
    EXPECT_EQ(linkedNotes, 10);
    EXPECT_EQ(preparedItems, 11);
}

TEST(FnvTerminalRuntimeTest, RejectsMissingAndUnsupportedDisplayNotesBeforePresentation)
{
    const ESM::FormId noteId = ESM::FormId::fromUint32(0x000fd774);
    ESM4::Terminal terminal = makeSingleDisplayNoteTerminal(ESM::FormId::fromUint32(0x000fd76b), false, noteId);
    MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;

    EXPECT_FALSE(prepare(terminal, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::MissingDisplayNote);

    MWWorld::ESMStore emptyStore;
    EXPECT_FALSE(prepare(terminal, &error, &emptyStore));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::MissingDisplayNote);

    const auto expectUnsupported = [&](std::string_view label, ESM4::Note note) {
        SCOPED_TRACE(label);
        MWWorld::ESMStore store;
        store.overrideRecord(note);
        EXPECT_FALSE(prepare(terminal, &error, &store));
        EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedDisplayNote);
    };

    ESM4::Note note = makeTextDisplayNote(noteId, "");
    expectUnsupported("empty DATA=1 text", note);
    note = makeTextDisplayNote(noteId, std::string("embedded\0text", 13));
    expectUnsupported("embedded NUL text", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mData = 0;
    expectUnsupported("DATA=0", note);
    note.mData = 2;
    note.mImage = "Architecture\\Urban\\MetroMap.dds";
    expectUnsupported("DATA=2 image", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mData = 3;
    note.mVoiceTopic = ESM::FormId::fromUint32(0x000e9abb);
    note.mVoiceSpeaker = ESM::FormId::fromUint32(0x000e9ac0);
    expectUnsupported("DATA=3 voice", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mImage = "forbidden.dds";
    expectUnsupported("DATA=1 with image state", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mVoiceTopic = ESM::FormId::fromUint32(0x000e9abb);
    expectUnsupported("DATA=1 with voice state", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mQuests.push_back(ESM::FormId::fromUint32(0x00000001));
    expectUnsupported("DATA=1 with quest state", note);
    note = makeTextDisplayNote(noteId, "text");
    note.mFlags = ESM4::Rec_Deleted;
    expectUnsupported("non-live NOTE flags", note);

    MWWorld::ESMStore validStore;
    validStore.overrideRecord(makeTextDisplayNote(noteId, "Prepared text"));
    terminal.mMenuItems.push_back(makeStrictMenuItem("Later bad item", "Must not render", 0));
    terminal.mMenuItems.back().mConditions.emplace_back();
    terminal.mResultText = terminal.mMenuItems.back().mResultText;
    const std::optional<MWWorld::PreparedTerminalSession> rejected = prepare(terminal, &error, &validStore);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedMenuItem);

    RecordingPresenter presenter;
    if (rejected)
        (void)MWWorld::runPreparedTerminalSession(*rejected, presenter);
    EXPECT_TRUE(presenter.mCalls.empty());
}

TEST(FnvTerminalRuntimeTest, PresentsPreparedTextNoteThroughExistingReadOnlyPath)
{
    const ESM::FormId noteId = ESM::FormId::fromUint32(0x010121fe);
    ESM4::Note note = makeTextDisplayNote(noteId, "Security meeting transcript");
    MWWorld::ESMStore store;
    store.overrideRecord(note);
    ESM4::Terminal terminal = makeSingleDisplayNoteTerminal(ESM::FormId::fromUint32(0x010121fb), true, noteId);
    const std::optional<MWWorld::PreparedTerminalSession> session = prepare(terminal, nullptr, &store);
    ASSERT_TRUE(session.has_value());

    RecordingPresenter presenter;
    presenter.mResponses = { 0, 0, -1 };
    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, presenter), MWWorld::TerminalSessionRunResult::Cancelled);
    ASSERT_EQ(presenter.mCalls.size(), 3);
    EXPECT_EQ(presenter.mCalls[0].mMessage, "Read-only terminal");
    EXPECT_EQ(presenter.mCalls[1].mMessage, "Security meeting transcript");
    EXPECT_EQ(presenter.mCalls[2].mMessage, "Read-only terminal");
    EXPECT_EQ(note.mText, "Security meeting transcript");
    EXPECT_EQ(*terminal.mMenuItems[0].mDisplayNote, noteId);
}

TEST(FnvTerminalRuntimeTest, RejectsNonFnvMissingWrongAndDeletedTargets)
{
    const ESM4::Terminal terminal = makeBaseRetailTerminal();
    MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;

    EXPECT_FALSE(
        MWWorld::prepareFnvTerminalSession({ MWWorld::ESM4Game::Fallout3, ESM::REC_TERM4, false, &terminal }, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::NotFalloutNewVegas);

    EXPECT_FALSE(MWWorld::prepareFnvTerminalSession(
        { MWWorld::ESM4Game::FalloutNewVegas, ESM::REC_ACTI4, false, &terminal }, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::WrongTargetType);

    EXPECT_FALSE(MWWorld::prepareFnvTerminalSession(
        { MWWorld::ESM4Game::FalloutNewVegas, ESM::REC_TERM4, true, &terminal }, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::DeletedTarget);

    EXPECT_FALSE(MWWorld::prepareFnvTerminalSession(
        { MWWorld::ESM4Game::FalloutNewVegas, ESM::REC_TERM4, false, nullptr }, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::MissingTarget);
}

TEST(FnvTerminalRuntimeTest, RejectsUnknownFlagsMissingFieldsAndUnsupportedTopFields)
{
    const auto expectRejected
        = [](std::string_view label, ESM4::Terminal terminal, MWWorld::FnvTerminalPreparationError expected) {
              SCOPED_TRACE(label);
              MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
              EXPECT_FALSE(prepare(terminal, &error));
              EXPECT_EQ(error, expected);
          };

    ESM4::Terminal terminal = makeBaseRetailTerminal();
    terminal.mFlags = ESM4::Rec_Constant;
    expectRejected("unknown record flag", terminal, MWWorld::FnvTerminalPreparationError::UnsupportedRecordFlags);
    terminal = makeBaseRetailTerminal();
    terminal.mFlags = ESM4::Rec_Deleted;
    expectRejected("deleted base flag", terminal, MWWorld::FnvTerminalPreparationError::UnsupportedRecordFlags);

    const auto expectMissing = [&expectRejected](std::string_view label, auto mutate) {
        ESM4::Terminal value = makeBaseRetailTerminal();
        mutate(value);
        expectRejected(label, std::move(value), MWWorld::FnvTerminalPreparationError::MissingRequiredField);
    };
    expectMissing("id", [](ESM4::Terminal& value) { value.mId = {}; });
    expectMissing("EDID", [](ESM4::Terminal& value) { value.mEditorId.clear(); });
    expectMissing("FULL", [](ESM4::Terminal& value) { value.mFullName.clear(); });
    expectMissing("MODL", [](ESM4::Terminal& value) { value.mModel.clear(); });
    expectMissing("OBND", [](ESM4::Terminal& value) { value.mObjectBounds = {}; });
    expectMissing("DESC", [](ESM4::Terminal& value) { value.mText.clear(); });
    expectMissing("ITXT sequence", [](ESM4::Terminal& value) { value.mMenuItems.clear(); });
    expectMissing("RNAM mirror", [](ESM4::Terminal& value) { value.mResultText = "stale"; });

    const auto expectUnsupportedTop = [&expectRejected](std::string_view label, auto mutate) {
        ESM4::Terminal value = makeBaseRetailTerminal();
        mutate(value);
        expectRejected(label, std::move(value), MWWorld::FnvTerminalPreparationError::UnsupportedTopLevelField);
    };
    expectUnsupportedTop("SCRI", [](ESM4::Terminal& value) { value.mScriptId = ESM::FormId::fromUint32(0x00000001); });
    expectUnsupportedTop("SNAM", [](ESM4::Terminal& value) { value.mSound = ESM::FormId::fromUint32(0x00000002); });
    expectUnsupportedTop(
        "PNAM", [](ESM4::Terminal& value) { value.mPasswordNote = ESM::FormId::fromUint32(0x00000003); });
    expectUnsupportedTop("MODT", [](ESM4::Terminal& value) { value.mModelData.push_back(1); });
    expectUnsupportedTop("MODS", [](ESM4::Terminal& value) { value.mModelTextureSwaps.push_back(1); });
}

TEST(FnvTerminalRuntimeTest, AllowsOnlyTwoOpaqueExactDnamShapes)
{
    const std::array<std::array<std::uint8_t, 4>, 2> accepted{
        std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x04, 0x00 },
        std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x08, 0x00 },
    };
    for (const auto& shape : accepted)
    {
        ESM4::Terminal terminal = makeBaseRetailTerminal();
        terminal.mData.mBytes = shape;
        EXPECT_TRUE(prepare(terminal).has_value());
    }

    for (const std::array<std::uint8_t, 4>& shape : {
             std::array<std::uint8_t, 4>{ 0x01, 0x02, 0x04, 0x00 },
             std::array<std::uint8_t, 4>{ 0x00, 0x03, 0x04, 0x00 },
             std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x05, 0x00 },
             std::array<std::uint8_t, 4>{ 0x00, 0x02, 0x08, 0x01 },
         })
    {
        ESM4::Terminal terminal = makeBaseRetailTerminal();
        terminal.mData.mBytes = shape;
        MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
        EXPECT_FALSE(prepare(terminal, &error));
        EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedDataShape);
    }

    ESM4::Terminal wrongSize = makeBaseRetailTerminal();
    wrongSize.mData.mSerializedSize = 3;
    MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
    EXPECT_FALSE(prepare(wrongSize, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedDataShape);
}

TEST(FnvTerminalRuntimeTest, RejectsEveryUnsupportedMenuFieldAndNonemptyScriptBody)
{
    const auto expectUnsupported = [](std::string_view label, auto mutate) {
        SCOPED_TRACE(label);
        ESM4::Terminal terminal = makeBaseRetailTerminal();
        mutate(terminal.mMenuItems[0]);
        terminal.mResultText = terminal.mMenuItems.back().mResultText;
        MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
        EXPECT_FALSE(prepare(terminal, &error));
        EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedMenuItem);
    };

    expectUnsupported("empty ITXT", [](ESM4::Terminal::MenuItem& item) { item.mText.clear(); });
    expectUnsupported(
        "embedded NUL ITXT", [](ESM4::Terminal::MenuItem& item) { item.mText = std::string("hidden\0text", 11); });
    expectUnsupported("empty RNAM", [](ESM4::Terminal::MenuItem& item) { item.mResultText.clear(); });
    expectUnsupported("ANAM 1", [](ESM4::Terminal::MenuItem& item) { item.mFlags = 1; });
    expectUnsupported("ANAM 3", [](ESM4::Terminal::MenuItem& item) { item.mFlags = 3; });
    expectUnsupported(
        "TNAM", [](ESM4::Terminal::MenuItem& item) { item.mSubmenu = ESM::FormId::fromUint32(0x00000101); });
    expectUnsupported("CTDA", [](ESM4::Terminal::MenuItem& item) { item.mConditions.emplace_back(); });
    expectUnsupported("SCHR unused", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.unused = 1; });
    expectUnsupported("SCHR refs", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.refCount = 1; });
    expectUnsupported(
        "SCHR compiled size", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.compiledSize = 1; });
    expectUnsupported(
        "SCHR variables", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.variableCount = 1; });
    expectUnsupported("SCHR type", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.type = 1; });
    expectUnsupported("SCHR disabled", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptHeader.flag = 0; });
    expectUnsupported("SCDA", [](ESM4::Terminal::MenuItem& item) { item.mScript.compiledData.push_back(0); });
    expectUnsupported("SCTX", [](ESM4::Terminal::MenuItem& item) { item.mScript.scriptSource = "return"; });
    expectUnsupported("SLSD SCVR",
        [](ESM4::Terminal::MenuItem& item) { item.mScript.localVarData.push_back({ 1, 0, 0, 0, 0, 0, "local" }); });
    expectUnsupported("SCRV", [](ESM4::Terminal::MenuItem& item) { item.mScript.localRefVarIndex.push_back(1); });
    expectUnsupported("SCRO",
        [](ESM4::Terminal::MenuItem& item) { item.mScript.references.push_back(ESM::FormId::fromUint32(0x00000102)); });
    expectUnsupported("legacy SCRO mirror",
        [](ESM4::Terminal::MenuItem& item) { item.mScript.globReference = ESM::FormId::fromUint32(0x00000103); });
}

TEST(FnvTerminalRuntimeTest, RejectsExactScriptedDeadMoneyFixtureAndLaterBadItemBeforeUi)
{
    // DeadMoney.esm TERM 01003514 exact payload SHA-256:
    // 4a8ec0c85d0683734036a537aaf4841a26c97eeb2dc4f1304abb53c65d4d6f28.
    // It has top SCRI 010000D5 plus conditions and executable menu scripts.
    ESM4::Terminal scripted = makeStrictTerminal(ESM::FormId::fromUint32(0x01003514), "NVDLC01HoloVaultTerminalA",
        "Sierra Madre Security Network\r\n", { 0x00, 0x02, 0x08, 0x00 }, "Check Security Hologram Status",
        "Currently patrolling default route.", 2, true);
    scripted.mScriptId = ESM::FormId::fromUint32(0x010000d5);
    MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
    EXPECT_FALSE(prepare(scripted, &error));
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedTopLevelField);

    ESM4::Terminal laterBad = makeBaseRetailTerminal();
    laterBad.mMenuItems.push_back(makeStrictMenuItem("Looks safe", "But is not", 0));
    laterBad.mMenuItems.back().mConditions.emplace_back();
    laterBad.mResultText = laterBad.mMenuItems.back().mResultText;
    const std::optional<MWWorld::PreparedTerminalSession> rejected = prepare(laterBad, &error);
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(error, MWWorld::FnvTerminalPreparationError::UnsupportedMenuItem);

    RecordingPresenter presenter;
    if (rejected)
        (void)MWWorld::runPreparedTerminalSession(*rejected, presenter);
    EXPECT_TRUE(presenter.mCalls.empty());
}

TEST(FnvTerminalRuntimeTest, PresentsBlockingDescriptionItemAndResultInOrder)
{
    const ESM4::Terminal terminal = makeBaseRetailTerminal();
    const std::optional<MWWorld::PreparedTerminalSession> session = prepare(terminal);
    ASSERT_TRUE(session.has_value());
    RecordingPresenter presenter;
    presenter.mResponses = { 0, 0 };

    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, presenter), MWWorld::TerminalSessionRunResult::Completed);
    ASSERT_EQ(presenter.mCalls.size(), 2);
    EXPECT_EQ(presenter.mCalls[0].mMessage, "Hello Billy boy!");
    EXPECT_EQ(presenter.mCalls[0].mButtons, (std::vector<std::string>{ "Compose Automated Blast Message" }));
    EXPECT_EQ(presenter.mCalls[1].mMessage, "System Offline...");
    EXPECT_EQ(presenter.mCalls[1].mButtons, (std::vector<std::string>{ "#{Interface:OK}" }));

    // Presentation consumes only the immutable prepared copy. No record or
    // reference state is available to this runner to mutate.
    EXPECT_EQ(terminal.mMenuItems[0].mFlags, 0);
    EXPECT_EQ(terminal.mResultText, "System Offline...");
}

TEST(FnvTerminalRuntimeTest, AnamTwoRedrawsAndSeventeenthWouldBeRedrawStopsDefensively)
{
    const ESM4::Terminal terminal = makeDeadMoneyRetailTerminalC();
    const std::optional<MWWorld::PreparedTerminalSession> session = prepare(terminal);
    ASSERT_TRUE(session.has_value());
    RecordingPresenter presenter;

    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, presenter),
        MWWorld::TerminalSessionRunResult::RedrawLimitExceeded);
    // Sixteen redraws are allowed. On the seventeenth ANAM=2 selection the
    // DESC and RNAM have already been shown, but no eighteenth DESC is opened.
    ASSERT_EQ(presenter.mCalls.size(), 34);
    for (std::size_t i = 0; i < presenter.mCalls.size(); i += 2)
    {
        EXPECT_EQ(presenter.mCalls[i].mMessage, "Sierra Madre Host Services Network\r\n");
        EXPECT_EQ(presenter.mCalls[i + 1].mMessage, "Dealer 1 Servicing Roulette Table.");
    }
}

TEST(FnvTerminalRuntimeTest, CancellationAndInvalidSelectionsCloseWithoutResultOrRedraw)
{
    const std::optional<MWWorld::PreparedTerminalSession> session = prepare(makeBaseRetailTerminal());
    ASSERT_TRUE(session.has_value());

    RecordingPresenter cancelled;
    cancelled.mResponses = { -1 };
    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, cancelled), MWWorld::TerminalSessionRunResult::Cancelled);
    EXPECT_EQ(cancelled.mCalls.size(), 1);

    RecordingPresenter invalidMenu;
    invalidMenu.mResponses = { 1 };
    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, invalidMenu),
        MWWorld::TerminalSessionRunResult::InvalidSelection);
    EXPECT_EQ(invalidMenu.mCalls.size(), 1);

    RecordingPresenter invalidAcknowledge;
    invalidAcknowledge.mResponses = { 0, 1 };
    EXPECT_EQ(MWWorld::runPreparedTerminalSession(*session, invalidAcknowledge),
        MWWorld::TerminalSessionRunResult::InvalidSelection);
    EXPECT_EQ(invalidAcknowledge.mCalls.size(), 2);
}
