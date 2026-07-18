#include "esm4talkingactivator.hpp"

#include <iomanip>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>

#include "../mwworld/actionesm4radio.hpp"
#include "../mwworld/nullaction.hpp"

namespace MWClass
{
    ESM::FormId selectFnvTalkingActivatorSound(const ESM4::TalkingActivator& activator)
    {
        if ((activator.mFlags & ESM4::TACT_RadioStation) == 0)
            return {};

        // FNV authors the station loop in SNAM. INAM is the radio-template sound and is a deterministic fallback.
        return !activator.mLoopSound.isZeroOrUnset() ? activator.mLoopSound : activator.mRadioTemplate;
    }

    ESM4TalkingActivator::ESM4TalkingActivator()
        : MWWorld::RegisteredClass<ESM4TalkingActivator, ESM4Base<ESM4::TalkingActivator>>(
            ESM4::TalkingActivator::sRecordId)
    {
    }

    std::string_view ESM4TalkingActivator::getName(const MWWorld::ConstPtr& ptr) const
    {
        return ptr.get<ESM4::TalkingActivator>()->mBase->mFullName;
    }

    MWGui::ToolTipInfo ESM4TalkingActivator::getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const
    {
        return ESM4Impl::getToolTipInfo(getName(ptr), count);
    }

    bool ESM4TalkingActivator::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return !getName(ptr).empty();
    }

    bool ESM4TalkingActivator::isActivator() const
    {
        return true;
    }

    bool ESM4TalkingActivator::useAnim() const
    {
        return true;
    }

    std::unique_ptr<MWWorld::Action> ESM4TalkingActivator::activate(
        const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        (void)actor;
        const ESM4::TalkingActivator& activator = *ptr.get<ESM4::TalkingActivator>()->mBase;
        const ESM::FormId broadcast = selectFnvTalkingActivatorSound(activator);
        Log(Debug::Info) << "FNV/ESM4 talking activator: activate editor=" << activator.mEditorId
                         << " form=" << ESM::RefId(activator.mId).toDebugString()
                         << " flags=0x" << std::hex << activator.mFlags << std::dec
                         << " loop=" << ESM::RefId(activator.mLoopSound).toDebugString()
                         << " template=" << ESM::RefId(activator.mRadioTemplate).toDebugString()
                         << " selected=" << ESM::RefId(broadcast).toDebugString();

        if (broadcast.isZeroOrUnset())
            return std::make_unique<MWWorld::NullAction>();

        return std::make_unique<MWWorld::ActionEsm4Radio>(
            ptr, ESM::RefId(), ESM::RefId(broadcast), ESM::RefId(activator.mId));
    }
}
