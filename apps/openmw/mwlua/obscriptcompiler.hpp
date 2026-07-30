#ifndef MWLUA_OBSCRIPTCOMPILER_H
#define MWLUA_OBSCRIPTCOMPILER_H

#include <components/esm/luascripts.hpp>

namespace LuaUtil
{
    class LuaState;
}

namespace VFS
{
    class Manager;
    class InMemoryArchive;
}

namespace MWLua
{
    // Compiles every ESM4 script record (SCPT) into a Lua file under
    // generated/obscript/ in the given archive and rebuilds the VFS index.
    // Failures are logged and skipped. Returns a config attaching each
    // generated script (as sCustom) to the base records referencing it via SCRI.
    ESM::LuaScriptsCfg compileObScripts(LuaUtil::LuaState& lua, VFS::Manager& vfs, VFS::InMemoryArchive& out);
}

#endif // MWLUA_OBSCRIPTCOMPILER_H
