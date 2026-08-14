#include "luabindings.hpp"

<<<<<<< HEAD
=======
#include <components/debug/debuglog.hpp>
>>>>>>> origin/main
#include <components/lua/asyncpackage.hpp>
#include <components/lua/utilpackage.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/datetimemanager.hpp"

#include "animationbindings.hpp"
#include "camerabindings.hpp"
#include "cellbindings.hpp"
<<<<<<< HEAD
#include "contentbindings.hpp"
=======
>>>>>>> origin/main
#include "corebindings.hpp"
#include "debugbindings.hpp"
#include "inputbindings.hpp"
#include "localscripts.hpp"
#include "markupbindings.hpp"
#include "menuscripts.hpp"
#include "nearbybindings.hpp"
#include "objectbindings.hpp"
#include "postprocessingbindings.hpp"
#include "soundbindings.hpp"
#include "types/types.hpp"
#include "uibindings.hpp"
#include "vfsbindings.hpp"
#include "worldbindings.hpp"

<<<<<<< HEAD
=======
//## VR_PATCH BEGIN
#include "vrbindings.hpp"
//## VR_PATCH END

>>>>>>> origin/main
namespace MWLua
{
    std::map<std::string, sol::object> initCommonPackages(const Context& context)
    {
        sol::state_view lua = context.mLua->unsafeState();
        MWWorld::DateTimeManager* tm = MWBase::Environment::get().getWorld()->getTimeManager();
        return {
            { "openmw.async",
                LuaUtil::getAsyncPackageInitializer(
                    lua, [tm] { return tm->getSimulationTime(); }, [tm] { return tm->getGameTime(); }) },
            { "openmw.markup", initMarkupPackage(context) },
            { "openmw.util", LuaUtil::initUtilPackage(lua) },
            { "openmw.vfs", initVFSPackage(context) },
        };
    }

    std::map<std::string, sol::object> initGlobalPackages(const Context& context)
    {
<<<<<<< HEAD
        initObjectBindingsForGlobalScripts(context);
        initCellBindingsForGlobalScripts(context);
        return {
            { "openmw.core", initCorePackage(context) },
            { "openmw.types", initTypesPackage(context) },
            { "openmw.world", initWorldPackage(context) },
        };
=======
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: object bindings begin";
        initObjectBindingsForGlobalScripts(context);
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: object bindings complete";
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: cell bindings begin";
        initCellBindingsForGlobalScripts(context);
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: cell bindings complete";
        std::map<std::string, sol::object> packages;
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: core begin";
        packages.emplace("openmw.core", initCorePackage(context));
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: types begin";
        packages.emplace("openmw.types", initTypesPackage(context));
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: world begin";
        packages.emplace("openmw.world", initWorldPackage(context));
        Log(Debug::Info) << "FNV/ESM4 Lua global packages: complete";
        return packages;
>>>>>>> origin/main
    }

    std::map<std::string, sol::object> initLocalPackages(const Context& context)
    {
        initObjectBindingsForLocalScripts(context);
        initCellBindingsForLocalScripts(context);
        LocalScripts::initializeSelfPackage(context);
        return {
            { "openmw.animation", initAnimationPackage(context) },
            { "openmw.core", initCorePackage(context) },
            { "openmw.types", initTypesPackage(context) },
            { "openmw.nearby", initNearbyPackage(context) },
        };
    }

    std::map<std::string, sol::object> initPlayerPackages(const Context& context)
    {
        return {
            { "openmw.ambient", initAmbientPackage(context) },
            { "openmw.camera", initCameraPackage(context.sol()) },
            { "openmw.debug", initDebugPackage(context) },
            { "openmw.input", initInputPackage(context) },
            { "openmw.postprocessing", initPostprocessingPackage(context) },
            { "openmw.ui", initUserInterfacePackage(context) },
<<<<<<< HEAD
=======
//## VR_PATCH BEGIN
            { "openmw.vr", initVRPackage(context) },
//## VR_PATCH END
>>>>>>> origin/main
        };
    }

    std::map<std::string, sol::object> initMenuPackages(const Context& context)
    {
        return {
            { "openmw.core", initCorePackage(context) },
            { "openmw.ambient", initAmbientPackage(context) },
            { "openmw.ui", initUserInterfacePackage(context) },
            { "openmw.menu", initMenuPackage(context) },
            { "openmw.input", initInputPackage(context) },
<<<<<<< HEAD
        };
    }

    std::map<std::string, sol::object> initLoadPackages(const Context& context)
    {
        return {
            { "openmw.core", initCorePackage(context) },
            { "openmw.content", initContentPackage(context) },
=======
            // ## VR_PATCH BEGIN
            { "openmw.vr", initVRPackage(context) },
            // ## VR_PATCH END
>>>>>>> origin/main
        };
    }
}
