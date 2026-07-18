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
#include "../mwbase/world.hpp"
#include "../mwworld/actionesm4radio.hpp"
#include "../mwworld/actionfnvcrafting.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvcraftingruntime.hpp"
#include "../mwworld/fnvradioprogram.hpp"
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
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store != nullptr && store->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas
            && MWWorld::getFnvCraftingStationCategory(activator))
        {
            MWWorld::FnvCraftingPreparationError error = MWWorld::FnvCraftingPreparationError::None;
            std::optional<MWWorld::PreparedFnvCraftingCatalog> catalog
                = MWWorld::prepareFnvCraftingCatalog({ store->getESM4Game(), store, &activator }, &error);
            if (catalog)
                return std::make_unique<MWWorld::ActionFnvCraftingStation>(ptr, std::move(*catalog));
            Log(Debug::Warning) << "FNV crafting: station catalog rejected form="
                                << ESM::RefId(activator.mId).toDebugString()
                                << " error=" << MWWorld::getFnvCraftingPreparationErrorName(error);
        }
        Log(Debug::Info) << "FNV/ESM4 activator: activate editor=" << activator.mEditorId
                         << " form=" << ESM::RefId(activator.mId).toDebugString()
                         << " loop=" << ESM::RefId(activator.mLoopingSound).toDebugString()
                         << " activation=" << ESM::RefId(activator.mActivationSound).toDebugString()
                         << " template=" << ESM::RefId(activator.mRadioTemplate).toDebugString()
                         << " station=" << ESM::RefId(activator.mRadioStation).toDebugString();
        std::string broadcastVoice;
        const ESM4::TalkingActivator* station = nullptr;
        if (!activator.mRadioStation.isZeroOrUnset() && store != nullptr)
            station = store->get<ESM4::TalkingActivator>().search(ESM::RefId(activator.mRadioStation));

        MWWorld::Esm4RadioPlaybackSelection selection = MWWorld::selectEsm4RadioBroadcast(
            activator.mLoopingSound, activator.mRadioTemplate, station != nullptr ? station->mLoopSound : ESM::FormId{},
            station != nullptr ? station->mRadioTemplate : ESM::FormId{}, {});

        if (selection.mSound.isZeroOrUnset() && station != nullptr)
        {
            broadcastVoice = resolveFo3StationVoice(*station);
            if (broadcastVoice.empty() && store->getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas)
            {
                if (MWBase::World* world = MWBase::Environment::tryGetWorld())
                {
                    MWWorld::FnvRadioProgramPreparationError error
                        = MWWorld::FnvRadioProgramPreparationError::None;
                    const std::optional<MWWorld::PreparedFnvRadioOneShot> program = MWWorld::prepareFnvRadioOneShot(
                        { store->getESM4Game(), store, &world->getESM4QuestRuntime(), station }, &error);
                    if (program)
                    {
                        selection = MWWorld::selectEsm4RadioBroadcast(activator.mLoopingSound,
                            activator.mRadioTemplate, station->mLoopSound, station->mRadioTemplate, program->mSound);
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
                                            << ESM::RefId(station->mId).toDebugString() << " error="
                                            << MWWorld::getFnvRadioProgramPreparationErrorName(error);
                    }
                }
                else
                {
                    Log(Debug::Warning) << "FNV radio: no world quest runtime station="
                                        << ESM::RefId(station->mId).toDebugString();
                }
            }
            Log(Debug::Info) << "FNV/ESM4 radio: resolved station activator=" << activator.mEditorId
                             << " station=" << station->mEditorId << " stationForm="
                             << ESM::RefId(station->mId).toDebugString() << " template="
                             << ESM::RefId(selection.mSound).toDebugString() << " voice=\""
                             << broadcastVoice << "\"";
        }

        if (activator.mActivationSound.isZeroOrUnset() && selection.mSound.isZeroOrUnset()
            && broadcastVoice.empty())
            return std::make_unique<MWWorld::NullAction>();

        return std::make_unique<MWWorld::ActionEsm4Radio>(ptr, ESM::RefId(activator.mActivationSound),
            ESM::RefId(selection.mSound), ESM::RefId(activator.mRadioStation), std::move(broadcastVoice),
            selection.mMode);
    }
}
