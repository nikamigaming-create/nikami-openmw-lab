#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm4/common.hpp>
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

    MWWorld::FnvTerminalSessionSource sourceFor(const ESM4::Terminal& terminal)
    {
        return { MWWorld::ESM4Game::FalloutNewVegas, ESM::REC_TERM4, false, &terminal };
    }

    std::optional<MWWorld::PreparedTerminalSession> prepare(
        const ESM4::Terminal& terminal, MWWorld::FnvTerminalPreparationError* error = nullptr)
    {
        return MWWorld::prepareFnvTerminalSession(sourceFor(terminal), error);
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
        "INAM", [](ESM4::Terminal::MenuItem& item) { item.mDisplayNote = ESM::FormId::fromUint32(0x00000100); });
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
