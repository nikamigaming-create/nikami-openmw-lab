#include "obscriptcompiler.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sol/environment.hpp>
#include <sol/state_view.hpp>
#include <sol/table.hpp>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadflor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadterm.hpp>
#include <components/esm4/loadweap.hpp>
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

        // Calls fn(recordFormId, scriptFormId) for every record of the given
        // types that links a script via SCRI.
        template <class... RecordTypes, class Fn>
        void forEachScriptedRecord(const MWWorld::ESMStore& store, Fn&& fn)
        {
            (
                [&] {
                    const MWWorld::Store<RecordTypes>& s = store.get<RecordTypes>();
                    for (size_t i = 0; i < s.getSize(); ++i)
                    {
                        const RecordTypes& record = *s.at(i);
                        if (record.mScriptId != ESM::FormId())
                            fn(record.mId, record.mScriptId);
                    }
                }(),
                ...);
        }

        bool hasXnvseUserFunction(std::string source)
        {
            std::ranges::transform(source, source.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return source.find("begin function") != std::string::npos;
        }

        std::string quoteLuaString(std::string_view value)
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
    }

    ESM::LuaScriptsCfg compileObScripts(LuaUtil::LuaState& lua, VFS::Manager& vfs, VFS::InMemoryArchive& out)
    {
        ESM::LuaScriptsCfg cfg;
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const MWWorld::Store<ESM4::Script>& scripts = store.get<ESM4::Script>();
        if (scripts.getSize() == 0)
            return cfg;

        std::map<ESM::FormId, size_t> cfgIndexByScript; // SCPT FormId -> index into cfg.mScripts
        struct CompiledScript
        {
            std::string mEditorId;
            std::string mRegistration;
        };
        std::map<ESM::FormId, CompiledScript> compiledScripts;
        std::set<int32_t> xnvseContentFiles;
        int generated = 0;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::state_view sol = view.sol();
            sol::table compiler = loadCompiler(lua, sol);
            const sol::protected_function compile
                = compiler.get<sol::protected_function>("compileRegistration");

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
                std::string registration = result.get<std::string>();
                out.addFile(VFS::Path::Normalized("generated/obscript/" + record.mEditorId + ".lua"),
                    registration + "return obs.makeLocalScript()\n");
                ++generated;
                compiledScripts.emplace(
                    record.mId, CompiledScript{ record.mEditorId, std::move(registration) });
                if (record.mId.hasContentFile() && hasXnvseUserFunction(record.mScript.scriptSource))
                    xnvseContentFiles.insert(record.mId.mContentFile);

                ESM::LuaScriptCfg& scriptCfg = cfg.mScripts.emplace_back();
                scriptCfg.mScriptPath = VFS::Path::Normalized("generated/obscript/" + record.mEditorId + ".lua");
                scriptCfg.mFlags = ESM::LuaScriptCfg::sCustom;
                cfgIndexByScript[record.mId] = cfg.mScripts.size() - 1;
            }
        });

        // Attach each generated script to the base records referencing it via SCRI.
        std::set<ESM::FormId> attachedScripts;
        forEachScriptedRecord<ESM4::Activator, ESM4::Armor, ESM4::Book, ESM4::Clothing, ESM4::Container, ESM4::Creature,
            ESM4::Door, ESM4::Flora, ESM4::Furniture, ESM4::Ingredient, ESM4::ItemMod, ESM4::LevelledCreature,
            ESM4::Light, ESM4::MiscItem, ESM4::Npc, ESM4::Potion, ESM4::Terminal, ESM4::Weapon>(
            store, [&](ESM::FormId recordId, ESM::FormId scriptId) {
                auto it = cfgIndexByScript.find(scriptId);
                if (it == cfgIndexByScript.end())
                    return;
                attachedScripts.insert(scriptId);
                cfg.mScripts[it->second].mRecords.push_back(
                    ESM::LuaScriptCfg::PerRecordCfg{ true, ESM::RefId::formIdRefId(recordId), {} });
            });

        // xNVSE quest mods keep quest scripts and their UDF records in one VM.
        // A script record containing Begin Function marks its content file as
        // extender-authored; all of that file's unattached records are loaded
        // into one player sandbox, while QUST state decides which GameMode
        // blocks actually tick.
        struct QuestHostRow
        {
            std::string mScript;
            std::string mQuest;
            float mDelay = 0;
            bool mStartEnabled = false;
        };
        std::vector<QuestHostRow> questRows;
        const MWWorld::Store<ESM4::Quest>& quests = store.get<ESM4::Quest>();
        for (size_t i = 0; i < quests.getSize(); ++i)
        {
            const ESM4::Quest& quest = *quests.at(i);
            const auto scriptIt = compiledScripts.find(quest.mQuestScript);
            if (scriptIt == compiledScripts.end() || !quest.mQuestScript.hasContentFile()
                || !xnvseContentFiles.contains(quest.mQuestScript.mContentFile))
                continue;
            questRows.push_back(
                { scriptIt->second.mEditorId, quest.mEditorId, std::max(0.f, quest.mData.questDelay),
                    (quest.mData.flags & ESM4::Quest::Flag_StartGameEnabled) != 0 });
        }

        std::ostringstream host;
        size_t hostScriptCount = 0;
        host << "-- generated shared ESM4/xNVSE quest and UDF host\n";
        for (const auto& [formId, script] : compiledScripts)
        {
            if (!formId.hasContentFile() || !xnvseContentFiles.contains(formId.mContentFile)
                || attachedScripts.contains(formId))
                continue;
            host << script.mRegistration << '\n';
            ++hostScriptCount;
        }
        if (hostScriptCount > 0 && !questRows.empty())
        {
            host << "local __obsHost = require('openmw_aux.obscript.runtime')\n";
            host << "return __obsHost.makeQuestHost({\n";
            for (const QuestHostRow& row : questRows)
            {
                host << "    { script = " << quoteLuaString(row.mScript) << ", quest = "
                     << quoteLuaString(row.mQuest) << ", delay = " << row.mDelay
                     << ", startEnabled = " << (row.mStartEnabled ? "true" : "false") << " },\n";
            }
            host << "})\n";
            out.addFile(VFS::Path::Normalized("generated/obscript/xnvse_quest_host.lua"), host.str());
        }

        const size_t attachedCount
            = std::ranges::count_if(cfg.mScripts, [](const ESM::LuaScriptCfg& s) { return !s.mRecords.empty(); });
        std::erase_if(cfg.mScripts, [](const ESM::LuaScriptCfg& s) { return s.mRecords.empty(); });
        if (hostScriptCount > 0 && !questRows.empty())
        {
            ESM::LuaScriptCfg& hostCfg = cfg.mScripts.emplace_back();
            hostCfg.mScriptPath = VFS::Path::Normalized("generated/obscript/xnvse_quest_host.lua");
            hostCfg.mFlags = ESM::LuaScriptCfg::sPlayer;
        }

        vfs.buildIndex();
        Log(Debug::Info) << "Compiled " << generated << " ObScript records to Lua, " << attachedCount
                         << " attached to base records, " << hostScriptCount << " in xNVSE shared host, "
                         << questRows.size() << " quest loops";
        return cfg;
    }
}
