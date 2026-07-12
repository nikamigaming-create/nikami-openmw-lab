#include "esm4activator.hpp"

#include <iomanip>
#include <sstream>

#include <components/debug/debuglog.hpp>
#include <components/esm4/dialogue.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadtact.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/actionesm4radio.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/nullaction.hpp"

namespace MWClass
{
    namespace
    {
        std::string resolveFo3StationVoice(const ESM4::TalkingActivator& station)
        {
            const std::string stationId = Misc::StringUtils::lowerCase(station.mEditorId);
            std::string dialoguePrefix;
            if (stationId.find("galaxynews") != std::string::npos)
                dialoguePrefix = "radiognr";
            else if (stationId.find("enclave") != std::string::npos)
                dialoguePrefix = "radioenclave";
            else
                return {};

            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            const Resource::ResourceSystem* resources
                = MWBase::Environment::get().getResourceSystem();
            if (store == nullptr || resources == nullptr)
                return {};
            const VFS::Manager* vfs = resources->getVFS();
            if (vfs == nullptr)
                return {};

            for (const ESM4::Dialogue& dialogue : store->get<ESM4::Dialogue>())
            {
                const std::string dialogueId = Misc::StringUtils::lowerCase(dialogue.mEditorId);
                if (dialogue.mDialType != ESM4::DTYP_Radio || !dialogueId.starts_with(dialoguePrefix))
                    continue;
                for (const ESM4::DialogInfo& info : store->get<ESM4::DialogInfo>())
                {
                    if (info.mTopic != dialogue.mId)
                        continue;
                    for (std::size_t index = 0; index < info.mResponses.size(); ++index)
                    {
                        const ESM4::DialogResponse& response = info.mResponses[index];
                        const std::uint32_t responseNumber = response.mData.responseNo != 0
                            ? response.mData.responseNo
                            : static_cast<std::uint32_t>(index + 1);
                        std::ostringstream suffix;
                        suffix << '_' << std::hex << std::nouppercase << std::setfill('0')
                               << std::setw(8) << info.mId.mIndex << '_' << std::dec << responseNumber;
                        const std::string stem = suffix.str();
                        for (const VFS::Path::Normalized& candidate
                            : vfs->getRecursiveDirectoryIterator("sound/voice"))
                        {
                            const std::string_view value = candidate.view();
                            if (value.ends_with(stem + ".ogg") || value.ends_with(stem + ".mp3")
                                || value.ends_with(stem + ".wav"))
                            {
                                Log(Debug::Info)
                                    << "FNV/ESM4 radio: resolved authored station voice station="
                                    << station.mEditorId << " dialogue=" << dialogue.mEditorId
                                    << " info=" << ESM::RefId(info.mId).toDebugString()
                                    << " response=" << responseNumber << " path=\"" << candidate << "\"";
                                return candidate.value();
                            }
                        }
                    }
                }
            }
            return {};
        }
    }

    ESM4Activator::ESM4Activator()
        : MWWorld::RegisteredClass<ESM4Activator, ESM4Base<ESM4::Activator>>(ESM4::Activator::sRecordId)
    {
    }

    std::string_view ESM4Activator::getName(const MWWorld::ConstPtr& ptr) const
    {
        return ptr.get<ESM4::Activator>()->mBase->mFullName;
    }

    MWGui::ToolTipInfo ESM4Activator::getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const
    {
        return ESM4Impl::getToolTipInfo(getName(ptr), count);
    }

    bool ESM4Activator::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return !getName(ptr).empty();
    }

    bool ESM4Activator::isActivator() const
    {
        return true;
    }

    bool ESM4Activator::useAnim() const
    {
        return true;
    }

    std::unique_ptr<MWWorld::Action> ESM4Activator::activate(
        const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        (void)actor;
        const ESM4::Activator& activator = *ptr.get<ESM4::Activator>()->mBase;
        Log(Debug::Info) << "FNV/ESM4 activator: activate editor=" << activator.mEditorId
                         << " form=" << ESM::RefId(activator.mId).toDebugString()
                         << " loop=" << ESM::RefId(activator.mLoopingSound).toDebugString()
                         << " activation=" << ESM::RefId(activator.mActivationSound).toDebugString()
                         << " template=" << ESM::RefId(activator.mRadioTemplate).toDebugString()
                         << " station=" << ESM::RefId(activator.mRadioStation).toDebugString();
        ESM::FormId broadcast = activator.mLoopingSound;
        std::string broadcastVoice;
        if (broadcast.isZeroOrUnset())
            broadcast = activator.mRadioTemplate;

        if (broadcast.isZeroOrUnset() && !activator.mRadioStation.isZeroOrUnset())
        {
            const auto& stations
                = MWBase::Environment::get().getESMStore()->get<ESM4::TalkingActivator>();
            if (const ESM4::TalkingActivator* station = stations.search(ESM::RefId(activator.mRadioStation)))
            {
                broadcast = !station->mLoopSound.isZeroOrUnset() ? station->mLoopSound : station->mRadioTemplate;
                if (broadcast.isZeroOrUnset())
                    broadcastVoice = resolveFo3StationVoice(*station);
                Log(Debug::Info) << "FNV/ESM4 radio: resolved station activator=" << activator.mEditorId
                                 << " station=" << station->mEditorId << " stationForm="
                                 << ESM::RefId(station->mId).toDebugString() << " template="
                                 << ESM::RefId(broadcast).toDebugString() << " voice=\""
                                 << broadcastVoice << "\"";
            }
        }

        if (activator.mActivationSound.isZeroOrUnset() && broadcast.isZeroOrUnset()
            && broadcastVoice.empty())
            return std::make_unique<MWWorld::NullAction>();

        return std::make_unique<MWWorld::ActionEsm4Radio>(ptr, ESM::RefId(activator.mActivationSound),
            ESM::RefId(broadcast), ESM::RefId(activator.mRadioStation), std::move(broadcastVoice));
    }
}
