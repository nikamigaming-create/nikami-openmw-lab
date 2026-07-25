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
obs._scriptAliases = {} -- quest/editor id (lower) -> attached script entry
obs._globals = setmetatable({}, zerotable_mt)  -- global variables (GameHour etc.)
obs._globalOverrides = {}
obs._memberOverrides = {}
obs._bindings = {}     -- command name (lower) -> function
obs._bindingMeta = {}  -- command name (lower) -> proof/provider metadata
obs._dispatchSeen = {} -- source script + command -> true
obs._unknown = {}      -- command name (lower) -> call count (coverage telemetry)
obs._log = nil         -- optional function(command, script) for first-use stub logging
obs._current = nil     -- script currently registering/executing
obs._udfs = {}         -- named and anonymous xNVSE user-defined functions
obs._lambdaCounter = 0
obs._functionReturn = nil
obs._mainLoopCallbacks = {}
obs._compatEventCallbacks = {}
obs._eventContext = nil
obs._keyState = {}
obs._disabledKeys = {}
obs._disabledButtons = {}
obs._disabledTriggers = {}
obs._disabledControls = {}
obs._heldControls = {}
obs._heldKeys = {}
obs._perkEntryValues = {}
obs._effectTraitValues = {}
obs._activePerks = {}
obs._activeEffects = {}
obs._auxiliary = {}
obs._ini = {}
obs._soundSourceFiles = {}
obs._soundTraits = {}
obs._detectionEventCooldown = 0
obs._crosshairOverride = nil
obs._customMapMarkerOverride = nil
obs._proofHostileObject = nil
obs._telemetrySequence = 0
obs._lastTelemetry = nil
obs._lastTelemetryByProvider = {}
obs._lastTelemetryByCommand = {}
obs._telemetryHistory = {}

-- Keep a small in-memory mirror of the same dispatch/event records written to
-- the engine log. Proof-only HUD scripts read this through the public
-- FNVObScriptCompat interface, so their labels cannot get ahead of execution.
function obs.recordTelemetry(kind, sourceScript, providerName, command, enginePath, state, details)
    obs._telemetrySequence = obs._telemetrySequence + 1
    local row = {
        sequence = obs._telemetrySequence,
        kind = tostring(kind or 'dispatch'),
        sourceScript = tostring(sourceScript or '__no_active_script'),
        provider = tostring(providerName or 'openmw'),
        commandOrEvent = tostring(command or ''),
        enginePath = tostring(enginePath or 'openmw.obscript'),
        state = tostring(state or 'executed'),
    }
    if type(details) == 'table' then
        for key, value in pairs(details) do
            row[key] = value
        end
    end
    obs._lastTelemetry = row
    obs._lastTelemetryByProvider[row.provider] = row
    obs._lastTelemetryByCommand[row.commandOrEvent:lower()] = row
    obs._telemetryHistory[#obs._telemetryHistory + 1] = row
    if #obs._telemetryHistory > 512 then
        table.remove(obs._telemetryHistory, 1)
    end
    return row
end

local function telemetryArguments(...)
    local count = math.min(select('#', ...), 8)
    local result = {}
    for index = 1, count do
        local value = select(index, ...)
        local kind = type(value)
        if kind == 'nil' or kind == 'number' or kind == 'string'
                or kind == 'boolean' then
            result[index] = value
        else
            result[index] = tostring(value)
        end
    end
    return result
end

-- Canonical event support table used by runtime diagnostics and the generated
-- official-corpus coverage report. An entry belongs here only when
-- makeLocalScript wires it to an authoritative engine event below.
local supportedEvents = {
    ['gamemode'] = true,
    ['onactivate'] = true,
    ['onopen'] = true,
    ['onload'] = true,
    ['onreset'] = true,
    ['ontriggerenter'] = true,
    ['ontriggerleave'] = true,
    ['ondeath'] = true,
    ['onhit'] = true,
    ['onhitwith'] = true,
    ['onstartcombat'] = true,
    ['oncombatend'] = true,
}

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

-- Some xNVSE commands use script variables as output parameters. Generated
-- code passes these short-lived handles instead of the variable's current
-- value, allowing a binding such as JohnnyGuitar's WorldToScreen to update
-- the original script-local storage.
function obs.out(storage, name)
    return {
        __obsOut = true,
        storage = storage,
        name = tostring(name):lower(),
    }
end

function obs.setout(handle, value)
    if type(handle) ~= 'table' or handle.__obsOut ~= true
            or type(handle.storage) ~= 'table' then
        return false
    end
    handle.storage[handle.name] = value
    return true
end

local function recordUnknown(name)
    local key = name:lower()
    obs._unknown[key] = (obs._unknown[key] or 0) + 1
    obs.recordTelemetry('fallback',
        obs._current and obs._current.name or '__no_active_script',
        'unsupported', name, 'openmw.obscript.unknownCommand', 'failed')
    if obs._unknown[key] == 1 and obs._log then
        obs._log(name, obs._current and obs._current.name or "__no_active_script")
    end
    return 0
end

local function dispatch(name, ...)
    local key = name:lower()
    if key == 'call' then
        return obs.callUdf(...)
    end
    if key == 'setfunctionvalue' then
        obs._functionReturn = select(1, ...)
        return obs._functionReturn or 0
    end
    local fn = obs._bindings[key]
    if fn then
        local meta = obs._bindingMeta[key]
        if meta ~= nil then
            local source = obs._current and obs._current.name or '__no_active_script'
            obs.recordTelemetry('dispatch', source, meta.provider or 'openmw',
                meta.command or name, meta.enginePath or 'openmw.obscript', 'executed',
                { arguments = telemetryArguments(...) })
            local seenKey = source:lower() .. '\0' .. key
            if not obs._dispatchSeen[seenKey] then
                obs._dispatchSeen[seenKey] = true
                print(('[obscript-compat] state=dispatch sourceScript=%s provider=%s command=%s enginePath=%s')
                    :format(source, meta.provider or 'openmw',
                        meta.command or name, meta.enginePath or 'openmw.obscript'))
            end
        end
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
    if key == 'ar_null' then
        return 0
    end
    if key == 'this' then
        return obs._eventContext and obs._eventContext.this or 0
    end
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
    if obs._resolveEditorId then
        local value = obs._resolveEditorId(name)
        if value ~= nil then
            return value
        end
    end
    -- unknown: report once as coverage telemetry, evaluate as 0 (vanilla-ish)
    return recordUnknown(name)
end

-- A bare command argument is ambiguous in ObScript: it may be a numeric
-- global (for example JAM's JDCLengthMin), or it may be a form/editor ID,
-- actor-value name, animation group, and so on. Resolve only known globals
-- here; bindings keep receiving every other token as its original string.
function obs.arg(name)
    local key = tostring(name):lower()
    if key == 'ar_null' then
        return 0
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
    return name
end

-- cross-script variable read: Quest.var / Ref.var
function obs.mv(base, name)
    local baseKey = tostring(base):lower()
    local nameKey = name:lower()
    local overrides = obs._memberOverrides[baseKey]
    if overrides and rawget(overrides, nameKey) ~= nil then
        return overrides[nameKey]
    end
    local entry = obs._scripts[baseKey] or obs._scriptAliases[baseKey]
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
    local baseKey = tostring(base):lower()
    local entry = obs._scripts[baseKey] or obs._scriptAliases[baseKey]
    if entry ~= nil then
        entry.locals[name:lower()] = value
        return
    end
    -- The native quest store is numeric, while xNVSE quest/ref variables may
    -- hold forms, arrays, strings, and UDF handles. Preserve those values in
    -- the compatibility namespace instead of sending an invalid numeric
    -- mutation across the global-script boundary.
    if type(value) ~= 'number' then
        local overrides = obs._memberOverrides[baseKey]
        if overrides == nil then
            overrides = {}
            obs._memberOverrides[baseKey] = overrides
        end
        overrides[name:lower()] = value
        return
    end
    if obs._setMemberVariable and obs._setMemberVariable(base, name, value) then
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

function obs.boolnum(value)
    return obs.b(value) and 1 or 0
end

local function objectIdentity(value)
    local kind = type(value)
    if kind ~= 'table' and kind ~= 'userdata' then
        return nil
    end
    for _, key in ipairs({ 'id', 'recordId' }) do
        local ok, identity = pcall(function() return value[key] end)
        if ok and identity ~= nil and identity ~= 0
                and tostring(identity) ~= '' then
            return key .. ':' .. tostring(identity)
        end
    end
    return nil
end

-- OpenMW can expose the same in-world reference through distinct userdata
-- proxies in separate local-script sandboxes. Gamebryo compares form handles,
-- not Lua wrapper identity, so xNVSE expressions must do the same.
function obs.eq(left, right)
    local result
    if left == right then
        result = true
    else
        local leftIdentity = objectIdentity(left)
        local rightIdentity = objectIdentity(right)
        result = leftIdentity ~= nil and leftIdentity == rightIdentity
    end
    local leftIdentity = objectIdentity(left)
    local rightIdentity = objectIdentity(right)
    if obs._current ~= nil
            and tostring(obs._current.name):lower()
                == 'jhmonhiteventhandler' then
        print(('[obscript-compat] state=form-compare '
                .. 'scenarioId=JHM.hit-marker '
                .. 'sourceScript=JHMOnHitEventHandler provider=xnvse-core '
                .. 'command=FormEquality enginePath=openmw.object.id '
                .. 'left=%s right=%s result=%d')
            :format(tostring(leftIdentity or left),
                tostring(rightIdentity or right), result and 1 or 0))
    end
    return result
end

-- xNVSE expression helpers. They deliberately return the assigned value so
-- constructs such as `eval condition && (x = value)` preserve expression
-- semantics after transpilation.
function obs.setlocal(storage, name, value)
    rawset(storage, name, value)
    return value
end

function obs.setvexpr(name, value)
    obs.setv(name, value)
    return value
end

function obs.msetvexpr(base, name, value)
    obs.msetv(base, name, value)
    return value
end

function obs.index(value, key)
    if type(value) ~= 'table' and type(value) ~= 'userdata' then
        return 0
    end
    local ok, result = pcall(function() return value[key] end)
    if not ok or result == nil then
        return 0
    end
    return result
end

function obs.setindex(value, key, assigned)
    if type(value) == 'table' or type(value) == 'userdata' then
        pcall(function() value[key] = assigned end)
    end
    return assigned
end

function obs.pair(key, value)
    return { __obsPair = true, key = key, value = value }
end

function obs.add(left, right)
    if type(left) == 'string' or type(right) == 'string' then
        return obs.str(left) .. obs.str(right)
    end
    return (tonumber(left) or 0) + (tonumber(right) or 0)
end

function obs.str(value)
    if value == nil then return '' end
    if type(value) == 'number' and value == math.floor(value) then
        return string.format('%d', value)
    end
    if value == 0 then return '' end
    if type(value) == 'userdata' and obs._bindings ~= nil
            and obs._bindings['lngetname'] ~= nil then
        local ok, name = pcall(obs._bindings['lngetname'], value)
        if ok and name ~= nil and tostring(name) ~= '' then
            return tostring(name)
        end
    end
    return tostring(value)
end

function obs.deref(value)
    if type(value) == 'table' then
        if rawget(value, '__obsDeref') ~= nil then
            return rawget(value, '__obsDeref')
        end
        if rawget(value, 1) ~= nil then
            return rawget(value, 1)
        end
    end
    return value
end

function obs.bit(op, left, right)
    local a = math.floor(tonumber(left) or 0) % 4294967296
    local b = math.floor(tonumber(right) or 0) % 4294967296
    local result, place = 0, 1
    for _ = 1, 32 do
        local abit, bbit = a % 2, b % 2
        if (op == '&' and abit == 1 and bbit == 1)
                or (op == '|' and (abit == 1 or bbit == 1)) then
            result = result + place
        end
        a = math.floor(a / 2)
        b = math.floor(b / 2)
        place = place * 2
    end
    return result
end

local function invokeUdf(udf, ...)
    local fn = udf
    local entry = nil
    if type(udf) == 'table' then
        fn = udf.fn
        entry = udf.entry
    end
    local previousEntry = obs._current
    local previousReturn = obs._functionReturn
    if entry ~= nil then
        obs._current = entry
    end
    obs._functionReturn = nil
    local resultValues = { pcall(fn, ...) }
    local succeeded = table.remove(resultValues, 1)
    if not succeeded then
        local errorMessage = resultValues[1]
        print(('[obscript-compat] state=script-error sourceScript=%s phase=udf error=%s')
            :format(entry and entry.name or '__anonymous_udf',
                tostring(errorMessage):gsub('%s+', '_')))
        obs._functionReturn = previousReturn
        obs._current = previousEntry
        return 0
    end
    local directResult = resultValues[1]
    local result = directResult
    if result == nil then
        result = obs._functionReturn
    end
    obs._functionReturn = previousReturn
    obs._current = previousEntry
    return result or 0
end

function obs.udf(name, fn)
    obs._udfs[tostring(name):lower()] = {
        fn = fn,
        entry = obs._current,
        name = tostring(name),
    }
    return name
end

function obs.lambda(label, fn)
    obs._lambdaCounter = obs._lambdaCounter + 1
    local handle = ('__obs_lambda_%s_%d'):format(tostring(label), obs._lambdaCounter)
    obs._udfs[handle:lower()] = {
        fn = fn,
        entry = obs._current,
        name = handle,
    }
    return handle
end

function obs.callUdf(handle, ...)
    if type(handle) == 'function' then
        return invokeUdf(handle, ...)
    end
    local udf = obs._udfs[tostring(handle):lower()]
    if udf == nil then
        return recordUnknown('Call:' .. tostring(handle))
    end
    return invokeUdf(udf, ...)
end

local function callbackIdentity(handle, filter)
    return tostring(handle):lower() .. '\0' .. tostring(filter or ''):lower()
end

function obs.setCompatEventCallback(kind, handle, enabled, filter, providerName, command)
    local eventKind = tostring(kind):lower()
    local callbacks = obs._compatEventCallbacks[eventKind]
    if callbacks == nil then
        callbacks = {}
        obs._compatEventCallbacks[eventKind] = callbacks
    end
    local identity = callbackIdentity(handle, filter)
    if obs.b(enabled) then
        callbacks[identity] = {
            handle = handle,
            filter = filter,
            provider = providerName or 'xnvse-core',
            command = command or kind,
        }
    else
        if filter == nil then
            local handlePrefix = tostring(handle):lower() .. '\0'
            for key in pairs(callbacks) do
                if key:sub(1, #handlePrefix) == handlePrefix then
                    callbacks[key] = nil
                end
            end
        else
            callbacks[identity] = nil
        end
    end
    obs.recordTelemetry('event-registration',
        obs._current and obs._current.name or '__no_active_script',
        providerName or 'xnvse-core', command or kind,
        'openmw.obscript.compatEvents',
        obs.b(enabled) and 'registered' or 'unregistered')
    print(('[obscript-compat] state=event-registration sourceScript=%s provider=%s '
            .. 'command=%s event=%s callback=%s enabled=%s filter=%s '
            .. 'enginePath=openmw.obscript.compatEvents')
        :format(obs._current and obs._current.name or '__no_active_script',
            providerName or 'xnvse-core', command or kind, eventKind,
            tostring(handle), obs.b(enabled) and '1' or '0', tostring(filter or '')))
    return 0
end

local function eventFilterMatches(callback, payload)
    if callback.filter == nil then
        return true
    end
    if payload == nil then
        return false
    end
    if payload.key ~= nil then
        return tonumber(callback.filter) == tonumber(payload.key)
    end
    local expected = obs.resolveRef(callback.filter)
    local actual = obs.resolveRef(payload.target or payload.this)
    if expected == actual then
        return true
    end
    local expectedRecord = obs.resolveRecordId(callback.filter)
    local actualRecord = obs.resolveRecordId(payload.target or payload.this)
    return expectedRecord ~= nil and expectedRecord == actualRecord
end

function obs.dispatchCompatEvent(kind, payload)
    local eventKind = tostring(kind):lower()
    local callbacks = obs._compatEventCallbacks[eventKind]
    if callbacks == nil then
        return 0
    end
    local ordered = {}
    for identity, callback in pairs(callbacks) do
        ordered[#ordered + 1] = { identity = identity, callback = callback }
    end
    table.sort(ordered, function(left, right) return left.identity < right.identity end)
    local invoked = 0
    for _, row in ipairs(ordered) do
        local callback = row.callback
        if eventFilterMatches(callback, payload) then
            local previousContext = obs._eventContext
            obs._eventContext = payload or {}
            obs.recordTelemetry('event-dispatch', callback.handle,
                callback.provider, callback.command,
                'openmw.obscript.compatEvents', eventKind)
            if not callback.dispatched then
                callback.dispatched = true
                print(('[obscript-compat] state=event-dispatch provider=%s command=%s '
                        .. 'event=%s callback=%s enginePath=openmw.obscript.compatEvents')
                    :format(callback.provider, callback.command, eventKind, tostring(callback.handle)))
            end
            obs.callUdf(callback.handle, unpack((payload and payload.args) or {}))
            obs._eventContext = previousContext
            invoked = invoked + 1
        end
    end
    return invoked
end

function obs.getRegisteredKeys()
    local keys = {}
    local seen = {}
    for _, kind in ipairs({ 'keydown', 'keyup' }) do
        for _, callback in pairs(obs._compatEventCallbacks[kind] or {}) do
            local key = tonumber(callback.filter)
            if key ~= nil and not seen[key] then
                seen[key] = true
                keys[#keys + 1] = key
            end
        end
    end
    table.sort(keys)
    return keys
end

function obs.discard(_) end

function obs.fx(value, ...)  -- call on arbitrary expression (rare)
    return value
end

-- --------------------------- engine-facing API

-- install an engine binding: obs.bind("SetStage", function(quest, stage) ... end)
-- Optional metadata is emitted at the first real dispatch from each authored
-- script. Proof tooling consumes the same records that runtime validation does.
function obs.bind(name, fn, meta)
    local key = name:lower()
    obs._bindings[key] = fn
    obs._bindingMeta[key] = meta
end

function obs.setMainLoopCallback(handle, enabled, callDelay, mode)
    local key = tostring(handle):lower()
    if obs.b(enabled) then
        obs._mainLoopCallbacks[key] = {
            handle = handle,
            callDelay = math.max(1, math.floor(tonumber(callDelay) or 1)),
            mode = math.floor(tonumber(mode) or 3),
            counter = 0,
        }
    else
        obs._mainLoopCallbacks[key] = nil
    end
    obs.recordTelemetry('callback',
        obs._current and obs._current.name or '__no_active_script',
        'jip-ln', 'SetGameMainLoopCallback',
        'openmw.obscript.mainLoopCallbacks',
        obs.b(enabled) and ('registered:' .. tostring(handle))
            or ('unregistered:' .. tostring(handle)))
    print(('[obscript-compat] state=callback sourceScript=%s provider=jip-ln command=SetGameMainLoopCallback '
            .. 'callback=%s enabled=%s enginePath=openmw.obscript.mainLoopCallbacks')
        :format(obs._current and obs._current.name or '__no_active_script',
            tostring(handle), obs.b(enabled) and '1' or '0'))
    return 0
end

function obs.runMainLoopCallbacks()
    local callbacks = {}
    for key, callback in pairs(obs._mainLoopCallbacks) do
        callbacks[#callbacks + 1] = { key = key, callback = callback }
    end
    table.sort(callbacks, function(left, right) return left.key < right.key end)
    for _, row in ipairs(callbacks) do
        local callback = row.callback
        if type(callback) ~= 'table' then
            callback = { handle = callback, callDelay = 1, mode = 3, counter = 0 }
            obs._mainLoopCallbacks[row.key] = callback
        end
        -- GameMode is bit 0 in JIP LN's mode mask. Menu-only callbacks must
        -- not be run from the player/world update host.
        if obs.bit('&', callback.mode or 3, 1) ~= 0 then
            callback.counter = (callback.counter or 0) + 1
            if callback.counter >= (callback.callDelay or 1) then
                callback.counter = 0
                obs.callUdf(callback.handle)
            end
        end
    end
end

function obs.isCommandSupported(name)
    if type(name) ~= 'string' or obs._bindings[name:lower()] == nil then
        return false
    end
    local meta = obs._bindingMeta[name:lower()]
    return meta == nil or meta.corpus ~= false
end

function obs.isEventSupported(name)
    return type(name) == 'string' and supportedEvents[name:lower()] == true
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
            local ok, errorMessage = pcall(handler.fn)
            if not ok then
                print(('[obscript-compat] state=script-error sourceScript=%s phase=event error=%s')
                    :format(entry.name, tostring(errorMessage):gsub('%s+', '_')))
            end
        end
    end
    obs._current = nil
end

--- Builds one player-local host for authored ESM4 quest scripts and xNVSE
-- UDFs. All registration chunks execute in this sandbox, so cross-script
-- variables, arrays, strings, lambdas, and UDF handles share one namespace.
-- `quests` is an array of `{ script, quest, delay }` rows. Only quests whose
-- authoritative native quest state is running receive GameMode ticks.
function obs.makeQuestHost(quests)
    local bindingsOk, bindingsError = pcall(require, 'openmw_aux.obscript.bindings')

    quests = quests or {}
    for _, row in ipairs(quests) do
        local entry = obs._scripts[tostring(row.script):lower()]
        if entry ~= nil and row.quest ~= nil then
            obs._scriptAliases[tostring(row.quest):lower()] = entry
        end
    end
    local scriptCount = 0
    local udfCount = 0
    for _ in pairs(obs._scripts) do scriptCount = scriptCount + 1 end
    for _ in pairs(obs._udfs) do udfCount = udfCount + 1 end

    if obs._log == nil then
        obs._log = function(command, script)
            print(('[obscript-compat] state=unimplemented sourceScript=%s command=%s')
                :format(tostring(script), tostring(command)))
        end
    end
    print(('[obscript-compat] state=ready host=player scripts=%d quests=%d udfs=%d bindings=%s')
        :format(scriptCount, #quests, udfCount, bindingsOk and 'ready' or 'failed'))
    if not bindingsOk then
        print(('[obscript-compat] state=binding-failed error=%s'):format(tostring(bindingsError)))
    end

    local elapsed = {}
    local reportedQuestState = {}
    local firstUpdate = true

    local function questIsRunning(row)
        local binding = obs._bindings['getquestrunning']
        local running = binding ~= nil and obs.b(binding(row.quest))
        if not reportedQuestState[row.script] then
            reportedQuestState[row.script] = true
            print(('[obscript-compat] state=quest-gate sourceScript=%s quest=%s authoredStart=%s nativeRunning=%s')
                :format(tostring(row.script), tostring(row.quest),
                    row.startEnabled and '1' or '0', running and '1' or '0'))
        end
        return running
    end

    local function tickQuest(row, dt)
        if not questIsRunning(row) then
            elapsed[row.script] = 0
            return
        end
        local delay = math.max(0, tonumber(row.delay) or 0)
        local accumulated = (elapsed[row.script] or delay) + dt
        if delay > 0 and accumulated < delay then
            elapsed[row.script] = accumulated
            return
        end
        elapsed[row.script] = delay > 0 and (accumulated % delay) or 0
        local entry = obs._scripts[tostring(row.script):lower()]
        local handlers = entry and entry.handlers['gamemode']
        if handlers then
            fireAll(entry, handlers)
        end
    end

    local engineHandlers = {
        onUpdate = function(dt)
            obs._dt = dt
            obs._gameLoaded = firstUpdate
            obs._gameRestarted = false
            obs._globalOverrides = {}
            obs._memberOverrides = {}
            for _, row in ipairs(quests) do
                tickQuest(row, dt)
            end
            obs.runMainLoopCallbacks()
            obs.dispatchCompatEvent('renderupdate', {
                this = obs._getCrosshairRef and obs._getCrosshairRef() or 0,
                args = { obs._getCrosshairRef and obs._getCrosshairRef() or 0 },
            })
            if obs._uiBridge ~= nil then
                obs._uiBridge.setGameplayState {
                    jbt = obs.mv('JBT', 'iBulletTime'),
                    jhb = obs.mv('JHB', 'iHoldBreath'),
                    jvs = obs.mv('JVS', 'iSprint'),
                    jwh = obs.mv('JWH', 'iWheel'),
                    jlm = obs.mv('JLM', 'iLootMenu'),
                }
                obs._uiBridge.onFrame(dt)
            end
            firstUpdate = false
            obs._gameLoaded = false
        end,
        onSave = function()
            local state = {
                scripts = {},
                elapsed = {},
                auxiliary = obs._auxiliary,
                ini = obs._ini,
                mainLoopCallbacks = obs._mainLoopCallbacks,
            }
            for name, entry in pairs(obs._scripts) do
                local locals = {}
                for key, value in pairs(entry.locals) do
                    locals[key] = value
                end
                state.scripts[name] = locals
            end
            for name, value in pairs(elapsed) do
                state.elapsed[name] = value
            end
            return state
        end,
        onLoad = function(state)
            if state and state.scripts then
                for name, locals in pairs(state.scripts) do
                    local entry = obs._scripts[name]
                    if entry then
                        for key in pairs(entry.locals) do
                            rawset(entry.locals, key, nil)
                        end
                        for key, value in pairs(locals) do
                            rawset(entry.locals, key, value)
                        end
                    end
                end
            end
            elapsed = state and state.elapsed or {}
            obs._auxiliary = state and state.auxiliary or {}
            obs._ini = state and state.ini or {}
            obs._mainLoopCallbacks = state and state.mainLoopCallbacks or {}
            firstUpdate = true
        end,
    }

    return {
        interfaceName = 'FNVObScriptCompat',
        interface = {
            getStatus = function()
                local unknownCount = 0
                for _ in pairs(obs._unknown) do unknownCount = unknownCount + 1 end
                return {
                    scripts = scriptCount,
                    quests = #quests,
                    udfs = udfCount,
                    unknownCommands = unknownCount,
                    dispatches = (function()
                        local count = 0
                        for _ in pairs(obs._dispatchSeen) do count = count + 1 end
                        return count
                    end)(),
                }
            end,
            getCoverageReport = function(limit) return obs.coverageReport(limit) end,
            getTelemetrySnapshot = function()
                return {
                    sequence = obs._telemetrySequence,
                    last = obs._lastTelemetry,
                    byProvider = obs._lastTelemetryByProvider,
                    byCommand = obs._lastTelemetryByCommand,
                    history = obs._telemetryHistory,
                }
            end,
            getUIStatus = function()
                if obs._getUIStatus == nil then return {} end
                return obs._getUIStatus()
            end,
            getKnvseAnimationStatus = function()
                if obs._getKnvseAnimationStatus == nil then return {} end
                return obs._getKnvseAnimationStatus()
            end,
            getScriptVariable = function(script, name)
                local key = tostring(script):lower()
                local entry = obs._scripts[key] or obs._scriptAliases[key]
                return entry and entry.locals[tostring(name):lower()] or 0
            end,
            setScriptVariable = function(script, name, value)
                local key = tostring(script):lower()
                local entry = obs._scripts[key] or obs._scriptAliases[key]
                if entry == nil then return false end
                entry.locals[tostring(name):lower()] = value
                return true
            end,
            getGlobalVariable = function(name)
                return obs.v(tostring(name))
            end,
            setGlobalVariable = function(name, value)
                obs.setv(tostring(name), value)
                return true
            end,
            getRegisteredKeys = function() return obs.getRegisteredKeys() end,
            runScriptEvent = function(script, event)
                return obs.fire(tostring(script), tostring(event))
            end,
            callUdf = function(handle, ...)
                return obs.callUdf(handle, ...)
            end,
            dispatchKey = function(key, pressed)
                local numericKey = tonumber(key) or 0
                obs._keyState[numericKey] = pressed and true or false
                return obs.dispatchCompatEvent(pressed and 'keydown' or 'keyup', {
                    key = numericKey,
                    args = { numericKey },
                })
            end,
            dispatchEvent = function(kind, payload)
                if type(payload) == 'table'
                        and obs._canonicalizeObject ~= nil then
                    for _, key in ipairs(
                            { 'this', 'target', 'attacker', 'weapon', 'projectile' }) do
                        if payload[key] ~= nil then
                            payload[key] = obs._canonicalizeObject(payload[key])
                        end
                    end
                    if type(payload.args) == 'table' then
                        for index, value in ipairs(payload.args) do
                            payload.args[index] = obs._canonicalizeObject(value)
                        end
                    end
                end
                return obs.dispatchCompatEvent(kind, payload)
            end,
            seedInventoryFixture = function(target, recordId, count)
                if obs._seedInventoryFixture == nil then return false end
                return obs._seedInventoryFixture(target, recordId, count)
            end,
            dispatchCommand = function(name, ...)
                return obs.f(tostring(name), ...)
            end,
            dispatchMemberCommand = function(base, name, ...)
                return obs.m(base, tostring(name), ...)
            end,
            dispatchProviderProbeCommand = function(name, ...)
                local previous = obs._current
                obs._current = { name = 'compatibility probe (not JAM)' }
                local ok, result = pcall(obs.f, tostring(name), ...)
                obs._current = previous
                if not ok then error(result) end
                return result
            end,
            dispatchProviderProbeMemberCommand = function(base, name, ...)
                local previous = obs._current
                obs._current = { name = 'compatibility probe (not JAM)' }
                local ok, result = pcall(
                    obs.m, base, tostring(name), ...)
                obs._current = previous
                if not ok then error(result) end
                return result
            end,
            recordProofState = function(scenarioId, sourceScript, providerName,
                    commandOrEvent, enginePath, state, details)
                local payload = type(details) == 'table' and details or {}
                payload.scenarioId = tostring(scenarioId or '')
                return obs.recordTelemetry('proof-state', sourceScript,
                    providerName, commandOrEvent, enginePath, state, payload)
            end,
            setControlPressed = function(control, pressed)
                local numericControl = tonumber(control) or -1
                if pressed then
                    obs._heldControls[numericControl] = true
                else
                    obs._heldControls[numericControl] = nil
                end
                return true
            end,
            setCrosshairFixture = function(object, promptText, hostile)
                obs._crosshairOverride = object
                obs._proofHostileObject = hostile and object or nil
                if obs._uiBridge ~= nil
                        and obs._uiBridge.setNativeReticleContext ~= nil then
                    obs._uiBridge.setNativeReticleContext {
                        promptVisible = object ~= nil and promptText ~= nil
                            and tostring(promptText) ~= '',
                        promptText = tostring(promptText or ''),
                        systemColor = hostile and 2 or 1,
                    }
                end
                return true
            end,
            clearCrosshairFixture = function()
                obs._crosshairOverride = nil
                obs._proofHostileObject = nil
                if obs._uiBridge ~= nil
                        and obs._uiBridge.setNativeReticleContext ~= nil then
                    obs._uiBridge.setNativeReticleContext {
                        promptVisible = false,
                        promptText = '',
                        systemColor = 1,
                    }
                end
                return true
            end,
            setCustomMapMarkerFixture = function(target)
                if obs._setCustomMapMarkerFixture == nil then return false end
                return obs._setCustomMapMarkerFixture(target)
            end,
            setExternalProofOverlay = function(active)
                if obs._uiBridge ~= nil
                        and obs._uiBridge.setExternalProofOverlay ~= nil then
                    obs._uiBridge.setExternalProofOverlay(active)
                    return true
                end
                return false
            end,
        },
        engineHandlers = engineHandlers,
        eventHandlers = {
            FNVObScriptOutgoingHit = function(data)
                obs.dispatchCompatEvent('hit', {
                    this = data.target,
                    target = data.target,
                    attacker = data.attacker,
                    weapon = data.weapon,
                    damage = data.damage,
                    hitLocation = data.hitLocation,
                    critical = data.critical,
                    args = {},
                })
            end,
            FNVObScriptIncomingHit = function(data)
                local payload = {
                    this = data.target,
                    target = data.target,
                    attacker = data.attacker,
                    weapon = data.weapon,
                    damage = data.damage,
                    hitLocation = data.hitLocation,
                    critical = data.critical,
                    args = {},
                }
                obs.dispatchCompatEvent('hit', payload)
                payload.args = { tonumber(data.damage) or 0, data.attacker or 0 }
                obs.dispatchCompatEvent('healthdamage', payload)
            end,
            FNVObScriptFireWeapon = function(data)
                obs.dispatchCompatEvent('fireweapon', {
                    this = data.actor,
                    target = data.actor,
                    weapon = data.weapon,
                    args = { data.weapon or 0 },
                })
            end,
        },
    }
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

    local firstUpdate = true
    local engineHandlers = {
        onActive = function()
            local h = entry.handlers["onload"]
            if h then fireAll(entry, h) end
        end,
        onUpdate = function(dt)
            obs._dt = dt
            obs._gameLoaded = firstUpdate
            obs._gameRestarted = false
            -- Global events requested in the previous frame have now
            -- reached the authoritative engine state.
            obs._globalOverrides = {}
            obs._memberOverrides = {}
            local h = entry.handlers["gamemode"]
            if h then fireAll(entry, h) end
            firstUpdate = false
            obs._gameLoaded = false
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
            firstUpdate = true
        end,
    }

    -- The engine uses the presence of this handler to decide whether native
    -- activation must be buffered for retail OnActivate/OnOpen semantics.
    -- Do not register it for scripts that have no authored activation block.
    if entry.handlers["onactivate"] or entry.handlers["onopen"] then
        engineHandlers.onActivated = function(actor)
            -- Opening a container counts as activation, so OnOpen blocks
            -- are dispatched from here as well.
            obs._actionRef = actor
            local h = entry.handlers["onactivate"]
            if h then fireAll(entry, h, actor) end
            h = entry.handlers["onopen"]
            if h then fireAll(entry, h, actor) end
            obs._actionRef = nil
        end
    end

    if entry.handlers["ontriggerenter"] then
        engineHandlers.onTriggerEnter = function(actor)
            obs._actionRef = actor
            local h = entry.handlers["ontriggerenter"]
            if h then fireAll(entry, h, actor) end
            obs._actionRef = nil
        end
    end

    if entry.handlers["ontriggerleave"] then
        engineHandlers.onTriggerLeave = function(actor)
            obs._actionRef = actor
            local h = entry.handlers["ontriggerleave"]
            if h then fireAll(entry, h, actor) end
            obs._actionRef = nil
        end
    end

    return {
        engineHandlers = engineHandlers,
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
            CombatStarted = function(data)
                local h = entry.handlers["onstartcombat"]
                if h then fireAll(entry, h, data and data.target) end
            end,
            CombatEnded = function()
                local h = entry.handlers["oncombatend"]
                if h then fireAll(entry, h) end
            end,
        },
    }
end

return obs
