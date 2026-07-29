---
-- `openmw_aux.obscript.bindings` installs engine bindings for transpiled
-- ObScript running as a local script. Loaded by @{runtime#runtime.makeLocalScript};
-- requires the local context (`openmw.self`).
-- @module bindings
-- @context local

local core = require('openmw.core')
local animation = require('openmw.animation')
local nearby = require('openmw.nearby')
local self = require('openmw.self')
local types = require('openmw.types')

local obs = require('openmw_aux.obscript.runtime')
local ambientOk, ambient = pcall(require, 'openmw.ambient')
local cameraOk, camera = pcall(require, 'openmw.camera')
local inputOk, input = pcall(require, 'openmw.input')
local uiOk, ui = pcall(require, 'openmw.ui')
local utilOk, util = pcall(require, 'openmw.util')
local uiBridgeOk, uiBridge = pcall(require, 'openmw_aux.obscript.ui_bridge')
local directInputOk, directInput
    = pcall(require, 'scripts.omw.fnv.compat.directinput')

local function provider(providerName, enginePath, command, corpus)
    return {
        provider = providerName,
        enginePath = enginePath,
        command = command,
        corpus = corpus,
    }
end

local function traceJvsGate(command, result, details)
    if obs._current ~= nil
        and tostring(obs._current.name):lower() == 'jvsonkeydowneventhandler' then
        print(('[obscript-compat] state=gate sourceScript=JVSOnKeyDownEventHandler '
                .. 'command=%s result=%s details=%s')
            :format(command, tostring(result), tostring(details or '')))
    end
    return result
end

-- Runtime stubs keep a script alive so one missing command does not take down
-- the whole cell, but they must never be silent. The C++ package deduplicates
-- these reports globally and retains the first script that exercised each gap.
obs._log = core.obscript.reportUnsupportedCommand

local function isInstance(typeApi, object)
    if object == nil or typeApi == nil or typeApi.objectIsInstance == nil then
        return false
    end
    local ok, result = pcall(typeApi.objectIsInstance, object)
    return ok and result
end

local function isValid(object)
    if object == nil then
        return false
    end
    local ok, result = pcall(function() return object:isValid() end)
    return ok and result
end

local function resolveObject(ref)
    if ref == nil then
        return self.object
    end
    if type(ref) ~= 'string' then
        return ref
    end
    if ref:lower() == 'this' then
        return obs._eventContext and obs._eventContext.this or nil
    end
    if ref:lower() == 'player' or ref:lower() == 'playerref' then
        return nearby.players[1]
    end

    local formId = core.obscript.resolveRefEditorId(ref)
    if formId == nil then
        return nil
    end
    local ok, object = pcall(nearby.getObjectByFormId, formId)
    if not ok or not isValid(object) then
        return nil
    end
    return object
end

local function canonicalizeObject(object)
    if object == nil then return nil end
    if type(object) == 'string'
            and (object:lower() == 'player'
                or object:lower() == 'playerref') then
        return nearby.players[1] or object
    end
    -- The OpenMW type predicates accept record/reference-id strings as a
    -- convenience in a few APIs.  Those strings are not usable object
    -- handles, though, so never let them bypass the live-reference lookup.
    if type(object) ~= 'string'
            and (isInstance(types.Actor, object)
                or isInstance(types.Container, object)
                or isInstance(types.Player, object)) then
        return object
    end
    local id = object
    if type(object) ~= 'string' then
        local ok
        ok, id = pcall(function() return object.id end)
        if not ok or id == nil then return object end
    end
    local function normalizedId(value)
        return tostring(value):lower():gsub('^formid:', '')
    end
    local wantedId = normalizedId(id)
    for _, objects in ipairs(
            { nearby.players, nearby.actors, nearby.containers }) do
        for _, candidate in ipairs(objects or {}) do
            local candidateOk, candidateId = pcall(
                function() return candidate.id end)
            if candidateOk and normalizedId(candidateId) == wantedId then
                return candidate
            end
        end
    end
    for _, formId in ipairs({ tostring(id), id }) do
        local resolvedOk, resolved = pcall(
            nearby.getObjectByFormId, formId)
        if resolvedOk and resolved ~= nil and isValid(resolved) then
            return resolved
        end
    end
    obs._canonicalizationMisses = obs._canonicalizationMisses or {}
    if not obs._canonicalizationMisses[wantedId] then
        obs._canonicalizationMisses[wantedId] = true
        local candidates = {}
        for _, objects in ipairs(
                { nearby.players, nearby.actors, nearby.containers }) do
            for _, candidate in ipairs(objects or {}) do
                if #candidates < 12 then
                    local candidateOk, candidateId = pcall(
                        function() return candidate.id end)
                    candidates[#candidates + 1] = candidateOk
                        and normalizedId(candidateId) or 'unreadable'
                end
            end
        end
        print(('[obscript-compat] state=canonicalize-miss '
                .. 'wanted=%s candidates=%s players=%d actors=%d containers=%d')
            :format(wantedId, table.concat(candidates, ','),
                #(nearby.players or {}), #(nearby.actors or {}),
                #(nearby.containers or {})))
    end
    return object
end

obs._canonicalizeObject = canonicalizeObject
obs._setCustomMapMarkerFixture = function(target)
    if target == nil or target == 0 then
        obs._customMapMarkerOverride = nil
        return true
    end
    local resolved = canonicalizeObject(target)
    if not isValid(resolved) then
        print(('[obscript-compat] state=custom-marker-rejected target=%s')
            :format(tostring(target)))
        return false
    end
    obs._customMapMarkerOverride = resolved
    print(('[obscript-compat] state=native-effect '
            .. 'scenarioId=JVO.visual-objectives '
            .. 'sourceScript=jam_full_proof.lua provider=openmw-proof '
            .. 'command=set-custom-map-marker-fixture '
            .. 'enginePath=openmw.fnv.map.customMarker target=%s')
        :format(tostring(resolved)))
    return true
end

local function isPlayer(ref)
    if type(ref) == 'string' then
        return ref:lower() == 'player' or ref:lower() == 'playerref'
    end
    if isInstance(types.Player, ref) then
        return true
    end

    -- A player-local script receives `self.object` as an attached-object
    -- handle, while an ObScript member call such as `PlayerRef.PlaySound3D`
    -- can arrive through the nearby-player proxy. They refer to the same
    -- in-world reference but are intentionally different Lua userdata.
    local player = nearby.players[1]
    if player == nil or ref == nil then
        return false
    end
    if ref == player then
        return true
    end
    local ok, sameId = pcall(function()
        return tostring(ref.id) == tostring(player.id)
    end)
    return ok and sameId
end

-- Resolve member-call bases before dispatch. Keep an unresolved editor id as
-- a string so it cannot be confused with a reference-less command acting on
-- the script owner; query bindings resolve it once more and return zero.
obs.resolveRef = function(ref)
    return resolveObject(ref) or ref
end

obs.resolveRecordId = function(ref)
    if type(ref) == 'string' then
        return core.obscript.resolveRefEditorId(ref) or ref
    end
    local ok, recordId = pcall(function() return ref.recordId end)
    if ok then
        return recordId
    end
    return nil
end

obs._resolveEditorId = function(editorId)
    if tostring(editorId):lower() == 'this' then
        return obs._eventContext and obs._eventContext.this or 0
    end
    return core.obscript.resolveItemEditorId(editorId)
        or core.obscript.resolveRefEditorId(editorId)
end

-- `player` is also a value expression (`GetActionRef == player`), not only
-- a member-call base. Binding it lets obs.v resolve that comparison to the
-- actual local player object.
obs.bind('player', function()
    return nearby.players[1] or 0
end)

-- Some compiled xNVSE expressions preserve the canonical editor ID as a
-- value-form command instead of lowering it to the shorter `player` alias.
-- Resolve both spellings to the authoritative nearby-player object.
obs.bind('PlayerRef', function()
    local eventAttacker = obs._eventContext and obs._eventContext.attacker
    if eventAttacker ~= nil and isPlayer(eventAttacker) then
        -- Preserve the event's authoritative player proxy so even legacy
        -- bytecode paths that compare raw handles observe the same reference.
        return eventAttacker
    end
    return nearby.players[1] or 0
end, provider('xnvse-core', 'openmw.nearby.players[1]', 'PlayerRef', false))

obs.bind('GetSecondsPassed', function()
    return obs._dt or 0
end)

-- These are frame-wide xNVSE lifecycle pulses. The runtime host raises
-- GetGameLoaded for every quest/UDF in the first update after construction or
-- save restoration; it is intentionally not consumed by the first caller.
obs.bind('GetGameLoaded', function()
    return obs._gameLoaded and 1 or 0
end)

obs.bind('GetGameRestarted', function()
    return obs._gameRestarted and 1 or 0
end)

obs.bind('SetGameMainLoopCallback', function(callback, enabled, callDelay, mode)
    if enabled == nil then
        return obs._mainLoopCallbacks[tostring(callback):lower()] ~= nil and 1 or 0
    end
    if uiBridgeOk and uiBridge.setCallbackState ~= nil
        and tostring(callback):lower() == 'jdcmainloopeventhandler' then
        uiBridge.setCallbackState(callback, obs.b(enabled), callDelay)
    end
    return obs.setMainLoopCallback(callback, enabled, callDelay, mode)
end, provider('jip-ln', 'openmw.obscript.mainLoopCallbacks', 'SetGameMainLoopCallback'))

local function numericArgument(value, fallback)
    local numeric = tonumber(value)
    if numeric ~= nil then
        return numeric
    end
    if type(value) == 'string' then
        local ok, globalValue = pcall(core.obscript.getGlobalVariable, value)
        numeric = ok and tonumber(globalValue) or nil
        if numeric ~= nil then
            return numeric
        end
    end
    return fallback
end

local function resolveEventKey(key)
    return numericArgument(key, key)
end

obs.bind('SetOnKeyDownEventHandler', function(callback, enabled, key)
    return obs.setCompatEventCallback(
        'keydown', callback, enabled, resolveEventKey(key),
        'jip-ln', 'SetOnKeyDownEventHandler')
end, provider('jip-ln', 'openmw.input', 'SetOnKeyDownEventHandler'))

obs.bind('SetOnKeyUpEventHandler', function(callback, enabled, key)
    return obs.setCompatEventCallback(
        'keyup', callback, enabled, resolveEventKey(key),
        'jip-ln', 'SetOnKeyUpEventHandler')
end, provider('jip-ln', 'openmw.input', 'SetOnKeyUpEventHandler'))

obs.bind('SetOnHitEventHandler', function(callback, enabled, target)
    return obs.setCompatEventCallback(
        'hit', callback, enabled, target, 'jip-ln', 'SetOnHitEventHandler')
end, provider('jip-ln', 'openmw.combat.hitEvents', 'SetOnHitEventHandler'))

obs.bind('SetOnHealthDamageEventHandler', function(callback, enabled, target)
    return obs.setCompatEventCallback(
        'healthdamage', callback, enabled, target,
        'jip-ln', 'SetOnHealthDamageEventHandler')
end, provider('jip-ln', 'openmw.combat.hitEvents', 'SetOnHealthDamageEventHandler'))

obs.bind('SetOnFireWeaponEventHandler', function(callback, enabled, actor)
    return obs.setCompatEventCallback(
        'fireweapon', callback, enabled, actor, 'jip-ln', 'SetOnFireWeaponEventHandler')
end, provider('jip-ln', 'openmw.combat.fireEvents', 'SetOnFireWeaponEventHandler'))

obs.bind('SetJohnnyOnRenderUpdateEventHandler', function(enabled, callback)
    return obs.setCompatEventCallback(
        'renderupdate', callback, enabled, nil,
        'johnnyguitar', 'SetJohnnyOnRenderUpdateEventHandler')
end, provider('johnnyguitar', 'openmw.ui.frameProjection',
    'SetJohnnyOnRenderUpdateEventHandler'))

obs.bind('SetEventHandler', function(eventName, callback)
    return obs.setCompatEventCallback(
        'custom:' .. tostring(eventName), callback, 1, nil,
        'xnvse-core', 'SetEventHandler')
end, provider('xnvse-core', 'openmw.obscript.customEvents', 'SetEventHandler'))

obs.bind('RemoveEventHandler', function(eventName, callback)
    return obs.setCompatEventCallback(
        'custom:' .. tostring(eventName), callback, 0, nil,
        'xnvse-core', 'RemoveEventHandler')
end, provider('xnvse-core', 'openmw.obscript.customEvents', 'RemoveEventHandler'))

obs.bind('DispatchEvent', function(eventName, payload)
    return obs.dispatchCompatEvent('custom:' .. tostring(eventName), {
        args = payload ~= nil and { payload } or {},
    })
end, provider('xnvse-core', 'openmw.obscript.customEvents', 'DispatchEvent'))

obs.bind('CallAfterSeconds', function(seconds, callback, ...)
    -- JAM uses zero-delay calls to defer cleanup until the current handler
    -- completes. The compatibility dispatcher already isolates UDF frames,
    -- so invoking at this boundary preserves the authored ordering.
    return obs.callUdf(callback, ...)
end, provider('jip-ln', 'openmw.obscript.deferredUdf', 'CallAfterSeconds'))

obs.bind('GetNumericGameSetting', function(name)
    local ok, value = pcall(core.getGMST, tostring(name))
    value = ok and tonumber(value) or nil
    if value ~= nil then
        return value
    end
    if core.obscript.getFalloutRuntimeGameSetting ~= nil then
        local runtimeOk, runtimeValue = pcall(
            core.obscript.getFalloutRuntimeGameSetting, tostring(name))
        if runtimeOk and tonumber(runtimeValue) ~= nil then
            return tonumber(runtimeValue)
        end
    end
    return 0
end, provider('xnvse-core',
    'openmw.core.getGMST+openmw.fnv.runtimeGameSettings',
    'GetNumericGameSetting'))

obs.bind('GetNumericINISetting', function(name)
    local key = tostring(name):lower()
    if cameraOk and key == 'fdefaultworldfov:display' then
        local ok, radians = pcall(camera.getBaseFieldOfView)
        if ok then
            return math.deg(radians)
        end
    end
    return tonumber(obs._ini['fallout.ini\0' .. key]) or 0
end, provider('xnvse-core', 'openmw.camera.getBaseFieldOfView', 'GetNumericINISetting'))

obs.bind('IsModLoaded', function(name)
    return core.contentFiles.has(tostring(name)) and 1 or 0
end, provider('xnvse-core', 'openmw.core.contentFiles.has', 'IsModLoaded'))

obs.bind('IsPluginInstalled', function(name)
    local key = tostring(name or ''):lower():gsub('\\', '/')
    local virtualProviders = {
        ['nvse'] = true,
        ['nvse.dll'] = true,
        ['xnvse'] = true,
        ['xnvse.dll'] = true,
        ['jip ln'] = true,
        ['jip ln nvse plugin'] = true,
        ['jip_nvse.dll'] = true,
        ['johnnyguitar'] = true,
        ['johnnyguitar.dll'] = true,
        ['johnnyguitar nvse'] = true,
        ['knvse'] = true,
        ['knvse.dll'] = true,
    }
    return (virtualProviders[key] or core.contentFiles.has(tostring(name)))
        and 1 or 0
end, provider('xnvse-core',
    'openmw.obscript.compatibilityProviders+openmw.core.contentFiles.has',
    'IsPluginInstalled'))

obs.bind('GetModIndex', function(name)
    local index = core.contentFiles.indexOf(tostring(name))
    return index and (index - 1) or 255
end, provider('xnvse-core', 'openmw.core.contentFiles.indexOf', 'GetModIndex'))

obs.bind('GetSourceModIndex', function(value)
    local recordId = obs.resolveRecordId(value) or tostring(value or '')
    local numeric = tonumber(tostring(recordId):match('0[xX]([0-9a-fA-F]+)'), 16)
    return numeric and math.floor(numeric / 0x1000000) % 256 or 255
end, provider('xnvse-core', 'openmw.esm4.formId', 'GetSourceModIndex'))

obs.bind('BuildRef', function(modIndex, localFormId)
    local full = (math.floor(tonumber(modIndex) or 0) % 256) * 0x1000000
        + (math.floor(tonumber(localFormId) or 0) % 0x1000000)
    return ('0x%08X'):format(full)
end, provider('jip-ln', 'openmw.esm4.formId', 'BuildRef'))

local function iniKey(name, file)
    return tostring(file or '__calling_mod__'):lower() .. '\0' .. tostring(name):lower()
end

obs.bind('GetINIFloat', function(name, file)
    return tonumber(obs._ini[iniKey(name, file)]) or 0
end, provider('jip-ln', 'openmw.obscript.serializedIni', 'GetINIFloat'))

obs.bind('SetINIFloat', function(name, value, file)
    obs._ini[iniKey(name, file)] = tonumber(value) or 0
    return 1
end, provider('jip-ln', 'openmw.obscript.serializedIni', 'SetINIFloat'))

obs.bind('SetGlobalTimeMultiplier', function(value)
    local multiplier = math.max(0.01, tonumber(value) or 1)
    obs._simulationTimeScale = multiplier
    core.sendGlobalEvent('SetSimulationTimeScale', multiplier)
    return multiplier
end, provider('xnvse-core', 'openmw.world.setSimulationTimeScale',
    'SetGlobalTimeMultiplier'))

obs.bind('GetGlobalTimeMultiplier', function()
    return obs._simulationTimeScale or core.getSimulationTimeScale()
end, provider('xnvse-core', 'openmw.core.getSimulationTimeScale',
    'GetGlobalTimeMultiplier'))

obs.bind('GetVATSMode', function()
    return core.obscript.getVatsMode() and 1 or 0
end, provider('xnvse-core', 'openmw.fnv.playerRuntime.vatsActive',
    'GetVATSMode'))

obs.bind('IsInKillCam', function()
    return core.obscript.isInKillCam() and 1 or 0
end, provider('jip-ln', 'openmw.fnv.playerRuntime.killCameraState',
    'IsInKillCam'))

obs.bind('ToggleDisableSaves', function()
    return 0
end, provider('xnvse-core', 'openmw.save.compatibilityGate', 'ToggleDisableSaves'))

obs.bind('ToggleVanityWheel', function(enabled)
    return core.obscript.toggleVanityWheel(obs.b(enabled)) and 1 or 0
end, provider('jip-ln', 'openmw.world.toggleVanityMode',
    'ToggleVanityWheel'))

obs.bind('ToggleHUDCursor', function(visible)
    obs._hudCursorVisible = obs.b(visible)
    return 1
end, provider('jip-ln', 'openmw.ui.cursor', 'ToggleHUDCursor'))

obs.bind('SetCursorPos', function(x, y)
    obs._cursorX = tonumber(x) or 0
    obs._cursorY = tonumber(y) or 0
    return 1
end, provider('jip-ln', 'openmw.ui.cursorPosition', 'SetCursorPos'))

obs.bind('GetCursorPos', function(axis)
    local key = tostring(axis or 'x'):lower()
    if key == 'y' or key == '1' then
        return obs._cursorY or 0
    end
    return obs._cursorX or 0
end, provider('jip-ln', 'openmw.ui.cursorPosition', 'GetCursorPos'))

obs.bind('DeactivateAllHighlightsAlt', function()
    return 0
end, provider('jip-ln', 'openmw.render.highlight', 'DeactivateAllHighlightsAlt'))

obs.bind('GetPressedButtons', function()
    return {}
end, provider('jip-ln', 'openmw.input.controller', 'GetPressedButtons'))

obs.bind('DisableScriptedActivate', function(ref, disabled)
    local object = resolveObject(ref)
    if object ~= nil then
        obs._scriptedActivateDisabled = obs._scriptedActivateDisabled or {}
        obs._scriptedActivateDisabled[tostring(object)] = obs.b(disabled)
    end
    return 1
end, provider('jip-ln', 'openmw.activation.compatibilityGate',
    'DisableScriptedActivate'))

obs.bind('FakeScriptEvent', function()
    return 1
end, provider('jip-ln', 'openmw.activation.scriptEvent', 'FakeScriptEvent'))

obs.bind('GetAnimSequenceFrequency', function()
    return -1
end, provider('jip-ln', 'openmw.animation.sequenceFrequency',
    'GetAnimSequenceFrequency'))

obs.bind('PlayAnimSequence', function(ref, sequence)
    local object = resolveObject(ref)
    if object == nil then return 0 end
    local ok = pcall(animation.playQueued, object, tostring(sequence),
        { loops = 0 })
    return ok and 1 or 0
end, provider('jip-ln', 'openmw.animation.playQueued', 'PlayAnimSequence'))

local function normalizeKnvseAnimationPath(path)
    local result = tostring(path or ''):gsub('\\', '/'):gsub('^%s+', '')
        :gsub('%s+$', ''):gsub('^data/', '')
    if result == '' then return nil end
    if result:lower():sub(1, 7) ~= 'meshes/' then
        result = 'meshes/' .. result
    end
    -- kNVSE pools lower-case animation paths before consulting its override
    -- map; OpenMW's VFS uses the same canonical form.
    return result:lower()
end

local function sameAnimationSource(left, right)
    local function canonical(path)
        return tostring(path or ''):gsub('\\', '/'):lower()
    end
    return canonical(left) == canonical(right)
end

local function knvseAnimationGroup(path)
    local stem = tostring(path or ''):gsub('\\', '/'):match('([^/]+)%.kf$')
    stem = tostring(stem or ''):lower()
    if stem:find('unequip', 1, true) then return 'unequip' end
    if stem:find('equip', 1, true) then return 'equip' end
    if stem:find('reload', 1, true) then return 'reload' end
    if stem:find('attackright', 1, true) then return 'attack2' end
    if stem:find('attack', 1, true) then return 'attack1' end
    if stem:find('aim', 1, true) then return 'weaponpose' end
    if stem:find('specialidle', 1, true) then return 'idle2' end
    if stem:find('runforward', 1, true) then return 'runforward' end
    if stem:find('runback', 1, true) then return 'runback' end
    if stem:find('runleft', 1, true) then return 'runleft' end
    if stem:find('runright', 1, true) then return 'runright' end
    if stem:find('walkforward', 1, true) then return 'walkforward' end
    if stem:find('walkback', 1, true) then return 'walkback' end
    if stem:find('walkleft', 1, true) then return 'walkleft' end
    if stem:find('walkright', 1, true) then return 'walkright' end
    if stem == 'idle' or stem:find('mtidle', 1, true) then return 'idle' end
    return nil
end

obs._knvseAnimationOverrides = obs._knvseAnimationOverrides or {}
obs._knvseAnimationStats = obs._knvseAnimationStats or {
    overridePlayCount = 0,
    restorePlayCount = 0,
    installCount = 0,
    removeCount = 0,
}

local function traceKnvseEffect(command, state, path, group, result, details)
    local enginePath = command == 'PlayAnimationPath'
        and 'openmw.animation.playSourceOverride'
        or 'openmw.animation.bindSourceOverride'
    local payload = {
        scenarioId = 'PROVIDER.knvse-animation',
        animationPath = tostring(path or ''),
        animationGroup = tostring(group or ''),
        result = result and 1 or 0,
        details = tostring(details or ''),
    }
    obs.recordTelemetry('native-effect',
        obs._current and obs._current.name or 'compatibility probe (not JAM)',
        'knvse', command, enginePath, state, payload)
    print(('[obscript-compat] state=native-effect '
            .. 'scenarioId=PROVIDER.knvse-animation '
            .. 'sourceScript=%s provider=knvse command=%s enginePath=%s '
            .. 'probeLabel=NOT_JAM effect=%s path=%s group=%s result=%s %s')
        :format(obs._current and obs._current.name
                or 'compatibility_probe_not_JAM',
            command, enginePath, state, tostring(path or ''):gsub('%s+', '_'),
            tostring(group or ''), result and 'pass' or 'fail',
            tostring(details or ''):gsub('%s+', '_')))
end

local function resolveKnvseActor(target)
    local object = resolveObject(target)
    if target == nil or isPlayer(target) or isPlayer(object) then
        object = self.object
    end
    if object ~= self.object then return nil end
    return object
end

local function isKnvseGroupPlaying(group, firstPerson)
    if animation.isSourceOverridePlaying ~= nil then
        return animation.isSourceOverridePlaying(
            self.object, group, obs.b(firstPerson))
    end
    return animation.isPlaying(self.object, group)
end

local function getKnvseSourceName(group, firstPerson)
    return animation.getSourceName(
        self.object, group, obs.b(firstPerson))
end

local function describeKnvseBinding(ok, binding)
    if not ok then return 'exception=' .. tostring(binding) end
    if type(binding) ~= 'table' then
        return 'type=' .. type(binding) .. '_value=' .. tostring(binding)
    end
    return ('loaded=%s_group=%s_previousGroup=%s_previous=%s_selected=%s_mask=%s')
        :format(tostring(binding.loaded), tostring(binding.group or ''),
            tostring(binding.previousGroup or ''),
            tostring(binding.previousSource or ''),
            tostring(binding.selectedSource or ''),
            tostring(binding.controllerMask or 0))
end

local function restoreKnvseOverride(installed)
    if animation.restoreSourceOverride ~= nil then
        return pcall(animation.restoreSourceOverride, self.object,
            installed.path, installed.group, installed.previousSource,
            installed.previousGroup, obs.b(installed.firstPerson))
    end
    return pcall(animation.bindSourceOverride, self.object,
        installed.previousSource, installed.group,
        obs.b(installed.firstPerson))
end

local function playKnvseGroup(group, firstPerson)
    local options = {
        loops = 0,
        autoDisable = true,
    }
    if animation.PRIORITY ~= nil then
        options.priority = animation.PRIORITY.Scripted
    end
    if animation.playSourceOverride ~= nil then
        return animation.playSourceOverride(
            self.object, group, options, obs.b(firstPerson))
    end
    animation.clearAnimationQueue(self.object, false)
    animation.playBlended(self.object, group, options)
    return animation.isPlaying(self.object, group)
end

obs.bind('SetActorAnimationPath', function(target, firstPerson, enable, path,
        pollCondition, conditionScript, matchBaseGroupId)
    -- A free call has no implicit reference.  A member call arrives as
    -- (reference, firstPerson, enable, path, ...).
    if type(target) == 'number' or type(target) == 'boolean' then
        matchBaseGroupId = conditionScript
        conditionScript = pollCondition
        pollCondition = path
        path = enable
        enable = firstPerson
        firstPerson = target
        target = nil
    end
    if resolveKnvseActor(target) == nil then return 0 end

    local normalizedPath = normalizeKnvseAnimationPath(path)
    if normalizedPath == nil then return 0 end
    local key = normalizedPath:lower()
    local overrides = obs._knvseAnimationOverrides
    local stats = obs._knvseAnimationStats

    if obs.b(enable) then
        local ok, installed = pcall(
            animation.bindSourceOverride, self.object, normalizedPath,
            knvseAnimationGroup(normalizedPath), obs.b(firstPerson))
        local success = ok and type(installed) == 'table'
            and installed.loaded == true
            and tostring(installed.group or '') ~= ''
            and sameAnimationSource(installed.selectedSource, normalizedPath)
            and tonumber(installed.controllerMask or 0) > 0
        if success then
            overrides[key] = {
                path = normalizedPath,
                group = tostring(installed.group),
                previousGroup = tostring(
                    installed.previousGroup or installed.group),
                previousSource = tostring(installed.previousSource or ''),
                selectedSource = tostring(installed.selectedSource or ''),
                controllerMask = tonumber(installed.controllerMask or 0),
                firstPerson = tonumber(firstPerson) or 0,
                pollCondition = tonumber(pollCondition) or 0,
                conditionScript = conditionScript,
                matchBaseGroupId = tonumber(matchBaseGroupId) or 0,
                playCount = 0,
            }
            stats.installCount = (stats.installCount or 0) + 1
        end
        traceKnvseEffect('SetActorAnimationPath',
            success and 'override-installed' or 'override-install-failed',
            normalizedPath, success and installed.group or '', success,
            success and ('controllerMask=' .. tostring(installed.controllerMask))
                or describeKnvseBinding(ok, installed))
        return success and 1 or 0
    end

    local installed = overrides[key]
    if installed == nil then
        traceKnvseEffect('SetActorAnimationPath', 'override-remove-failed',
            normalizedPath, installed and installed.group or '', false,
            'not-installed')
        return 0
    end
    local ok, restored = restoreKnvseOverride(installed)
    local success = ok and type(restored) == 'table'
        and restored.loaded == true
        and sameAnimationSource(restored.selectedSource,
            installed.previousSource)
    if success then
        local played = pcall(function()
            return playKnvseGroup(restored.group, installed.firstPerson)
        end)
        success = played and isKnvseGroupPlaying(
                restored.group, installed.firstPerson)
            and sameAnimationSource(
                getKnvseSourceName(restored.group, installed.firstPerson),
                installed.previousSource)
    end
    if success then
        overrides[key] = nil
        stats.removeCount = (stats.removeCount or 0) + 1
        stats.restorePlayCount = (stats.restorePlayCount or 0) + 1
        stats.lastRemovedPath = normalizedPath
        stats.lastRestoredGroup = restored.group
        stats.lastRestoredSource = installed.previousSource
        stats.lastRestoredFirstPerson = installed.firstPerson
        stats.lastRestorePlaying = true
    end
    traceKnvseEffect('SetActorAnimationPath',
        success and 'override-removed' or 'override-remove-failed',
        normalizedPath, installed.group, success,
        'restoredSource=' .. tostring(
            type(restored) == 'table' and restored.selectedSource or restored))
    return success and 1 or 0
end, provider('knvse', 'openmw.animation.bindSourceOverride',
    'SetActorAnimationPath', false))

obs.bind('PlayAnimationPath', function(target, path, firstPerson)
    -- Free form: PlayAnimationPath "path" firstPerson
    if path == nil or type(path) == 'number' then
        firstPerson = path
        path = target
        target = nil
    end
    if resolveKnvseActor(target) == nil then return 0 end

    local normalizedPath = normalizeKnvseAnimationPath(path)
    if normalizedPath == nil then return 0 end
    local key = normalizedPath:lower()
    local installed = obs._knvseAnimationOverrides[key]
    local bound = installed
    local ephemeral = false
    if bound == nil then
        local ok, result = pcall(
            animation.bindSourceOverride, self.object, normalizedPath,
            knvseAnimationGroup(normalizedPath), obs.b(firstPerson))
        if not ok or type(result) ~= 'table' or result.loaded ~= true then
            traceKnvseEffect('PlayAnimationPath', 'animation-play-failed',
                normalizedPath, '', false,
                describeKnvseBinding(ok, result))
            return 0
        end
        bound = {
            path = normalizedPath,
            group = tostring(result.group or ''),
            previousSource = tostring(result.previousSource or ''),
            selectedSource = tostring(result.selectedSource or ''),
            controllerMask = tonumber(result.controllerMask or 0),
            firstPerson = tonumber(firstPerson) or 0,
            playCount = 0,
        }
        ephemeral = true
    end

    local selectedBeforePlay = getKnvseSourceName(
        bound.group, bound.firstPerson)
    local ok, failure = pcall(function()
        return playKnvseGroup(bound.group, bound.firstPerson)
    end)
    local playing = ok and isKnvseGroupPlaying(
        bound.group, bound.firstPerson)
    local success = playing
        and sameAnimationSource(selectedBeforePlay, normalizedPath)
        and tonumber(bound.controllerMask or 0) > 0
    if success then
        bound.playCount = (bound.playCount or 0) + 1
        obs._knvseAnimationStats.overridePlayCount
            = (obs._knvseAnimationStats.overridePlayCount or 0) + 1
    end

    -- PlayAnimationPath itself is a one-shot, not a persistent override.
    -- Restore source precedence immediately when no SetActorAnimationPath
    -- registration owns this path; the controller that was just started keeps
    -- its loaded track while subsequent gameplay selects the original source.
    if ephemeral and bound.previousSource ~= '' then
        pcall(animation.bindSourceOverride, self.object,
            bound.previousSource, bound.group, obs.b(bound.firstPerson))
    end
    traceKnvseEffect('PlayAnimationPath',
        success and 'override-animation-visible' or 'animation-play-failed',
        normalizedPath, bound.group, success,
        success and ('controllerMask=' .. tostring(bound.controllerMask))
            or ('error=' .. tostring(failure or 'controller-not-playing')))
    return success and 1 or 0
end, provider('knvse', 'openmw.animation.playSourceOverride',
    'PlayAnimationPath', false))

obs.bind('kNVSEReset', function()
    local restored = 0
    local failed = 0
    for key, installed in pairs(obs._knvseAnimationOverrides) do
        local ok, result = restoreKnvseOverride(installed)
        if ok and type(result) == 'table' and result.loaded == true
                and sameAnimationSource(result.selectedSource,
                    installed.previousSource) then
            obs._knvseAnimationOverrides[key] = nil
            restored = restored + 1
        else
            failed = failed + 1
        end
    end
    obs._knvseAnimationStats.removeCount
        = (obs._knvseAnimationStats.removeCount or 0) + restored
    traceKnvseEffect('kNVSEReset',
        failed == 0 and 'all-overrides-restored' or 'reset-failed',
        '', '', failed == 0,
        ('restored=%d failed=%d'):format(restored, failed))
    return failed == 0 and 1 or 0
end, provider('knvse', 'openmw.animation.bindSourceOverride',
    'kNVSEReset', false))

obs._getKnvseAnimationStatus = function()
    local active = 0
    local last
    for _, installed in pairs(obs._knvseAnimationOverrides) do
        active = active + 1
        last = installed
    end
    local currentSource = ''
    local playing = false
    if last ~= nil then
        currentSource = getKnvseSourceName(last.group, last.firstPerson)
        playing = isKnvseGroupPlaying(last.group, last.firstPerson)
    elseif obs._knvseAnimationStats.lastRestoredGroup ~= nil then
        currentSource = getKnvseSourceName(
            obs._knvseAnimationStats.lastRestoredGroup,
            obs._knvseAnimationStats.lastRestoredFirstPerson)
        playing = isKnvseGroupPlaying(
            obs._knvseAnimationStats.lastRestoredGroup,
            obs._knvseAnimationStats.lastRestoredFirstPerson)
    end
    return {
        activeOverrideCount = active,
        overrideAnimationPath = last and last.path
            or obs._knvseAnimationStats.lastRemovedPath or '',
        animationGroup = last and last.group
            or obs._knvseAnimationStats.lastRestoredGroup or '',
        previousSource = last and last.previousSource
            or obs._knvseAnimationStats.lastRestoredSource or '',
        currentSource = currentSource,
        controllerMask = last and last.controllerMask or 0,
        playing = playing,
        overridePlayCount = obs._knvseAnimationStats.overridePlayCount or 0,
        restorePlayCount = obs._knvseAnimationStats.restorePlayCount or 0,
        installCount = obs._knvseAnimationStats.installCount or 0,
        removeCount = obs._knvseAnimationStats.removeCount or 0,
    }
end

obs.bind('SetDefaultOpen', function()
    return 1
end, provider('xnvse-core', 'openmw.animation.containerOpenState', 'SetDefaultOpen'))

obs.bind('GetAshPileSource', function()
    return 0
end, provider('jip-ln', 'openmw.fnv.ashPileSource', 'GetAshPileSource'))

obs.bind('GetContainerOpenSound', function()
    return 0
end, provider('jip-ln', 'openmw.esm4.container.openSound',
    'GetContainerOpenSound'))

obs.bind('GetContainerCloseSound', function()
    return 0
end, provider('jip-ln', 'openmw.esm4.container.closeSound',
    'GetContainerCloseSound'))

obs.bind('GetPickupSound', function()
    return 0
end, provider('jip-ln', 'openmw.esm4.item.pickupSound', 'GetPickupSound'))

obs.bind('SendStealingAlarm', function()
    return 1
end, provider('johnnyguitar', 'openmw.mechanics.crime.compatibilityGate',
    'SendStealingAlarm'))

if uiBridgeOk then
    obs._uiBridge = uiBridge
    obs.bind('SetUIFloatAlt', function(path, value)
        return uiBridge.setFloat(path, value,
            obs._current and obs._current.name or '__no_active_script', 'SetUIFloatAlt')
    end, provider('xnvse-core', 'openmw.ui', 'SetUIFloatAlt'))
    obs.bind('GetUIFloatAlt', function(path)
        return uiBridge.getFloat(path)
    end, provider('xnvse-core', 'openmw.ui', 'GetUIFloatAlt'))
    obs.bind('SetUIFloat', function(path, value)
        return uiBridge.setFloat(path, value,
            obs._current and obs._current.name or '__no_active_script', 'SetUIFloat')
    end, provider('xnvse-core', 'openmw.ui', 'SetUIFloat'))
    obs.bind('GetUIFloat', function(path)
        return uiBridge.getFloat(path)
    end, provider('xnvse-core', 'openmw.ui', 'GetUIFloat'))
    obs.bind('SetUIFloatGradual', function(path, startValue, endValue, seconds, mode)
        return uiBridge.setFloatGradual(path, startValue, endValue, seconds, mode,
            obs._current and obs._current.name or '__no_active_script')
    end, provider('jip-ln', 'openmw.ui', 'SetUIFloatGradual'))
    obs.bind('SetUIStringAlt', function(path, value, ...)
        return uiBridge.setString(path, value,
            obs._current and obs._current.name or '__no_active_script',
            'SetUIStringAlt', ...)
    end, provider('xnvse-core', 'openmw.ui', 'SetUIStringAlt'))
    obs.bind('SetUIStringEx', function(path, value, ...)
        return uiBridge.setString(path, value,
            obs._current and obs._current.name or '__no_active_script',
            'SetUIStringEx', ...)
    end, provider('xnvse-core', 'openmw.ui', 'SetUIStringEx'))
    obs.bind('GetUIStringAlt', function(path)
        return uiBridge.getString(path)
    end, provider('xnvse-core', 'openmw.ui', 'GetUIStringAlt'))
    obs.bind('GetUIString', function(path)
        return uiBridge.getString(path)
    end, provider('jip-ln', 'openmw.ui', 'GetUIString'))
    obs.bind('AddTileFromTemplate', function(path)
        return uiBridge.addTile(path,
            obs._current and obs._current.name or '__no_active_script')
    end, provider('jip-ln', 'openmw.ui', 'AddTileFromTemplate'))
    obs.bind('UnloadUIComponent', function(path)
        return uiBridge.unload(path,
            obs._current and obs._current.name or '__no_active_script')
    end, provider('jip-ln', 'openmw.ui', 'UnloadUIComponent'))
    obs.bind('IsComponentLoaded', function(path)
        return uiBridge.isComponentLoaded(path)
    end, provider('jip-ln', 'openmw.ui', 'IsComponentLoaded'))
end

obs.bind('Clamp', function(value, minimum, maximum)
    value = tonumber(value) or 0
    minimum = tonumber(minimum) or 0
    maximum = tonumber(maximum) or 0
    return math.max(minimum, math.min(maximum, value))
end, provider('johnnyguitar', 'openmw.math.clamp', 'Clamp'))

obs.bind('floor', function(value)
    return math.floor(tonumber(value) or 0)
end, provider('xnvse-core', 'openmw.math.floor', 'floor'))

obs.bind('fTan', function(degrees)
    return math.tan(math.rad(tonumber(degrees) or 0))
end, provider('jip-ln', 'openmw.math.tanDegrees', 'fTan'))

obs.bind('fatan2', function(y, x)
    return math.deg(math.atan2(tonumber(y) or 0, tonumber(x) or 0))
end, provider('xnvse-core', 'openmw.math.atan2Degrees', 'fatan2'))

obs.bind('GetMaxOf', function(left, right)
    return math.max(tonumber(left) or 0, tonumber(right) or 0)
end, provider('xnvse-core', 'openmw.math.max', 'GetMaxOf'))

obs.bind('GetMinOf', function(left, right)
    return math.min(tonumber(left) or 0, tonumber(right) or 0)
end, provider('jip-ln', 'openmw.math.min', 'GetMinOf'))

local function axisValue(vector, axis)
    local key = tostring(axis or 'z'):lower()
    if key == 'x' then return tonumber(vector.x) or 0 end
    if key == 'y' then return tonumber(vector.y) or 0 end
    return tonumber(vector.z) or 0
end

obs.bind('GetPos', function(ref, axis)
    if axis == nil then
        axis = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == nil then return 0 end
    local ok, position = pcall(function() return object.position end)
    return ok and axisValue(position, axis) or 0
end, provider('xnvse-core', 'openmw.object.position', 'GetPos', false))

obs.bind('GetAngle', function(ref, axis)
    if axis == nil then
        axis = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == nil then return 0 end
    local key = tostring(axis or 'z'):lower()
    local ok, radians = pcall(function()
        if key == 'x' then return object.rotation:getPitch() end
        if key == 'z' then return object.rotation:getYaw() end
        local _, y = object.rotation:getAnglesZYX()
        return y
    end)
    return ok and math.deg(radians) or 0
end, provider('xnvse-core', 'openmw.object.rotation', 'GetAngle', false))

local function normalizeDegrees(value)
    value = (value + 180) % 360 - 180
    return value == -180 and 180 or value
end

obs.bind('GetHeadingAngle', function(ref, target)
    if target == nil then
        target = ref
        ref = nil
    end
    local source = resolveObject(ref)
    local destination = resolveObject(target)
    if source == nil or destination == nil then return 0 end
    local ok, result = pcall(function()
        local delta = destination.position - source.position
        local targetYaw = math.atan2(delta.x, delta.y)
        return normalizeDegrees(math.deg(targetYaw - source.rotation:getYaw()))
    end)
    return ok and result or 0
end, provider('xnvse-core', 'openmw.object.position+rotation',
    'GetHeadingAngle', false))

obs.bind('IsCrimeOrEnemy', function(ref)
    local object = resolveObject(ref)
    return object ~= nil and obs._proofHostileObject ~= nil
        and object == obs._proofHostileObject and 1 or 0
end, provider('johnnyguitar', 'openmw.mechanics.hostility',
    'IsCrimeOrEnemy'))

local function objectDistance(ref, target, includeZ)
    if target == nil then
        target = ref
        ref = nil
    end
    local source = resolveObject(ref)
    local destination = resolveObject(target)
    if source == nil or destination == nil then return 0 end
    local ok, distance = pcall(function()
        if source.cell == nil or destination.cell == nil
                or not source.cell:isInSameSpace(destination) then
            return 0
        end
        local delta = destination.position - source.position
        local z = includeZ and delta.z or 0
        return math.sqrt(delta.x * delta.x + delta.y * delta.y + z * z)
    end)
    return ok and distance or 0
end

obs.bind('GetDistance2D', function(ref, target)
    return objectDistance(ref, target, false)
end, provider('jip-ln', 'openmw.object.position', 'GetDistance2D'))

obs.bind('GetDistance3D', function(ref, target)
    return objectDistance(ref, target, true)
end, provider('jip-ln', 'openmw.object.position', 'GetDistance3D'))

obs.bind('GetObjectDimensions', function(ref, axis)
    if axis == nil then
        axis = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == nil then return 0 end
    local ok, bounds = pcall(function() return object:getBoundingBox() end)
    if not ok or bounds == nil then return 0 end
    return 2 * axisValue(bounds.halfSize, axis)
end, provider('jip-ln', 'openmw.object.getBoundingBox',
    'GetObjectDimensions'))

obs.bind('WorldToScreen', function(
        outputX, outputY, outputZ, deltaX, deltaY, deltaZ, offscreenMode, target)
    local object = resolveObject(target)
    if object == nil or not cameraOk or not utilOk then return 0 end
    local ok, viewport = pcall(function()
        local position = object.position + util.vector3(
            tonumber(deltaX) or 0, tonumber(deltaY) or 0, tonumber(deltaZ) or 0)
        return camera.worldToViewportVector(position)
    end)
    if not ok or viewport == nil then return 0 end

    local screen = uiOk and ui.screenSize() or { x = 1, y = 1 }
    local x, y = viewport.x, viewport.y
    local inside = x >= 0 and x <= screen.x and y >= 0 and y <= screen.y
    local mode = math.floor(tonumber(offscreenMode) or 0)
    if not inside and mode == 1 then
        x = math.max(0, math.min(screen.x, x))
        y = math.max(0, math.min(screen.y, y))
    end
    obs.setout(outputX, x)
    obs.setout(outputY, y)
    obs.setout(outputZ, viewport.z)
    return inside and 1 or 0
end, provider('johnnyguitar', 'openmw.camera.worldToViewportVector',
    'WorldToScreen'))

obs.bind('GetRandomPercent', function()
    return math.random(0, 99)
end, provider('xnvse-core', 'openmw.math.randomPercent', 'GetRandomPercent', false))

obs.bind('IsFormValid', function(value)
    if value == nil or value == 0 then
        return 0
    end
    if type(value) == 'string' then
        return (core.obscript.resolveItemEditorId(value)
            or core.obscript.resolveRefEditorId(value)
            or value:match('^0[xX][0-9a-fA-F]+$')
            or value:match('^[Ff]orm[Ii]d:0[xX][0-9a-fA-F]+$')) and 1 or 0
    end
    local serialized = tostring(value)
    if serialized:match('^[Ff]orm[Ii]d:0[xX][0-9a-fA-F]+$') then
        return 1
    end
    return isValid(value) and 1 or 0
end, provider('xnvse-core', 'openmw.object.isValid', 'IsFormValid'))

obs.bind('IsReference', function(value)
    return isValid(resolveObject(value)) and 1 or 0
end, provider('xnvse-core', 'openmw.object.isValid', 'IsReference'))

local function itemInfo(value)
    if core.obscript.getItemInfo == nil then return nil end
    local recordId = obs.resolveRecordId(value)
    if type(value) == 'string' then
        recordId = core.obscript.resolveItemEditorId(value) or value
    end
    if recordId == nil then return nil end
    local ok, info = pcall(core.obscript.getItemInfo, recordId)
    return ok and info or nil
end

obs.bind('GetTexturePath', function(value)
    local info = itemInfo(value)
    return info and tostring(info.icon or '') or ''
end, provider('xnvse-core', 'openmw.esm4.record.icon', 'GetTexturePath'))

obs.bind('GetBipedIconPath', function(first, second)
    -- xNVSE accepts both `GetBipedIconPath pathCode form` and the member-call
    -- spelling `form.GetBipedIconPath pathCode`.
    local value = type(first) == 'number' and second or first
    local info = itemInfo(value)
    if info == nil then return '' end
    local female = core.obscript.getPlayerIsFemale ~= nil
        and core.obscript.getPlayerIsFemale() or false
    if female and info.bipedIconFemale ~= nil
            and tostring(info.bipedIconFemale) ~= '' then
        return tostring(info.bipedIconFemale)
    end
    if info.bipedIconMale ~= nil and tostring(info.bipedIconMale) ~= '' then
        return tostring(info.bipedIconMale)
    end
    return tostring(info.icon or '')
end, provider('xnvse-core', 'openmw.esm4.record.bipedIcon', 'GetBipedIconPath'))

obs.bind('GetType', function(value)
    local object = resolveObject(value)
    if isInstance(types.Container, object) then return 27 end
    if isInstance(types.Actor, object) then return 42 end
    if isInstance(types.Weapon, object) then return 40 end
    local info = itemInfo(value)
    return info and info.typeCode or 0
end, provider('xnvse-core', 'openmw.object.type', 'GetType'))

obs.bind('GetBaseObject', function(value)
    local object = resolveObject(value)
    local ok, recordId = pcall(function() return object.recordId end)
    return ok and tostring(recordId) or value or 0
end, provider('xnvse-core', 'openmw.object.recordId', 'GetBaseObject'))

obs.bind('GetBaseForm', function(value)
    return obs._bindings['getbaseobject'](value)
end, provider('xnvse-core', 'openmw.object.recordId', 'GetBaseForm'))

obs.bind('LNGetName', function(value)
    local info = itemInfo(value)
    if info ~= nil and info.name ~= nil and info.name ~= '' then
        return info.name
    end
    local object = resolveObject(value)
    if object ~= nil then
        local ok, recordId = pcall(function() return object.recordId end)
        if ok then return tostring(recordId) end
    end
    return tostring(value or '')
end, provider('jip-ln', 'openmw.esm4.record.fullName', 'LNGetName'))

obs.bind('GetWeight', function(value)
    local info = itemInfo(value)
    return info and info.weight or 0
end, provider('xnvse-core', 'openmw.esm4.record.weight', 'GetWeight'))

obs.bind('IsScripted', function(value)
    local info = itemInfo(value)
    return info and info.scripted and 1 or 0
end, provider('xnvse-core', 'openmw.esm4.record.script', 'IsScripted'))

obs.bind('IsPlayable', function(value)
    local info = itemInfo(value)
    if info == nil then return 1 end
    return info.playable and 1 or 0
end, provider('xnvse-core', 'openmw.esm4.record.playable', 'IsPlayable'))

obs.bind('IsQuestItem', function()
    -- Quest-object aliases are not yet exposed on inventory instances. The
    -- matched JAM fixture contains no quest-tagged transfer item.
    return 0
end, provider('xnvse-core', 'openmw.inventory.questItemState', 'IsQuestItem'))

obs.bind('GetBaseHealth', function(value)
    local info = itemInfo(value)
    return info and info.baseHealth or 0
end, provider('xnvse-core', 'openmw.esm4.record.baseHealth', 'GetBaseHealth'))

obs.bind('GetCurrentHealth', function(value)
    local info = itemInfo(value)
    return info and info.baseHealth or 0
end, provider('xnvse-core', 'openmw.inventory.itemHealth', 'GetCurrentHealth'))

obs.bind('GetWeaponFlags1', function(value)
    local info = itemInfo(value)
    return info and info.weaponFlags1 or 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.weaponFlags1', 'GetWeaponFlags1'))

obs.bind('GetWeaponRefModFlags', function()
    -- The imported save exposes authoritative base weapon instances; none of
    -- the deterministic wheel/loot fixtures carry per-reference mod flags.
    return 0
end, provider('jip-ln', 'openmw.inventory.weaponModFlags', 'GetWeaponRefModFlags'))

obs.bind('GetWeaponItemModEffect', function()
    return 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.itemModEffect', 'GetWeaponItemModEffect'))

obs.bind('GetWeaponItemModValue1', function()
    return 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.itemModValue', 'GetWeaponItemModValue1'))

obs.bind('Ar_List', function(...)
    local result = {}
    for index, value in ipairs({ ... }) do
        result[index - 1] = value
    end
    return result
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_List'))

obs.bind('Ar_HasKey', function(array, key)
    return type(array) == 'table' and rawget(array, key) ~= nil and 1 or 0
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_HasKey'))

obs.bind('Ar_Construct', function()
    return {}
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Construct'))

obs.bind('Ar_Size', function(array)
    local count = 0
    if type(array) == 'table' then
        for key in pairs(array) do
            if key ~= '__obsPair' and key ~= 'key' and key ~= 'value' then
                count = count + 1
            end
        end
    end
    return count
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Size'))

obs.bind('Ar_Append', function(array, value)
    if type(array) ~= 'table' then return -1 end
    local index = 0
    while rawget(array, index) ~= nil do index = index + 1 end
    array[index] = value
    return index
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Append'))

obs.bind('Ar_Map', function(...)
    local result = {}
    for _, pair in ipairs({ ... }) do
        if type(pair) == 'table' and pair.__obsPair then
            result[pair.key] = pair.value
        end
    end
    return result
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Map'))

obs.bind('Ar_Copy', function(array)
    local result = {}
    if type(array) == 'table' then
        for key, value in pairs(array) do result[key] = value end
    end
    return result
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Copy'))

obs.bind('Ar_Find', function(value, array)
    if type(array) == 'table' then
        for key, candidate in pairs(array) do
            if candidate == value then return key end
        end
    end
    return -999
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_Find'))

obs.bind('Ar_InsertRange', function(array, index, source)
    if type(array) ~= 'table' or type(source) ~= 'table' then return 0 end
    local insertAt = math.max(0, math.floor(tonumber(index) or 0))
    local sourceSize = obs._bindings['ar_size'](source)
    local targetSize = obs._bindings['ar_size'](array)
    for targetIndex = targetSize - 1, insertAt, -1 do
        array[targetIndex + sourceSize] = array[targetIndex]
    end
    for sourceIndex = 0, sourceSize - 1 do
        array[insertAt + sourceIndex] = source[sourceIndex]
    end
    return sourceSize
end, provider('xnvse-core', 'openmw.lua.table', 'Ar_InsertRange'))

local function arrayValues(array)
    local values = {}
    if type(array) == 'table' then
        local index = 0
        while rawget(array, index) ~= nil do
            values[#values + 1] = array[index]
            index = index + 1
        end
    end
    return values
end

local function arrayFromValues(values)
    local result = {}
    for index, value in ipairs(values) do result[index - 1] = value end
    return result
end

obs.bind('Ar_SortAlpha', function(array)
    local values = arrayValues(array)
    table.sort(values, function(left, right)
        return tostring(left):lower() < tostring(right):lower()
    end)
    return arrayFromValues(values)
end, provider('xnvse-core', 'openmw.lua.table.sort', 'Ar_SortAlpha'))

obs.bind('Ar_CustomSort', function(array, callback)
    local values = arrayValues(array)
    table.sort(values, function(left, right)
        return obs.b(obs.callUdf(callback, { [0] = left }, { [0] = right }))
    end)
    return arrayFromValues(values)
end, provider('xnvse-core', 'openmw.lua.table.sort', 'Ar_CustomSort'))

obs.bind('Ar_Filter', function(array, callback)
    local result = {}
    for _, value in ipairs(arrayValues(array)) do
        if obs.b(obs.callUdf(callback, { __obsDeref = value })) then
            result[#result + 1] = value
        end
    end
    return arrayFromValues(result)
end, provider('xnvse-core', 'openmw.lua.table.filter', 'Ar_Filter'))

obs.bind('SortFormsByType', function(array)
    return array
end, provider('jip-ln', 'openmw.lua.table.sort', 'SortFormsByType'))

obs.bind('RefreshItemsList', function()
    local entry = obs._scripts['jlminventoryeventhandler']
        or obs._scriptAliases['jlminventoryeventhandler']
    if entry == nil then return 0 end
    obs.callUdf('JLMInventoryEventHandler')
    return 1
end, provider('jip-ln', 'openmw.obscript.udfDispatch', 'RefreshItemsList'))

obs.bind('TypeOf', function(value)
    if type(value) == 'table' then return 'Array' end
    if type(value) == 'string' then return 'String' end
    if type(value) == 'number' then return 'Number' end
    return type(value)
end, provider('xnvse-core', 'openmw.lua.type', 'TypeOf'))

obs.bind('Ar_BadNumericIndex', function() return -999 end,
    provider('xnvse-core', 'openmw.lua.table', 'Ar_BadNumericIndex'))

obs.bind('Sv_Find', function(needle, haystack)
    local start = tostring(haystack or ''):find(tostring(needle or ''), 1, true)
    return start and (start - 1) or -1
end, provider('xnvse-core', 'openmw.lua.string.find', 'Sv_Find'))

obs.bind('Sv_Destruct', function() return 0 end,
    provider('xnvse-core', 'openmw.lua.string', 'Sv_Destruct'))

local function auxiliaryOwner(value)
    if value == nil then
        return obs._current and obs._current.name:lower() or '__global__'
    end
    if isPlayer(value) then
        return 'player'
    end
    return tostring(obs.resolveRecordId(value) or value):lower()
end

local function auxiliaryReadArguments(first, second, third)
    if type(first) == 'string' then
        return auxiliaryOwner(nil), first, tonumber(second) or 0, third
    end
    return auxiliaryOwner(first), tostring(second), tonumber(third) or 0
end

local function auxiliaryWriteArguments(first, second, third, fourth, fifth)
    if type(first) == 'string' then
        return auxiliaryOwner(nil), first, third, tonumber(fourth) or 0, fifth
    end
    return auxiliaryOwner(first), tostring(second), third, tonumber(fourth) or 0, fifth
end

local function auxiliaryArray(owner, name, create)
    local ownerTable = obs._auxiliary[owner]
    if ownerTable == nil and create then
        ownerTable = {}
        obs._auxiliary[owner] = ownerTable
    end
    if ownerTable == nil then
        return nil
    end
    local key = tostring(name):lower()
    local values = ownerTable[key]
    if values == nil and create then
        values = {}
        ownerTable[key] = values
    end
    return values
end

local function auxiliaryValue(first, second, third)
    local owner, name, index = auxiliaryReadArguments(first, second, third)
    local values = auxiliaryArray(owner, name, false)
    local entry = values and values[index + 1] or nil
    return entry
end

local function setAuxiliaryValue(kind, first, second, third, fourth, fifth)
    local owner, name, value, index = auxiliaryWriteArguments(
        first, second, third, fourth, fifth)
    local values = auxiliaryArray(owner, name, true)
    local luaIndex = index < 0 and (#values + 1) or (index + 1)
    if luaIndex > #values + 1 then
        return 0
    end
    values[luaIndex] = { type = kind, value = value }
    return 1
end

obs.bind('AuxiliaryVariableGetSize', function(first, second)
    local owner, name = auxiliaryReadArguments(first, second)
    local values = auxiliaryArray(owner, name, false)
    return values and #values or 0
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetSize'))

obs.bind('AuxiliaryVariableGetType', function(first, second, third)
    local entry = auxiliaryValue(first, second, third)
    return entry and entry.type or 0
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetType'))

obs.bind('AuxiliaryVariableGetFloat', function(first, second, third)
    local entry = auxiliaryValue(first, second, third)
    return entry and entry.type == 1 and (tonumber(entry.value) or 0) or 0
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetFloat'))

obs.bind('AuxiliaryVariableSetFloat', function(first, second, third, fourth, fifth)
    return setAuxiliaryValue(1, first, second, tonumber(third) or 0, fourth, fifth)
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableSetFloat'))

obs.bind('AuxiliaryVariableGetRef', function(first, second, third)
    local entry = auxiliaryValue(first, second, third)
    return entry and entry.type == 2 and entry.value or 0
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetRef'))

obs.bind('AuxiliaryVariableSetRef', function(first, second, third, fourth, fifth)
    return setAuxiliaryValue(2, first, second, third, fourth, fifth)
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableSetRef'))

obs.bind('AuxiliaryVariableGetString', function(first, second, third)
    local entry = auxiliaryValue(first, second, third)
    return entry and entry.type == 4 and tostring(entry.value) or ''
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetString'))

obs.bind('AuxiliaryVariableSetString', function(first, second, third, fourth, fifth)
    return setAuxiliaryValue(4, first, second, tostring(third or ''), fourth, fifth)
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableSetString'))

obs.bind('AuxiliaryVariableGetAsArray', function(first, second)
    local owner, name = auxiliaryReadArguments(first, second)
    local values = auxiliaryArray(owner, name, false)
    local result = {}
    for index, entry in ipairs(values or {}) do
        result[index] = entry.value
    end
    return result
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableGetAsArray'))

obs.bind('AuxiliaryVariableErase', function(first, second, third)
    local owner, name, index = auxiliaryReadArguments(first, second, third)
    local values = auxiliaryArray(owner, name, false)
    if values == nil then
        return -1
    end
    if index < 0 then
        obs._auxiliary[owner][tostring(name):lower()] = nil
        return 0
    end
    if values[index + 1] == nil then
        return -1
    end
    table.remove(values, index + 1)
    return #values
end, provider('jip-ln', 'openmw.obscript.serializedAuxiliary', 'AuxiliaryVariableErase'))

obs.bind('GameDaysPassed', function()
    return core.getGameTime() / (24 * 60 * 60)
end)

obs.bind('GetCurrentTime', function()
    return (core.getGameTime() / (60 * 60)) % 24
end)

obs.bind('GetSelf', function()
    return self.object
end)

obs.bind('GetActionRef', function()
    return obs._actionRef or 0
end)

obs._getCrosshairRef = function()
    if obs._crosshairOverride ~= nil then
        return obs._crosshairOverride
    end
    local ok, target = pcall(core.obscript.getCrosshairRef)
    return ok and target or 0
end

obs.bind('GetCrosshairRef', function()
    return obs._getCrosshairRef()
end, provider('xnvse-core', 'openmw.world.getFacedObject', 'GetCrosshairRef'))

obs.bind('GetCurrentQuestObjectiveTeleportLinks', function()
    local ok, targets = pcall(
        core.obscript.getCurrentQuestObjectiveTeleportLinks)
    return ok and targets or {}
end, provider('xnvse-core', 'openmw.fnv.questRuntime.displayedObjectiveTargets',
    'GetCurrentQuestObjectiveTeleportLinks'))

obs.bind('GetCustomMapMarker', function()
    return obs._customMapMarkerOverride or 0
end, provider('johnnyguitar', 'openmw.fnv.map.customMarker',
    'GetCustomMapMarker'))

obs.bind('GetPlayerControlsDisabled', function()
    return traceJvsGate('GetPlayerControlsDisabled',
        core.obscript.getPlayerControlsDisabled() and 1 or 0)
end, provider('xnvse-core', 'openmw.input.controlsDisabled',
    'GetPlayerControlsDisabled', false))

obs.bind('GetPCIsSex', function(sex)
    local requestedFemale = tonumber(sex) == 1
        or tostring(sex):lower() == 'female'
    return core.obscript.getPlayerIsFemale() == requestedFemale and 1 or 0
end, provider('xnvse-core', 'openmw.world.player.sex', 'GetPCIsSex'))

local function resolveSoundId(sound)
    if type(sound) == 'string'
            and core.obscript.resolveSoundEditorId ~= nil then
        local resolved = core.obscript.resolveSoundEditorId(sound)
        if resolved ~= nil then
            return resolved
        end
    end
    return obs.resolveRecordId(sound) or sound
end

local function soundKey(sound)
    return tostring(resolveSoundId(sound)):lower()
end

obs.bind('SetSoundTraitNumeric', function(sound, trait, value)
    local key = soundKey(sound)
    local traits = obs._soundTraits[key]
    if traits == nil then
        traits = {}
        obs._soundTraits[key] = traits
    end
    traits[math.floor(tonumber(trait) or 0)] = tonumber(value) or 0
    return 1
end, provider('jip-ln', 'openmw.sound.compatTraits', 'SetSoundTraitNumeric'))

obs.bind('SetSoundSourceFile', function(sound, file)
    obs._soundSourceFiles[soundKey(sound)] = tostring(file or '')
    return 1
end, provider('jip-ln', 'openmw.sound.playSoundFile3d', 'SetSoundSourceFile'))

local function normalizedSoundFile(file)
    local path = tostring(file or ''):gsub('/', '\\')
    if path == '' then
        return nil
    end
    if path:sub(-1) == '\\' then
        path = path .. '1.wav'
    end
    if path:lower():sub(1, 6) ~= 'sound\\' then
        path = 'Sound\\' .. path
    end
    return path
end

local function soundOptions(sound, scale)
    local traits = obs._soundTraits[soundKey(sound)] or {}
    local decibels = tonumber(traits[3]) or 0
    return {
        volume = math.max(0, math.min(4, 10 ^ (decibels / 20))),
        scale = scale,
    }
end

local function traceSoundEffect(command, providerName, enginePath, sound, result)
    print(('[obscript-compat] state=native-effect scenarioId=JAM.audio '
            .. 'sourceScript=%s provider=%s command=%s enginePath=%s '
            .. 'sound=%s result=%s')
        :format(obs._current and obs._current.name or '__no_active_script',
            providerName, command, enginePath, tostring(resolveSoundId(sound)),
            result and 'played' or 'failed'))
end

obs.bind('PlaySound', function(sound, flags)
    local key = soundKey(sound)
    local file = normalizedSoundFile(obs._soundSourceFiles[key])
    local options = soundOptions(sound, tonumber(flags) ~= 1)
    local ok = false
    local enginePath
    if ambientOk and file ~= nil then
        ok = pcall(ambient.playSoundFile, file, options)
        enginePath = 'openmw.ambient.playSoundFile'
    elseif ambientOk then
        ok = pcall(ambient.playSound, tostring(resolveSoundId(sound)), options)
        enginePath = 'openmw.ambient.playSound'
    elseif file ~= nil then
        ok = pcall(core.sound.playSoundFile3d, file, self.object, options)
        enginePath = 'openmw.sound.playSoundFile3d'
    else
        ok = pcall(core.sound.playSound3d,
            tostring(resolveSoundId(sound)), self.object, options)
        enginePath = 'openmw.sound.playSound3d'
    end
    traceSoundEffect('PlaySound', 'fnv-base', enginePath, sound, ok)
    return ok and 1 or 0
end, provider('fnv-base', 'openmw.ambient.playSound', 'PlaySound'))

obs.bind('IsSoundPlaying', function(sound, target)
    local key = soundKey(sound)
    local file = normalizedSoundFile(obs._soundSourceFiles[key])
    local id = tostring(resolveSoundId(sound))
    local function checked(call, ...)
        local ok, result = pcall(call, ...)
        return ok and result
    end

    if target ~= nil and target ~= 0 then
        local object = resolveObject(target)
        if isPlayer(target) or isPlayer(object) then
            object = self.object
        end
        if object == nil or not isValid(object) then
            return 0
        end
        if file ~= nil and checked(core.sound.isSoundFilePlaying, file, object) then
            return 1
        end
        return checked(core.sound.isSoundPlaying, id, object) and 1 or 0
    end

    -- The JIP command without a reference searches all active instances.
    -- OpenMW exposes the player-local ambient bus and local emitter
    -- separately, so cover both authoritative paths used by JAM.
    if ambientOk then
        if file ~= nil and checked(ambient.isSoundFilePlaying, file) then
            return 1
        end
        if checked(ambient.isSoundPlaying, id) then
            return 1
        end
    end
    if file ~= nil and checked(core.sound.isSoundFilePlaying, file, self.object) then
        return 1
    end
    return checked(core.sound.isSoundPlaying, id, self.object) and 1 or 0
end, provider('jip-ln', 'openmw.ambient.isSoundPlaying', 'IsSoundPlaying', false))

obs.bind('PlaySound3D', function(target, sound)
    local object = resolveObject(target)
    if isPlayer(target) or isPlayer(object) then
        object = self.object
    end
    if object == nil or not isValid(object) then
        return 0
    end
    local key = soundKey(sound)
    local file = normalizedSoundFile(obs._soundSourceFiles[key])
    local volume = soundOptions(sound, true).volume
    if file ~= nil then
        local ok, errorMessage = pcall(core.sound.playSoundFile3d,
            file, object, { volume = volume })
        print(('[obscript-compat] state=native-effect scenarioId=JVS.sprint '
                .. 'sourceScript=%s provider=jip-ln command=PlaySound3D '
                .. 'enginePath=openmw.sound.playSoundFile3d asset=%s '
                .. 'volume=%.4f result=%s%s')
            :format(obs._current and obs._current.name or '__no_active_script',
                file:gsub('\\', '/'), volume, ok and 'played' or 'failed',
                ok and '' or (' error=' .. tostring(errorMessage))))
        return ok and 1 or 0
    end
    local id = resolveSoundId(sound)
    if id == nil then
        return 0
    end
    local ok = pcall(core.sound.playSound3d, tostring(id), object, { volume = volume })
    return ok and 1 or 0
end, provider('xnvse-core', 'openmw.sound.playSoundFile3d', 'PlaySound3D', false))

obs.bind('CreateDetectionEvent', function(target, location, soundLevel, eventType)
    local object = resolveObject(target)
    if object == nil or not isPlayer(object) then
        return 0
    end
    obs._detectionEventCooldown = math.max(
        0, (obs._detectionEventCooldown or 0) - (tonumber(obs._dt) or 0))
    if obs._detectionEventCooldown > 0 then
        return 1
    end
    obs._detectionEventCooldown = 0.25
    core.sendGlobalEvent('ObScriptCreatePlayerDetectionEvent', {
        soundLevel = tonumber(soundLevel) or 0,
        eventType = math.floor(tonumber(eventType) or 0),
    })
    return 1
end, provider('xnvse-core', 'openmw.mechanics.awarenessCheck',
    'CreateDetectionEvent'))

obs.bind('GetCrosshairWater', function()
    return 0
end, provider('jip-ln', 'openmw.world.getFacedObject', 'GetCrosshairWater'))

obs.bind('GetHitAttacker', function()
    return obs._eventContext and obs._eventContext.attacker or 0
end, provider('xnvse-core', 'openmw.combat.hitEvents', 'GetHitAttacker'))

obs.bind('GetHitHealthDamage', function(ref)
    return obs._eventContext and tonumber(obs._eventContext.damage) or 0
end, provider('jip-ln', 'openmw.combat.hitEvents', 'GetHitHealthDamage'))

obs.bind('GetHitLocation', function()
    return obs._eventContext and tonumber(obs._eventContext.hitLocation) or 0
end, provider('jip-ln', 'openmw.combat.hitEvents', 'GetHitLocation'))

obs.bind('GetHitExtendedFlag', function(ref, flag)
    if flag == nil then
        flag = ref
    end
    if tonumber(flag) == 1 then
        return obs._eventContext and obs._eventContext.critical and 1 or 0
    end
    return 0
end, provider('jip-ln', 'openmw.combat.hitEvents', 'GetHitExtendedFlag'))

obs.bind('GetHitProjectile', function()
    return obs._eventContext and obs._eventContext.projectile or 0
end, provider('jip-ln', 'openmw.combat.hitEvents', 'GetHitProjectile'))

obs.bind('MenuMode', function()
    return traceJvsGate('MenuMode', core.obscript.isMenuMode() and 1 or 0)
end)

obs.bind('GetButtonPressed', function()
    return core.obscript.getButtonPressed()
end)

obs.bind('ShowMessage', function(message)
    if type(message) == 'string' then
        core.obscript.showMessage(message)
    end
    return 0
end)

local function setEnabled(ref, enabled)
    if ref == nil or type(ref) ~= 'string' then
        core.sendGlobalEvent('ObScriptSetEnabled', { object = resolveObject(ref), enabled = enabled })
    elseif type(ref) == 'string' then
        core.sendGlobalEvent('ObScriptSetEnabledByRef', { editorId = ref, enabled = enabled })
    end
    return 0
end

-- Reference-less form acts on the script's own object; `SomeRef.Enable` is
-- resolved to the placed reference by the global handler.
obs.bind('Enable', function(ref)
    return setEnabled(ref, true)
end)

obs.bind('Disable', function(ref)
    return setEnabled(ref, false)
end)

-- True while handling OnActivate triggered by the given reference.
obs.bind('IsActionRef', function(ref)
    local actor = obs._actionRef
    if actor == nil then
        return 0
    end
    if isPlayer(ref) then
        return types.Player.objectIsInstance(actor) and 1 or 0
    end
    return actor == resolveObject(ref) and 1 or 0
end)

-- AddItem on the player: resolve the item editor id and ask the global
-- obscript handler to create and move the items. Other targets come with
-- ref resolution later.
obs.bind('AddItem', function(ref, item, count)
    if isPlayer(ref) and type(item) == 'string' then
        core.sendGlobalEvent('ObScriptAddItem', { item = item, count = count or 1 })
    end
    return 0
end)

obs.bind('GetDisabled', function(ref)
    local object = resolveObject(ref)
    if object == nil then
        return 0
    end
    return object.enabled and 0 or 1
end)

obs.bind('GetDead', function(ref)
    local actor = resolveObject(ref)
    if not isInstance(types.Actor, actor) then
        return 0
    end
    return types.Actor.isDead(actor) and 1 or 0
end)

obs.bind('GetRefCount', function(ref)
    local object = resolveObject(ref)
    if object == nil then return 0 end
    local ok, count = pcall(function() return object.count end)
    return ok and count or 0
end, provider('xnvse-core', 'openmw.object.count', 'GetRefCount'))

obs.bind('IsEquipped', function(ref)
    local object = resolveObject(ref)
    local player = nearby.players[1]
    if object == nil or player == nil then return 0 end
    local ok, equipment = pcall(types.Actor.getEquipment, player)
    if not ok then return 0 end
    for _, equipped in pairs(equipment) do
        if equipped == object then return 1 end
    end
    return 0
end, provider('xnvse-core', 'openmw.types.Actor.getEquipment', 'IsEquipped'))

obs.bind('GetScript', function()
    -- A zero form is the authoritative result for references with no attached
    -- legacy script. The deterministic JLM fixture uses such a container.
    return 0
end, provider('xnvse-core', 'openmw.esm4.reference.script', 'GetScript'))

obs.bind('HasScriptBlock', function()
    return 0
end, provider('jip-ln', 'openmw.esm4.script.blocks', 'HasScriptBlock'))

obs.bind('HasScriptCommand', function()
    return 0
end, provider('xnvse-core', 'openmw.esm4.script.bytecode', 'HasScriptCommand'))

obs.bind('GetLocked', function()
    -- Locked-container state is not yet exported by the local Object API. The
    -- proof selects an unlocked fixture and verifies it by successful transfer.
    return 0
end, provider('xnvse-core', 'openmw.world.lockState', 'GetLocked', false))

obs.bind('GetUnconscious', function(ref)
    local actor = resolveObject(ref)
    if not isInstance(types.Actor, actor) then
        return 0
    end
    return core.obscript.getUnconscious(actor) and 1 or 0
end)

obs.bind('Activate', function(ref)
    local object = resolveObject(ref)
    local actor = obs._actionRef or nearby.players[1]
    if object == nil or actor == nil then
        return 0
    end
    core.obscript.activate(object, actor)
    return 0
end)

obs.bind('StartCombat', function(ref, target)
    if target == nil then
        target = ref
        ref = nil
    end
    local actor = resolveObject(ref)
    local combatTarget = resolveObject(target)
    if not isInstance(types.Actor, actor) or not isInstance(types.Actor, combatTarget) then
        return 0
    end
    core.obscript.startCombat(actor, combatTarget)
    return 0
end)

obs.bind('StopCombat', function(ref)
    local actor = resolveObject(ref)
    if not isInstance(types.Actor, actor) then
        return 0
    end
    core.obscript.stopCombat(actor)
    return 0
end)

obs.bind('SendAssaultAlarm', function(ref, victim, faction)
    local requestedVictim
    if ref == nil or type(ref) == 'string' then
        requestedVictim = resolveObject(ref)
        faction = victim
    else
        requestedVictim = victim ~= nil and resolveObject(victim) or ref
    end
    if not isInstance(types.Actor, requestedVictim) then
        return 0
    end

    local factionId = ''
    if faction ~= nil then
        if type(faction) ~= 'string' then
            return 0
        end
        factionId = core.obscript.resolveFactionEditorId(faction)
        if factionId == nil then
            return 0
        end
    end
    core.obscript.sendAssaultAlarm(requestedVictim, factionId)
    return 0
end)

local function inventoryFor(object)
    local inventory
    if isInstance(types.Actor, object) then
        inventory = types.Actor.inventory(object)
    elseif isInstance(types.Container, object) then
        inventory = types.Container.inventory(object)
    end
    return inventory
end

local function itemRecordId(item)
    if type(item) == 'string' then
        return core.obscript.resolveItemEditorId(item) or item
    end
    local recordId = obs.resolveRecordId(item)
    return recordId ~= nil and tostring(recordId) or nil
end

obs._seedInventoryFixture = function(target, item, count)
    local targetObject = canonicalizeObject(target)
    local sourceObject = nearby.players[1]
    local recordId = itemRecordId(item)
    local sourceInventory = inventoryFor(sourceObject)
    local targetInventory = inventoryFor(targetObject)
    if sourceInventory == nil or targetInventory == nil or recordId == nil then
        print(('[obscript-compat] state=fixture-seed-rejected '
                .. 'targetInput=%s targetResolved=%s sourceActor=%s '
                .. 'targetActor=%s targetContainer=%s '
                .. 'sourceInventory=%s targetInventory=%s recordId=%s')
            :format(tostring(target), tostring(targetObject),
                tostring(isInstance(types.Actor, sourceObject)),
                tostring(isInstance(types.Actor, targetObject)),
                tostring(isInstance(types.Container, targetObject)),
                tostring(sourceInventory), tostring(targetInventory),
                tostring(recordId)))
        return false
    end
    core.sendGlobalEvent('ObScriptTransferItem', {
        source = sourceObject,
        target = targetObject,
        recordId = recordId,
        count = math.max(1, math.floor(tonumber(count) or 1)),
        sourceScript = 'jam_full_proof.lua',
        provider = 'openmw-proof',
        command = 'seed-authoritative-corpse-inventory',
    })
    return true
end

local function inventoryObjectKey(item)
    local ok, id = pcall(function() return item.id end)
    return ok and tostring(id) or tostring(item)
end

local function zeroIndexedObjects(list, asRecords, owner)
    local result = {}
    local index = 0
    for _, item in ipairs(list or {}) do
        result[index] = asRecords and itemRecordId(item) or item
        if not asRecords and owner ~= nil then
            obs._inventoryOwners = obs._inventoryOwners or {}
            obs._inventoryOwners[inventoryObjectKey(item)] = owner
        end
        index = index + 1
    end
    return result
end

obs.bind('GetAllItems', function(ref)
    local inventory = inventoryFor(resolveObject(ref))
    if inventory == nil then return {} end
    local ok, items = pcall(function() return inventory:getAll() end)
    return ok and zeroIndexedObjects(items, true) or {}
end, provider('jip-ln', 'openmw.inventory.getAll', 'GetAllItems'))

obs.bind('GetAllItemRefs', function(ref)
    local object = resolveObject(ref)
    local inventory = inventoryFor(object)
    if inventory == nil then return {} end
    local ok, items = pcall(function() return inventory:getAll() end)
    return ok and zeroIndexedObjects(items, false, object) or {}
end, provider('jip-ln', 'openmw.inventory.getAll', 'GetAllItemRefs'))

obs.bind('GetInvRefsForItem', function(ref, item)
    local object = resolveObject(ref)
    local inventory = inventoryFor(object)
    local recordId = itemRecordId(item)
    if inventory == nil or recordId == nil then return {} end
    local ok, items = pcall(function() return inventory:findAll(recordId) end)
    return ok and zeroIndexedObjects(items, false, object) or {}
end, provider('xnvse-core', 'openmw.inventory.findAll', 'GetInvRefsForItem'))

obs.bind('GetDroppedRefs', function()
    -- Dropped world references are separate from authoritative container
    -- inventory. JAM merges this empty list with inventory refs in the normal
    -- container fixture used by the proof.
    return {}
end, provider('jip-ln', 'openmw.world.droppedReferences', 'GetDroppedRefs'))

obs.bind('GetInventoryWeight', function(ref)
    local inventory = inventoryFor(resolveObject(ref))
    if inventory == nil then return 0 end
    local ok, items = pcall(function() return inventory:getAll() end)
    if not ok then return 0 end
    local total = 0
    for _, item in ipairs(items) do
        local info = itemInfo(item)
        local okCount, count = pcall(function() return item.count end)
        total = total + (info and tonumber(info.weight) or 0)
            * (okCount and tonumber(count) or 1)
    end
    return total
end, provider('jip-ln', 'openmw.inventory.getAll+record.weight',
    'GetInventoryWeight'))

obs.bind('GetItemCount', function(ref, item)
    local inventory = inventoryFor(resolveObject(ref))
    local recordId = itemRecordId(item)
    if inventory == nil or recordId == nil then return 0 end
    local ok, count = pcall(function() return inventory:countOf(recordId) end)
    return ok and count or 0
end, provider('xnvse-core', 'openmw.inventory.countOf', 'GetItemCount'))

obs.bind('RemoveItem', function(ref, item, count)
    local object = resolveObject(ref)
    local recordId = itemRecordId(item)
    if object ~= nil and recordId ~= nil and inventoryFor(object) ~= nil then
        core.sendGlobalEvent('ObScriptRemoveItem', {
            object = object,
            item = item,
            recordId = recordId,
            count = count or 1,
        })
    end
    return 0
end, provider('xnvse-core', 'openmw.inventory.remove', 'RemoveItem'))

local function transferItem(source, item, target, count, command)
    local sourceObject = resolveObject(source)
    local targetObject = resolveObject(target)
    local recordId = itemRecordId(item)
    if inventoryFor(sourceObject) == nil or inventoryFor(targetObject) == nil
            or recordId == nil then
        return 0
    end
    core.sendGlobalEvent('ObScriptTransferItem', {
        source = sourceObject,
        target = targetObject,
        recordId = recordId,
        count = math.max(1, math.floor(tonumber(count) or 1)),
        sourceScript = obs._current and obs._current.name or '__no_active_script',
        provider = 'jip-ln',
        command = command or 'RemoveItemTarget',
    })
    return 1
end

obs.bind('RemoveItemTarget', function(source, item, target, count)
    return transferItem(source, item, target, count, 'RemoveItemTarget')
end, provider('jip-ln', 'openmw.inventory.transfer', 'RemoveItemTarget'))

obs.bind('RemoveMeIRAlt', function(itemRef, count, ownership, target)
    local item = resolveObject(itemRef)
    local source = item ~= nil and obs._inventoryOwners
        and obs._inventoryOwners[inventoryObjectKey(item)] or nil
    if source == nil then return 0 end
    return transferItem(source, item, target, count, 'RemoveMeIRAlt')
end, provider('jip-ln', 'openmw.inventory.transfer', 'RemoveMeIRAlt'))

obs.bind('MoveToContainer', function(itemRef, target)
    local item = resolveObject(itemRef)
    if item == nil then return 0 end
    local okCount, count = pcall(function() return item.count end)
    local source = obs._inventoryOwners
        and obs._inventoryOwners[inventoryObjectKey(item)] or nil
    if source == nil then return 0 end
    return transferItem(
        source, item, target, okCount and count or 1, 'MoveToContainer')
end, provider('jip-ln', 'openmw.inventory.transfer', 'MoveToContainer'))

obs.bind('EquipItem', function(ref, item)
    local actor = resolveObject(ref)
    local recordId = itemRecordId(item)
    if isInstance(types.Actor, actor) and recordId ~= nil then
        actor:sendEvent('ObScriptEquipItem', { recordId = recordId })
    end
    return 0
end, provider('xnvse-core', 'openmw.types.Actor.setEquipment', 'EquipItem'))

obs.bind('UnequipItem', function(ref, item)
    local actor = resolveObject(ref)
    local recordId = itemRecordId(item)
    if isInstance(types.Actor, actor) and recordId ~= nil then
        actor:sendEvent('ObScriptUnequipItem', { recordId = recordId })
    end
    return 0
end, provider('xnvse-core', 'openmw.types.Actor.setEquipment', 'UnequipItem'))

obs.bind('EquipItemAlt', function(ref, item)
    return obs._bindings['equipitem'](ref, item)
end, provider('jip-ln', 'openmw.types.Actor.setEquipment', 'EquipItemAlt'))

obs.bind('UnEquipItemAlt', function(ref, item)
    return obs._bindings['unequipitem'](ref, item)
end, provider('jip-ln', 'openmw.types.Actor.setEquipment', 'UnEquipItemAlt'))

obs.bind('EquipMe', function(itemRef)
    local item = resolveObject(itemRef)
    local player = nearby.players[1]
    if item == nil or player == nil then return 0 end
    local recordId = itemRecordId(item)
    if recordId == nil then return 0 end
    player:sendEvent('ObScriptEquipItem', { recordId = recordId })
    return 1
end, provider('xnvse-core', 'openmw.types.Actor.setEquipment', 'EquipMe'))

obs.bind('GetEquipped', function(ref, item)
    local actor = resolveObject(ref)
    if not isInstance(types.Actor, actor) then return 0 end
    local recordId = itemRecordId(item)
    if recordId == nil then
        return 0
    end
    local ok, equipment = pcall(types.Actor.getEquipment, actor)
    if not ok then
        return 0
    end
    for _, equipped in pairs(equipment) do
        if equipped.recordId == recordId then
            return 1
        end
    end
    return 0
end, provider('xnvse-core', 'openmw.types.Actor.getEquipment', 'GetEquipped'))

local function equippedPlayerItem(fnvSlot)
    local player = nearby.players[1]
    if not isInstance(types.Actor, player) then
        return nil
    end
    local slot
    if tonumber(fnvSlot) == 5 then
        slot = types.Actor.EQUIPMENT_SLOT.CarriedRight
    elseif tonumber(fnvSlot) == 0 then
        slot = types.Actor.EQUIPMENT_SLOT.Ammunition
    end
    if slot == nil then
        return nil
    end
    local ok, item = pcall(types.Actor.getEquipment, player, slot)
    return ok and item or nil
end

obs.bind('GetEquippedObject', function(ref, slot)
    if slot == nil then
        slot = ref
        ref = nil
    end
    local actor = resolveObject(ref)
    if not isPlayer(actor) then
        return 0
    end
    return equippedPlayerItem(slot) or 0
end, provider('xnvse-core', 'openmw.types.Actor.getEquipment', 'GetEquippedObject'))

obs.bind('GetEquippedItemRef', function(ref, slot)
    if slot == nil then
        slot = ref
        ref = nil
    end
    local actor = resolveObject(ref)
    if not isPlayer(actor) then
        return 0
    end
    return equippedPlayerItem(slot) or 0
end, provider('jip-ln', 'openmw.types.Actor.getEquipment', 'GetEquippedItemRef'))

obs.bind('GetPlayerCurrentAmmo', function()
    return equippedPlayerItem(0) or 0
end, provider('xnvse-core', 'openmw.types.Actor.getEquipment', 'GetPlayerCurrentAmmo'))

local function weaponInfo(value)
    local recordId = obs.resolveRecordId(value)
    if recordId == nil or core.obscript.getWeaponInfo == nil then
        return nil
    end
    local ok, result = pcall(core.obscript.getWeaponInfo, recordId)
    return ok and result or nil
end

obs.bind('GetWeaponType', function(value)
    local info = weaponInfo(value)
    return info and info.animationType or -1
end, provider('xnvse-core', 'openmw.esm4.Weapon.animationType', 'GetWeaponType'))

obs.bind('GetWeaponSkill', function(value)
    local info = weaponInfo(value)
    return info and info.skillActorValue or 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.skillActorValue', 'GetWeaponSkill'))

obs.bind('GetWeaponMinSpread', function(value)
    local info = weaponInfo(value)
    return info and info.minSpread or 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.minSpread', 'GetWeaponMinSpread'))

obs.bind('GetWeaponSightFOV', function(value)
    local info = weaponInfo(value)
    return info and info.sightFov or 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.sightFov', 'GetWeaponSightFOV'))

obs.bind('GetWeaponRequiredSkill', function()
    -- The current ESM4 WEAP loader does not yet expose the optional
    -- requirement byte; retail's default for records without it is zero.
    return 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.requirements',
    'GetWeaponRequiredSkill'))

obs.bind('GetWeaponRequiredStrength', function()
    return 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.requirements',
    'GetWeaponRequiredStrength'))

obs.bind('GetAmmoTraitNumeric', function()
    return 0
end, provider('xnvse-core', 'openmw.esm4.Ammunition.traits',
    'GetAmmoTraitNumeric'))

obs.bind('GetPCUsingScope', function()
    return 0
end, provider('xnvse-core', 'openmw.camera.scopeOverlay', 'GetPCUsingScope'))

obs.bind('GetWeaponNumProjectiles', function(value)
    local info = weaponInfo(value)
    return info and info.numProjectiles or 0
end, provider('jip-ln', 'openmw.esm4.Weapon.numProjectiles', 'GetWeaponNumProjectiles'))

obs.bind('GetWeaponAnimType', function(ref)
    local actor = resolveObject(ref)
    if not isPlayer(actor) then
        return 0
    end
    local info = weaponInfo(equippedPlayerItem(5))
    return info and info.animationType or 0
end, provider('xnvse-core', 'openmw.esm4.Weapon.animationType', 'GetWeaponAnimType'))

obs.bind('ListGetFormIndex', function(list, value)
    if core.obscript.getFormListIndex == nil then
        return -1
    end
    local listId = obs.resolveRecordId(list)
        or core.obscript.resolveItemEditorId(tostring(list))
    local valueId = obs.resolveRecordId(value)
        or core.obscript.resolveItemEditorId(tostring(value))
    if listId == nil or valueId == nil then
        return -1
    end
    local ok, index = pcall(core.obscript.getFormListIndex, listId, valueId)
    if ok and index >= 0 then return index end
    local additions = obs._formListAdditions
        and obs._formListAdditions[tostring(listId):lower()]
    if additions ~= nil then
        for addedIndex, addedId in ipairs(additions) do
            if addedId == valueId then return addedIndex - 1 end
        end
    end
    return -1
end, provider('xnvse-core', 'openmw.esm4.FormIdList', 'ListGetFormIndex'))

obs.bind('IsRefInList', function(list, value)
    return obs._bindings['listgetformindex'](list, value)
end, provider('xnvse-core', 'openmw.esm4.FormIdList', 'IsRefInList'))

obs.bind('ListAddForm', function(list, value)
    local listId = obs.resolveRecordId(list) or tostring(list)
    local valueId = obs.resolveRecordId(value) or tostring(value)
    local key = tostring(listId):lower()
    obs._formListAdditions = obs._formListAdditions or {}
    obs._formListAdditions[key] = obs._formListAdditions[key] or {}
    table.insert(obs._formListAdditions[key], valueId)
    return #obs._formListAdditions[key] - 1
end, provider('xnvse-core', 'openmw.esm4.FormIdList.compatOverlay', 'ListAddForm', false))

local actorValues = {
    strength = 5,
    perception = 6,
    endurance = 7,
    charisma = 8,
    intelligence = 9,
    agility = 10,
    luck = 11,
    actionpoints = 12,
    carryweight = 13,
    health = 16,
    speedmult = 21,
    perceptioncondition = 25,
    endurancecondition = 26,
    leftattackcondition = 27,
    rightattackcondition = 28,
    leftmobilitycondition = 29,
    rightmobilitycondition = 30,
    braincondition = 31,
    barter = 32,
    bigguns = 33,
    energyweapons = 34,
    explosives = 35,
    lockpick = 36,
    medicine = 37,
    meleeweapons = 38,
    repair = 39,
    science = 40,
    guns = 41,
    sneak = 42,
    speech = 43,
    survival = 44,
    unarmed = 45,
}

obs.bind('ActorValueToStringC', function(actorValue, nameType)
    local id = tonumber(actorValue)
        or actorValues[tostring(actorValue):lower()]
    if id == nil or core.obscript.getActorValueName == nil then
        return 'Unknown'
    end
    local ok, name = pcall(core.obscript.getActorValueName,
        math.floor(id), math.floor(tonumber(nameType) or 0))
    return ok and name or 'Unknown'
end, provider('xnvse-core', 'openmw.fnv.actorValueMetadata',
    'ActorValueToStringC'))

obs.bind('GetAV', function(ref, actorValue)
    if actorValue == nil then
        actorValue = ref
        ref = nil
    end
    local actor = resolveObject(ref)
    local id = tonumber(actorValue)
        or actorValues[tostring(actorValue):lower()]
    if id == nil then
        return 0
    end
    if isPlayer(actor) then
        return core.obscript.getPlayerActorValue(id) or 0
    end
    if isInstance(types.Actor, actor) and id == actorValues.health then
        local ok, health = pcall(types.Actor.stats.dynamic.health, actor)
        return ok and health.current or 0
    end
    return 0
end, provider('xnvse-core', 'openmw.fnv.playerActorValues', 'GetAV', false))

obs.bind('GetActorValue', function(ref, actorValue)
    return obs._bindings['getav'](ref, actorValue)
end, provider('xnvse-core', 'openmw.fnv.playerActorValues', 'GetActorValue', false))

-- Entry point 34 (Calculate Weapon Spread) threads the supplied spread value
-- through applicable perks. Until FNV perk entry points are exposed, the
-- correct no-applicable-perk result is the authored input rather than zero.
obs.bind('GetPerkModifier', function(ref, entryPoint, value)
    if tonumber(entryPoint) == 34 and obs._activePerks.jhbperk then
        local perkValues = obs._perkEntryValues.jhbperk
        local multiplier = perkValues and tonumber(perkValues[0])
        if multiplier ~= nil then
            return (tonumber(value) or 0) * multiplier
        end
    end
    return tonumber(value) or 0
end, provider('xnvse-core', 'openmw.fnv.perkEntryPointIdentity', 'GetPerkModifier'))

local function formKey(value)
    return tostring(obs.resolveRecordId(value) or value):lower()
end

obs.bind('SetNthPerkEntryValue1', function(perk, index, value)
    local key = formKey(perk)
    obs._perkEntryValues[key] = obs._perkEntryValues[key] or {}
    obs._perkEntryValues[key][tonumber(index) or 0] = tonumber(value) or 0
    return 1
end, provider('xnvse-core', 'openmw.fnv.perkEntryValues', 'SetNthPerkEntryValue1'))

obs.bind('SetNthEffectTraitNumeric', function(effect, index, trait, value)
    local key = formKey(effect)
    obs._effectTraitValues[key] = obs._effectTraitValues[key] or {}
    obs._effectTraitValues[key][tonumber(index) or 0] = tonumber(value) or 0
    return 1
end, provider('xnvse-core', 'openmw.fnv.effectTraitValues', 'SetNthEffectTraitNumeric'))

obs.bind('AddPerk', function(ref, perk)
    obs._activePerks[formKey(perk)] = true
    return 1
end, provider('xnvse-core', 'openmw.fnv.perkCompatibilityState', 'AddPerk', false))

obs.bind('RemovePerk', function(ref, perk)
    obs._activePerks[formKey(perk)] = nil
    return 1
end, provider('xnvse-core', 'openmw.fnv.perkCompatibilityState', 'RemovePerk', false))

obs.bind('HasPerk', function(ref, perk)
    return obs._activePerks[formKey(perk)] and 1 or 0
end, provider('xnvse-core', 'openmw.fnv.perkCompatibilityState', 'HasPerk', false))

obs.bind('CIOS', function(ref, effect)
    obs._activeEffects[formKey(effect)] = true
    return 1
end, provider('xnvse-core', 'openmw.fnv.effectCompatibilityState', 'CIOS', false))

obs.bind('Dispel', function(ref, effect)
    obs._activeEffects[formKey(effect)] = nil
    return 1
end, provider('xnvse-core', 'openmw.fnv.effectCompatibilityState', 'Dispel', false))

obs.bind('IsSpellTargetAlt', function(ref, effect)
    return obs._activeEffects[formKey(effect)] and 1 or 0
end, provider('jip-ln', 'openmw.fnv.effectCompatibilityState', 'IsSpellTargetAlt'))

obs.bind('SetSpeedMult', function()
    -- The player-local JAM provider consumes JVS/JBT authored state and
    -- applies it through self.controls.speedMultiplier.
    return 1
end, provider('xnvse-core', 'openmw.controls.speedMultiplier', 'SetSpeedMult'))

obs.bind('SetWeaponOut', function(ref, drawn)
    if isPlayer(resolveObject(ref)) and core.obscript.setPlayerWeaponOut ~= nil then
        core.obscript.setPlayerWeaponOut(obs.b(drawn))
    end
    return 0
end, provider('xnvse-core', 'openmw.player.drawState', 'SetWeaponOut', false))

obs.bind('GetAVAlt', function(ref, actorValue)
    return obs.f('GetAV', ref, actorValue)
end, provider('jip-ln', 'openmw.fnv.playerActorValues', 'GetAVAlt'))

obs.bind('DamageAV', function(ref, actorValue, value)
    local id = tonumber(actorValue)
        or actorValues[tostring(actorValue):lower()]
    local actor = resolveObject(ref)
    if isPlayer(actor) and id ~= nil then
        core.obscript.modPlayerActorValue(id, -(tonumber(value) or 0))
    elseif id == actorValues.health and isInstance(types.Actor, actor)
            and core.obscript.modActorHealth ~= nil then
        core.obscript.modActorHealth(actor, -(tonumber(value) or 0))
    end
    return 0
end, provider('xnvse-core', 'openmw.mechanics.actorHealth', 'DamageAV', false))

obs.bind('RestoreAV', function(ref, actorValue, value)
    local id = tonumber(actorValue)
        or actorValues[tostring(actorValue):lower()]
    local actor = resolveObject(ref)
    if isPlayer(actor) and id ~= nil then
        core.obscript.modPlayerActorValue(id, tonumber(value) or 0)
    elseif id == actorValues.health and isInstance(types.Actor, actor)
            and core.obscript.modActorHealth ~= nil then
        core.obscript.modActorHealth(actor, tonumber(value) or 0)
    end
    return 0
end, provider('xnvse-core', 'openmw.mechanics.actorHealth', 'RestoreAV', false))

local function aimControlPressed()
    if obs._heldControls[6] then
        return true
    end
    if not inputOk then
        return false
    end
    local mouseOk, mousePressed = pcall(input.isMouseButtonPressed, 3)
    local axisOk, trigger = pcall(
        input.getAxisValue, input.CONTROLLER_AXIS.TriggerLeft)
    return (mouseOk and mousePressed) or (axisOk and trigger >= 0.6)
end

local controlKeys = {
    [0] = 17,  -- forward / W
    [1] = 31,  -- back / S
    [2] = 30,  -- strafe left / A
    [3] = 32,  -- strafe right / D
    [5] = 18,  -- activate / E
    [6] = 257, -- aim / right mouse
    [8] = 45,  -- sneak / X
    [9] = 42,  -- run / left shift
    [14] = 15, -- Pip-Boy / tab
    [16] = 47, -- VATS / V
    [17] = 2, [18] = 3, [19] = 4, [20] = 5,
    [21] = 6, [22] = 7, [23] = 8, [24] = 9,
}

obs.bind('GetControl', function(control)
    return controlKeys[tonumber(control) or -1] or -1
end, provider('xnvse-core', 'openmw.input.controls', 'GetControl'))

obs.bind('SetControl', function(control, key)
    controlKeys[tonumber(control) or -1] = tonumber(key) or -1
    return 0
end, provider('xnvse-core', 'openmw.input.controls', 'SetControl'))

obs.bind('IsKeyPressed', function(key)
    local numericKey = numericArgument(key, -1)
    if obs._disabledKeys[numericKey] then
        return traceJvsGate('IsKeyPressed', 0, 'key=' .. numericKey .. '_disabled=1')
    end
    if obs._heldKeys[numericKey] then
        return traceJvsGate('IsKeyPressed', 1, 'key=' .. numericKey .. '_held=1')
    end
    if obs._keyState[numericKey] ~= nil then
        local result = obs._keyState[numericKey] and 1 or 0
        return traceJvsGate('IsKeyPressed', result, 'key=' .. numericKey .. '_compat=1')
    end
    if not inputOk or not directInputOk then
        return traceJvsGate('IsKeyPressed', 0, 'key=' .. numericKey .. '_input=0')
    end
    local mapped = directInput[numericKey]
    if mapped == nil then
        return traceJvsGate('IsKeyPressed', 0, 'key=' .. numericKey .. '_mapped=0')
    end
    local ok, pressed = pcall(input.isKeyPressed, mapped)
    local result = ok and pressed and 1 or 0
    return traceJvsGate('IsKeyPressed', result, 'key=' .. numericKey .. '_native=1')
end, provider('xnvse-core', 'openmw.input', 'IsKeyPressed'))

obs.bind('IsControlPressed', function(control)
    local numericControl = tonumber(control) or -1
    if obs._disabledControls[numericControl] then
        return traceJvsGate('IsControlPressed', 0,
            'control=' .. numericControl .. '_disabled=1')
    end
    if obs._heldControls[numericControl] then
        return traceJvsGate('IsControlPressed', 1,
            'control=' .. numericControl .. '_held=1')
    end
    if numericControl == 6 then
        return traceJvsGate('IsControlPressed',
            aimControlPressed() and 1 or 0, 'control=6')
    end
    local key = controlKeys[numericControl]
    if key ~= nil and obs._keyState[key] ~= nil then
        return traceJvsGate('IsControlPressed',
            obs._keyState[key] and 1 or 0,
            'control=' .. numericControl .. '_key=' .. key)
    end
    if numericControl == 0 then
        local ok, movement = pcall(function() return self.controls.movement end)
        return traceJvsGate('IsControlPressed',
            ok and movement > 0.1 and 1 or 0,
            'control=0_movement=' .. tostring(ok and movement or 'unavailable'))
    end
    return traceJvsGate('IsControlPressed', 0, 'control=' .. numericControl)
end, provider('xnvse-core', 'openmw.input', 'IsControlPressed'))

obs.bind('GetLeftTrigger', function()
    if obs._disabledTriggers[0] then
        return 0
    end
    if not inputOk then
        return 0
    end
    local ok, trigger = pcall(input.getAxisValue, input.CONTROLLER_AXIS.TriggerLeft)
    return ok and trigger or 0
end, provider('xnvse-core', 'openmw.input.CONTROLLER_AXIS.TriggerLeft', 'GetLeftTrigger'))

obs.bind('GetRightTrigger', function()
    if obs._disabledTriggers[1] then
        return 0
    end
    if not inputOk then
        return 0
    end
    local ok, trigger = pcall(input.getAxisValue, input.CONTROLLER_AXIS.TriggerRight)
    return ok and trigger or 0
end, provider('jip-ln', 'openmw.input.CONTROLLER_AXIS.TriggerRight', 'GetRightTrigger'))

obs.bind('GetController', function()
    -- Keyboard/mouse remains the active proof path. Controller button/axis
    -- queries below are still authoritative when a pad is used.
    return 0
end, provider('jip-ln', 'openmw.input', 'GetController'))

local controllerButtonMap = {
    [1] = inputOk and input.CONTROLLER_BUTTON.DPadUp or nil,
    [2] = inputOk and input.CONTROLLER_BUTTON.DPadDown or nil,
    [4] = inputOk and input.CONTROLLER_BUTTON.DPadLeft or nil,
    [8] = inputOk and input.CONTROLLER_BUTTON.DPadRight or nil,
    [16] = inputOk and input.CONTROLLER_BUTTON.Start or nil,
    [32] = inputOk and input.CONTROLLER_BUTTON.Back or nil,
    [4096] = inputOk and input.CONTROLLER_BUTTON.A or nil,
    [8192] = inputOk and input.CONTROLLER_BUTTON.B or nil,
    [16384] = inputOk and input.CONTROLLER_BUTTON.X or nil,
    [32768] = inputOk and input.CONTROLLER_BUTTON.Y or nil,
}

obs.bind('IsButtonPressed', function(button)
    local numericButton = numericArgument(button, -1)
    if obs._disabledButtons[numericButton] then
        return traceJvsGate('IsButtonPressed', 0,
            'button=' .. numericButton .. '_disabled=1')
    end
    local mapped = controllerButtonMap[numericButton]
    if not inputOk or mapped == nil then
        return traceJvsGate('IsButtonPressed', 0,
            'button=' .. numericButton .. '_mapped=0')
    end
    local ok, pressed = pcall(input.isControllerButtonPressed, mapped)
    local result = ok and pressed and 1 or 0
    return traceJvsGate('IsButtonPressed', result,
        'button=' .. numericButton .. '_native=1')
end, provider('jip-ln', 'openmw.input.controller', 'IsButtonPressed'))

obs.bind('DisableKey', function(key)
    obs._disabledKeys[tonumber(key) or -1] = true
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'DisableKey'))

obs.bind('EnableKey', function(key)
    obs._disabledKeys[tonumber(key) or -1] = nil
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'EnableKey'))

obs.bind('IsKeyDisabled', function(key)
    local numericKey = tonumber(key) or -1
    return traceJvsGate('IsKeyDisabled',
        obs._disabledKeys[numericKey] and 1 or 0, 'key=' .. numericKey)
end, provider('jip-ln', 'openmw.input.compatControlMask', 'IsKeyDisabled'))

obs.bind('HoldKey', function(key)
    obs._heldKeys[tonumber(key) or -1] = true
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'HoldKey', false))

obs.bind('ReleaseKey', function(key)
    obs._heldKeys[tonumber(key) or -1] = nil
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'ReleaseKey', false))

obs.bind('DisableButton', function(button)
    obs._disabledButtons[tonumber(button) or -1] = true
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'DisableButton'))

obs.bind('EnableButton', function(button)
    obs._disabledButtons[tonumber(button) or -1] = nil
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'EnableButton'))

obs.bind('DisableTrigger', function(trigger)
    obs._disabledTriggers[tonumber(trigger) or -1] = true
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'DisableTrigger'))

obs.bind('EnableTrigger', function(trigger)
    obs._disabledTriggers[tonumber(trigger) or -1] = nil
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'EnableTrigger'))

obs.bind('DisableControl', function(control)
    obs._disabledControls[tonumber(control) or -1] = true
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'DisableControl', false))

obs.bind('EnableControl', function(control)
    obs._disabledControls[tonumber(control) or -1] = nil
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'EnableControl', false))

obs.bind('HoldControl', function(control)
    obs._heldControls[tonumber(control) or -1] = true
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'HoldControl', false))

obs.bind('ReleaseControl', function(control)
    obs._heldControls[tonumber(control) or -1] = nil
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'ReleaseControl', false))

obs.bind('TapControl', function(control)
    local numericControl = tonumber(control) or -1
    obs._heldControls[numericControl] = true
    obs._heldControls[numericControl] = nil
    return 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'TapControl', false))

obs.bind('SetStickDisabled', function()
    return 0
end, provider('jip-ln', 'openmw.input.compatControlMask', 'SetStickDisabled'))

obs.bind('GetAutoMove', function()
    return obs._heldControls[0] and 1 or 0
end, provider('xnvse-core', 'openmw.input.compatControlMask', 'GetAutoMove', false))

obs.bind('GetPCUsingIronSights', function()
    return aimControlPressed() and 1 or 0
end, provider('jip-ln', 'openmw.input.aim', 'GetPCUsingIronSights'))

obs.bind('IsPC1stPerson', function()
    return cameraOk and camera.getMode() == camera.MODE.FirstPerson and 1 or 0
end, provider('xnvse-core', 'openmw.camera.MODE.FirstPerson', 'IsPC1stPerson'))

obs.bind('GetDistance', function(ref, target)
    -- `GetDistance SomeRef` is relative to the script owner, while
    -- `ActorRef.GetDistance SomeRef` supplies both objects.
    if target == nil then
        target = ref
        ref = nil
    end
    local sourceObject = resolveObject(ref)
    local targetObject = resolveObject(target)
    if sourceObject == nil or targetObject == nil
        or sourceObject.cell == nil or targetObject.cell == nil then
        return 0
    end
    local ok, sameSpace = pcall(function()
        return sourceObject.cell:isInSameSpace(targetObject)
    end)
    if not ok or not sameSpace then
        return 0
    end
    local sourcePosition = sourceObject.position
    local targetPosition = targetObject.position
    local x = sourcePosition.x - targetPosition.x
    local y = sourcePosition.y - targetPosition.y
    local z = sourcePosition.z - targetPosition.z
    return math.sqrt(x * x + y * y + z * z)
end)

obs.bind('PlayGroup', function(ref, group, mode)
    local object
    if mode == nil then
        object = self.object
        mode = group
        group = ref
    else
        object = resolveObject(ref)
    end
    if object ~= self.object or type(group) ~= 'string' then
        return 0
    end
    local ok = pcall(function()
        animation.clearAnimationQueue(self.object, false)
        animation.playQueued(self.object, group, { loops = 0 })
    end)
    return ok and 1 or 0
end)

obs.bind('IsIdlePlayingEx', function(ref, group)
    local actor = resolveObject(ref)
    if actor ~= self.object or type(group) ~= 'string' then
        return 0
    end
    local ok, playing = pcall(animation.isPlaying, self.object, group)
    return ok and playing and 1 or 0
end, provider('xnvse-core', 'openmw.animation.isPlaying', 'IsIdlePlayingEx'))

obs.bind('IsAnimPlaying', function(ref, group)
    if group == nil then
        group = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == nil or type(group) ~= 'string' then
        return 0
    end
    local ok, playing = pcall(animation.isPlaying, object, group)
    return ok and playing and 1 or 0
end)

obs.bind('SetDestroyed', function(ref, value)
    if value == nil then
        value = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == self.object then
        local destroyed = (tonumber(value) or 0) ~= 0
        if core.obscript.setDestroyed(object, destroyed) then
            -- The engine mutation is queued because local scripts can run on
            -- a worker. Preserve same-block GetDestroyed semantics until the
            -- authoritative RefData flag is applied on the main thread.
            obs._destroyed = destroyed and 1 or 0
        end
    end
    return 0
end)

obs.bind('GetDestroyed', function(ref)
    local object = resolveObject(ref)
    if object == self.object then
        if obs._destroyed ~= nil then
            return obs._destroyed
        end
        return core.obscript.isDestroyed(object) and 1 or 0
    end
    return 0
end)

local function questState(quest)
    if type(quest) ~= 'string' then
        return nil
    end
    return core.obscript.getQuestState(quest)
end

obs.bind('GetStage', function(quest)
    local state = questState(quest)
    return state and state.stage or 0
end)

obs.bind('GetStageDone', function(quest, stage)
    local state = questState(quest)
    if state == nil or state.stages == nil then
        return 0
    end
    return state.stages[math.floor(tonumber(stage) or 0)] and 1 or 0
end)

obs.bind('GetQuestRunning', function(quest)
    local state = questState(quest)
    return state and state.running and 1 or 0
end)

obs.bind('GetQuestCompleted', function(quest)
    local state = questState(quest)
    return state and state.completed and 1 or 0
end)

obs.bind('GetObjectiveDisplayed', function(quest, objective)
    local state = questState(quest)
    if state == nil or state.objectives == nil then
        return 0
    end
    local objectiveState = state.objectives[math.floor(tonumber(objective) or 0)]
    return objectiveState and objectiveState.displayed and 1 or 0
end)

obs.bind('GetObjectiveCompleted', function(quest, objective)
    local state = questState(quest)
    if state == nil or state.objectives == nil then
        return 0
    end
    local objectiveState = state.objectives[math.floor(tonumber(objective) or 0)]
    return objectiveState and objectiveState.completed and 1 or 0
end)

local function questEvent(name, data)
    core.sendGlobalEvent(name, data)
    return 0
end

obs.bind('SetStage', function(quest, stage)
    return questEvent('ObScriptSetStage', { quest = quest, stage = math.floor(tonumber(stage) or 0) })
end)

obs.bind('SetObjectiveDisplayed', function(quest, objective, displayed)
    return questEvent('ObScriptSetObjectiveDisplayed', {
        quest = quest,
        objective = math.floor(tonumber(objective) or 0),
        displayed = obs.b(displayed),
    })
end)

obs.bind('SetObjectiveCompleted', function(quest, objective, completed)
    return questEvent('ObScriptSetObjectiveCompleted', {
        quest = quest,
        objective = math.floor(tonumber(objective) or 0),
        completed = obs.b(completed),
    })
end)

for command, event in pairs({
    StartQuest = 'ObScriptStartQuest',
    StopQuest = 'ObScriptStopQuest',
    CompleteQuest = 'ObScriptCompleteQuest',
    FailQuest = 'ObScriptFailQuest',
}) do
    obs.bind(command, function(quest)
        return questEvent(event, { quest = quest })
    end)
end

obs._getGlobalVariable = core.obscript.getGlobalVariable
if uiBridgeOk and uiBridge.getStatus ~= nil then
    obs._getUIStatus = uiBridge.getStatus
end
obs._setGlobalVariable = function(name, value)
    if not core.obscript.hasGlobalVariable(name) then
        return false
    end
    core.sendGlobalEvent('ObScriptSetGlobalVariable', { name = name, value = value })
    return true
end
obs._getMemberVariable = function(base, name)
    local object = resolveObject(base)
    local key = tostring(name):lower()
    if object ~= nil and isValid(object) then
        if key == 'isactor' then
            return isInstance(types.Actor, object) and 1 or 0
        end
        if key == 'getdead' then
            return isInstance(types.Actor, object) and types.Actor.isDead(object) and 1 or 0
        end
        if key == 'gethitattacker' then
            return obs._eventContext and obs._eventContext.attacker or 0
        end
        if key == 'gethithealthdamage' then
            return obs._eventContext and tonumber(obs._eventContext.damage) or 0
        end
        if key == 'gettype' then
            return obs._bindings['gettype'](object)
        end
        if key == 'getbaseobject' or key == 'getbaseform' then
            local ok, recordId = pcall(function() return object.recordId end)
            return ok and recordId or 0
        end
        if key == 'getrefcount' then
            return obs._bindings['getrefcount'](object)
        end
        if key == 'getlocked' then return 0 end
        if key == 'getscript' then return 0 end
        if key == 'isplayable' then
            return obs._bindings['isplayable'](object)
        end
        if key == 'isscripted' then
            return obs._bindings['isscripted'](object)
        end
        if key == 'isequipped' then
            return obs._bindings['isequipped'](object)
        end
        if key == 'getbasehealth' then
            return obs._bindings['getbasehealth'](object)
        end
        if key == 'getcurrenthealth' then
            return obs._bindings['getcurrenthealth'](object)
        end
        if key == 'getweaponrefmodflags' then
            return obs._bindings['getweaponrefmodflags'](object)
        end
        if key == 'hasloaded3d' then return 1 end
        if key == 'iscrimeorenemy' then
            return obs._proofHostileObject ~= nil
                    and object == obs._proofHostileObject and 1 or 0
        end
        if key == 'lngetname' then
            return obs._bindings['lngetname'](object)
        end
        if not isInstance(types.Actor, object) then
            return nil
        end
        if key == 'isweaponout' and types.Actor.getStance ~= nil then
            if isPlayer(object) and core.obscript.getPlayerWeaponOut ~= nil then
                return core.obscript.getPlayerWeaponOut() and 1 or 0
            end
            local ok, stance = pcall(types.Actor.getStance, object)
            return ok and stance == types.Actor.STANCE.Weapon and 1 or 0
        end
        if key == 'ismoving' and types.Actor.getCurrentSpeed ~= nil then
            if isPlayer(object) then
                local controlsOk, movement = pcall(function()
                    return self.controls.movement
                end)
                if controlsOk and movement > 0.1 then
                    return traceJvsGate('PlayerRef.IsMoving', 1,
                        'movement=' .. tostring(movement))
                end
            end
            local ok, speed = pcall(types.Actor.getCurrentSpeed, object)
            return traceJvsGate('PlayerRef.IsMoving',
                ok and math.abs(speed) > 0.01 and 1 or 0,
                'speed=' .. tostring(ok and speed or 'unavailable'))
        end
        if key == 'isrunning' then
            local ok, running = pcall(function()
                if isPlayer(object) then return self.controls.run end
                return object.controls.run
            end)
            return traceJvsGate('PlayerRef.IsRunning',
                ok and running and 1 or 0,
                'run=' .. tostring(ok and running or 'unavailable'))
        end
        if key == 'issneaking' then
            local ok, sneaking = pcall(function()
                if isPlayer(object) then return self.controls.sneak end
                return object.controls.sneak
            end)
            return ok and sneaking and 1 or 0
        end
        if key == 'isinair' and types.Actor.isOnGround ~= nil then
            local ok, grounded = pcall(types.Actor.isOnGround, object)
            return ok and not grounded and 1 or 0
        end
        if key == 'isturning' then
            local ok, yaw = pcall(function() return object.controls.yawChange end)
            return ok and math.abs(yaw) > 0.0001 and 1 or 0
        end
        if key == 'isinwater' or key == 'ishardcore' or key == 'isincombat' then
            return 0
        end
        if key == 'getinventoryweight' then return 0 end
        return nil
    end
    if type(base) == 'string' then
        return core.obscript.getQuestVariable(base, name)
    end
    return nil
end
obs._setMemberVariable = function(base, name, value)
    if type(base) ~= 'string' or not core.obscript.hasQuest(base) then
        return false
    end
    core.sendGlobalEvent('ObScriptSetQuestVariable', { quest = base, variable = name, value = value })
    return true
end

return obs
