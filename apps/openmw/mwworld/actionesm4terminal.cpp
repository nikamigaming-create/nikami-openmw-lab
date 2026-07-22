#include "actionesm4terminal.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace
{
    constexpr std::size_t MaxTerminalRedraws = 16;

    class WindowTerminalSessionPresenter final : public MWWorld::TerminalSessionPresenter
    {
    public:
        int show(std::string_view message, const std::vector<std::string>& buttons) override
        {
            MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
            windowManager->interactiveMessageBox(message, buttons, true);
            return windowManager->readPressedButton();
        }
    };
}

namespace MWWorld
{
    TerminalSessionRunResult runPreparedTerminalSession(
        const PreparedTerminalSession& session, TerminalSessionPresenter& presenter)
    {
        std::vector<std::string> menuButtons;
        menuButtons.reserve(session.getMenuItems().size());
        for (const PreparedTerminalMenuItem& item : session.getMenuItems())
            menuButtons.emplace_back(item.getText());

        if (menuButtons.empty())
            return TerminalSessionRunResult::InvalidSelection;

        const std::vector<std::string> acknowledgeButton{ "#{Interface:OK}" };
        std::size_t redraws = 0;
        while (true)
        {
            const int selection = presenter.show(session.getDescription(), menuButtons);
            if (selection < 0)
                return TerminalSessionRunResult::Cancelled;
            if (static_cast<std::size_t>(selection) >= session.getMenuItems().size())
                return TerminalSessionRunResult::InvalidSelection;

            const PreparedTerminalMenuItem& item = session.getMenuItems()[selection];
            const int acknowledged = presenter.show(item.getResultText(), acknowledgeButton);
            if (acknowledged < 0)
                return TerminalSessionRunResult::Cancelled;
            if (acknowledged != 0)
                return TerminalSessionRunResult::InvalidSelection;
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
        (void)actor;
        const Ptr& target = getTarget();
        if (target.isEmpty() || target.getType() != ESM::REC_TERM4 || target.mRef->isDeleted())
        {
            Log(Debug::Warning) << "FNV/ESM4 terminal: target disappeared before read-only session target="
                                << target.toString();
            return;
        }
        WindowTerminalSessionPresenter presenter;
        const TerminalSessionRunResult result = runPreparedTerminalSession(mSession, presenter);
        Log(Debug::Info) << "FNV/ESM4 terminal: closed read-only session form="
                         << ESM::RefId(mSession.getTerminal()).toDebugString()
                         << " result=" << static_cast<int>(result);
    }
}
