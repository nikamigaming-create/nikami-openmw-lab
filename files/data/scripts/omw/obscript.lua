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
            if obj ~= nil and obj:isValid() then
                obj.enabled = data.enabled
            end
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
    },
}
