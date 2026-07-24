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
    constexpr VFS::Path::NormalizedView retailCoveragePath("openmw_aux/obscript/fnv_retail_coverage.lua");
    TestingOpenMW::VFSTestFile retailCoverageFile(readDataFile("openmw_aux/obscript/fnv_retail_coverage.lua"));
    constexpr VFS::Path::NormalizedView globalBindingsPath("scripts/omw/obscript.lua");
    TestingOpenMW::VFSTestFile globalBindingsFile(readDataFile("scripts/omw/obscript.lua"));

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
                local reports = {}
                obs._log = function(command, script)
                    reports[#reports + 1] = command .. ':' .. script
                end
                obs.locals('UnknownCommandScript')
                local value = obs.f('NeverImplemented', 1, 2, 3)
                obs.f('NeverImplemented')
                obs._log = nil
                return value, (obs._unknown['neverimplemented'] or 0) - before,
                    #reports, reports[1]
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
                obs.on('OnReset', function() log[#log + 1] = 'reset' end)
                local script = obs.makeLocalScript()
                local h = script.engineHandlers
                if type(h) ~= 'table' then
                    return 'no engineHandlers'
                end
                h.onActive()
                h.onUpdate(0.25)
                h.onActivated('someactor')
                h.onReset()
                local dtSeen = obs._dt
                local save = h.onSave()
                return #log, log[1], log[2], log[3], log[4], dtSeen, type(save.locals)
            end,

            makeLocalScriptEmpty = function()
                -- fresh runtime per sandbox in the engine; here simulate by
                -- checking a script with no blocks yields usable handlers but
                -- does not claim authored activation ownership.
                obs.locals('EmptyIfaceScript')
                local script = obs.makeLocalScript()
                return type(script) == 'table' and 1 or 0,
                    script.engineHandlers.onActivated == nil
            end,

            filteredGameplayEvents = function(log)
                obs.locals('FilteredGameplayEvents')
                obs.on('OnDeath', function() log[#log + 1] = 'death:any' end)
                obs.on('OnDeath', function() log[#log + 1] = 'death:player' end, 'player')
                obs.on('OnHit', function() log[#log + 1] = 'hit:any' end)
                obs.on('OnHit', function() log[#log + 1] = 'hit:attacker' end, 'attacker')
                obs.on('OnHit', function() log[#log + 1] = 'hit:other' end, 'other')
                obs.on('OnHitWith', function() log[#log + 1] = 'hitwith:weapon' end, 'WeaponEditor')
                obs.on('OnActivate', function() log[#log + 1] = 'activate:actor' end, 'actor')
                obs.on('OnActivate', function() log[#log + 1] = 'activate:other' end, 'other')
                obs.on('OnTriggerEnter', function()
                    log[#log + 1] = 'enter:' .. tostring(obs._actionRef)
                end, 'actor')
                obs.on('OnTriggerEnter', function() log[#log + 1] = 'enter:other' end, 'other')
                obs.on('OnTriggerLeave', function() log[#log + 1] = 'leave:any' end)
                obs.resolveRecordId = function(value)
                    if value == 'WeaponEditor' then return 'weapon-form' end
                    if type(value) == 'table' then return value.recordId end
                    return value
                end
                local script = obs.makeLocalScript()
                local events = script.eventHandlers
                events.Died({})
                events.Died({ killer = 'player' })
                events.Hit({ attacker = 'attacker', weapon = { recordId = 'weapon-form' } })
                script.engineHandlers.onActivated('actor')
                script.engineHandlers.onTriggerEnter('actor')
                script.engineHandlers.onTriggerLeave('actor')
                return #log
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
                    obs.m('PlacedRef', 'GetUnconscious'),
                    obs.m('player', 'GetUnconscious'),
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

            menuAnimationAndMessage = function()
                return obs.f('MenuMode'),
                    obs.f('GetButtonPressed'),
                    obs.f('ShowMessage', 'VCG04Message'),
                    obs.f('IsAnimPlaying', 'Left'),
                    obs.m('PlacedRef', 'IsAnimPlaying', 'Right')
            end,

            retailActivation = function(mutations)
                obs._actionRef = nearby.players[1]
                obs.f('Activate')
                obs._actionRef = nil
                local mutation = mutations[#mutations]
                return mutation.name, mutation.args[1].id, mutation.args[2].id
            end,

            questQueries = function()
                return {
                    stage = obs.f('GetStage', 'VMQ01'),
                    stageDone = obs.f('GetStageDone', 'VMQ01', 10),
                    running = obs.f('GetQuestRunning', 'VMQ01'),
                    completed = obs.f('GetQuestCompleted', 'VMQ01'),
                    objectiveDisplayed = obs.f('GetObjectiveDisplayed', 'VMQ01', 25),
                    objectiveCompleted = obs.f('GetObjectiveCompleted', 'VMQ01', 25),
                    questVariable = obs.mv('VFreeformGoodsprings', 'OpenedSafe'),
                    globalVariable = obs.v('VStoryEventVictorMet'),
                    missingQuest = obs.f('GetStage', 'MissingQuest'),
                }
            end,

            questMutations = function(events)
                obs.f('SetStage', 'VMQ01', 30)
                obs.f('SetObjectiveDisplayed', 'VMQ01', 25, 0)
                obs.f('SetObjectiveCompleted', 'VMQ01', 25, 1)
                obs.msetv('VFreeformGoodsprings', 'OpenedSafe', 2)
                obs.setv('VStoryEventVictorMet', 2)
                obs.f('StartQuest', 'VMQ01')
                obs.f('StopQuest', 'VMQ01')
                obs.f('CompleteQuest', 'VMQ01')
                obs.f('FailQuest', 'VMQ01')
                return #events,
                    obs.mv('VFreeformGoodsprings', 'OpenedSafe'),
                    obs.v('VStoryEventVictorMet')
            end,

            harvestActivation = function(events, animations)
                local S = obs.locals('BarrelCactusScript')
                obs.on('OnActivate', function()
                    if obs.b((S.State == 0) and obs.b(
                        (obs.v('GetActionRef') == obs.v('player')))) then
                        obs.m('player', 'AddItem', 'NVFreshBarrelCactusFruit', 1)
                        S.State = 1
                        obs.f('PlayGroup', 'Forward', 1)
                        obs.f('SetDestroyed', 1)
                    end
                end)
                obs.on('OnLoad', function()
                    if obs.b(S.State == 0) then
                        obs.f('PlayGroup', 'Backward', 1)
                    elseif obs.b(S.State == 1) then
                        obs.f('PlayGroup', 'Forward', 1)
                    end
                end)
                obs.on('OnReset', function()
                    if obs.b(S.State == 1) then
                        obs.f('PlayGroup', 'Backward', 1)
                        S.State = 0
                        obs.f('SetDestroyed', 0)
                    end
                end)
                local script = obs.makeLocalScript()
                script.engineHandlers.onActivated(nearby.players[1])
                local saved = script.engineHandlers.onSave()
                S.State = 0
                obs.f('SetDestroyed', 0)
                script.engineHandlers.onLoad(saved)
                script.engineHandlers.onActive()
                script.engineHandlers.onActivated(nearby.players[1])
                script.engineHandlers.onReset()
                local resetState = S.State
                local resetDestroyed = obs.f('GetDestroyed')
                script.engineHandlers.onActivated(nearby.players[1])
                return #events, events[1].name, events[1].data.item,
                    events[1].data.count, S.State, animations[1].group,
                    animations[1].loops, obs.f('GetDestroyed'),
                    animations[2].group, animations[3].group,
                    resetState, resetDestroyed, events[2].data.item
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

            corpusCoverage = function()
                local corpus = require('openmw_aux.obscript.fnv_retail_coverage')
                local mismatches = {}
                local implementedCommands = 0
                local implementedEvents = 0
                local explicitUnsupported = 0
                local requiredGaps = 0

                local function check(rows, supportFn, kind)
                    for _, row in ipairs(rows) do
                        local supported = supportFn(row.name)
                        local expected = row.status == 'implemented'
                        if supported ~= expected then
                            mismatches[#mismatches + 1] = kind .. ':' .. row.name
                        end
                        if row.status == 'implemented' then
                            if kind == 'command' then
                                implementedCommands = implementedCommands + 1
                            else
                                implementedEvents = implementedEvents + 1
                            end
                        elseif row.status == 'required-gap' then
                            requiredGaps = requiredGaps + 1
                        elseif row.status == 'unsupported-explicit' then
                            explicitUnsupported = explicitUnsupported + 1
                        else
                            mismatches[#mismatches + 1] = 'unknown-status:' .. row.name
                        end
                    end
                end

                check(corpus.commands, obs.isCommandSupported, 'command')
                check(corpus.events, obs.isEventSupported, 'event')
                return {
                    scripts = corpus.scripts,
                    totalLines = corpus.totalLines,
                    commands = #corpus.commands,
                    events = #corpus.events,
                    implementedCommands = implementedCommands,
                    implementedEvents = implementedEvents,
                    explicitUnsupported = explicitUnsupported,
                    requiredGaps = requiredGaps,
                    declaredRequiredGaps = corpus.requiredGaps,
                    mismatchCount = #mismatches,
                    firstMismatch = mismatches[1],
                }
            end,
        }
        )X");

    constexpr VFS::Path::NormalizedView bindingsFactoryPath("obscript/bindingsfactory.lua");
    TestingOpenMW::VFSTestFile bindingsFactoryFile(R"X(
        local sameCell = {}
        sameCell.isInSameSpace = function(_, obj) return obj.cell == sameCell end

        local function object(id, kind, enabled, dead, itemCount, player, x, y, z, unconscious)
            local obj = {
                id = id,
                kind = kind,
                enabled = enabled,
                dead = dead,
                itemCount = itemCount or 0,
                player = player or false,
                cell = sameCell,
                position = { x = x or 0, y = y or 0, z = z or 0 },
                unconscious = unconscious or false,
            }
            obj.isValid = function() return true end
            return obj
        end

        local own = object('self', 'actor', false, false, 1)
        local placed = object('placed', 'actor', false, true, 3, false, 3, 4, 0, true)
        local player = object('player', 'actor', true, false, 12, true, 3, 0, 0)
        local crate = object('crate', 'container', true, false, 4)
        local byFormId = {
            ['form:placed'] = placed,
            ['form:crate'] = crate,
        }
        local events = {}
        local animations = {}
        local mutations = {}

        local function mutation(name)
            return function(...)
                mutations[#mutations + 1] = { name = name, args = { ... } }
                return true
            end
        end

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
                hasQuest = function(quest)
                    return quest:lower() == 'vmq01' or quest:lower() == 'vfreeformgoodsprings'
                end,
                getQuestState = function(quest)
                    if quest:lower() ~= 'vmq01' then return nil end
                    return {
                        stage = 25,
                        running = true,
                        completed = false,
                        stages = { [10] = true },
                        objectives = {
                            [25] = { displayed = true, completed = false },
                        },
                    }
                end,
                getQuestVariable = function(quest, variable)
                    if quest:lower() == 'vfreeformgoodsprings'
                        and variable:lower() == 'openedsafe' then
                        return 1
                    end
                    return nil
                end,
                hasGlobalVariable = function(name)
                    return name:lower() == 'vstoryeventvictormet'
                end,
                getGlobalVariable = function(name)
                    if name:lower() == 'vstoryeventvictormet' then return 1 end
                    return nil
                end,
                setQuestStage = mutation('setQuestStage'),
                setObjectiveDisplayed = mutation('setObjectiveDisplayed'),
                setObjectiveCompleted = mutation('setObjectiveCompleted'),
                startQuest = mutation('startQuest'),
                stopQuest = mutation('stopQuest'),
                completeQuest = mutation('completeQuest'),
                failQuest = mutation('failQuest'),
                setQuestVariable = mutation('setQuestVariable'),
                setGlobalVariable = mutation('setGlobalVariable'),
                isMenuMode = function() return true end,
                getButtonPressed = function() return -1 end,
                showMessage = function(message)
                    mutations[#mutations + 1] = { name = 'showMessage', args = { message } }
                    return true
                end,
                getUnconscious = function(object) return object.unconscious end,
                isDestroyed = function(object) return object.destroyed or false end,
                setDestroyed = function(object, destroyed)
                    object.destroyed = destroyed
                    mutations[#mutations + 1] = {
                        name = 'setDestroyed',
                        args = { object, destroyed },
                    }
                    return true
                end,
                activate = mutation('activate'),
            },
            sendGlobalEvent = function(name, data)
                events[#events + 1] = { name = name, data = data }
            end,
        }
        local nearby = {
            players = { player },
            getObjectByFormId = function(formId) return byFormId[formId] end,
        }
        local animation = {
            clearAnimationQueue = function() end,
            isPlaying = function(obj, group)
                return (obj == own and group == 'Left') or (obj == placed and group == 'Right')
            end,
            playQueued = function(_, group, options)
                animations[#animations + 1] = {
                    group = group,
                    loops = options.loops,
                }
            end,
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
        local world = {
            players = { player },
            createObject = function()
                return { moveInto = function() end, remove = function() end }
            end,
        }
        return {
            packages = {
                ['openmw.animation'] = animation,
                ['openmw.core'] = core,
                ['openmw.nearby'] = nearby,
                ['openmw.self'] = { object = own },
                ['openmw.types'] = types,
                ['openmw.world'] = world,
            },
            events = events,
            animations = animations,
            mutations = mutations,
        }
        )X");

    struct ObScriptRuntimeTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { runtimePath, &runtimeFile },
            { bindingsPath, &bindingsFile },
            { retailCoveragePath, &retailCoverageFile },
            { globalBindingsPath, &globalBindingsFile },
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
        auto r = LuaUtil::call(s["unknownCommandIsZero"]).get<std::tuple<int, int, int, std::string>>();
        EXPECT_EQ(std::get<0>(r), 0); // unimplemented command evaluates to 0
        EXPECT_EQ(std::get<1>(r), 2); // every execution is counted
        EXPECT_EQ(std::get<2>(r), 1); // but only the first use is reported
        EXPECT_EQ(std::get<3>(r), "NeverImplemented:UnknownCommandScript");
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
                         .get<std::tuple<int, std::string, std::string, std::string, std::string, double, std::string>>();
            EXPECT_EQ(std::get<0>(r), 4); // OnLoad, GameMode, OnActivate, and OnReset fired
            EXPECT_EQ(std::get<1>(r), "loaded");
            EXPECT_EQ(std::get<2>(r), "update");
            EXPECT_EQ(std::get<3>(r), "activated:someactor");
            EXPECT_EQ(std::get<4>(r), "reset");
            EXPECT_DOUBLE_EQ(std::get<5>(r), 0.25); // dt captured for GetSecondsPassed
            EXPECT_EQ(std::get<6>(r), "table"); // onSave returns serializable locals
        });
    }

    TEST_F(ObScriptRuntimeTest, MakeLocalScriptWithoutBlocks)
    {
        sol::table s = script();
        auto r = LuaUtil::call(s["makeLocalScriptEmpty"]).get<std::tuple<int, bool>>();
        EXPECT_EQ(std::get<0>(r), 1);
        EXPECT_TRUE(std::get<1>(r));
    }

    TEST_F(ObScriptRuntimeTest, GameplayEventFiltersRequireMatchingAuthoritativeReference)
    {
        sol::table s = script();
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table log = view.sol().create_table();
            EXPECT_EQ(LuaUtil::call(s["filteredGameplayEvents"], log).get<int>(), 9);
            EXPECT_EQ(log[1].get<std::string>(), "death:any");
            EXPECT_EQ(log[2].get<std::string>(), "death:any");
            EXPECT_EQ(log[3].get<std::string>(), "death:player");
            EXPECT_EQ(log[4].get<std::string>(), "hit:any");
            EXPECT_EQ(log[5].get<std::string>(), "hit:attacker");
            EXPECT_EQ(log[6].get<std::string>(), "hitwith:weapon");
            EXPECT_EQ(log[7].get<std::string>(), "activate:actor");
            EXPECT_EQ(log[8].get<std::string>(), "enter:actor");
            EXPECT_EQ(log[9].get<std::string>(), "leave:any");
        });
    }

    TEST_F(ObScriptRuntimeTest, ObjectQueryBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-bindings-test", extraPackages);
            const auto values
                = LuaUtil::call(s["queries"]).get<std::tuple<int, int, int, int, int, int, int, int, int, int>>();
            EXPECT_EQ(std::get<0>(values), 1); // disabled script owner
            EXPECT_EQ(std::get<1>(values), 1); // disabled placed actor
            EXPECT_EQ(std::get<2>(values), 0); // missing reference is safe
            EXPECT_EQ(std::get<3>(values), 1); // dead placed actor
            EXPECT_EQ(std::get<4>(values), 0); // living player
            EXPECT_EQ(std::get<5>(values), 1); // unconscious placed actor
            EXPECT_EQ(std::get<6>(values), 0); // conscious player
            EXPECT_EQ(std::get<7>(values), 12); // actor inventory
            EXPECT_EQ(std::get<8>(values), 4); // container inventory
            EXPECT_EQ(std::get<9>(values), 0); // unknown item
        });
    }

    TEST_F(ObScriptRuntimeTest, RetailActivateUsesScriptOwnerAndActionRef)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-activation-bindings-test", extraPackages);
            const auto values = LuaUtil::call(s["retailActivation"], factory["mutations"])
                                    .get<std::tuple<std::string, std::string, std::string>>();
            EXPECT_EQ(std::get<0>(values), "activate");
            EXPECT_EQ(std::get<1>(values), "self");
            EXPECT_EQ(std::get<2>(values), "player");
        });
    }

    TEST_F(ObScriptRuntimeTest, ResolverPreservesExistingBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
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

    TEST_F(ObScriptRuntimeTest, RetailCorpusCoverageMatchesRuntimeBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-corpus-coverage-test", extraPackages);
            sol::table coverage = LuaUtil::call(s["corpusCoverage"]).get<sol::table>();
            EXPECT_EQ(coverage["scripts"].get<int>(), 3708);
            EXPECT_EQ(coverage["totalLines"].get<int>(), 165335);
            EXPECT_EQ(coverage["commands"].get<int>(), 166);
            EXPECT_EQ(coverage["events"].get<int>(), 23);
            EXPECT_GT(coverage["implementedCommands"].get<int>(), 0);
            EXPECT_GT(coverage["implementedEvents"].get<int>(), 0);
            EXPECT_GT(coverage["explicitUnsupported"].get<int>(), 0);
            EXPECT_EQ(coverage["requiredGaps"].get<int>(), coverage["declaredRequiredGaps"].get<int>());
            EXPECT_EQ(coverage["mismatchCount"].get<int>(), 0);
        });
    }

    TEST_F(ObScriptRuntimeTest, CommonReadOnlyBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
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

    TEST_F(ObScriptRuntimeTest, MenuAnimationAndMessageBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-menu-bindings-test", extraPackages);
            const auto values = LuaUtil::call(s["menuAnimationAndMessage"])
                                    .get<std::tuple<int, int, int, int, int>>();
            EXPECT_EQ(std::get<0>(values), 1);
            EXPECT_EQ(std::get<1>(values), -1);
            EXPECT_EQ(std::get<2>(values), 0);
            EXPECT_EQ(std::get<3>(values), 1);
            EXPECT_EQ(std::get<4>(values), 1);

            sol::table mutations = factory["mutations"];
            ASSERT_EQ(mutations.size(), 1);
            EXPECT_EQ(mutations[1].get<sol::table>()["name"].get<std::string>(), "showMessage");
            EXPECT_EQ(mutations[1].get<sol::table>()["args"].get<sol::table>()[1].get<std::string>(),
                "VCG04Message");
        });
    }

    TEST_F(ObScriptRuntimeTest, QuestAndGlobalStateBindings)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-quest-bindings-test", extraPackages);
            sol::table values = LuaUtil::call(s["questQueries"]).get<sol::table>();
            EXPECT_EQ(values["stage"].get<int>(), 25);
            EXPECT_EQ(values["stageDone"].get<int>(), 1);
            EXPECT_EQ(values["running"].get<int>(), 1);
            EXPECT_EQ(values["completed"].get<int>(), 0);
            EXPECT_EQ(values["objectiveDisplayed"].get<int>(), 1);
            EXPECT_EQ(values["objectiveCompleted"].get<int>(), 0);
            EXPECT_EQ(values["questVariable"].get<int>(), 1);
            EXPECT_EQ(values["globalVariable"].get<int>(), 1);
            EXPECT_EQ(values["missingQuest"].get<int>(), 0);

            const auto mutations = LuaUtil::call(s["questMutations"], factory["events"])
                                       .get<std::tuple<int, int, int>>();
            EXPECT_EQ(std::get<0>(mutations), 9);
            EXPECT_EQ(std::get<1>(mutations), 2);
            EXPECT_EQ(std::get<2>(mutations), 2);

            sol::table events = factory["events"];
            EXPECT_EQ(events[1].get<sol::table>()["name"].get<std::string>(), "ObScriptSetStage");
            EXPECT_EQ(events[1].get<sol::table>()["data"].get<sol::table>()["stage"].get<int>(), 30);
            EXPECT_EQ(events[4].get<sol::table>()["name"].get<std::string>(), "ObScriptSetQuestVariable");
            EXPECT_EQ(events[5].get<sol::table>()["name"].get<std::string>(), "ObScriptSetGlobalVariable");
            EXPECT_EQ(events[9].get<sol::table>()["name"].get<std::string>(), "ObScriptFailQuest");
        });
    }

    TEST_F(ObScriptRuntimeTest, GlobalHandlersMutateAuthoritativeQuestState)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.core", packages["openmw.core"] },
                { "openmw.types", packages["openmw.types"] },
                { "openmw.world", packages["openmw.world"] },
            };
            sol::table script
                = mLua.runInNewSandbox(VFS::Path::Normalized(globalBindingsPath), "obscript-global-test", extraPackages);
            sol::table handlers = script["eventHandlers"];
            sol::table data = view.sol().create_table();
            data["quest"] = "VMQ01";
            data["stage"] = 30;
            LuaUtil::call(handlers["ObScriptSetStage"], data);

            data = view.sol().create_table();
            data["quest"] = "VMQ01";
            data["objective"] = 25;
            data["displayed"] = false;
            LuaUtil::call(handlers["ObScriptSetObjectiveDisplayed"], data);

            data = view.sol().create_table();
            data["quest"] = "VFreeformGoodsprings";
            data["variable"] = "OpenedSafe";
            data["value"] = 2;
            LuaUtil::call(handlers["ObScriptSetQuestVariable"], data);

            data = view.sol().create_table();
            data["name"] = "VStoryEventVictorMet";
            data["value"] = 1;
            LuaUtil::call(handlers["ObScriptSetGlobalVariable"], data);

            sol::table mutations = factory["mutations"];
            ASSERT_EQ(mutations.size(), 4);
            EXPECT_EQ(mutations[1].get<sol::table>()["name"].get<std::string>(), "setQuestStage");
            EXPECT_EQ(mutations[1].get<sol::table>()["args"].get<sol::table>()[1].get<std::string>(), "VMQ01");
            EXPECT_EQ(mutations[1].get<sol::table>()["args"].get<sol::table>()[2].get<int>(), 30);
            EXPECT_EQ(
                mutations[2].get<sol::table>()["name"].get<std::string>(), "setObjectiveDisplayed");
            EXPECT_EQ(mutations[3].get<sol::table>()["name"].get<std::string>(), "setQuestVariable");
            EXPECT_EQ(mutations[4].get<sol::table>()["name"].get<std::string>(), "setGlobalVariable");
        });
    }

    TEST_F(ObScriptRuntimeTest, HarvestActivationAddsRetailIngredient)
    {
        mLua.protectedCall([&](LuaUtil::LuaView&) {
            sol::table factory = mLua.runInNewSandbox(VFS::Path::Normalized(bindingsFactoryPath));
            sol::table packages = factory["packages"];
            const std::map<std::string, sol::main_object> extraPackages{
                { "openmw.animation", packages["openmw.animation"] },
                { "openmw.core", packages["openmw.core"] },
                { "openmw.nearby", packages["openmw.nearby"] },
                { "openmw.self", packages["openmw.self"] },
                { "openmw.types", packages["openmw.types"] },
            };
            sol::table s = mLua.runInNewSandbox(
                VFS::Path::Normalized(bindingsDriverPath), "obscript-harvest-test", extraPackages);
            const auto values = LuaUtil::call(s["harvestActivation"], factory["events"], factory["animations"])
                                    .get<std::tuple<int, std::string, std::string, int, int, std::string, int, int,
                                        std::string, std::string, int, int, std::string>>();
            EXPECT_EQ(std::get<0>(values), 2);
            EXPECT_EQ(std::get<1>(values), "ObScriptAddItem");
            EXPECT_EQ(std::get<2>(values), "NVFreshBarrelCactusFruit");
            EXPECT_EQ(std::get<3>(values), 1);
            EXPECT_EQ(std::get<4>(values), 1);
            EXPECT_EQ(std::get<5>(values), "Forward");
            EXPECT_EQ(std::get<6>(values), 0);
            EXPECT_EQ(std::get<7>(values), 1);
            EXPECT_EQ(std::get<8>(values), "Forward");
            EXPECT_EQ(std::get<9>(values), "Backward");
            EXPECT_EQ(std::get<10>(values), 0);
            EXPECT_EQ(std::get<11>(values), 0);
            EXPECT_EQ(std::get<12>(values), "NVFreshBarrelCactusFruit");
            sol::table mutations = factory["mutations"];
            ASSERT_EQ(mutations.size(), 4);
            for (int i = 1; i <= 4; ++i)
                EXPECT_EQ(mutations[i].get<sol::table>()["name"].get<std::string>(), "setDestroyed");
            EXPECT_TRUE(mutations[1].get<sol::table>()["args"].get<sol::table>()[2].get<bool>());
            EXPECT_FALSE(mutations[2].get<sol::table>()["args"].get<sol::table>()[2].get<bool>());
            EXPECT_FALSE(mutations[3].get<sol::table>()["args"].get<sol::table>()[2].get<bool>());
            EXPECT_TRUE(mutations[4].get<sol::table>()["args"].get<sol::table>()[2].get<bool>());
        });
    }

}
