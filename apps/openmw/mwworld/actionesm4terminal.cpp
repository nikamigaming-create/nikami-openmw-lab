#include "actionesm4terminal.hpp"

#include <cstddef>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/fnvmenuxml.hpp"

#include "esm4questruntime.hpp"
#include "esmstore.hpp"
#include "class.hpp"
#include "containerstore.hpp"
#include "fnvplayerruntimestate.hpp"
#include "fnvterminalhacking.hpp"

namespace
{
    constexpr std::size_t MaxTerminalRedraws = 16;
    constexpr std::size_t MaxTerminalPageChanges = 128;

    enum class LiveHackingResult
    {
        Cancelled,
        Solved,
        LockedOut,
        Unavailable,
    };

    class TerminalPresentationScope
    {
        MWBase::WindowManager* mWindowManager;

    public:
        explicit TerminalPresentationScope(const MWWorld::Ptr& target)
            : mWindowManager(MWBase::Environment::get().getWindowManager())
        {
            mWindowManager->beginFalloutTerminalSession(target);
        }

        ~TerminalPresentationScope() { mWindowManager->endFalloutTerminalSession(); }
    };

    class WindowTerminalSessionPresenter final : public MWWorld::TerminalSessionPresenter
    {
        const MWGui::FnvMenuXmlDocument& mMenu;

    public:
        explicit WindowTerminalSessionPresenter(const MWGui::FnvMenuXmlDocument& menu)
            : mMenu(menu)
        {
        }

        int show(std::string_view message, const std::vector<std::string>& buttons) override
        {
            MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
            windowManager->interactiveFnvMenuMessageBox(mMenu, "computers_depth_rect", "computers_welcome",
                "computers_file_directory", message, buttons, true);
            return windowManager->readPressedButton();
        }
    };

    class LiveTerminalSessionRuntime final : public MWWorld::TerminalSessionRuntime
    {
    public:
        bool conditionsPass(const std::vector<ESM4::TargetCondition>& conditions) override
        {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().evaluateConditions(conditions);
        }

        void addNote(ESM::FormId note) override
        {
            MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState().addNote(note);
        }

        void executeResultScript(const ESM4::ScriptDefinition& script) override
        {
            if (!MWBase::Environment::get().getWorld()->getESM4QuestRuntime().executeResultScript(script))
                Log(Debug::Warning) << "FNV/ESM4 terminal: result script could not execute transactionally";
        }

        std::optional<MWWorld::PreparedTerminalSession> prepareSubmenu(ESM::FormId terminal) override
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM4::Terminal* record = store.get<ESM4::Terminal>().search(ESM::RefId(terminal));
            if (record == nullptr)
                return std::nullopt;

            MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
            std::optional<MWWorld::PreparedTerminalSession> result = MWWorld::prepareFnvTerminalSession(
                { store.getESM4Game(), ESM::REC_TERM4, false, record, &store }, &error);
            if (!result)
            {
                Log(Debug::Warning) << "FNV/ESM4 terminal: rejected submenu form="
                                    << ESM::RefId(terminal).toDebugString() << " reason="
                                    << MWWorld::getFnvTerminalPreparationErrorName(error);
            }
            return result;
        }
    };

    void showTerminalNotice(std::string_view message)
    {
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        windowManager->interactiveMessageBox(message, { "#{Interface:OK}" }, true);
        windowManager->readPressedButton();
    }

    std::optional<ESM::FormId> findFnvPerk(std::string_view editorId)
    {
        const auto& perks = MWBase::Environment::get().getESMStore()->get<ESM4::Perk>();
        for (const ESM4::Perk& perk : perks)
        {
            if (perk.mEditorId == editorId)
                return perk.mId;
        }
        return std::nullopt;
    }

    std::optional<MWGui::FnvMenuXmlDocument> loadAuthoredTerminalMenu(std::string_view path)
    {
        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::None;
        std::optional<MWGui::FnvMenuXmlDocument> result = MWGui::loadFnvMenuXml(*vfs, path, &error);
        if (!result)
            Log(Debug::Error) << "FNV/ESM4 terminal: authored menu failed to load path=" << path
                              << " reason=" << MWGui::getFnvMenuXmlParseErrorName(error);
        return result;
    }

    LiveHackingResult runLiveHacking(
        MWWorld::FnvTerminalDifficulty difficulty, ESM::FormId placement, float science)
    {
        const std::optional<MWGui::FnvMenuXmlDocument> menu = loadAuthoredTerminalMenu("menus/hacking_menu.xml");
        if (!menu)
            return LiveHackingResult::Unavailable;
        const MWWorld::Store<ESM::GameSetting>& settings
            = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();
        const ESM::GameSetting* minimumWordsSetting = settings.search("iHackingMinWords");
        const ESM::GameSetting* maximumWordsSetting = settings.search("iHackingMaxWords");
        const ESM::GameSetting* levelMultiplierSetting = settings.search("fHackLevelMult");
        if (minimumWordsSetting == nullptr || maximumWordsSetting == nullptr || levelMultiplierSetting == nullptr)
        {
            Log(Debug::Error) << "FNV/ESM4 terminal: required hacking GMST records are unavailable";
            return LiveHackingResult::Unavailable;
        }
        const std::size_t wordCount = MWWorld::getFnvTerminalWordCount(science, difficulty,
            minimumWordsSetting->mValue.getInteger(), maximumWordsSetting->mValue.getInteger(),
            levelMultiplierSetting->mValue.getFloat());
        if (wordCount < 2)
        {
            Log(Debug::Error) << "FNV/ESM4 terminal: authored hacking GMST values produced an invalid word count";
            return LiveHackingResult::Unavailable;
        }

        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const Files::IStreamPtr stream = vfs->find(VFS::Path::NormalizedView("menus/falloutdict.txt"));
        if (!stream)
        {
            Log(Debug::Error) << "FNV/ESM4 terminal: authored menus/falloutdict.txt is unavailable";
            return LiveHackingResult::Unavailable;
        }
        const std::string contents{ std::istreambuf_iterator<char>(*stream), {} };
        const std::vector<std::string> dictionary = MWWorld::parseFnvTerminalDictionary(contents);
        std::vector<std::string> words = MWWorld::buildFnvTerminalWordSet(
            dictionary, difficulty, placement.mIndex ^ (static_cast<std::uint32_t>(placement.mContentFile) << 24),
            wordCount);
        if (words.size() < 2)
        {
            Log(Debug::Error) << "FNV/ESM4 terminal: authored dictionary has no viable word set difficulty="
                              << static_cast<int>(difficulty);
            return LiveHackingResult::Unavailable;
        }

        const std::uint32_t boardSeed
            = placement.mIndex ^ (static_cast<std::uint32_t>(placement.mContentFile) << 24) ^ 0x9e3779b9u;
        const MWWorld::FnvTerminalBoard board = MWWorld::buildFnvTerminalHackingBoard(words, boardSeed);
        if (!board.isValid())
        {
            Log(Debug::Error) << "FNV/ESM4 terminal: failed to build authored hacking board geometry";
            return LiveHackingResult::Unavailable;
        }
        const std::size_t answer = (placement.mIndex * 2654435761u) % words.size();
        MWWorld::FnvTerminalHackingSession session(std::move(words), answer);
        std::vector<bool> consumedTargets(board.mTargets.size(), false);
        std::string originalMemory;
        originalMemory.reserve(MWWorld::FnvTerminalBoardColumnCount * MWWorld::FnvTerminalBoardRowCount
            * MWWorld::FnvTerminalBoardDataCharactersPerRow);
        for (const std::string& row : board.mRows)
            originalMemory += row.substr(7);
        std::string log = ">";
        std::uint32_t interactionSeed = boardSeed;
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        while (!session.isSolved() && !session.isLockedOut())
        {
            std::ostringstream prompt;
            prompt << windowManager->getGameSettingString(
                          "sHackingHeader", "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL")
                   << '\n' << windowManager->getGameSettingString("sHackingHeader2", "ENTER PASSWORD NOW") << '\n'
                   << windowManager->getGameSettingString("sHackingHeader3", "ATTEMPT(S) LEFT:") << ' ';
            for (int attempt = 0; attempt < session.getAttemptsRemaining(); ++attempt)
                prompt << "[] ";

            MWGui::FnvHackingMenuPresentation presentation;
            presentation.mRows = board.mRows;
            presentation.mLog = log;
            std::vector<std::size_t> targetMap;
            const auto eraseMemoryRange = [&](std::size_t begin, std::size_t end) {
                constexpr std::size_t columnCapacity = MWWorld::FnvTerminalBoardRowCount
                    * MWWorld::FnvTerminalBoardDataCharactersPerRow;
                for (std::size_t index = begin; index <= end; ++index)
                {
                    const std::size_t column = index / columnCapacity;
                    const std::size_t inColumn = index % columnCapacity;
                    const std::size_t row = inColumn / MWWorld::FnvTerminalBoardDataCharactersPerRow;
                    const std::size_t character = inColumn % MWWorld::FnvTerminalBoardDataCharactersPerRow;
                    presentation.mRows[column * MWWorld::FnvTerminalBoardRowCount + row][7 + character] = '.';
                }
            };
            for (const MWWorld::FnvTerminalBoardWord& placed : board.mWords)
            {
                if (!session.isWordActive(placed.mWord))
                    eraseMemoryRange(placed.mBegin, placed.mEnd);
            }
            for (std::size_t index = 0; index < board.mTargets.size(); ++index)
            {
                const MWWorld::FnvTerminalBoardTarget& target = board.mTargets[index];
                if (consumedTargets[index])
                {
                    eraseMemoryRange(target.mBegin, target.mEnd);
                    continue;
                }
                if (target.mKind == MWWorld::FnvTerminalBoardTargetKind::Word
                    && !session.isWordActive(target.mWord))
                    continue;
                presentation.mTargets.push_back({ target.mBegin, target.mEnd });
                targetMap.push_back(index);
            }
            windowManager->interactiveFnvMenuMessageBox(*menu, "hacking_depth_rect", "hacking_header_rect",
                "hacking_password_file_rect", prompt.str(), {}, true, -1, &presentation);
            const int selected = windowManager->readPressedButton();
            if (selected < 0 || static_cast<std::size_t>(selected) >= targetMap.size())
                return LiveHackingResult::Cancelled;
            const std::size_t boardTargetIndex = targetMap[static_cast<std::size_t>(selected)];
            const MWWorld::FnvTerminalBoardTarget& target = board.mTargets[boardTargetIndex];
            const std::string selectedText
                = originalMemory.substr(target.mBegin, target.mEnd - target.mBegin + 1);
            log += selectedText;
            log.push_back('\n');
            if (target.mKind == MWWorld::FnvTerminalBoardTargetKind::Bracket)
            {
                const MWWorld::FnvTerminalBracketOutcome outcome
                    = session.activateBracket(interactionSeed++);
                if (outcome.mResult == MWWorld::FnvTerminalBracketResult::Invalid)
                    return LiveHackingResult::Unavailable;
                for (std::size_t index = 0; index < board.mTargets.size(); ++index)
                {
                    const auto& candidate = board.mTargets[index];
                    if (candidate.mKind == MWWorld::FnvTerminalBoardTargetKind::Bracket
                        && candidate.mBegin <= target.mEnd && target.mBegin <= candidate.mEnd)
                        consumedTargets[index] = true;
                }
                if (outcome.mResult == MWWorld::FnvTerminalBracketResult::DudRemoved)
                    log += windowManager->getGameSettingString("sHackingDudRemoved", "Dud removed.");
                else
                {
                    log += windowManager->getGameSettingString("sHackingToleranceReset1", "Allowance");
                    log.push_back(' ');
                    log += windowManager->getGameSettingString("sHackingToleranceReset2", "replenished.");
                }
                log += "\n>";
                continue;
            }

            const MWWorld::FnvTerminalGuessOutcome outcome = session.guess(target.mWord);
            switch (outcome.mResult)
            {
                case MWWorld::FnvTerminalGuessResult::Correct:
                    showTerminalNotice(std::string(windowManager->getGameSettingString(
                        "sHackingGranted", "Exact match!")));
                    return LiveHackingResult::Solved;
                case MWWorld::FnvTerminalGuessResult::Incorrect:
                {
                    log += windowManager->getGameSettingString("sHackingDenied", "Entry denied");
                    log.push_back('\n');
                    log += std::to_string(outcome.mLikeness);
                    log.push_back('/');
                    log += std::to_string(session.getWords()[0].size());
                    log.push_back(' ');
                    log += windowManager->getGameSettingString("sHackingCorrect", "correct");
                    log += "\n>";
                    break;
                }
                case MWWorld::FnvTerminalGuessResult::LockedOut:
                    showTerminalNotice("TERMINAL LOCKED\nPLEASE CONTACT AN ADMINISTRATOR.");
                    return LiveHackingResult::LockedOut;
                case MWWorld::FnvTerminalGuessResult::Invalid:
                    return LiveHackingResult::Unavailable;
            }
        }
        return session.isSolved() ? LiveHackingResult::Solved : LiveHackingResult::LockedOut;
    }
}

namespace MWWorld
{
    TerminalSessionRunResult runPreparedTerminalSession(
        const PreparedTerminalSession& session, TerminalSessionPresenter& presenter, TerminalSessionRuntime* runtime)
    {
        const std::vector<std::string> acknowledgeButton{ "#{Interface:OK}" };
        const PreparedTerminalSession* page = &session;
        std::optional<PreparedTerminalSession> ownedPage;
        std::size_t redraws = 0;
        std::size_t pageChanges = 0;
        while (true)
        {
            std::vector<const PreparedTerminalMenuItem*> visibleItems;
            std::vector<std::string> menuButtons;
            visibleItems.reserve(page->getMenuItems().size());
            menuButtons.reserve(page->getMenuItems().size());
            for (const PreparedTerminalMenuItem& item : page->getMenuItems())
            {
                if (!item.getConditions().empty()
                    && (runtime == nullptr || !runtime->conditionsPass(item.getConditions())))
                    continue;
                visibleItems.push_back(&item);
                menuButtons.emplace_back(item.getText());
            }

            if (menuButtons.empty())
                return TerminalSessionRunResult::InvalidSelection;

            const int selection = presenter.show(page->getDescription(), menuButtons);
            if (selection < 0)
                return TerminalSessionRunResult::Cancelled;
            if (static_cast<std::size_t>(selection) >= visibleItems.size())
                return TerminalSessionRunResult::InvalidSelection;

            const PreparedTerminalMenuItem& item = *visibleItems[selection];
            if (item.getSubmenu().has_value())
            {
                if (runtime == nullptr)
                    return TerminalSessionRunResult::MissingSubmenu;
                if (pageChanges == MaxTerminalPageChanges)
                    return TerminalSessionRunResult::SubmenuLimitExceeded;
                ++pageChanges;
                std::optional<PreparedTerminalSession> nextPage = runtime->prepareSubmenu(*item.getSubmenu());
                if (!nextPage)
                    return TerminalSessionRunResult::MissingSubmenu;
                ownedPage.emplace(std::move(*nextPage));
                page = &*ownedPage;
                continue;
            }

            if ((item.getFlags() & 1u) != 0 && item.getDisplayNote().has_value() && runtime != nullptr)
                runtime->addNote(*item.getDisplayNote());

            if (runtime != nullptr)
                runtime->executeResultScript(item.getScript());

            if (!item.getResultText().empty())
            {
                const int acknowledged = presenter.show(item.getResultText(), acknowledgeButton);
                if (acknowledged < 0)
                    return TerminalSessionRunResult::Cancelled;
                if (acknowledged != 0)
                    return TerminalSessionRunResult::InvalidSelection;
            }
            if (!item.redrawsMenu())
                return TerminalSessionRunResult::Completed;

            if (redraws == MaxTerminalRedraws)
                return TerminalSessionRunResult::RedrawLimitExceeded;
            ++redraws;
        }
    }

    ActionEsm4Terminal::ActionEsm4Terminal(const Ptr& target, PreparedTerminalSession session)
        : Action(false, target)
        , mSession(std::move(session))
    {
    }

    void ActionEsm4Terminal::executeImp(const Ptr& actor)
    {
        const Ptr& target = getTarget();
        if (actor.isEmpty() || target.isEmpty() || target.getType() != ESM::REC_TERM4 || target.mRef->isDeleted())
        {
            Log(Debug::Warning) << "FNV/ESM4 terminal: target disappeared before read-only session target="
                                << target.toString();
            return;
        }

        const ESM::FormId placement = target.getCellRef().getRefNum();
        if (!placement.hasContentFile())
        {
            Log(Debug::Warning) << "FNV/ESM4 terminal: activation has no stable placement " << target.toString();
            return;
        }

        const TerminalPresentationScope presentation(target);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        FalloutPlayerRuntimeState& playerState = world->getFalloutPlayerRuntimeState();
        const FalloutTerminalState savedState = playerState.getTerminalState(placement);
        const std::optional<FalloutRuntimeActorValue> science
            = playerState.getCurrentActorValue(FnvTerminalScienceActorValue);
        const bool hasPassword = !mSession.getPasswordNote().isZeroOrUnset()
            && actor.getClass().getContainerStore(actor).count(ESM::RefId(mSession.getPasswordNote())) > 0;
        const FnvTerminalDifficulty difficulty = mSession.getData().mBytes[0]
                <= static_cast<std::uint8_t>(FnvTerminalDifficulty::RequiresKey)
            ? static_cast<FnvTerminalDifficulty>(mSession.getData().mBytes[0])
            : FnvTerminalDifficulty::RequiresKey;
        float requiredScience = 255.f;
        if (difficulty != FnvTerminalDifficulty::RequiresKey)
        {
            const std::string_view settingId = getFnvTerminalMinimumScienceGameSetting(difficulty);
            const ESM::GameSetting* setting = world->getStore().get<ESM::GameSetting>().search(settingId);
            if (setting == nullptr)
            {
                Log(Debug::Error) << "FNV/ESM4 terminal: missing hacking skill setting=" << settingId;
                return;
            }
            requiredScience = setting->mValue.getFloat();
        }
        const std::optional<ESM::FormId> computerWhiz = findFnvPerk("ComputerWhiz");
        const bool hasComputerWhiz = computerWhiz && playerState.hasPerk(*computerWhiz);
        const FnvTerminalAccessDecision access = resolveFnvTerminalAccess({ mSession.getData(),
            science ? science->mValue : 0.f, requiredScience, hasPassword, target.getCellRef().getLockLevel() < 0,
            hasFalloutTerminalState(savedState, FalloutTerminalState::Hacked),
            hasFalloutTerminalState(savedState, FalloutTerminalState::LockedOut), hasComputerWhiz,
            hasFalloutTerminalState(savedState, FalloutTerminalState::ComputerWhizRetryConsumed) });

        switch (access.mResult)
        {
            case FnvTerminalAccessResult::NeedsHacking:
            case FnvTerminalAccessResult::ComputerWhizRetry:
            {
                const bool consumingComputerWhiz = access.mResult == FnvTerminalAccessResult::ComputerWhizRetry;
                if (consumingComputerWhiz)
                    playerState.setTerminalState(placement, FalloutTerminalState::ComputerWhizRetryConsumed);
                Log(Debug::Info) << "FNV/ESM4 terminal: hacking begin placement=" << ESM::RefId(placement)
                                 << " difficulty=" << static_cast<int>(access.mDifficulty);
                const LiveHackingResult hacking
                    = runLiveHacking(access.mDifficulty, placement, science ? science->mValue : 0.f);
                Log(Debug::Info) << "FNV/ESM4 terminal: hacking complete placement=" << ESM::RefId(placement)
                                 << " result=" << static_cast<int>(hacking);
                if (hacking == LiveHackingResult::LockedOut)
                {
                    playerState.setTerminalState(placement, consumingComputerWhiz
                            || hasFalloutTerminalState(savedState, FalloutTerminalState::ComputerWhizRetryConsumed)
                        ? FalloutTerminalState::LockedOut | FalloutTerminalState::ComputerWhizRetryConsumed
                        : FalloutTerminalState::LockedOut);
                    return;
                }
                if (hacking != LiveHackingResult::Solved)
                    return;
                playerState.setTerminalState(placement, FalloutTerminalState::Hacked);
                const std::string_view rewardSettingId = getFnvTerminalXpRewardGameSetting(access.mDifficulty);
                const ESM::GameSetting* rewardSetting
                    = world->getStore().get<ESM::GameSetting>().search(rewardSettingId);
                if (rewardSetting == nullptr
                    || playerState.modCurrentActorValue(FalloutPlayerRuntimeState::ExperienceActorValue,
                           static_cast<float>(rewardSetting->mValue.getInteger()))
                        != FalloutActorValueMutationResult::Applied)
                    Log(Debug::Warning) << "FNV/ESM4 terminal: failed to apply authored hack XP reward setting="
                                        << rewardSettingId;
                break;
            }
            case FnvTerminalAccessResult::InsufficientScience:
            {
                std::ostringstream message;
                message << "SCIENCE SKILL OF " << static_cast<int>(access.mRequiredScience) << " REQUIRED.";
                showTerminalNotice(message.str());
                return;
            }
            case FnvTerminalAccessResult::RequiresKey:
                showTerminalNotice("A PASSWORD IS REQUIRED TO ACCESS THIS TERMINAL.");
                return;
            case FnvTerminalAccessResult::LockedOut:
                showTerminalNotice("TERMINAL LOCKED\nPLEASE CONTACT AN ADMINISTRATOR.");
                return;
            case FnvTerminalAccessResult::InvalidData:
                Log(Debug::Warning) << "FNV/ESM4 terminal: invalid authored access data form="
                                    << ESM::RefId(mSession.getTerminal()).toDebugString();
                return;
            case FnvTerminalAccessResult::Open:
            case FnvTerminalAccessResult::PasswordAccepted:
                break;
        }

        const std::optional<MWGui::FnvMenuXmlDocument> computersMenu
            = loadAuthoredTerminalMenu("menus/computers_menu.xml");
        if (!computersMenu)
            return;

        if (!mSession.getSound().isZeroOrUnset())
            MWBase::Environment::get().getSoundManager()->playSound(
                ESM::RefId(mSession.getSound()), 1.f, 1.f);

        WindowTerminalSessionPresenter presenter(*computersMenu);
        LiveTerminalSessionRuntime runtime;
        const TerminalSessionRunResult result = runPreparedTerminalSession(mSession, presenter, &runtime);
        Log(Debug::Info) << "FNV/ESM4 terminal: closed session form="
                         << ESM::RefId(mSession.getTerminal()).toDebugString()
                         << " result=" << static_cast<int>(result);
    }
}
