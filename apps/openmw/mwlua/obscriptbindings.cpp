#include "obscriptbindings.hpp"

#include <map>
#include <string>
#include <string_view>

#include <sol/state_view.hpp>
#include <sol/table.hpp>
#include <sol/usertype.hpp>

#include <components/esm/refid.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/store.hpp"

#include "context.hpp"

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

        return LuaUtil::makeReadOnly(api);
    }
}
