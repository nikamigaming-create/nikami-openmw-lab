#ifndef COMPONENTS_LUA_CONFIGURATION_H
#define COMPONENTS_LUA_CONFIGURATION_H

#include <map>
#include <optional>

#include <components/esm/luascripts.hpp>
#include <components/esm3/refnum.hpp>
#include <components/vfs/pathutil.hpp>

<<<<<<< HEAD
namespace ESM
{
    class ESMReader;
    class ESMWriter;
}

=======
>>>>>>> origin/main
namespace LuaUtil
{
    using ScriptIdsWithInitializationData = std::map<int, std::string_view>;

    class ScriptsConfiguration
    {
    public:
<<<<<<< HEAD
        void init(ESM::LuaScriptsCfg, bool);

        size_t size() const { return mScripts.size(); }
        const ESM::LuaScriptCfg& operator[](size_t id) const { return mScripts[id]; }

        std::optional<int> findId(VFS::Path::NormalizedView path) const;
        std::optional<int> mapId(int savedId) const;
=======
        void init(ESM::LuaScriptsCfg);

        size_t size() const { return mScripts.size(); }
        const ESM::LuaScriptCfg& operator[](int id) const { return mScripts[id]; }

        std::optional<int> findId(VFS::Path::NormalizedView path) const;
>>>>>>> origin/main

        bool isCustomScript(int id) const { return mScripts[id].mFlags & ESM::LuaScriptCfg::sCustom; }

        ScriptIdsWithInitializationData getMenuConf() const { return getConfByFlag(ESM::LuaScriptCfg::sMenu); }
        ScriptIdsWithInitializationData getGlobalConf() const { return getConfByFlag(ESM::LuaScriptCfg::sGlobal); }
        ScriptIdsWithInitializationData getPlayerConf() const { return getConfByFlag(ESM::LuaScriptCfg::sPlayer); }
<<<<<<< HEAD
        ScriptIdsWithInitializationData getLoadConf() const { return getConfByFlag(ESM::LuaScriptCfg::sLoad); }
        ScriptIdsWithInitializationData getLocalConf(
            uint32_t type, const ESM::RefId& recordId, ESM::RefNum refnum) const;

        void read(ESM::ESMReader&);
        void write(ESM::ESMWriter&) const;

=======
        ScriptIdsWithInitializationData getLocalConf(
            uint32_t type, const ESM::RefId& recordId, ESM::RefNum refnum) const;

>>>>>>> origin/main
    private:
        ScriptIdsWithInitializationData getConfByFlag(ESM::LuaScriptCfg::Flags flag) const;

        std::vector<ESM::LuaScriptCfg> mScripts;
        std::map<VFS::Path::Normalized, int, std::less<>> mPathToIndex;

        struct DetailedConf
        {
            int mScriptId;
            bool mAttach;
            std::string_view mInitializationData;
        };
        std::map<uint32_t, std::vector<int>> mScriptsPerType;
        std::map<ESM::RefId, std::vector<DetailedConf>, std::less<>> mScriptsPerRecordId;
        std::map<ESM::RefNum, std::vector<DetailedConf>> mScriptsPerRefNum;
<<<<<<< HEAD
        std::map<int, int> mScriptIdMapping;
=======
>>>>>>> origin/main
    };

    // Parse ESM::LuaScriptsCfg from text and add to `cfg`.
    void parseOMWScripts(ESM::LuaScriptsCfg& cfg, std::string_view data);

    std::string scriptCfgToString(const ESM::LuaScriptCfg& script);

}

#endif // COMPONENTS_LUA_CONFIGURATION_H
