#include "obscriptbindings.hpp"

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sol/state_view.hpp>
#include <sol/table.hpp>
#include <sol/usertype.hpp>

#include <components/esm/refid.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/debug/debuglog.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/esm4questruntime.hpp"
#include "../mwworld/globalvariablename.hpp"
#include "../mwworld/store.hpp"

#include "context.hpp"
#include "luamanagerimp.hpp"
#include "object.hpp"

namespace sol
{
    template <>
    struct is_automagical<ESM4::Script> : std::false_type
    {
    };

    template <>
    struct is_automagical<MWWorld::Store<ESM4::Script>> : std::false_type
    {
    };
}

namespace MWLua
{
    sol::table initCoreObScriptBindings(const Context& context)
    {
        sol::state_view lua = context.sol();
        sol::table api(lua, sol::create);

        auto reportedUnsupportedCommands = std::make_shared<std::set<std::string>>();
        api["reportUnsupportedCommand"]
            = [reportedUnsupportedCommands](std::string_view command, std::string_view script) {
                  const std::string canonical = Misc::StringUtils::lowerCase(command);
                  if (!reportedUnsupportedCommands->insert(canonical).second)
                      return;
                  Log(Debug::Warning) << "FNV/ESM4 ObScript unsupported command: command=" << command
                                      << " firstScript=" << script;
              };

        auto recordBindingsClass = lua.new_usertype<ESM4::Script>("ESM4_Script");
        recordBindingsClass[sol::meta_function::to_string]
            = [](const ESM4::Script& rec) { return "ESM4_Script[" + ESM::RefId(rec.mId).toDebugString() + "]"; };
        recordBindingsClass["id"] = sol::readonly_property(
            [](const ESM4::Script& rec) -> std::string { return ESM::RefId(rec.mId).serializeText(); });
        recordBindingsClass["editorId"]
            = sol::readonly_property([](const ESM4::Script& rec) -> std::string_view { return rec.mEditorId; });
        recordBindingsClass["text"] = sol::readonly_property(
            [](const ESM4::Script& rec) -> std::string_view { return rec.mScript.scriptSource; });

        using StoreT = MWWorld::Store<ESM4::Script>;
        sol::usertype<StoreT> storeBindingsClass = lua.new_usertype<StoreT>("ESM4_Script Store");
        storeBindingsClass[sol::meta_function::to_string]
            = [](const StoreT& store) { return "{" + std::to_string(store.getSize()) + " ESM4_Script records}"; };
        storeBindingsClass[sol::meta_function::length] = [](const StoreT& store) { return store.getSize(); };
        storeBindingsClass[sol::meta_function::index] = sol::overload(
            [](const StoreT& store, size_t index) -> const ESM4::Script* {
                if (index == 0 || index > store.getSize())
                    return nullptr;
                return store.at(LuaUtil::fromLuaIndex(index));
            },
            [](const StoreT& store, std::string_view id) -> const ESM4::Script* {
                return store.search(ESM::RefId::deserializeText(id));
            });
        storeBindingsClass[sol::meta_function::ipairs] = lua["ipairsForArray"].get<sol::function>();
        storeBindingsClass[sol::meta_function::pairs] = lua["ipairsForArray"].get<sol::function>();

        api["records"] = &MWBase::Environment::get().getESMStore()->get<ESM4::Script>();

        // Case-insensitive editor-id lookup, built lazily on first use.
        using EditorIdIndex = std::map<std::string, ESM::RefId>;
        auto resolve = [](const EditorIdIndex& index, std::string_view editorId) -> sol::optional<std::string> {
            auto it = index.find(Misc::StringUtils::lowerCase(editorId));
            if (it == index.end())
                return sol::nullopt;
            return it->second.serializeText();
        };

        // Item record types, for ObScript arguments like `player.AddItem <EditorId> <count>`.
        api["resolveItemEditorId"] = [resolve](std::string_view editorId) {
            static const EditorIdIndex index = [] {
                EditorIdIndex res;
                const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
                auto addAll = [&](const auto& s) {
                    for (size_t i = 0; i < s.getSize(); ++i)
                    {
                        const auto& record = *s.at(i);
                        if (!record.mEditorId.empty())
                            res.emplace(Misc::StringUtils::lowerCase(record.mEditorId), ESM::RefId(record.mId));
                    }
                };
                addAll(store.get<ESM4::Ammunition>());
                addAll(store.get<ESM4::Armor>());
                addAll(store.get<ESM4::Book>());
                addAll(store.get<ESM4::Clothing>());
                addAll(store.get<ESM4::Ingredient>());
                addAll(store.get<ESM4::MiscItem>());
                addAll(store.get<ESM4::Potion>());
                addAll(store.get<ESM4::Weapon>());
                return res;
            }();
            return resolve(index, editorId);
        };

        // Persistent placed references (e.g. `GSSchoolTerminal01Ref.Disable`).
        api["resolveRefEditorId"] = [resolve](std::string_view editorId) {
            static const EditorIdIndex index = [] {
                EditorIdIndex res;
                const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
                const MWWorld::Store<ESM4::Reference>& refs = store.get<ESM4::Reference>();
                for (size_t i = 0; i < refs.getSize(); ++i)
                {
                    const ESM4::Reference& ref = *refs.at(i);
                    if (!ref.mEditorId.empty())
                        res.emplace(Misc::StringUtils::lowerCase(ref.mEditorId), ESM::RefId(ref.mId));
                }
                return res;
            }();
            return resolve(index, editorId);
        };

        api["resolveFactionEditorId"] = [resolve](std::string_view editorId) {
            static const EditorIdIndex index = [] {
                EditorIdIndex res;
                const MWWorld::Store<ESM4::Faction>& factions
                    = MWBase::Environment::get().getESMStore()->get<ESM4::Faction>();
                for (std::size_t i = 0; i < factions.getSize(); ++i)
                {
                    const ESM4::Faction& faction = *factions.at(i);
                    if (!faction.mEditorId.empty())
                        res.emplace(Misc::StringUtils::lowerCase(faction.mEditorId), ESM::RefId(faction.mId));
                }
                return res;
            }();
            return resolve(index, editorId);
        };

        api["isMenuMode"] = [] {
            return MWBase::Environment::get().getWindowManager()->isGuiMode();
        };
        api["getButtonPressed"] = [] {
            return MWBase::Environment::get().getWindowManager()->readPressedButton();
        };
        api["showMessage"] = [](std::string_view editorId) {
            using MessageIndex = std::map<std::string, const ESM4::Message*>;
            static const MessageIndex index = [] {
                MessageIndex result;
                const MWWorld::Store<ESM4::Message>& messages
                    = MWBase::Environment::get().getESMStore()->get<ESM4::Message>();
                for (std::size_t i = 0; i < messages.getSize(); ++i)
                {
                    const ESM4::Message& message = *messages.at(i);
                    if (!message.mEditorId.empty())
                        result.emplace(Misc::StringUtils::lowerCase(message.mEditorId), &message);
                }
                return result;
            }();

            const auto found = index.find(Misc::StringUtils::lowerCase(editorId));
            if (found == index.end())
                return false;
            const ESM4::Message& message = *found->second;
            std::vector<std::string> buttons;
            buttons.reserve(message.mButtons.size());
            for (const ESM4::MessageButton& button : message.mButtons)
            {
                if (!button.mConditions.empty())
                {
                    Log(Debug::Warning) << "FNV/ESM4 ObScript ShowMessage rejected conditioned button: message="
                                        << message.mEditorId << " button=" << button.mText;
                    return false;
                }
                buttons.push_back(button.mText);
            }
            MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
            if (buttons.empty())
                windowManager->messageBox(message.mDescription);
            else
                windowManager->interactiveMessageBox(message.mDescription, buttons);
            return true;
        };
        api["getUnconscious"] = [](const Object& object) {
            try
            {
                const MWWorld::Ptr& ptr = object.ptrOrEmpty();
                if (ptr.isEmpty() || !ptr.getClass().isActor())
                    return false;
                return ptr.getClass().getCreatureStats(ptr).getKnockedDown();
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "FNV/ESM4 ObScript GetUnconscious skipped stale reference: object="
                                    << object.id().toString() << " error=" << error.what();
                return false;
            }
        };
        api["isDestroyed"] = [](const Object& object) {
            try
            {
                const MWWorld::Ptr& ptr = object.ptrOrEmpty();
                return !ptr.isEmpty() && ptr.getRefData().isDestroyed();
            }
            catch (const std::exception& error)
            {
                Log(Debug::Warning) << "FNV/ESM4 ObScript GetDestroyed skipped stale reference: object="
                                    << object.id().toString() << " error=" << error.what();
                return false;
            }
        };
        api["setDestroyed"] = [context](const Object& object, bool destroyed) {
            const MWWorld::Ptr& ptr = object.ptrOrEmpty();
            if (ptr.isEmpty())
                return false;

            context.mLuaManager->addAction(
                [object = Object(ptr), destroyed] {
                    const MWWorld::Ptr& delayedObject = object.ptrOrEmpty();
                    if (delayedObject.isEmpty())
                        return;
                    delayedObject.getRefData().setDestroyed(destroyed);
                    Log(Debug::Info) << "FNV/ESM4 ObScript SetDestroyed: object="
                                     << delayedObject.toString() << " destroyed=" << destroyed;
                },
                "ObScriptSetDestroyed");
            return true;
        };
        api["startCombat"] = [context](const Object& actor, const Object& target) {
            const MWWorld::Ptr& actorPtr = actor.ptrOrEmpty();
            const MWWorld::Ptr& targetPtr = target.ptrOrEmpty();
            if (actorPtr.isEmpty() || targetPtr.isEmpty())
                return false;

            context.mLuaManager->addAction(
                [actor = Object(actorPtr), target = Object(targetPtr)] {
                    const MWWorld::Ptr& delayedActor = actor.ptrOrEmpty();
                    const MWWorld::Ptr& delayedTarget = target.ptrOrEmpty();
                    if (delayedActor.isEmpty() || delayedTarget.isEmpty())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript StartCombat skipped stale reference: actor="
                                            << actor.id().toString() << " target=" << target.id().toString();
                        return;
                    }
                    if (!delayedActor.getClass().isActor() || !delayedTarget.getClass().isActor())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript StartCombat rejected non-actor: actor="
                                            << delayedActor.toString() << " target=" << delayedTarget.toString();
                        return;
                    }
                    MWBase::Environment::get().getMechanicsManager()->startCombat(
                        delayedActor, delayedTarget, nullptr);
                    Log(Debug::Info) << "FNV/ESM4 ObScript StartCombat: actor=" << delayedActor.toString()
                                     << " target=" << delayedTarget.toString();
                },
                "ObScriptStartCombat");
            return true;
        };
        api["stopCombat"] = [context](const Object& actor) {
            const MWWorld::Ptr& actorPtr = actor.ptrOrEmpty();
            if (actorPtr.isEmpty())
                return false;

            context.mLuaManager->addAction(
                [actor = Object(actorPtr)] {
                    const MWWorld::Ptr& delayedActor = actor.ptrOrEmpty();
                    if (delayedActor.isEmpty())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript StopCombat skipped stale reference: actor="
                                            << actor.id().toString();
                        return;
                    }
                    if (!delayedActor.getClass().isActor())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript StopCombat rejected non-actor: actor="
                                            << delayedActor.toString();
                        return;
                    }
                    MWBase::Environment::get().getMechanicsManager()->stopCombat(delayedActor);
                    Log(Debug::Info) << "FNV/ESM4 ObScript StopCombat: actor=" << delayedActor.toString();
                },
                "ObScriptStopCombat");
            return true;
        };
        api["sendAssaultAlarm"] = [context](const Object& requestedVictim, std::string_view serializedFaction) {
            const MWWorld::Ptr& victimPtr = requestedVictim.ptrOrEmpty();
            ESM::RefId faction;
            if (!serializedFaction.empty())
            {
                try
                {
                    faction = ESM::RefId::deserializeText(serializedFaction);
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }
            if (victimPtr.isEmpty() && faction.empty())
                return false;

            context.mLuaManager->addAction(
                [victim = Object(victimPtr), faction] {
                    const MWWorld::Ptr& delayedVictim = victim.ptrOrEmpty();
                    if (delayedVictim.isEmpty() && faction.empty())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript SendAssaultAlarm skipped stale victim: "
                                            << victim.id().toString();
                        return;
                    }
                    MWBase::MechanicsManager* mechanics
                        = MWBase::Environment::get().getMechanicsManager();
                    if (mechanics == nullptr || !mechanics->sendFalloutAssaultAlarm(delayedVictim, faction))
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript SendAssaultAlarm rejected: victim="
                                            << victim.id().toString() << " faction=" << faction;
                    }
                },
                "ObScriptSendAssaultAlarm");
            return true;
        };
        api["activate"] = [context](const Object& object, const Object& actor) {
            const MWWorld::Ptr& objectPtr = object.ptrOrEmpty();
            const MWWorld::Ptr& actorPtr = actor.ptrOrEmpty();
            if (objectPtr.isEmpty() || actorPtr.isEmpty())
                return false;

            context.mLuaManager->addAction(
                [object = Object(objectPtr), actor = Object(actorPtr)] {
                    const MWWorld::Ptr& delayedObject = object.ptrOrEmpty();
                    const MWWorld::Ptr& delayedActor = actor.ptrOrEmpty();
                    if (delayedObject.isEmpty() || delayedActor.isEmpty())
                    {
                        Log(Debug::Warning) << "FNV/ESM4 ObScript Activate skipped stale reference: object="
                                            << object.id().toString() << " actor=" << actor.id().toString();
                        return;
                    }
                    if (delayedObject.getRefData().isDestroyed())
                        return;
                    // ObScript handlers can run on the Lua worker while cells
                    // are being unloaded. Class/container access must remain
                    // on the main thread after SafePtr has been revalidated.
                    if (!delayedObject.getRefData().activateByScript()
                        && delayedObject.getContainerStore() == nullptr)
                        return;
                    std::unique_ptr<MWWorld::Action> action
                        = delayedObject.getClass().activate(delayedObject, delayedActor);
                    if (action)
                        action->execute(delayedActor);
                },
                "ObScriptActivate");
            return true;
        };

        api["hasQuest"] = [](std::string_view id) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().search(id) != nullptr;
        };
        api["getQuestState"] = [lua](std::string_view id) -> sol::object {
            const MWWorld::ESM4QuestState* state
                = MWBase::Environment::get().getWorld()->getESM4QuestRuntime().search(id);
            if (state == nullptr)
                return sol::make_object(lua, sol::nil);

            sol::table result(lua, sol::create);
            result["stage"] = state->mCurrentStage;
            result["running"] = (state->mFlags & MWWorld::ESM4QuestState::Flag_Running) != 0;
            result["completed"] = (state->mFlags & MWWorld::ESM4QuestState::Flag_Completed) != 0;
            result["failed"] = (state->mFlags & MWWorld::ESM4QuestState::Flag_Failed) != 0;

            sol::table stages(lua, sol::create);
            for (const auto& [stage, done] : state->mStageDone)
                stages[stage] = done;
            result["stages"] = std::move(stages);

            sol::table objectives(lua, sol::create);
            for (const auto& [objective, flags] : state->mObjectiveStatus)
            {
                sol::table objectiveState(lua, sol::create);
                objectiveState["displayed"] = (flags & MWWorld::ESM4QuestState::Objective_Displayed) != 0;
                objectiveState["completed"] = (flags & MWWorld::ESM4QuestState::Objective_Completed) != 0;
                objectives[objective] = std::move(objectiveState);
            }
            result["objectives"] = std::move(objectives);
            return sol::make_object(lua, std::move(result));
        };
        api["getQuestVariable"] = [](std::string_view quest, std::string_view variable) -> sol::optional<float> {
            const std::optional<float> value
                = MWBase::Environment::get().getWorld()->getESM4QuestRuntime().getQuestVariable(quest, variable);
            return value ? sol::optional<float>(*value) : sol::nullopt;
        };
        api["setQuestStage"] = [](std::string_view quest, int stage) {
            if (stage < 0 || stage > 255)
                return false;
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().setStage(
                quest, static_cast<std::uint8_t>(stage));
        };
        api["setObjectiveDisplayed"] = [](std::string_view quest, int objective, bool displayed) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().setObjectiveDisplayed(
                quest, objective, displayed);
        };
        api["setObjectiveCompleted"] = [](std::string_view quest, int objective, bool completed) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().setObjectiveCompleted(
                quest, objective, completed);
        };
        api["startQuest"] = [](std::string_view quest) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().startQuest(quest);
        };
        api["stopQuest"] = [](std::string_view quest) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().stopQuest(quest);
        };
        api["completeQuest"] = [](std::string_view quest) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().completeQuest(quest);
        };
        api["failQuest"] = [](std::string_view quest) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().failQuest(quest);
        };
        api["setQuestVariable"] = [](std::string_view quest, std::string_view variable, float value) {
            return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().setQuestVariable(
                quest, variable, value);
        };
        api["getGlobalVariable"] = [](std::string_view name) -> sol::optional<float> {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::GlobalVariableName global(name);
            const char type = world->getGlobalVariableType(global);
            if (type == 'f')
                return world->getGlobalFloat(global);
            if (type == 's' || type == 'l')
                return static_cast<float>(world->getGlobalInt(global));
            return sol::nullopt;
        };
        api["hasGlobalVariable"] = [](std::string_view name) {
            return MWBase::Environment::get().getWorld()->getGlobalVariableType(
                       MWWorld::GlobalVariableName(name))
                != ' ';
        };
        api["setGlobalVariable"] = [](std::string_view name, float value) {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::GlobalVariableName global(name);
            const char type = world->getGlobalVariableType(global);
            if (type == 'f')
            {
                world->setGlobalFloat(global, value);
                return true;
            }
            if (type == 's' || type == 'l')
            {
                world->setGlobalInt(global, static_cast<int>(value));
                return true;
            }
            return false;
        };

        return LuaUtil::makeReadOnly(api);
    }
}
