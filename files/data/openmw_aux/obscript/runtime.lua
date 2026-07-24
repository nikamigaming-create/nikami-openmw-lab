---
-- `openmw_aux.obscript.runtime` is the runtime library for transpiled
-- ObScript (see `openmw_aux.obscript.transpiler`).
-- It provides script-local storage, cross-script variables, an event registry,
-- per-frame scheduling, and a uniform dispatch layer where every engine command lands.
-- Engine bindings are installed with @{#runtime.bind};
-- any command without a binding is stubbed, counted, and evaluates to 0,
-- so every transpiled script loads and runs regardless of implementation coverage.
--
-- The semantics encode vanilla ObScript behavior:
--
--  * variables are zero-initialized
--  * conditions treat any nonzero number as true
--  * one shared variable namespace per script, readable cross-script
--  * GameMode blocks run every frame for every loaded script
-- Implementation can be found in `resources/vfs/openmw_aux/obscript/runtime.lua`.
-- @module runtime
-- @context global
-- @usage
-- local obs = require('openmw_aux.obscript.runtime')
-- obs.bind('ShowMessage', function(msgId) ... end)
-- obs.frame() -- from an onUpdate engine handler

local obs = {}

-- --------------------------- state

local zerotable_mt = { __index = function() return 0 end }

obs._scripts = {}      -- script name (lower) -> { locals = S, handlers = {event->fn} }
obs._globals = setmetatable({}, zerotable_mt)  -- global variables (GameHour etc.)
obs._globalOverrides = {}
obs._memberOverrides = {}
obs._bindings = {}     -- command name (lower) -> function
obs._unknown = {}      -- command name (lower) -> call count (coverage telemetry)
obs._log = nil         -- optional function(command, script) for first-use stub logging
obs._current = nil     -- script currently registering/executing

local function scriptEntry(name)
    local key = name:lower()
    local entry = obs._scripts[key]
    if not entry then
        entry = { name = name, locals = setmetatable({}, zerotable_mt), handlers = {} }
        obs._scripts[key] = entry
    end
    return entry
end

-- --------------------------- API used by generated code

function obs.locals(scriptName)
    local entry = scriptEntry(scriptName)
    obs._current = entry
    return entry.locals
end

function obs.on(event, fn, ...)
    -- registers a block handler for the script currently loading
    local entry = obs._current
    if entry == nil then
        entry = scriptEntry("__anonymous")
    end
    local key = event:lower()
    local handlers = entry.handlers[key]
    if handlers == nil then
        handlers = {}
        entry.handlers[key] = handlers
    end
    handlers[#handlers + 1] = { fn = fn, filters = { ... } }
end

-- ObScript truthiness: any nonzero number is true
function obs.b(x)
    if type(x) == "number" then
        return x ~= 0
    end
    return x and true or false
end

local function recordUnknown(name)
    local key = name:lower()
    obs._unknown[key] = (obs._unknown[key] or 0) + 1
    if obs._unknown[key] == 1 and obs._log then
        obs._log(name, obs._current and obs._current.name or "__no_active_script")
    end
    return 0
end

local function dispatch(name, ...)
    local key = name:lower()
    local fn = obs._bindings[key]
    if fn then
        return fn(...) or 0
    end
    return recordUnknown(name)
end

-- free function call: SetStage(...), ShowMessage(...)
function obs.f(name, ...)
    return dispatch(name, ...)
end

-- member call: player.AddItem(...), Ref.Say(...)
function obs.m(base, name, ...)
    return dispatch(name, obs.resolveRef(base), ...)
end

-- bare name in value position: local was handled by the emitter, so this is a
-- global variable, a zero-arg command, or an editor id used as a value
function obs.v(name)
    local key = name:lower()
    if obs._bindings[key] then
        return dispatch(name)
    end
    if rawget(obs._globalOverrides, key) ~= nil then
        return obs._globalOverrides[key]
    end
    if obs._getGlobalVariable then
        local value = obs._getGlobalVariable(name)
        if value ~= nil then
            return value
        end
    end
    if rawget(obs._globals, key) ~= nil then
        return obs._globals[key]
    end
    -- unknown: report once as coverage telemetry, evaluate as 0 (vanilla-ish)
    return recordUnknown(name)
end

-- cross-script variable read: Quest.var / Ref.var
function obs.mv(base, name)
    local baseKey = tostring(base):lower()
    local nameKey = name:lower()
    local overrides = obs._memberOverrides[baseKey]
    if overrides and rawget(overrides, nameKey) ~= nil then
        return overrides[nameKey]
    end
    local entry = obs._scripts[baseKey]
    if entry then
        return entry.locals[nameKey]
    end
    if obs._getMemberVariable then
        local value = obs._getMemberVariable(base, name)
        if value ~= nil then
            return value
        end
    end
    -- script not loaded (or base is a ref whose script we don't know): 0
    return 0
end

function obs.setv(name, value)
    local key = name:lower()
    if obs._setGlobalVariable and obs._setGlobalVariable(name, value) then
        obs._globalOverrides[key] = value
    else
        obs._globals[key] = value
    end
end

function obs.msetv(base, name, value)
    if obs._setMemberVariable and obs._setMemberVariable(base, name, value) then
        local baseKey = tostring(base):lower()
        local overrides = obs._memberOverrides[baseKey]
        if overrides == nil then
            overrides = {}
            obs._memberOverrides[baseKey] = overrides
        end
        overrides[name:lower()] = value
        return
    end
    scriptEntry(tostring(base)).locals[name:lower()] = value
end

function obs.discard(_) end

function obs.fx(value, ...)  -- call on arbitrary expression (rare)
    return value
end

-- --------------------------- engine-facing API

-- install an engine binding: obs.bind("SetStage", function(quest, stage) ... end)
function obs.bind(name, fn)
    obs._bindings[name:lower()] = fn
end

-- resolve a reference handle (string editor id, or already-resolved object)
-- engine installs the real resolver; default is identity
obs.resolveRef = function(x) return x end
obs.resolveRecordId = function(x)
    if type(x) == "string" then return x end
    local ok, value = pcall(function() return x.recordId end)
    if ok then return value end
    return nil
end

-- run one frame: fire every loaded script's GameMode handlers
function obs.frame()
    for _, entry in pairs(obs._scripts) do
        local h = entry.handlers["gamemode"]
        if h then
            obs._current = entry
            for _, handler in ipairs(h) do
                if #handler.filters == 0 then
                    handler.fn()
                end
            end
        end
    end
    obs._current = nil
end

-- fire a specific event on a specific script (OnActivate, OnDeath, ...)
function obs.fire(scriptName, event)
    local entry = obs._scripts[scriptName:lower()]
    if not entry then return false end
    local h = entry.handlers[event:lower()]
    if not h then return false end
    obs._current = entry
    for _, handler in ipairs(h) do
        if #handler.filters == 0 then
            handler.fn()
        end
    end
    obs._current = nil
    return true
end

-- coverage report: which commands were called but unimplemented, by frequency
function obs.coverageReport(limit)
    local list = {}
    for name, count in pairs(obs._unknown) do
        list[#list + 1] = { name = name, count = count }
    end
    table.sort(list, function(a, b) return a.count > b.count end)
    local out = {}
    for i = 1, math.min(limit or 25, #list) do
        out[#out + 1] = string.format("%6d  %s", list[i].count, list[i].name)
    end
    return table.concat(out, "\n")
end

-- --------------------------- local-script interface

local function fireAll(entry, handlers, eventRef)
    obs._current = entry
    for _, handler in ipairs(handlers) do
        local matches = #handler.filters == 0
        if not matches and eventRef ~= nil then
            local expected = obs.resolveRef(handler.filters[1])
            local actual = obs.resolveRef(eventRef)
            matches = expected == actual
            if not matches then
                local expectedRecord = obs.resolveRecordId(handler.filters[1])
                local actualRecord = obs.resolveRecordId(eventRef)
                matches = expectedRecord ~= nil and expectedRecord == actualRecord
            end
        end
        if matches then
            handler.fn()
        end
    end
    obs._current = nil
end

--- Builds the local-script table (`engineHandlers`) for the script loaded in
-- this sandbox. Each attached object gets its own sandbox and therefore its
-- own runtime instance and locals. Called as the last line of generated code.
function obs.makeLocalScript()
    -- engine bindings for the local context; absent outside it (e.g. console lab)
    pcall(require, 'openmw_aux.obscript.bindings')

    local entry = obs._current
    if entry == nil then
        for _, e in pairs(obs._scripts) do
            entry = e
            break
        end
    end
    if entry == nil then
        return {} -- script registered no blocks
    end

    return {
        engineHandlers = {
            onActive = function()
                local h = entry.handlers["onload"]
                if h then fireAll(entry, h) end
            end,
            onUpdate = function(dt)
                obs._dt = dt
                -- Global events requested in the previous frame have now
                -- reached the authoritative engine state.
                obs._globalOverrides = {}
                obs._memberOverrides = {}
                local h = entry.handlers["gamemode"]
                if h then fireAll(entry, h) end
            end,
            onActivated = function(actor)
                -- Opening a container counts as activation, so OnOpen blocks
                -- are dispatched from here as well.
                obs._actionRef = actor
                local h = entry.handlers["onactivate"]
                if h then fireAll(entry, h, actor) end
                h = entry.handlers["onopen"]
                if h then fireAll(entry, h, actor) end
                obs._actionRef = nil
            end,
            onTriggerEnter = function(actor)
                obs._actionRef = actor
                local h = entry.handlers["ontriggerenter"]
                if h then fireAll(entry, h, actor) end
                obs._actionRef = nil
            end,
            onTriggerLeave = function(actor)
                obs._actionRef = actor
                local h = entry.handlers["ontriggerleave"]
                if h then fireAll(entry, h, actor) end
                obs._actionRef = nil
            end,
            onReset = function()
                local h = entry.handlers["onreset"]
                if h then fireAll(entry, h) end
            end,
            onSave = function()
                local locals = {}
                for k, v in pairs(entry.locals) do
                    locals[k] = v
                end
                return { locals = locals, destroyed = obs._destroyed }
            end,
            onLoad = function(data)
                if data and data.locals then
                    -- Generated handlers close over the original locals table,
                    -- so restore it in place rather than replacing it.
                    for key in pairs(entry.locals) do
                        rawset(entry.locals, key, nil)
                    end
                    for key, value in pairs(data.locals) do
                        rawset(entry.locals, key, value)
                    end
                end
                obs._destroyed = data and data.destroyed or nil
            end,
        },
        eventHandlers = {
            Died = function(data)
                local h = entry.handlers["ondeath"]
                -- Unfiltered OnDeath blocks always execute. Filtered blocks
                -- remain dormant when no authoritative last hitter exists.
                if h then fireAll(entry, h, data and data.killer) end
            end,
            Hit = function(data)
                local h = entry.handlers["onhit"]
                if h then fireAll(entry, h, data and data.attacker) end
                h = entry.handlers["onhitwith"]
                if h then fireAll(entry, h, data and data.weapon) end
            end,
        },
    }
end

return obs
