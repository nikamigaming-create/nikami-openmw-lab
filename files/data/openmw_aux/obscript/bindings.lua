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
    pcall(function()
        animation.clearAnimationQueue(self.object, false)
        animation.playQueued(self.object, group, { loops = 0 })
    end)
    return 0
end)

obs.bind('SetDestroyed', function(ref, value)
    if value == nil then
        value = ref
        ref = nil
    end
    local object = resolveObject(ref)
    if object == self.object then
        obs._destroyed = tonumber(value) or 0
    end
    return 0
end)

obs.bind('GetDestroyed', function(ref)
    local object = resolveObject(ref)
    if object == self.object then
        return obs._destroyed or 0
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
obs._setGlobalVariable = function(name, value)
    if not core.obscript.hasGlobalVariable(name) then
        return false
    end
    core.sendGlobalEvent('ObScriptSetGlobalVariable', { name = name, value = value })
    return true
end
obs._getMemberVariable = function(base, name)
    if type(base) ~= 'string' then
        return nil
    end
    return core.obscript.getQuestVariable(base, name)
end
obs._setMemberVariable = function(base, name, value)
    if type(base) ~= 'string' or not core.obscript.hasQuest(base) then
        return false
    end
    core.sendGlobalEvent('ObScriptSetQuestVariable', { quest = base, variable = name, value = value })
    return true
end

return obs
