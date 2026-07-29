#include "mwsecompatcompiler.hpp"

#include <algorithm>
#include <cctype>
#include <istream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/vfs/inmemoryarchive.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

namespace MWLua
{
    namespace
    {
        constexpr std::string_view sModsPrefix = "mwse/mods/";
        constexpr std::string_view sLibPrefix = "mwse/lib/";

        struct Metadata
        {
            std::string mName;
            std::string mVersion;
            std::string mHomepage;
            std::string mLuaMod;
        };

        struct Entrypoint
        {
            std::string mModule;
            std::string mSource;
            Metadata mMetadata;
        };

        bool endsWith(std::string_view value, std::string_view suffix)
        {
            return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
        }

        std::string readText(VFS::Manager& vfs, VFS::Path::NormalizedView path)
        {
            Files::IStreamPtr stream = vfs.get(path);
            return std::string(std::istreambuf_iterator<char>(*stream), {});
        }

        std::string trim(std::string value)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
            value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
            return value;
        }

        std::string parseTomlString(std::string_view line, std::string_view key)
        {
            const size_t equals = line.find('=');
            if (equals == std::string_view::npos || trim(std::string(line.substr(0, equals))) != key)
                return {};
            std::string value = trim(std::string(line.substr(equals + 1)));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                return value.substr(1, value.size() - 2);
            return {};
        }

        Metadata parseMetadata(std::string_view source)
        {
            Metadata metadata;
            std::string section;
            std::istringstream input{ std::string(source) };
            for (std::string line; std::getline(input, line);)
            {
                line = trim(std::move(line));
                if (line.empty() || line.front() == '#')
                    continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    section = line.substr(1, line.size() - 2);
                    continue;
                }
                if (section == "package")
                {
                    if (metadata.mName.empty())
                        metadata.mName = parseTomlString(line, "name");
                    if (metadata.mVersion.empty())
                        metadata.mVersion = parseTomlString(line, "version");
                    if (metadata.mHomepage.empty())
                        metadata.mHomepage = parseTomlString(line, "homepage");
                }
                else if (section == "tools.mwse" && metadata.mLuaMod.empty())
                    metadata.mLuaMod = parseTomlString(line, "lua-mod");
            }
            return metadata;
        }

        std::string moduleFromPath(std::string_view relativePath)
        {
            std::string module(relativePath);
            if (endsWith(module, ".lua"))
                module.resize(module.size() - 4);
            std::replace(module.begin(), module.end(), '/', '.');
            std::replace(module.begin(), module.end(), '\\', '.');
            return module;
        }

        std::string lowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string quoteLua(std::string_view value)
        {
            std::string result;
            result.reserve(value.size() + 2);
            result.push_back('"');
            for (const char c : value)
            {
                if (c == '\\' || c == '"')
                    result.push_back('\\');
                if (c == '\n')
                    result.append("\\n");
                else if (c == '\r')
                    result.append("\\r");
                else
                    result.push_back(c);
            }
            result.push_back('"');
            return result;
        }

        void mirrorLuaRoot(VFS::Manager& vfs, VFS::InMemoryArchive& out, std::string_view prefix,
            std::vector<Entrypoint>* entrypoints, const std::map<std::string, Metadata>& metadataByModule,
            std::map<std::string, std::set<std::string>>& directoryEntries, size_t& aliases)
        {
            for (const VFS::Path::Normalized& path : vfs.getRecursiveDirectoryIterator(prefix))
            {
                const std::string_view value = path.value();
                if (!endsWith(value, ".lua") || value.size() <= prefix.size())
                    continue;

                const std::string relative(value.substr(prefix.size()));
                out.addFile(VFS::Path::Normalized(relative), readText(vfs, path));
                ++aliases;
                if (const size_t slash = value.find_last_of('/'); slash != std::string_view::npos)
                    directoryEntries[lowerAscii(std::string(value.substr(0, slash + 1)))]
                        .insert(std::string(value.substr(slash + 1)));

                if (entrypoints == nullptr || !endsWith(relative, "/main.lua"))
                    continue;

                Entrypoint entrypoint;
                entrypoint.mModule = moduleFromPath(relative);
                entrypoint.mSource = std::string(value);
                const std::string moduleRoot
                    = entrypoint.mModule.substr(0, entrypoint.mModule.size() - std::string_view(".main").size());
                if (const auto it = metadataByModule.find(moduleRoot); it != metadataByModule.end())
                    entrypoint.mMetadata = it->second;
                if (entrypoint.mMetadata.mName.empty())
                    entrypoint.mMetadata.mName = moduleRoot;
                entrypoints->push_back(std::move(entrypoint));
            }
        }
    }

    ESM::LuaScriptsCfg compileMwseCompatMods(VFS::Manager& vfs, VFS::InMemoryArchive& out)
    {
        ESM::LuaScriptsCfg cfg;

        std::map<std::string, Metadata> metadataByModule;
        for (const VFS::Path::Normalized& path : vfs.getRecursiveDirectoryIterator())
        {
            const std::string_view value = path.value();
            if (!endsWith(value, "-metadata.toml"))
                continue;
            Metadata metadata = parseMetadata(readText(vfs, path));
            if (!metadata.mLuaMod.empty())
                metadataByModule.insert_or_assign(lowerAscii(metadata.mLuaMod), std::move(metadata));
        }

        std::vector<Entrypoint> entrypoints;
        std::map<std::string, std::set<std::string>> directoryEntries;
        size_t aliases = 0;
        mirrorLuaRoot(vfs, out, sModsPrefix, &entrypoints, metadataByModule, directoryEntries, aliases);
        mirrorLuaRoot(vfs, out, sLibPrefix, nullptr, metadataByModule, directoryEntries, aliases);
        if (entrypoints.empty())
            return cfg;

        std::sort(entrypoints.begin(), entrypoints.end(),
            [](const Entrypoint& lhs, const Entrypoint& rhs) { return lhs.mModule < rhs.mModule; });

        out.addFile(VFS::Path::Normalized("logging/logger.lua"),
            "return require('openmw_aux.mwse.runtime').logging\n");
        out.addFile(VFS::Path::Normalized("ssl/https.lua"),
            "return require('openmw_aux.mwse.runtime').https\n");
        out.addFile(VFS::Path::Normalized("mwse/mcm.lua"),
            "return require('openmw_aux.mwse.runtime').mcm\n");
        out.addFile(VFS::Path::Normalized("mwse/common.lua"),
            "return require('openmw_aux.mwse.runtime').common\n");
        out.addFile(VFS::Path::Normalized("inspect.lua"),
            "local inspect = require('openmw_aux.mwse.runtime').inspect\n"
            "return setmetatable({ inspect = inspect }, {\n"
            "    __call = function(_, ...) return inspect(...) end,\n"
            "})\n");

        std::ostringstream host;
        host << "-- generated shared MWSE-Lua compatibility host\n"
             << "local runtime = require('openmw_aux.mwse.runtime')\n"
             << "return runtime.bootstrap({\n";
        for (const Entrypoint& entrypoint : entrypoints)
        {
            host << "    { module = " << quoteLua(entrypoint.mModule) << ", source = "
                 << quoteLua(entrypoint.mSource) << ", name = " << quoteLua(entrypoint.mMetadata.mName)
                 << ", version = " << quoteLua(entrypoint.mMetadata.mVersion) << ", homepage = "
                 << quoteLua(entrypoint.mMetadata.mHomepage) << " },\n";
        }
        host << "}, {\n";
        for (const auto& [directory, entries] : directoryEntries)
        {
            host << "    [" << quoteLua(directory) << "] = {";
            for (const std::string& entry : entries)
                host << quoteLua(entry) << ", ";
            host << "},\n";
        }
        host << "})\n";

        out.addFile(VFS::Path::Normalized("generated/mwse/compat_host.lua"), host.str());
        out.addFile(VFS::Path::Normalized("generated/mwse/compat_global.lua"),
            R"lua(-- generated MWSE compatibility world mutation bridge
local util = require('openmw.util')
local world = require('openmw.world')

local function createReference(data)
    local player = world.players[tonumber(data.playerIndex) or 1]
    if player == nil then
        return
    end
    local object = world.createObject(tostring(data.recordId), tonumber(data.count) or 1)
    if data.scale ~= nil then
        object:setScale(tonumber(data.scale) or 1)
    end
    local position = data.position or {}
    local targetPosition = util.vector3(
        tonumber(position.x) or player.position.x,
        tonumber(position.y) or player.position.y,
        tonumber(position.z) or player.position.z)
    object:teleport(player.cell, targetPosition, {
        rotation = util.transform.rotateZ(tonumber(data.rotationZ) or 0),
        onGround = data.onGround == true,
    })
    player:sendEvent('MWSECompatReferenceCreated', {
        requestId = data.requestId,
        object = object,
    })
    print('MWSE compat world: state=reference-created request='
        .. tostring(data.requestId) .. ' record=' .. tostring(data.recordId))
end

return {
    eventHandlers = {
        MWSECompatCreateReference = createReference,
    },
}
)lua");
        vfs.buildIndex();

        ESM::LuaScriptCfg& globalCfg = cfg.mScripts.emplace_back();
        globalCfg.mScriptPath = VFS::Path::Normalized("generated/mwse/compat_global.lua");
        globalCfg.mFlags = ESM::LuaScriptCfg::sGlobal;

        ESM::LuaScriptCfg& hostCfg = cfg.mScripts.emplace_back();
        hostCfg.mScriptPath = VFS::Path::Normalized("generated/mwse/compat_host.lua");
        hostCfg.mFlags = ESM::LuaScriptCfg::sPlayer;

        Log(Debug::Info) << "MWSE compat: discovered " << entrypoints.size() << " entrypoint(s), mirrored "
                         << aliases << " Lua module(s), generated shared player host";
        for (const Entrypoint& entrypoint : entrypoints)
            Log(Debug::Info) << "MWSE compat: entrypoint=" << entrypoint.mModule
                             << " name=" << entrypoint.mMetadata.mName
                             << " version=" << entrypoint.mMetadata.mVersion
                             << " source=" << entrypoint.mSource;
        return cfg;
    }
}
