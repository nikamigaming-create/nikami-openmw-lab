#include "types.hpp"

<<<<<<< HEAD
#include "usertypeutil.hpp"
=======
#include "modelproperty.hpp"
>>>>>>> origin/main

#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/resourcehelpers.hpp>
<<<<<<< HEAD
=======
#include <components/resource/resourcesystem.hpp>
>>>>>>> origin/main

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace sol
{
    template <>
    struct is_automagical<ESM::Miscellaneous> : std::false_type
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
                = [](const T& rec) -> std::string { return "ESM3_Miscellaneous[" + rec.mId.toDebugString() + "]"; };
            record["id"] = sol::readonly_property([](const T& rec) -> ESM::RefId { return rec.mId; });

            Types::addProperty(record, "name", &ESM::Miscellaneous::mName);
            Types::addModelProperty(record);
            Types::addProperty(record, "mwscript", &ESM::Miscellaneous::mScript);
            Types::addIconProperty(record);
            Types::addProperty(record, "value", &ESM::Miscellaneous::mData, &ESM::Miscellaneous::MCDTstruct::mValue);
            Types::addProperty(record, "weight", &ESM::Miscellaneous::mData, &ESM::Miscellaneous::MCDTstruct::mWeight);
            Types::addFlagProperty(record, "isKey", ESM::Miscellaneous::Key, &ESM::Miscellaneous::mData,
                &ESM::Miscellaneous::MCDTstruct::mFlags);
        }
    }

    // Populates a misc struct from a Lua table.
    ESM::Miscellaneous tableToMisc(const sol::table& rec)
    {
        auto misc = Types::initFromTemplate<ESM::Miscellaneous>(rec);
=======
namespace
{
    // Populates a misc struct from a Lua table.
    ESM::Miscellaneous tableToMisc(const sol::table& rec)
    {
        ESM::Miscellaneous misc;
        if (rec["template"] != sol::nil)
            misc = LuaUtil::cast<ESM::Miscellaneous>(rec["template"]);
        else
            misc.blank();
>>>>>>> origin/main
        if (rec["name"] != sol::nil)
            misc.mName = rec["name"];
        if (rec["model"] != sol::nil)
            misc.mModel = Misc::ResourceHelpers::meshPathForESM3(rec["model"].get<std::string_view>());
        if (rec["icon"] != sol::nil)
            misc.mIcon = rec["icon"];
        if (rec["mwscript"] != sol::nil)
        {
            std::string_view scriptId = rec["mwscript"].get<std::string_view>();
            misc.mScript = ESM::RefId::deserializeText(scriptId);
        }
        if (rec["weight"] != sol::nil)
<<<<<<< HEAD
            misc.mData.mWeight = rec["weight"].get<Misc::FiniteFloat>();
=======
            misc.mData.mWeight = rec["weight"];
>>>>>>> origin/main
        if (rec["value"] != sol::nil)
            misc.mData.mValue = rec["value"];
        return misc;
    }
<<<<<<< HEAD

    void addMutableMiscType(sol::state_view& lua)
    {
        addUserType<MutableRecord<ESM::Miscellaneous>>(lua, "ESM3_MutableMiscellaneous");
    }

    void addMiscellaneousBindings(sol::table miscellaneous, const Context& context)
    {
=======
}

namespace MWLua
{
    void addMiscellaneousBindings(sol::table miscellaneous, const Context& context)
    {
        auto vfs = MWBase::Environment::get().getResourceSystem()->getVFS();

>>>>>>> origin/main
        addRecordFunctionBinding<ESM::Miscellaneous>(miscellaneous, context);
        miscellaneous["createRecordDraft"] = tableToMisc;

        // Deprecated. Moved to itemData; should be removed later
        miscellaneous["setSoul"] = [](const GObject& object, std::string_view soulId) {
            ESM::RefId creature = ESM::RefId::deserializeText(soulId);
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

            if (!store.get<ESM::Creature>().search(creature))
            {
                // TODO: Add Support for NPC Souls
                throw std::runtime_error("Cannot use non-existent creature as a soul: " + std::string(soulId));
            }

            object.ptr().getCellRef().setSoul(creature);
        };
<<<<<<< HEAD
        miscellaneous["getSoul"]
            = [](const Object& object) -> ESM::RefId { return object.ptr().getCellRef().getSoul(); };
        miscellaneous["soul"] = miscellaneous["getSoul"]; // for compatibility; should be removed later

        sol::state_view lua = context.sol();
        addUserType<ESM::Miscellaneous>(lua, "ESM3_Miscellaneous");
    }
}
=======
        miscellaneous["getSoul"] = [](const Object& object) -> sol::optional<std::string> {
            ESM::RefId soul = object.ptr().getCellRef().getSoul();
            return LuaUtil::serializeRefId(soul);
        };
        miscellaneous["soul"] = miscellaneous["getSoul"]; // for compatibility; should be removed later

        sol::usertype<ESM::Miscellaneous> record = context.sol().new_usertype<ESM::Miscellaneous>("ESM3_Miscellaneous");
        record[sol::meta_function::to_string]
            = [](const ESM::Miscellaneous& rec) { return "ESM3_Miscellaneous[" + rec.mId.toDebugString() + "]"; };
        record["id"] = sol::readonly_property(
            [](const ESM::Miscellaneous& rec) -> std::string { return rec.mId.serializeText(); });
        record["name"] = sol::readonly_property([](const ESM::Miscellaneous& rec) -> std::string { return rec.mName; });
        addModelProperty(record);
        record["mwscript"] = sol::readonly_property([](const ESM::Miscellaneous& rec) -> sol::optional<std::string> {
            return LuaUtil::serializeRefId(rec.mScript);
        });
        record["icon"] = sol::readonly_property([vfs](const ESM::Miscellaneous& rec) -> std::string {
            return Misc::ResourceHelpers::correctIconPath(rec.mIcon, vfs);
        });
        record["isKey"] = sol::readonly_property(
            [](const ESM::Miscellaneous& rec) -> bool { return rec.mData.mFlags & ESM::Miscellaneous::Key; });
        record["value"] = sol::readonly_property([](const ESM::Miscellaneous& rec) -> int { return rec.mData.mValue; });
        record["weight"]
            = sol::readonly_property([](const ESM::Miscellaneous& rec) -> float { return rec.mData.mWeight; });
    }
}
>>>>>>> origin/main
