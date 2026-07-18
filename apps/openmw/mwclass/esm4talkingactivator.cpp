#include "esm4talkingactivator.hpp"

#include <iomanip>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/actionesm4radio.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvradioprogram.hpp"
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
        const bool isRadioStation = (activator.mFlags & ESM4::TACT_RadioStation) != 0;
        MWWorld::Esm4RadioPlaybackSelection selection = MWWorld::selectEsm4RadioBroadcast(
            isRadioStation ? activator.mLoopSound : ESM::FormId{},
            isRadioStation ? activator.mRadioTemplate : ESM::FormId{}, {}, {}, {});
        if (isRadioStation && selection.mSound.isZeroOrUnset())
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            MWBase::World* world = MWBase::Environment::tryGetWorld();
            if (store != nullptr && world != nullptr
                && store->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas)
            {
                MWWorld::FnvRadioProgramPreparationError error
                    = MWWorld::FnvRadioProgramPreparationError::None;
                const std::optional<MWWorld::PreparedFnvRadioOneShot> program = MWWorld::prepareFnvRadioOneShot(
                    { store->getESM4Game(), store, &world->getESM4QuestRuntime(), &activator }, &error);
                if (program)
                {
                    selection = MWWorld::selectEsm4RadioBroadcast(
                        activator.mLoopSound, activator.mRadioTemplate, {}, {}, program->mSound);
                    Log(Debug::Info) << "FNV radio: prepared authored one-shot station="
                                     << ESM::RefId(program->mStation).toDebugString() << " quest="
                                     << ESM::RefId(program->mQuest).toDebugString() << " topic="
                                     << ESM::RefId(program->mTopic).toDebugString() << " info="
                                     << ESM::RefId(program->mInfo).toDebugString() << " sound="
                                     << ESM::RefId(program->mSound).toDebugString();
                }
                else
                {
                    Log(Debug::Warning) << "FNV radio: rejected authored one-shot station="
                                        << ESM::RefId(activator.mId).toDebugString() << " error="
                                        << MWWorld::getFnvRadioProgramPreparationErrorName(error);
                }
            }
        }
        Log(Debug::Info) << "FNV/ESM4 talking activator: activate editor=" << activator.mEditorId
                         << " form=" << ESM::RefId(activator.mId).toDebugString()
                         << " flags=0x" << std::hex << activator.mFlags << std::dec
                         << " loop=" << ESM::RefId(activator.mLoopSound).toDebugString()
                         << " template=" << ESM::RefId(activator.mRadioTemplate).toDebugString()
                         << " selected=" << ESM::RefId(selection.mSound).toDebugString();

        if (selection.mSound.isZeroOrUnset())
            return std::make_unique<MWWorld::NullAction>();

        return std::make_unique<MWWorld::ActionEsm4Radio>(
            ptr, ESM::RefId(), ESM::RefId(selection.mSound), ESM::RefId(activator.mId), std::string{},
            selection.mMode);
    }
}
