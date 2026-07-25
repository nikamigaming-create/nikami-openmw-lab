-- Global-context handler for world mutations requested by transpiled
-- ObScript local scripts (see openmw_aux/obscript/bindings.lua).

local core = require('openmw.core')
local types = require('openmw.types')
local world = require('openmw.world')

return {
    eventHandlers = {
        ObScriptSetEnabled = function(data)
            data.object.enabled = data.enabled
        end,
        ObScriptSetEnabledByRef = function(data)
            local formId = core.obscript.resolveRefEditorId(data.editorId)
            if formId == nil then
                print('[obscript] SetEnabled: unknown reference editor id: ' .. tostring(data.editorId))
                return
            end
            local obj = world.getObjectByFormId(formId)
            if obj == nil or not obj:isValid() then
                print('[obscript] SetEnabled: no valid object for ' .. tostring(data.editorId)
                    .. ' (' .. tostring(formId) .. ')')
                return
            end
            obj.enabled = data.enabled
        end,
        ObScriptAddItem = function(data)
            local recordId = core.obscript.resolveItemEditorId(data.item)
            if recordId == nil then
                print('[obscript] AddItem: unknown item editor id: ' .. tostring(data.item))
                return
            end
            local count = math.max(1, math.floor(tonumber(data.count) or 1))
            local obj = world.createObject(recordId, count)
            local ok = pcall(function()
                obj:moveInto(types.Actor.inventory(world.players[1]))
            end)
            if not ok then
                -- ESM4 inventory support is not implemented yet; drop the item
                -- and record the limitation instead of erroring.
                obj:remove()
                print('[obscript] AddItem: engine cannot add this item type to inventories yet: '
                    .. tostring(data.item))
            end
        end,
        ObScriptRemoveItem = function(data)
            if data.object == nil or not data.object:isValid() then
                print('[obscript] RemoveItem: target is no longer valid')
                return
            end
            local recordId = data.recordId
                or core.obscript.resolveItemEditorId(data.item)
            if recordId == nil then
                print('[obscript] RemoveItem: unknown item editor id: ' .. tostring(data.item))
                return
            end
            local inventory
            if types.Actor.objectIsInstance(data.object) then
                inventory = types.Actor.inventory(data.object)
            elseif types.Container.objectIsInstance(data.object) then
                inventory = types.Container.inventory(data.object)
            else
                print('[obscript] RemoveItem: target has no inventory: ' .. tostring(data.object))
                return
            end
            local item = inventory:find(recordId)
            if item == nil then
                return
            end
            local count = math.max(1, math.floor(tonumber(data.count) or 1))
            item:remove(math.min(count, item.count))
        end,
        ObScriptTransferItem = function(data)
            if data.source == nil or not data.source:isValid()
                    or data.target == nil or not data.target:isValid() then
                print('[obscript-compat] state=transfer-rejected '
                    .. 'scenarioId=JLM.loot-menu reason=invalid-source-or-target')
                return
            end
            local function inventoryFor(object)
                if types.Actor.objectIsInstance(object) then
                    return types.Actor.inventory(object)
                elseif types.Container.objectIsInstance(object) then
                    return types.Container.inventory(object)
                end
                return nil
            end
            local sourceInventory = inventoryFor(data.source)
            local targetInventory = inventoryFor(data.target)
            local recordId = data.recordId
                or core.obscript.resolveItemEditorId(data.item)
            if sourceInventory == nil or targetInventory == nil or recordId == nil then
                print('[obscript-compat] state=transfer-rejected '
                    .. 'scenarioId=JLM.loot-menu reason=invalid-inventory-or-record '
                    .. 'sourceActor=' .. tostring(types.Actor.objectIsInstance(data.source))
                    .. ' targetActor=' .. tostring(types.Actor.objectIsInstance(data.target))
                    .. ' sourceInventory=' .. tostring(sourceInventory)
                    .. ' targetInventory=' .. tostring(targetInventory)
                    .. ' recordId=' .. tostring(recordId))
                return
            end
            local sourceBefore = sourceInventory:countOf(recordId)
            local targetBefore = targetInventory:countOf(recordId)
            local requested = math.max(1, math.floor(tonumber(data.count) or 1))
            local transferCount = math.min(requested, sourceBefore)
            if transferCount <= 0 then
                return
            end
            local item = sourceInventory:find(recordId)
            if item == nil then return end
            if transferCount < item.count then
                item:split(transferCount):moveInto(targetInventory)
            else
                item:moveInto(targetInventory)
            end
            print(('[obscript-compat] state=native-effect '
                    .. 'scenarioId=JLM.loot-menu sourceScript=%s '
                    .. 'provider=%s command=%s '
                    .. 'enginePath=openmw.inventory.transfer recordId=%s '
                    .. 'sourceBefore=%d targetBefore=%d transferCount=%d')
                :format(tostring(data.sourceScript or 'ObScriptTransferItem'),
                    tostring(data.provider or 'jip-ln'),
                    tostring(data.command or 'RemoveItemTarget'),
                    tostring(recordId), sourceBefore, targetBefore, transferCount))
        end,
        ObScriptSetStage = function(data)
            core.obscript.setQuestStage(data.quest, data.stage)
        end,
        ObScriptSetObjectiveDisplayed = function(data)
            core.obscript.setObjectiveDisplayed(data.quest, data.objective, data.displayed)
        end,
        ObScriptSetObjectiveCompleted = function(data)
            core.obscript.setObjectiveCompleted(data.quest, data.objective, data.completed)
        end,
        ObScriptStartQuest = function(data)
            core.obscript.startQuest(data.quest)
        end,
        ObScriptStopQuest = function(data)
            core.obscript.stopQuest(data.quest)
        end,
        ObScriptCompleteQuest = function(data)
            core.obscript.completeQuest(data.quest)
        end,
        ObScriptFailQuest = function(data)
            core.obscript.failQuest(data.quest)
        end,
        ObScriptSetQuestVariable = function(data)
            core.obscript.setQuestVariable(data.quest, data.variable, data.value)
        end,
        ObScriptSetGlobalVariable = function(data)
            core.obscript.setGlobalVariable(data.name, data.value)
        end,
        ObScriptCreatePlayerDetectionEvent = function(data)
            core.obscript.createPlayerDetectionEvent(
                tonumber(data.soundLevel) or 0,
                math.floor(tonumber(data.eventType) or 0))
        end,
    },
}
