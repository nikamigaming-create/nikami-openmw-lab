#include "obscriptcompiler.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <sol/environment.hpp>
#include <sol/state_view.hpp>
#include <sol/table.hpp>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/lua/luastate.hpp>
#include <components/vfs/inmemoryarchive.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/store.hpp"

namespace MWLua
{
    namespace
    {
        // The compiler modules are engine-internal: they run in an internal
        // lib environment (see lua_ui/content.cpp for the pattern), with
        // `require` resolving modules from the VFS so that the
        // openmw_aux/obscript/* modules can find each other.
        sol::table loadCompiler(LuaUtil::LuaState& lua, sol::state_view& sol)
        {
            sol::environment env = lua.newInternalLibEnvironment();
            sol::table loaded(sol, sol::create);
            env["require"] = [&lua, env, loaded](std::string_view name) mutable -> sol::object {
                sol::object cached = loaded[name];
                if (cached != sol::nil)
                    return cached;
                std::string path(name);
                std::replace(path.begin(), path.end(), '.', '/');
                path += ".lua";
                sol::protected_function loader = lua.loadFromVFS(VFS::Path::Normalized(path));
                sol::set_environment(env, loader);
                sol::protected_function_result result = loader();
                if (!result.valid())
                    throw std::runtime_error("Failed to load " + path + ": " + result.get<sol::error>().what());
                loaded[name] = result.get<sol::object>();
                return loaded[name];
            };
            sol::protected_function loader = lua.loadFromVFS(VFS::Path::Normalized("openmw_aux/obscript/compiler.lua"));
            sol::set_environment(env, loader);
            sol::protected_function_result result = loader();
            if (!result.valid())
                throw std::runtime_error(
                    std::string("Failed to load ObScript compiler: ") + result.get<sol::error>().what());
            return result.get<sol::table>();
        }
    }

    int compileObScripts(LuaUtil::LuaState& lua, VFS::Manager& vfs, VFS::InMemoryArchive& out)
    {
        const MWWorld::Store<ESM4::Script>& scripts = MWBase::Environment::get().getESMStore()->get<ESM4::Script>();
        if (scripts.getSize() == 0)
            return 0;

        int generated = 0;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::state_view sol = view.sol();
            sol::table compiler = loadCompiler(lua, sol);
            const sol::protected_function compile = compiler.get<sol::protected_function>("compile");

            for (size_t i = 0; i < scripts.getSize(); ++i)
            {
                const ESM4::Script& record = *scripts.at(i);
                if (record.mScript.scriptSource.empty())
                    continue;
                sol::protected_function_result result = compile(record.mScript.scriptSource);
                if (!result.valid())
                {
                    Log(Debug::Warning) << "Failed to compile ObScript " << record.mEditorId << ": "
                                        << result.get<sol::error>().what();
                    continue;
                }
                out.addFile(VFS::Path::Normalized("generated/obscript/" + record.mEditorId + ".lua"),
                    result.get<std::string>());
                ++generated;
            }
        });

        vfs.buildIndex();
        Log(Debug::Info) << "Compiled " << generated << " ObScript records to Lua";
        return generated;
    }
}
