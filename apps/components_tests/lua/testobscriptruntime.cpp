#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <components/lua/luastate.hpp>
#include <components/testing/expecterror.hpp>
#include <components/testing/util.hpp>

namespace
{
    using namespace testing;

    std::string readDataFile(const char* name)
    {
        const auto path = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "data" / name;
        std::ifstream stream(path);
        if (!stream)
            throw std::runtime_error("test data file not found: " + path.string());
        std::stringstream buf;
        buf << stream.rdbuf();
        return buf.str();
    }

    constexpr VFS::Path::NormalizedView runtimePath("openmw_aux/obscript/runtime.lua");
    TestingOpenMW::VFSTestFile runtimeFile(readDataFile("openmw_aux/obscript/runtime.lua"));
    constexpr VFS::Path::NormalizedView bindingsPath("openmw_aux/obscript/bindings.lua");
    TestingOpenMW::VFSTestFile bindingsFile(readDataFile("openmw_aux/obscript/bindings.lua"));

    // The driver simulates what transpiled scripts and the engine host do:
    // register handlers, install bindings, read/write variables, fire events.
    constexpr VFS::Path::NormalizedView driverPath("obscript/runtimetests.lua");
    TestingOpenMW::VFSTestFile driverFile(R"X(
        local obs = require('openmw_aux.obscript.runtime')
        return {
            obs = obs,

            loadScript = function(name, handlers)
                -- what a transpiled script does at load time
                local S = obs.locals(name)
                for event, fn in pairs(handlers or {}) do
                    obs.on(event, fn)
                end
                return S
            end,

            truthiness = function()
                return obs.b(0), obs.b(1), obs.b(-2.5), obs.b(nil), obs.b(true)
            end,

            zeroInit = function()
                local S = obs.locals('ZeroInitScript')
                return S.neverSet
            end,

            crossScript = function()
                local S = obs.locals('QuestA')
                S.stage = 7
                return obs.mv('questa', 'stage'), obs.mv('NoSuchScript', 'x')
            end,

            setvRoundTrip = function()
                obs.setv('SomeGlobal', 42)
                return obs.v('someglobal'), obs.v('NeverSetGlobal')
            end,

            msetv = function()
                obs.msetv('QuestB', 'flag', 3)
                return obs.mv('QuestB', 'flag')
            end,

            bindAndDispatch = function()
                local got = nil
                obs.bind('TestCmd', function(a, b) got = { a, b } return 99 end)
                local result = obs.f('testcmd', 'x', 2)
                return result, got[1], got[2]
            end,

            memberDispatchResolvesRef = function()
                obs.resolveRef = function(x) return 'resolved:' .. tostring(x) end
                local seen = nil
                obs.bind('MemberCmd', function(ref) seen = ref return 0 end)
                obs.m('SomeRef', 'MemberCmd')
                obs.resolveRef = function(x) return x end
                return seen
            end,

            unknownCommandIsZero = function()
                local before = obs._unknown['neverimplemented'] or 0
                local value = obs.f('NeverImplemented', 1, 2, 3)
                return value, (obs._unknown['neverimplemented'] or 0) - before
            end,

            fireEvents = function(log)
                obs.locals('FireScript')
                obs.on('OnActivate', function() log[#log + 1] = 'activated' end)
                -- two handlers for one event: both must run, in order
                obs.on('OnActivate', function() log[#log + 1] = 'second' end)
                local hit = obs.fire('firescript', 'onactivate')
                local miss = obs.fire('firescript', 'OnDeath')
                local noScript = obs.fire('NoSuchScript', 'OnActivate')
                return hit, miss, noScript
            end,

            frameRunsGameMode = function(log)
                obs.locals('FrameScript')
                obs.on('GameMode', function() log[#log + 1] = 'tick' end)
                obs.frame()
                obs.frame()
                return #log
            end,

            coverage = function()
                obs.f('CoverageProbeCmd')
                obs.f('CoverageProbeCmd')
                return obs.coverageReport(50)
            end,

            makeLocalScript = function(log)
                obs.locals('LocalIfaceScript')
                obs.on('GameMode', function() log[#log + 1] = 'update' end)
                obs.on('OnActivate', function()
                    log[#log + 1] = 'activated:' .. tostring(obs._actionRef)
                end)
                obs.on('OnLoad', function() log[#log + 1] = 'loaded' end)
                local script = obs.makeLocalScript()
                local h = script.engineHandlers
                if type(h) ~= 'table' then
                    return 'no engineHandlers'
                end
                h.onActive()
                h.onUpdate(0.25)
                h.onActivated('someactor')
                local dtSeen = obs._dt
                local save = h.onSave()
                return #log, log[1], log[2], log[3], dtSeen, type(save.locals)
            end,

            makeLocalScriptEmpty = function()
                -- fresh runtime per sandbox in the engine; here simulate by
                -- checking a script with no blocks yields usable handlers
                obs.locals('EmptyIfaceScript')
                local script = obs.makeLocalScript()
                return type(script) == 'table' and 1 or 0
            end,
        }
        )X");

    constexpr VFS::Path::NormalizedView bindingsDriverPath("obscript/bindingstests.lua");
    TestingOpenMW::VFSTestFile bindingsDriverFile(R"X(
        local nearby = require('openmw.nearby')
        local obs = require('openmw_aux.obscript.bindings')

        return {
            queries = function()
                return obs.f('GetDisabled'),
                    obs.m('PlacedRef', 'GetDisabled'),
                    obs.m('MissingRef', 'GetDisabled'),
                    obs.m('PlacedRef', 'GetDead'),
                    obs.m('player', 'GetDead'),
                    obs.m('player', 'GetItemCount', 'AmmoItem'),
                    obs.m('CrateRef', 'GetItemCount', 'AmmoItem'),
                    obs.m('player', 'GetItemCount', 'MissingItem')
            end,

            commonQueries = function()
                obs._actionRef = nearby.players[1]
                local selfId = obs.f('GetSelf').id
                local actionRefId = obs.f('GetActionRef').id
                obs._actionRef = nil
                return obs.f('GameDaysPassed'),
                    obs.f('GetCurrentTime'),
                    selfId,
                    actionRefId,
                    obs.m('player', 'GetEquipped', 'AmmoItem'),
                    obs.m('player', 'GetEquipped', 'MissingItem'),
                    obs.f('GetDistance', 'PlacedRef'),
                    obs.m('player', 'GetDistance', 'PlacedRef')
            end,

            harvestActivation = function(events)
                local S = obs.locals('BarrelCactusScript')
                obs.on('OnActivate', function()
                    if obs.b((S.State == 0) and obs.b(
                        (obs.v('GetActionRef') == obs.v('player')))) then
                        obs.m('player', 'AddItem', 'NVFreshBarrelCactusFruit', 1)
                        S.State = 1
                    end
                end)
                local script = obs.makeLocalScript()
                script.engineHandlers.onActivated(nearby.players[1])
                return #events, events[1].name, events[1].data.item,
                    events[1].data.count, S.State
            end,

            existingBindings = function(events)
                obs.m('PlacedRef', 'Enable')
                obs.m('player', 'AddItem', 'AmmoItem', 2)
                obs._actionRef = nearby.players[1]
                local isPlayer = obs.m('player', 'IsActionRef')
                obs._actionRef = nil
                return events[1].name, events[1].data.object.id,
                    events[2].name, events[2].data.item, events[2].data.count,
                    isPlayer
            end,
        }
        )X");

    constexpr VFS::Path::NormalizedView bindingsFactoryPath("obscript/bindingsfactory.lua");
    TestingOpenMW::VFSTestFile bindingsFactoryFile(R"X(
        local sameCell = {}
        sameCell.isInSameSpace = function(_, obj) return obj.cell == sameCell end

        local function object(id, kind, enabled, dead, itemCount, player, x, y, z)
            local obj = {
                id = id,
                kind = kind,
                enabled = enabled,
                dead = dead,
                itemCount = itemCount or 0,
                player = player or false,
                cell = sameCell,
                position = { x = x or 0, y = y or 0, z = z or 0 },
            }
            obj.isValid = function() return true end
            return obj
        end

        local own = object('self', 'container', false, false, 1)
        local placed = object('placed', 'actor', false, true, 3, false, 3, 4, 0)
        local player = object('player', 'actor', true, false, 12, true, 3, 0, 0)
        local crate = object('crate', 'container', true, false, 4)
        local byFormId = {
            ['form:placed'] = placed,
            ['form:crate'] = crate,
        }
        local events = {}

        local function inventory(obj)
            return {
                countOf = function(_, recordId)
                    if recordId == 'record:ammo' then return obj.itemCount end
                    return 0
                end,
            }
        end

        local core = {
            getGameTime = function() return 90000 end,
            obscript = {
                resolveRefEditorId = function(editorId)
                    local refs = { placedref = 'form:placed', crateref = 'form:crate' }
                    return refs[editorId:lower()]
                end,
                resolveItemEditorId = function(editorId)
                    local items = {
                        ammoitem = 'record:ammo',
                        nvfreshbarrelcactusfruit = 'record:cactus',
                    }
                    return items[editorId:lower()]
                end,
            },
            sendGlobalEvent = function(name, data)
                events[#events + 1] = { name = name, data = data }
            end,
        }
        local nearby = {
            players = { player },
            getObjectByFormId = function(formId) return byFormId[formId] end,
        }
        local types = {
            Actor = {
                objectIsInstance = function(obj) return obj.kind == 'actor' end,
                isDead = function(obj) return obj.dead end,
                inventory = inventory,
                getEquipment = function(obj)
                    if obj.player then
                        return { [0] = { recordId = 'record:ammo' } }
                    end
                    return {}
                end,
            },
            Container = {
                objectIsInstance = function(obj) return obj.kind == 'container' end,
                inventory = inventory,
            },
            Player = {
                objectIsInstance = function(obj) return obj.player end,
            },
        }
        return {
            packages = {
                ['openmw.core'] = core,
                ['openmw.nearby'] = nearby,
                ['openmw.self'] = { object = own },
                ['openmw.types'] = types,
            },
            events = events,
        }
        )X");

    struct ObScriptRuntimeTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { runtimePath, &runtimeFile },
            { bindingsPath, &bindingsFile },
            { driverPath, &driverFile },
            { bindingsDriverPath, &bindingsDriverFile },
            { bindingsFactoryPath, &bindingsFactoryFile },
        });

        LuaUtil::ScriptsConfiguration mCfg;
        LuaUtil::LuaState mLua{ mVFS.get(), &mCfg };

        sol::table script()
        {
            const VFS::Path::Normalized path(driverPath);
            return mLua.runInNewSandbox(path);
        }
    };

    TEST_F(ObScriptRuntimeTest, Truthiness)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["truthiness"]).get<std::tuple<bool, bool, bool, bool, bool>>();
        EXPECT_FALSE(std::get<0>(r)); // 0
        EXPECT_TRUE(std::get<1>(r)); // 1
        EXPECT_TRUE(std::get<2>(r)); // -2.5 (any nonzero number)
        EXPECT_FALSE(std::get<3>(r)); // nil
        EXPECT_TRUE(std::get<4>(r)); // true
    }

    TEST_F(ObScriptRuntimeTest, VariablesZeroInitialized)
    {
        EXPECT_EQ(LuaUtil::call(script()["zeroInit"]).get<int>(), 0);
    }

    TEST_F(ObScriptRuntimeTest, CrossScriptVariables)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["crossScript"]).get<std::tuple<int, int>>();
        EXPECT_EQ(std::get<0>(r), 7); // case-insensitive script + var lookup
        EXPECT_EQ(std::get<1>(r), 0); // unknown script reads as 0
    }

    TEST_F(ObScriptRuntimeTest, GlobalVariables)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["setvRoundTrip"]).get<std::tuple<int, int>>();
        EXPECT_EQ(std::get<0>(r), 42);
        EXPECT_EQ(std::get<1>(r), 0); // unknown global reads as 0
        EXPECT_EQ(LuaUtil::call(s["msetv"]).get<int>(), 3);
    }

    TEST_F(ObScriptRuntimeTest, BindAndDispatch)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["bindAndDispatch"]).get<std::tuple<int, std::string, int>>();
        EXPECT_EQ(std::get<0>(r), 99); // binding return value
        EXPECT_EQ(std::get<1>(r), "x"); // args pass through; name case-insensitive
        EXPECT_EQ(std::get<2>(r), 2);
        EXPECT_EQ(LuaUtil::call(s["memberDispatchResolvesRef"]).get<std::string>(), "resolved:SomeRef");
    }

    TEST_F(ObScriptRuntimeTest, UnknownCommandsStubToZero)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["unknownCommandIsZero"]).get<std::tuple<int, int>>();
        EXPECT_EQ(std::get<0>(r), 0); // unimplemented command evaluates to 0
        EXPECT_EQ(std::get<1>(r), 1); // and is counted once
    }

    TEST_F(ObScriptRuntimeTest, FireAndFrame)
    {
        sol::table s = script();
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table log = view.sol().create_table();
            auto r = LuaUtil::call(s["fireEvents"], log).get<std::tuple<bool, bool, bool>>();
            EXPECT_TRUE(std::get<0>(r)); // event with handlers fires
            EXPECT_FALSE(std::get<1>(r)); // event without handlers
            EXPECT_FALSE(std::get<2>(r)); // unknown script
            ASSERT_EQ(log.size(), 2); // both handlers ran, registration order
            EXPECT_EQ(log[1].get<std::string>(), "activated");
            EXPECT_EQ(log[2].get<std::string>(), "second");

            sol::table frames = view.sol().create_table();
            EXPECT_EQ(LuaUtil::call(s["frameRunsGameMode"], frames).get<int>(), 2);
        });
    }

    TEST_F(ObScriptRuntimeTest, CoverageReport)
    {
        const std::string report = LuaUtil::call(script()["coverage"]).get<std::string>();
        EXPECT_THAT(report, HasSubstr("2  coverageprobecmd"));
    }

    TEST_F(ObScriptRuntimeTest, MakeLocalScript)
    {
        sol::table s = script();
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table log = view.sol().create_table();
            auto r = LuaUtil::call(s["makeLocalScript"], log)
                         .get<std::tuple<int, std::string, std::string, std::string, double, std::string>>();
            EXPECT_EQ(std::get<0>(r), 3); // OnLoad, GameMode, and OnActivate fired
            EXPECT_EQ(std::get<1>(r), "loaded");
            EXPECT_EQ(std::get<2>(r), "update");
            EXPECT_EQ(std::get<3>(r), "activated:someactor");
            EXPECT_DOUBLE_EQ(std::get<4>(r), 0.25); // dt captured for GetSecondsPassed
            EXPECT_EQ(std::get<5>(r), "table"); // onSave returns serializable locals
        });
    }

    TEST_F(ObScriptRuntimeTest, MakeLocalScriptWithoutBlocks)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["makeLocalScriptEmpty"]).get<int>();
        EXPECT_EQ(r, 1);
    }

    TEST_F(ObScriptRuntimeTest, ObjectQueryBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-bindings-test", extraPackages);
            const auto values
                = LuaUtil::call(s["queries"]).get<std::tuple<int, int, int, int, int, int, int, int>>();
            EXPECT_EQ(std::get<0>(values), 1); // disabled script owner
            EXPECT_EQ(std::get<1>(values), 1); // disabled placed actor
            EXPECT_EQ(std::get<2>(values), 0); // missing reference is safe
            EXPECT_EQ(std::get<3>(values), 1); // dead placed actor
            EXPECT_EQ(std::get<4>(values), 0); // living player
            EXPECT_EQ(std::get<5>(values), 12); // actor inventory
            EXPECT_EQ(std::get<6>(values), 4); // container inventory
            EXPECT_EQ(std::get<7>(values), 0); // unknown item
        });
    }

    TEST_F(ObScriptRuntimeTest, ResolverPreservesExistingBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-bindings-test", extraPackages);
            const auto values = LuaUtil::call(s["existingBindings"], factory["events"])
                                    .get<std::tuple<std::string, std::string, std::string, std::string, int, int>>();
            EXPECT_EQ(std::get<0>(values), "ObScriptSetEnabled");
            EXPECT_EQ(std::get<1>(values), "placed");
            EXPECT_EQ(std::get<2>(values), "ObScriptAddItem");
            EXPECT_EQ(std::get<3>(values), "AmmoItem");
            EXPECT_EQ(std::get<4>(values), 2);
            EXPECT_EQ(std::get<5>(values), 1);
        });
    }

    TEST_F(ObScriptRuntimeTest, CommonReadOnlyBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-bindings-test", extraPackages);
            const auto values = LuaUtil::call(s["commonQueries"])
                                    .get<std::tuple<double, double, std::string, std::string, int, int, double, double>>();
            EXPECT_DOUBLE_EQ(std::get<0>(values), 90000.0 / 86400.0);
            EXPECT_DOUBLE_EQ(std::get<1>(values), 1.0);
            EXPECT_EQ(std::get<2>(values), "self");
            EXPECT_EQ(std::get<3>(values), "player");
            EXPECT_EQ(std::get<4>(values), 1);
            EXPECT_EQ(std::get<5>(values), 0);
            EXPECT_DOUBLE_EQ(std::get<6>(values), 5.0);
            EXPECT_DOUBLE_EQ(std::get<7>(values), 4.0);
        });
    }

    TEST_F(ObScriptRuntimeTest, HarvestActivationAddsRetailIngredient)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-harvest-test", extraPackages);
            const auto values = LuaUtil::call(s["harvestActivation"], factory["events"])
                                    .get<std::tuple<int, std::string, std::string, int, int>>();
            EXPECT_EQ(std::get<0>(values), 1);
            EXPECT_EQ(std::get<1>(values), "ObScriptAddItem");
            EXPECT_EQ(std::get<2>(values), "NVFreshBarrelCactusFruit");
            EXPECT_EQ(std::get<3>(values), 1);
            EXPECT_EQ(std::get<4>(values), 1);
        });
    }

}
