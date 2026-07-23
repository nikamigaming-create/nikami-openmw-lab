---
-- `openmw_aux.obscript.bindings` installs engine bindings for transpiled
-- ObScript running as a local script. Loaded by @{runtime#runtime.makeLocalScript};
-- requires the local context (`openmw.self`).
-- @module bindings
-- @context local

local core = require('openmw.core')
local nearby = require('openmw.nearby')
local self = require('openmw.self')
local types = require('openmw.types')

local obs = require('openmw_aux.obscript.runtime')

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
    if ref:lower() == 'player' then
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

local function isPlayer(ref)
    if type(ref) == 'string' then
        return ref:lower() == 'player'
    end
    return isInstance(types.Player, ref)
end

-- Resolve member-call bases before dispatch. Keep an unresolved editor id as
-- a string so it cannot be confused with a reference-less command acting on
-- the script owner; query bindings resolve it once more and return zero.
obs.resolveRef = function(ref)
    return resolveObject(ref) or ref
end

-- `player` is also a value expression (`GetActionRef == player`), not only
-- a member-call base. Binding it lets obs.v resolve that comparison to the
-- actual local player object.
obs.bind('player', function()
    return nearby.players[1] or 0
end)

obs.bind('GetSecondsPassed', function()
    return obs._dt or 0
end)

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

obs.bind('GetItemCount', function(ref, item)
    local object = resolveObject(ref)
    if object == nil or type(item) ~= 'string' then
        return 0
    end

    local inventory
    if isInstance(types.Actor, object) then
        inventory = types.Actor.inventory(object)
    elseif isInstance(types.Container, object) then
        inventory = types.Container.inventory(object)
    else
        return 0
    end

    local recordId = core.obscript.resolveItemEditorId(item)
    if recordId == nil then
        return 0
    end
    local ok, count = pcall(function() return inventory:countOf(recordId) end)
    return ok and count or 0
end)

obs.bind('GetEquipped', function(ref, item)
    local actor = resolveObject(ref)
    if not isInstance(types.Actor, actor) or type(item) ~= 'string' then
        return 0
    end
    local recordId = core.obscript.resolveItemEditorId(item)
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
end)

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

return obs
