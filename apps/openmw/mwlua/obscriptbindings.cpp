#include "obscriptbindings.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sol/state_view.hpp>
#include <sol/table.hpp>
#include <sol/usertype.hpp>

#include <components/esm3/loadnpc.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadavif.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadgmst.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/debug/debuglog.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/esm4questruntime.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/globalvariablename.hpp"
#include "../mwworld/player.hpp"
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

namespace
{
    template <class Record>
    const Record* findEsm4Record(const MWWorld::Store<Record>& store, std::string_view id)
    {
        const Record* record = nullptr;
        try
        {
            record = store.search(ESM::RefId::deserializeText(id));
        }
        catch (const std::exception&)
        {
        }
        if (record != nullptr)
            return record;
        for (std::size_t i = 0; i < store.getSize(); ++i)
        {
            const Record& candidate = *store.at(i);
            if (Misc::StringUtils::ciEqual(candidate.mEditorId, id))
                return &candidate;
        }
        return nullptr;
    }
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

        // Sound arguments in Fallout scripts are SOUN base records rather
        // than placed references. Resolve their editor IDs independently so
        // PlaySound/PlaySound3D reach the authoritative OpenMW sound store.
        api["resolveSoundEditorId"] = [resolve](std::string_view editorId) {
            static const EditorIdIndex index = [] {
                EditorIdIndex res;
                const MWWorld::Store<ESM4::Sound>& sounds
                    = MWBase::Environment::get().getESMStore()->get<ESM4::Sound>();
                for (size_t i = 0; i < sounds.getSize(); ++i)
                {
                    const ESM4::Sound& sound = *sounds.at(i);
                    if (!sound.mEditorId.empty())
                        res.emplace(Misc::StringUtils::lowerCase(sound.mEditorId), ESM::RefId(sound.mId));
                }
                return res;
            }();
            return resolve(index, editorId);
        };

        api["getWeaponInfo"] = [lua](std::string_view id) -> sol::object {
            const MWWorld::Store<ESM4::Weapon>& weapons
                = MWBase::Environment::get().getESMStore()->get<ESM4::Weapon>();
            const ESM4::Weapon* weapon = nullptr;
            try
            {
                weapon = weapons.search(ESM::RefId::deserializeText(id));
            }
            catch (const std::exception&)
            {
            }
            if (weapon == nullptr)
            {
                for (std::size_t i = 0; i < weapons.getSize(); ++i)
                {
                    const ESM4::Weapon& candidate = *weapons.at(i);
                    if (Misc::StringUtils::ciEqual(candidate.mEditorId, id))
                    {
                        weapon = &candidate;
                        break;
                    }
                }
            }
            if (weapon == nullptr)
                return sol::make_object(lua, sol::nil);

            sol::table result(lua, sol::create);
            result["id"] = ESM::RefId(weapon->mId).serializeText();
            result["editorId"] = weapon->mEditorId;
            result["animationType"] = weapon->mData.animationType;
            result["skillActorValue"] = weapon->mData.skillActorValue;
            result["minSpread"] = weapon->mData.minSpread;
            result["spread"] = weapon->mData.spread;
            result["sightFov"] = weapon->mData.sightFov;
            result["numProjectiles"] = weapon->mData.numProjectiles;
            result["hasBallistics"] = weapon->mData.hasBallistics;
            return sol::make_object(lua, std::move(result));
        };

        api["getItemInfo"] = [lua](std::string_view id) -> sol::object {
            const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
            auto makeResult = [lua](const auto& record, int typeCode, float weight, bool scripted,
                                  bool playable, float baseHealth, std::uint32_t weaponFlags1,
                                  std::string_view icon = {}, std::string_view bipedIconMale = {},
                                  std::string_view bipedIconFemale = {}) {
                sol::table result(lua, sol::create);
                result["id"] = ESM::RefId(record.mId).serializeText();
                result["editorId"] = record.mEditorId;
                result["name"] = record.mFullName.empty() ? record.mEditorId : record.mFullName;
                result["typeCode"] = typeCode;
                result["weight"] = weight;
                result["scripted"] = scripted;
                result["playable"] = playable;
                result["baseHealth"] = baseHealth;
                result["weaponFlags1"] = weaponFlags1;
                result["icon"] = icon;
                result["bipedIconMale"] = bipedIconMale;
                result["bipedIconFemale"] = bipedIconFemale;
                return sol::make_object(lua, std::move(result));
            };

            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Weapon>(), id))
                return makeResult(*record, 40, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true,
                    static_cast<float>(record->mData.health), record->mData.weaponFlags1, record->mIcon);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Ammunition>(), id))
                return makeResult(*record, 41, record->mData.mWeight, !record->mScript.isZeroOrUnset(), true,
                    static_cast<float>(record->mData.mHealth), 0, record->mIcon);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Armor>(), id))
                return makeResult(*record, 24, record->mData.weight, !record->mScriptId.isZeroOrUnset(),
                    (record->mGeneralFlags & ESM4::Armor::FO3_NonPlayable) == 0,
                    static_cast<float>(record->mData.health), 0, record->mIconMale, record->mIconMale,
                    record->mIconFemale);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Clothing>(), id))
                return makeResult(*record, 26, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true,
                    0.f, 0, record->mIconMale, record->mIconMale, record->mIconFemale);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Potion>(), id))
                return makeResult(
                    *record, 47, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true, 0.f, 0,
                    record->mIcon);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Ingredient>(), id))
                return makeResult(
                    *record, 49, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true, 0.f, 0,
                    record->mIcon);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::Book>(), id))
                return makeResult(
                    *record, 25, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true, 0.f, 0,
                    record->mIcon);
            if (const auto* record = findEsm4Record(esmStore.get<ESM4::MiscItem>(), id))
                return makeResult(
                    *record, 30, record->mData.weight, !record->mScriptId.isZeroOrUnset(), true, 0.f, 0,
                    record->mIcon);
            return sol::make_object(lua, sol::nil);
        };

        api["getFormListIndex"] = [](std::string_view listId, std::string_view itemId) {
            const MWWorld::Store<ESM4::FormIdList>& lists
                = MWBase::Environment::get().getESMStore()->get<ESM4::FormIdList>();
            const ESM4::FormIdList* list = nullptr;
            try
            {
                list = lists.search(ESM::RefId::deserializeText(listId));
            }
            catch (const std::exception&)
            {
            }
            if (list == nullptr)
            {
                for (std::size_t i = 0; i < lists.getSize(); ++i)
                {
                    const ESM4::FormIdList& candidate = *lists.at(i);
                    if (Misc::StringUtils::ciEqual(candidate.mEditorId, listId))
                    {
                        list = &candidate;
                        break;
                    }
                }
            }
            if (list == nullptr)
                return -1;

            const ESM::FormId* item = nullptr;
            try
            {
                const ESM::RefId parsed = ESM::RefId::deserializeText(itemId);
                item = parsed.getIf<ESM::FormId>();
                if (item != nullptr)
                {
                    for (std::size_t i = 0; i < list->mObjects.size(); ++i)
                    {
                        if (list->mObjects[i] == *item)
                            return static_cast<int>(i);
                    }
                }
            }
            catch (const std::exception&)
            {
            }
            return -1;
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
        api["getPlayerControlsDisabled"] = [] {
            return MWBase::Environment::get().getInputManager()->controlsDisabled();
        };
        api["getVatsMode"] = [] {
            return MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState().isVatsActive();
        };
        api["isInKillCam"] = [] {
            // OpenMW does not enter Fallout's cinematic kill-camera state.
            // Returning the authoritative engine state (always inactive) is
            // distinct from an unknown-command fallback.
            return false;
        };
        api["toggleVanityWheel"] = [](bool enabled) {
            return MWBase::Environment::get().getWorld()->toggleVanityMode(enabled);
        };
        api["getPlayerWeaponOut"] = [] {
            return MWBase::Environment::get().getWorld()->getPlayer().getDrawState()
                == MWMechanics::DrawState::Weapon;
        };
        api["getPlayerIsFemale"] = [] {
            const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            if (player.isEmpty())
                return false;
            if (player.getType() == ESM::REC_NPC_4)
                return MWClass::ESM4Npc::isFemale(player);
            const auto* ref = player.get<ESM::NPC>();
            return ref != nullptr && ref->mBase != nullptr && !ref->mBase->isMale();
        };
        api["setPlayerWeaponOut"] = [](bool drawn) {
            MWBase::Environment::get().getWorld()->getPlayer().setDrawState(
                drawn ? MWMechanics::DrawState::Weapon : MWMechanics::DrawState::Nothing);
        };
        api["createPlayerDetectionEvent"] = [](float soundLevel, int eventType) {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty())
                return 0;

            // Fallout stores a short-lived sound event on the actor process.
            // OpenMW's awareness model has no equivalent event object, so
            // translate it into fresh native awareness checks for actors in a
            // radius derived from the retail sound level.
            const float radius = std::max(0.f, soundLevel) * 40.96f;
            std::vector<MWWorld::Ptr> observers;
            mechanics->getActorsInRange(
                player.getRefData().getPosition().asVec3(), radius, observers);

            int checked = 0;
            int detected = 0;
            for (const MWWorld::Ptr& observer : observers)
            {
                if (observer.isEmpty() || observer == player
                    || observer.getClass().getCreatureStats(observer).isDead())
                    continue;
                observer.getClass().getCreatureStats(observer).updateAwareness(5.f);
                ++checked;
                if (mechanics->awarenessCheck(player, observer, false))
                    ++detected;
            }
            Log(Debug::Info)
                << "[obscript-compat] state=native-effect scenarioId=JVS.sprint"
                << " sourceScript=JVSMainLoopEventHandler provider=xnvse-core"
                << " command=CreateDetectionEvent enginePath=openmw.mechanics.awarenessCheck"
                << " soundLevel=" << soundLevel << " eventType=" << eventType
                << " radius=" << radius << " observers=" << checked << " detected=" << detected;
            return detected;
        };
        api["getCrosshairRef"] = []() -> sol::optional<LObject> {
            const MWWorld::Ptr target = MWBase::Environment::get().getWorld()->getFacedObject();
            if (target.isEmpty())
                return sol::nullopt;
            return LObject(getId(target));
        };
        api["getCurrentQuestObjectiveTeleportLinks"] = [lua]() {
            sol::table result(lua, sol::create);
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWWorld::ESM4QuestRuntime& runtime = world->getESM4QuestRuntime();
            int resultIndex = 0;
            for (const ESM4::Quest& quest : world->getStore().get<ESM4::Quest>())
            {
                const MWWorld::ESM4QuestState* state = runtime.search(quest.mId);
                if (state == nullptr
                    || (state->mFlags & MWWorld::ESM4QuestState::Flag_Running) == 0)
                    continue;

                for (const ESM4::QuestObjective& objective : quest.mObjectives)
                {
                    const auto status = state->mObjectiveStatus.find(objective.mIndex);
                    if (status == state->mObjectiveStatus.end()
                        || (status->second & MWWorld::ESM4QuestState::Objective_Displayed) == 0
                        || (status->second & MWWorld::ESM4QuestState::Objective_Completed) != 0)
                        continue;

                    for (const ESM4::QuestObjectiveTarget& target : objective.mTargets)
                    {
                        if (!target.mConditions.empty() && !runtime.evaluateConditions(target.mConditions))
                            continue;
                        const MWWorld::Ptr targetPtr
                            = world->searchPtr(ESM::RefId(target.mTarget), false, false);
                        if (targetPtr.isEmpty())
                            continue;

                        // JIP LN returns one array per objective route. The
                        // first element is the destination and the final
                        // element is the reference that supplied the route.
                        // OpenMW currently has no separate teleport-link
                        // resolver for quest targets, so a direct target is a
                        // one-hop route whose destination and source match.
                        sol::table route(lua, sol::create);
                        const LObject object(getId(targetPtr));
                        route[0] = object;
                        route[1] = object;
                        result[resultIndex++] = std::move(route);
                    }
                }
            }
            return result;
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
        api["getFalloutRuntimeGameSetting"] = [](std::string_view name) -> sol::optional<float> {
            // These values live in FalloutNV.exe's runtime SettingCollection,
            // not in GMST records. The exact defaults below were captured
            // from the supported 1.4.0.525 executable by the retail oracle.
            // xNVSE's GetNumericGameSetting exposes both collections through
            // one command, so the compatibility API must do the same.
            static const std::map<std::string, float> settings{
                { "fcrippledarm1hspreadpenalty", 0.2f },
                { "fcrippledarm2hspreadpenalty", 0.4f },
                { "fcrippledarms1hspreadpenalty", 0.4f },
                { "fcrippledarms2hspreadpenalty", 0.6f },
                { "fmingunspreadvalue", 0.01f },
                { "frunningspreadpenalty", 0.2f },
                { "fstandingspreadpenalty", 0.1f },
                { "funaimedspreadpenalty", 0.2f },
                { "fvatsparalyzepalmchance", 0.3f },
                { "fvatsplayerdamagemult", 0.75f },
                { "fvatsshotbursttime", 0.43f },
                { "fvatsshotlongbursttime", 1.75f },
                { "fwalkingspreadpenalty", 0.1f },
                { "fweapskillreqpenalty", 0.01f },
                { "fweapstrengthreqpenalty", 0.025f },
                { "fwobbletoskillconversion", 0.5f },
                { "ivatsconcentratedfirebonus", 5.f },
            };
            const auto it = settings.find(Misc::StringUtils::lowerCase(name));
            if (it == settings.end())
                return sol::nullopt;
            return it->second;
        };
        api["getStringGameSetting"] = [](std::string_view name) -> sol::optional<std::string> {
            const MWWorld::Store<ESM4::GameSetting>& settings
                = MWBase::Environment::get().getESMStore()->get<ESM4::GameSetting>();
            const ESM4::GameSetting* setting = findEsm4Record(settings, name);
            if (setting == nullptr)
                return sol::nullopt;
            const std::string* value = std::get_if<std::string>(&setting->mData);
            return value != nullptr ? sol::optional<std::string>(*value) : sol::nullopt;
        };
        api["getPlayerActorValue"] = [](std::uint32_t actorValue) -> sol::optional<float> {
            const MWWorld::FalloutPlayerRuntimeState& state
                = MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState();
            if (actorValue == 13)
            {
                const std::optional<float> capacity = state.getCarryCapacity();
                return capacity ? sol::optional<float>(*capacity) : sol::nullopt;
            }
            // Fallout limb/condition actor values are 100 at full condition;
            // the native save stores their damage as modifiers.
            if (actorValue >= 25 && actorValue <= 31)
                return std::max(0.f, 100.f + state.getSavedDamageModifier(actorValue));
            const std::optional<MWWorld::FalloutRuntimeActorValue> value = state.getCurrentActorValue(actorValue);
            return value ? sol::optional<float>(value->mValue) : sol::nullopt;
        };
        api["getActorValueName"] = [](std::uint32_t actorValue, std::uint32_t nameType)
            -> sol::optional<std::string> {
            struct Metadata
            {
                std::string_view mName;
                std::string_view mEditorId;
            };
            // FalloutNV.exe exposes these indices through ActorValueInfo.
            // The AVIF editor IDs let localized FULL names come from the
            // loaded ESM instead of being baked into the compatibility layer.
            static const std::map<std::uint32_t, Metadata> metadata{
                { 0, { "Aggression", "AVAggression" } },
                { 1, { "Confidence", "AVConfidence" } },
                { 2, { "Energy", "AVEnergy" } },
                { 3, { "Responsibility", "AVResponsibility" } },
                { 4, { "Mood", "AVMood" } },
                { 5, { "Strength", "AVStrength" } },
                { 6, { "Perception", "AVPerception" } },
                { 7, { "Endurance", "AVEndurance" } },
                { 8, { "Charisma", "AVCharisma" } },
                { 9, { "Intelligence", "AVIntelligence" } },
                { 10, { "Agility", "AVAgility" } },
                { 11, { "Luck", "AVLuck" } },
                { 12, { "ActionPoints", "AVActionPoints" } },
                { 13, { "CarryWeight", "AVCarryWeight" } },
                { 14, { "CritChance", "AVCritChance" } },
                { 15, { "HealRate", "AVHealRate" } },
                { 16, { "Health", "AVHealth" } },
                { 17, { "MeleeDamage", "AVMeleeDamage" } },
                { 18, { "DamageResistance", "AVDamageResist" } },
                { 19, { "PoisonResistance", "AVPoisonResist" } },
                { 20, { "RadResistance", "AVRadResist" } },
                { 21, { "SpeedMultiplier", "" } },
                { 22, { "Fatigue", "AVFatigue" } },
                { 23, { "Karma", "AVKarma" } },
                { 24, { "XP", "AVXP" } },
                { 25, { "PerceptionCondition", "AVPerceptionCondition" } },
                { 26, { "EnduranceCondition", "AVEnduranceCondition" } },
                { 27, { "LeftAttackCondition", "AVLeftAttackCondition" } },
                { 28, { "RightAttackCondition", "AVRightAttackCondition" } },
                { 29, { "LeftMobilityCondition", "AVLeftMobilityCondition" } },
                { 30, { "RightMobilityCondition", "AVRightMobilityCondition" } },
                { 31, { "BrainCondition", "AVBrainCondition" } },
                { 32, { "Barter", "AVBarter" } },
                { 33, { "BigGuns", "AVBigGuns" } },
                { 34, { "EnergyWeapons", "AVEnergyWeapons" } },
                { 35, { "Explosives", "AVExplosives" } },
                { 36, { "Lockpick", "AVLockpick" } },
                { 37, { "Medicine", "AVMedicine" } },
                { 38, { "MeleeWeapons", "AVMeleeWeapons" } },
                { 39, { "Repair", "AVRepair" } },
                { 40, { "Science", "AVScience" } },
                { 41, { "Guns", "AVSmallGuns" } },
                { 42, { "Sneak", "AVSneak" } },
                { 43, { "Speech", "AVSpeech" } },
                { 44, { "Survival", "AVThrowing" } },
                { 45, { "Unarmed", "AVUnarmed" } },
                { 46, { "InventoryWeight", "AVInventoryWeight" } },
                { 47, { "Paralysis", "AVParalysis" } },
                { 48, { "Invisibility", "AVInvisibility" } },
                { 49, { "Chameleon", "AVChameleon" } },
                { 50, { "NightEye", "AVNightEye" } },
                { 51, { "Turbo", "AVDetectLifeRange" } },
                { 52, { "FireResistance", "AVFireResist" } },
                { 53, { "WaterBreathing", "AVWaterBreathing" } },
                { 54, { "RadLevel", "AVRadiationRads" } },
                { 55, { "BloodyMess", "AVBloodyMess" } },
                { 56, { "UnarmedDamage", "AVUnarmedDamage" } },
                { 57, { "Assistance", "AVAssistance" } },
                { 58, { "ElectricResistance", "" } },
                { 59, { "FrostResistance", "" } },
                { 60, { "EnergyResistance", "AVEnergyResist" } },
                { 61, { "EMPResistance", "" } },
                { 62, { "Variable01", "AVVariable01" } },
                { 63, { "Variable02", "" } },
                { 64, { "Variable03", "" } },
                { 65, { "Variable04", "" } },
                { 66, { "Variable05", "" } },
                { 67, { "Variable06", "" } },
                { 68, { "Variable07", "" } },
                { 69, { "Variable08", "" } },
                { 70, { "Variable09", "" } },
                { 71, { "Variable10", "" } },
                { 72, { "IgnoreCrippledLimbs", "AVIgnoreCrippledLimbs" } },
                { 73, { "Dehydration", "AVDehydration" } },
                { 74, { "Hunger", "AVHunger" } },
                { 75, { "SleepDeprivation", "AVSleepDeprevation" } },
                { 76, { "DamageThreshold", "AVDamageThreshold" } },
            };
            const auto metadataIt = metadata.find(actorValue);
            if (metadataIt == metadata.end() || nameType > 2)
                return sol::nullopt;
            if (nameType != 1 || metadataIt->second.mEditorId.empty())
                return std::string(metadataIt->second.mName);

            const MWWorld::Store<ESM4::ActorValueInformation>& actorValues
                = MWBase::Environment::get().getESMStore()->get<ESM4::ActorValueInformation>();
            const ESM4::ActorValueInformation* record
                = findEsm4Record(actorValues, metadataIt->second.mEditorId);
            if (record == nullptr || record->mFullName.empty())
                return std::string(metadataIt->second.mName);
            return record->mFullName;
        };
        api["getPlayerMaxActionPoints"] = []() -> sol::optional<float> {
            const std::optional<float> value
                = MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState().getMaxActionPoints();
            return value ? sol::optional<float>(*value) : sol::nullopt;
        };
        api["setPlayerActorValue"] = [](std::uint32_t actorValue, float value) {
            return MWBase::Environment::get()
                       .getWorld()
                       ->getFalloutPlayerRuntimeState()
                       .setCurrentActorValue(actorValue, value)
                == MWWorld::FalloutActorValueMutationResult::Applied;
        };
        api["modPlayerActorValue"] = [](std::uint32_t actorValue, float delta) {
            return MWBase::Environment::get()
                       .getWorld()
                       ->getFalloutPlayerRuntimeState()
                       .modCurrentActorValue(actorValue, delta)
                == MWWorld::FalloutActorValueMutationResult::Applied;
        };
        api["modActorHealth"] = [context](const Object& actor, float delta) {
            const MWWorld::Ptr& ptr = actor.ptrOrEmpty();
            if (ptr.isEmpty() || !ptr.getClass().isActor())
                return false;
            context.mLuaManager->addAction(
                [actor = Object(ptr), delta] {
                    const MWWorld::Ptr& delayedActor = actor.ptrOrEmpty();
                    if (delayedActor.isEmpty() || !delayedActor.getClass().isActor())
                        return;
                    MWMechanics::CreatureStats& stats
                        = delayedActor.getClass().getCreatureStats(delayedActor);
                    MWMechanics::DynamicStat<float> health(stats.getHealth());
                    const float before = health.getCurrent();
                    const float after = std::clamp(
                        before + delta, 0.f, std::max(0.f, health.getModified()));
                    health.setCurrent(after);
                    stats.setHealth(health);
                    Log(Debug::Info)
                        << "[obscript-compat] state=native-effect"
                        << " scenarioId=JHM.hit-marker"
                        << " sourceScript=JHMOnHitEventHandler provider=xnvse-core"
                        << " command=DamageAV enginePath=openmw.mechanics.actorHealth"
                        << " actor=" << delayedActor.toString()
                        << " before=" << before << " after=" << after
                        << " delta=" << (after - before);
                },
                "ObScriptModActorHealth");
            return true;
        };

        return LuaUtil::makeReadOnly(api);
    }
}
