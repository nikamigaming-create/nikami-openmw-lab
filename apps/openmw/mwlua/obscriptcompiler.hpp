#ifndef MWLUA_OBSCRIPTCOMPILER_H
#define MWLUA_OBSCRIPTCOMPILER_H

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
    // Translates every ESM4 script record (SCPT) in the ESM store into a Lua
    // script file under generated/obscript/ in the given archive, using
    // openmw_aux/obscript/compiler.lua, and rebuilds the VFS index so the
    // generated files are visible. Scripts that fail to compile are logged
    // and skipped. Returns the number of scripts generated.
    int compileObScripts(LuaUtil::LuaState& lua, VFS::Manager& vfs, VFS::InMemoryArchive& out);
}

#endif // MWLUA_OBSCRIPTCOMPILER_H
