#include "esm4terminal.hpp"

#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/actionesm4terminal.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/nullaction.hpp"

namespace MWClass
{
    ESM4Terminal::ESM4Terminal()
        : MWWorld::RegisteredClass<ESM4Terminal, ESM4Base<ESM4::Terminal>>(ESM4::Terminal::sRecordId)
    {
    }

    std::string_view ESM4Terminal::getName(const MWWorld::ConstPtr& ptr) const
    {
        return ptr.get<ESM4::Terminal>()->mBase->mFullName;
    }

    MWGui::ToolTipInfo ESM4Terminal::getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const
    {
        return ESM4Impl::getToolTipInfo(getName(ptr), count);
    }

    bool ESM4Terminal::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return !getName(ptr).empty();
    }

    bool ESM4Terminal::isActivator() const
    {
        return true;
    }

    bool ESM4Terminal::useAnim() const
    {
        return true;
    }

    std::unique_ptr<MWWorld::Action> ESM4Terminal::activate(const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        (void)actor;
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const bool hasTarget = !ptr.isEmpty();
        const bool isTerminal = hasTarget && ptr.getType() == ESM::REC_TERM4;
        const MWWorld::LiveCellRef<ESM4::Terminal>* live = isTerminal ? ptr.get<ESM4::Terminal>() : nullptr;
        const MWWorld::FnvTerminalSessionSource source{ store.getESM4Game(), hasTarget ? ptr.getType() : 0u,
            hasTarget && ptr.mRef->isDeleted(), live != nullptr ? live->mBase : nullptr };

        MWWorld::FnvTerminalPreparationError error = MWWorld::FnvTerminalPreparationError::None;
        std::optional<MWWorld::PreparedTerminalSession> session = MWWorld::prepareFnvTerminalSession(source, &error);
        if (!session)
        {
            Log(Debug::Warning) << "FNV/ESM4 terminal: rejected activation target=" << ptr.toString()
                                << " reason=" << MWWorld::getFnvTerminalPreparationErrorName(error);
            return std::make_unique<MWWorld::NullAction>();
        }

        Log(Debug::Info) << "FNV/ESM4 terminal: prepared read-only session editor=" << live->mBase->mEditorId
                         << " form=" << ESM::RefId(live->mBase->mId).toDebugString()
                         << " items=" << session->getMenuItems().size();
        return std::make_unique<MWWorld::ActionEsm4Terminal>(ptr, std::move(*session));
    }
}
