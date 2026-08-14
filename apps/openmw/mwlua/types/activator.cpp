#include "types.hpp"

<<<<<<< HEAD
#include "usertypeutil.hpp"
=======
#include "modelproperty.hpp"
>>>>>>> origin/main

#include <components/esm3/loadacti.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>

namespace sol
{
    template <>
    struct is_automagical<ESM::Activator> : std::false_type
    {
    };
}
<<<<<<< HEAD

namespace MWLua
{
    namespace
    {
        template <class T>
        void addUserType(sol::state_view& lua, std::string_view name)
        {
            sol::usertype<T> record = lua.new_usertype<T>(name);

            record[sol::meta_function::to_string]
                = [](const T& rec) -> std::string { return "ESM3_Activator[" + rec.mId.toDebugString() + "]"; };
            record["id"] = sol::readonly_property([](const T& rec) -> ESM::RefId { return rec.mId; });

            Types::addProperty(record, "name", &ESM::Activator::mName);
            Types::addModelProperty(record);
            Types::addProperty(record, "mwscript", &ESM::Activator::mScript);
        }
    }

    // Populates a activator struct from a Lua table.
    ESM::Activator tableToActivator(const sol::table& rec)
    {
        auto activator = Types::initFromTemplate<ESM::Activator>(rec);
=======
namespace
{
    // Populates a activator struct from a Lua table.
    ESM::Activator tableToActivator(const sol::table& rec)
    {
        ESM::Activator activator;
        if (rec["template"] != sol::nil)
            activator = LuaUtil::cast<ESM::Activator>(rec["template"]);
        else
            activator.blank();
>>>>>>> origin/main
        if (rec["name"] != sol::nil)
            activator.mName = rec["name"];
        if (rec["model"] != sol::nil)
            activator.mModel = Misc::ResourceHelpers::meshPathForESM3(rec["model"].get<std::string_view>());
        if (rec["mwscript"] != sol::nil)
        {
            std::string_view scriptId = rec["mwscript"].get<std::string_view>();
            activator.mScript = ESM::RefId::deserializeText(scriptId);
        }
        return activator;
    }
<<<<<<< HEAD

    void addMutableActivatorType(sol::state_view& lua)
    {
        addUserType<MutableRecord<ESM::Activator>>(lua, "ESM3_MutableActivator");
    }

=======
}

namespace MWLua
{
>>>>>>> origin/main
    void addActivatorBindings(sol::table activator, const Context& context)
    {
        activator["createRecordDraft"] = tableToActivator;
        addRecordFunctionBinding<ESM::Activator>(activator, context);
<<<<<<< HEAD
        sol::state_view lua = context.sol();
        addUserType<ESM::Activator>(lua, "ESM3_Activator");
=======

        sol::usertype<ESM::Activator> record = context.sol().new_usertype<ESM::Activator>("ESM3_Activator");
        record[sol::meta_function::to_string]
            = [](const ESM::Activator& rec) { return "ESM3_Activator[" + rec.mId.toDebugString() + "]"; };
        record["id"]
            = sol::readonly_property([](const ESM::Activator& rec) -> std::string { return rec.mId.serializeText(); });
        record["name"] = sol::readonly_property([](const ESM::Activator& rec) -> std::string { return rec.mName; });
        addModelProperty(record);
        record["mwscript"] = sol::readonly_property([](const ESM::Activator& rec) -> sol::optional<std::string> {
            return LuaUtil::serializeRefId(rec.mScript);
        });
>>>>>>> origin/main
    }
}
