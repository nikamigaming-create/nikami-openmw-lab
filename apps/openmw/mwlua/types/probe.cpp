#include "types.hpp"

<<<<<<< HEAD
#include "usertypeutil.hpp"
=======
#include "modelproperty.hpp"
>>>>>>> origin/main

#include <components/esm3/loadprob.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/resourcehelpers.hpp>
<<<<<<< HEAD
=======
#include <components/resource/resourcesystem.hpp>

#include "apps/openmw/mwbase/environment.hpp"
>>>>>>> origin/main

namespace sol
{
    template <>
    struct is_automagical<ESM::Probe> : std::false_type
    {
    };
}

namespace MWLua
{
<<<<<<< HEAD
    namespace
    {
        template <class T>
        void addUserType(sol::state_view& lua, std::string_view name)
        {
            sol::usertype<T> record = lua.new_usertype<T>(name);

            record[sol::meta_function::to_string]
                = [](const T& rec) -> std::string { return "ESM3_Probe[" + rec.mId.toDebugString() + "]"; };
            record["id"] = sol::readonly_property([](const T& rec) -> ESM::RefId { return rec.mId; });

            Types::addProperty(record, "name", &ESM::Probe::mName);
            Types::addModelProperty(record);
            Types::addProperty(record, "mwscript", &ESM::Probe::mScript);
            Types::addIconProperty(record);
            Types::addProperty(record, "maxCondition", &ESM::Probe::mData, &ESM::Probe::Data::mUses);
            Types::addProperty(record, "value", &ESM::Probe::mData, &ESM::Probe::Data::mValue);
            Types::addProperty(record, "weight", &ESM::Probe::mData, &ESM::Probe::Data::mWeight);
            Types::addProperty(record, "quality", &ESM::Probe::mData, &ESM::Probe::Data::mQuality);
        }
    }

    ESM::Probe tableToProbe(const sol::table& rec)
    {
        auto probe = Types::initFromTemplate<ESM::Probe>(rec);
        if (rec["name"] != sol::nil)
            probe.mName = rec["name"];
        if (rec["model"] != sol::nil)
            probe.mModel = Misc::ResourceHelpers::meshPathForESM3(rec["model"].get<std::string_view>());
        if (rec["mwscript"] != sol::nil)
        {
            std::string_view scriptId = rec["mwscript"].get<std::string_view>();
            probe.mScript = ESM::RefId::deserializeText(scriptId);
        }
        if (rec["icon"] != sol::nil)
            probe.mIcon = rec["icon"];
        if (rec["maxCondition"] != sol::nil)
            probe.mData.mUses = rec["maxCondition"];
        if (rec["value"] != sol::nil)
            probe.mData.mValue = rec["value"];
        if (rec["weight"] != sol::nil)
            probe.mData.mWeight = rec["weight"].get<Misc::FiniteFloat>();
        if (rec["quality"] != sol::nil)
            probe.mData.mQuality = rec["quality"].get<Misc::FiniteFloat>();
        return probe;
    }

    void addMutableProbeType(sol::state_view& lua)
    {
        addUserType<MutableRecord<ESM::Probe>>(lua, "ESM3_MutableProbe");
    }

    void addProbeBindings(sol::table probe, const Context& context)
    {
        addRecordFunctionBinding<ESM::Probe>(probe, context);
        probe["createRecordDraft"] = tableToProbe;
        sol::state_view lua = context.sol();
        addUserType<ESM::Probe>(lua, "ESM3_Probe");
    }
}
=======
    void addProbeBindings(sol::table probe, const Context& context)
    {
        auto vfs = MWBase::Environment::get().getResourceSystem()->getVFS();

        addRecordFunctionBinding<ESM::Probe>(probe, context);

        sol::usertype<ESM::Probe> record = context.sol().new_usertype<ESM::Probe>("ESM3_Probe");
        record[sol::meta_function::to_string]
            = [](const ESM::Probe& rec) { return "ESM3_Probe[" + rec.mId.toDebugString() + "]"; };
        record["id"]
            = sol::readonly_property([](const ESM::Probe& rec) -> std::string { return rec.mId.serializeText(); });
        record["name"] = sol::readonly_property([](const ESM::Probe& rec) -> std::string { return rec.mName; });
        addModelProperty(record);
        record["mwscript"] = sol::readonly_property(
            [](const ESM::Probe& rec) -> sol::optional<std::string> { return LuaUtil::serializeRefId(rec.mScript); });
        record["icon"] = sol::readonly_property([vfs](const ESM::Probe& rec) -> std::string {
            return Misc::ResourceHelpers::correctIconPath(rec.mIcon, vfs);
        });
        record["maxCondition"] = sol::readonly_property([](const ESM::Probe& rec) -> int { return rec.mData.mUses; });
        record["value"] = sol::readonly_property([](const ESM::Probe& rec) -> int { return rec.mData.mValue; });
        record["weight"] = sol::readonly_property([](const ESM::Probe& rec) -> float { return rec.mData.mWeight; });
        record["quality"] = sol::readonly_property([](const ESM::Probe& rec) -> float { return rec.mData.mQuality; });
    }
}
>>>>>>> origin/main
