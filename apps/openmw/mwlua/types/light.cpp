#include "types.hpp"

<<<<<<< HEAD
#include "usertypeutil.hpp"
=======
#include "modelproperty.hpp"
>>>>>>> origin/main

#include <components/esm3/loadligh.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/util.hpp>
#include <components/misc/color.hpp>
<<<<<<< HEAD
#include <components/misc/finitevalues.hpp>
#include <components/misc/resourcehelpers.hpp>
=======
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>

#include "apps/openmw/mwbase/environment.hpp"
>>>>>>> origin/main

namespace sol
{
    template <>
    struct is_automagical<ESM::Light> : std::false_type
    {
    };
}

namespace
{
<<<<<<< HEAD
    void setRecordFlag(const sol::table& rec, std::string_view key, int flag, ESM::Light& record)
=======
    void setRecordFlag(const sol::table& rec, const std::string& key, int flag, ESM::Light& record)
>>>>>>> origin/main
    {
        if (auto luaFlag = rec[key]; luaFlag != sol::nil)
        {
            if (luaFlag)
            {
                record.mData.mFlags |= flag;
            }
            else
            {
                record.mData.mFlags &= ~flag;
            }
        }
    }
<<<<<<< HEAD
}

namespace MWLua
{
    namespace
    {
        template <class T>
        void addUserType(sol::state_view& lua, std::string_view name)
        {
            sol::usertype<T> record = lua.new_usertype<T>(name);

            record[sol::meta_function::to_string]
                = [](const T& rec) -> std::string { return "ESM3_Light[" + rec.mId.toDebugString() + "]"; };
            record["id"] = sol::readonly_property([](const T& rec) -> ESM::RefId { return rec.mId; });

            Types::addProperty(record, "name", &ESM::Light::mName);
            Types::addModelProperty(record);
            Types::addIconProperty(record);
            Types::addProperty(record, "sound", &ESM::Light::mSound);
            Types::addProperty(record, "mwscript", &ESM::Light::mScript);
            Types::addProperty(record, "weight", &ESM::Light::mData, &ESM::Light::LHDTstruct::mWeight);
            Types::addProperty(record, "value", &ESM::Light::mData, &ESM::Light::LHDTstruct::mValue);
            Types::addProperty(record, "duration", &ESM::Light::mData, &ESM::Light::LHDTstruct::mTime);
            Types::addProperty(record, "radius", &ESM::Light::mData, &ESM::Light::LHDTstruct::mRadius);
            const auto getColor = [](const T& rec) -> Misc::Color {
                const ESM::Light& light = Types::RecordType<T>::asRecord(rec);
                return Misc::Color::fromRGB(light.mData.mColor);
            };
            if constexpr (Types::RecordType<T>::isMutable)
            {
                record["color"] = sol::property(std::move(getColor), [](T& rec, const Misc::Color& color) {
                    ESM::Light& light = rec.find();
                    light.mData.mColor = color.toRGBA();
                });
            }
            else
            {
                record["color"] = sol::readonly_property(std::move(getColor));
            }
            Types::addFlagProperty(
                record, "isCarriable", ESM::Light::Carry, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isDynamic", ESM::Light::Dynamic, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isFire", ESM::Light::Fire, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isFlicker", ESM::Light::Flicker, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isFlickerSlow", ESM::Light::FlickerSlow, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isNegative", ESM::Light::Negative, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isOffByDefault", ESM::Light::OffDefault, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isPulse", ESM::Light::Pulse, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
            Types::addFlagProperty(
                record, "isPulseSlow", ESM::Light::PulseSlow, &ESM::Light::mData, &ESM::Light::LHDTstruct::mFlags);
        }
    }

    // Populates a light struct from a Lua table.
    ESM::Light tableToLight(const sol::table& rec)
    {
        auto light = Types::initFromTemplate<ESM::Light>(rec);
=======
    // Populates a light struct from a Lua table.
    ESM::Light tableToLight(const sol::table& rec)
    {
        ESM::Light light;
        if (rec["template"] != sol::nil)
            light = LuaUtil::cast<ESM::Light>(rec["template"]);
        else
            light.blank();
>>>>>>> origin/main
        if (rec["name"] != sol::nil)
            light.mName = rec["name"];
        if (rec["model"] != sol::nil)
            light.mModel = Misc::ResourceHelpers::meshPathForESM3(rec["model"].get<std::string_view>());
        if (rec["icon"] != sol::nil)
            light.mIcon = rec["icon"];
        if (rec["mwscript"] != sol::nil)
        {
            std::string_view scriptId = rec["mwscript"].get<std::string_view>();
            light.mScript = ESM::RefId::deserializeText(scriptId);
        }
        if (rec["weight"] != sol::nil)
<<<<<<< HEAD
            light.mData.mWeight = rec["weight"].get<Misc::FiniteFloat>();
=======
            light.mData.mWeight = rec["weight"];
>>>>>>> origin/main
        if (rec["value"] != sol::nil)
            light.mData.mValue = rec["value"];
        if (rec["duration"] != sol::nil)
            light.mData.mTime = rec["duration"];
        if (rec["radius"] != sol::nil)
            light.mData.mRadius = rec["radius"];
        if (rec["color"] != sol::nil)
        {
            sol::object color = rec["color"];
            if (color.is<Misc::Color>())
                light.mData.mColor = color.as<Misc::Color>().toRGBA();
            else
                light.mData.mColor = color.as<uint32_t>();
        }
        setRecordFlag(rec, "isCarriable", ESM::Light::Carry, light);
        setRecordFlag(rec, "isDynamic", ESM::Light::Dynamic, light);
        setRecordFlag(rec, "isFire", ESM::Light::Fire, light);
        setRecordFlag(rec, "isFlicker", ESM::Light::Flicker, light);
        setRecordFlag(rec, "isFlickerSlow", ESM::Light::FlickerSlow, light);
        setRecordFlag(rec, "isNegative", ESM::Light::Negative, light);
        setRecordFlag(rec, "isOffByDefault", ESM::Light::OffDefault, light);
        setRecordFlag(rec, "isPulse", ESM::Light::Pulse, light);
        setRecordFlag(rec, "isPulseSlow", ESM::Light::PulseSlow, light);

        return light;
    }
<<<<<<< HEAD

    void addMutableLightType(sol::state_view& lua)
    {
        addUserType<MutableRecord<ESM::Light>>(lua, "ESM3_MutableLight");
    }

    void addLightBindings(sol::table light, const Context& context)
    {
        addRecordFunctionBinding<ESM::Light>(light, context);
        light["createRecordDraft"] = tableToLight;
        sol::state_view lua = context.sol();
        addUserType<ESM::Light>(lua, "ESM3_Light");
=======
}

namespace MWLua
{
    void addLightBindings(sol::table light, const Context& context)
    {
        auto vfs = MWBase::Environment::get().getResourceSystem()->getVFS();

        addRecordFunctionBinding<ESM::Light>(light, context);
        light["createRecordDraft"] = tableToLight;

        sol::usertype<ESM::Light> record = context.sol().new_usertype<ESM::Light>("ESM3_Light");
        record[sol::meta_function::to_string]
            = [](const ESM::Light& rec) -> std::string { return "ESM3_Light[" + rec.mId.toDebugString() + "]"; };
        record["id"]
            = sol::readonly_property([](const ESM::Light& rec) -> std::string { return rec.mId.serializeText(); });
        record["name"] = sol::readonly_property([](const ESM::Light& rec) -> std::string { return rec.mName; });
        addModelProperty(record);
        record["icon"] = sol::readonly_property([vfs](const ESM::Light& rec) -> std::string {
            return Misc::ResourceHelpers::correctIconPath(rec.mIcon, vfs);
        });
        record["sound"]
            = sol::readonly_property([](const ESM::Light& rec) -> std::string { return rec.mSound.serializeText(); });
        record["mwscript"] = sol::readonly_property(
            [](const ESM::Light& rec) -> sol::optional<std::string> { return LuaUtil::serializeRefId(rec.mScript); });
        record["weight"] = sol::readonly_property([](const ESM::Light& rec) -> float { return rec.mData.mWeight; });
        record["value"] = sol::readonly_property([](const ESM::Light& rec) -> int { return rec.mData.mValue; });
        record["duration"] = sol::readonly_property([](const ESM::Light& rec) -> int { return rec.mData.mTime; });
        record["radius"] = sol::readonly_property([](const ESM::Light& rec) -> int { return rec.mData.mRadius; });
        record["color"] = sol::readonly_property(
            [](const ESM::Light& rec) -> Misc::Color { return Misc::Color::fromRGB(rec.mData.mColor); });
        record["isCarriable"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Carry; });
        record["isDynamic"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Dynamic; });
        record["isFire"]
            = sol::readonly_property([](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Fire; });
        record["isFlicker"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Flicker; });
        record["isFlickerSlow"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::FlickerSlow; });
        record["isNegative"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Negative; });
        record["isOffByDefault"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::OffDefault; });
        record["isPulse"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::Pulse; });
        record["isPulseSlow"] = sol::readonly_property(
            [](const ESM::Light& rec) -> bool { return rec.mData.mFlags & ESM::Light::PulseSlow; });
>>>>>>> origin/main
    }
}
