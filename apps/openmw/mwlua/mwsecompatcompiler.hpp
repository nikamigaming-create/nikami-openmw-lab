#ifndef OPENMW_MWLUA_MWSECOMPATCOMPILER_H
#define OPENMW_MWLUA_MWSECOMPATCOMPILER_H

#include <components/esm/luascripts.hpp>

namespace VFS
{
    class InMemoryArchive;
    class Manager;
}

namespace MWLua
{
    // Discovers legacy MWSE-Lua entrypoints in the VFS, mirrors the MWSE module
    // roots into OpenMW's module namespace, and emits one shared player host.
    //
    // A shared host is intentional: MWSE mods expect one Lua runtime with
    // process-wide globals and cross-mod require()/event interop.
    ESM::LuaScriptsCfg compileMwseCompatMods(VFS::Manager& vfs, VFS::InMemoryArchive& out);
}

#endif
