---
-- `openmw_aux.obscript.bindings` installs engine bindings for transpiled
-- ObScript running as a local script. Loaded by @{runtime#runtime.makeLocalScript};
-- requires the local context (`openmw.self`).
-- @module bindings
-- @context local

local core = require('openmw.core')
local self = require('openmw.self')
local types = require('openmw.types')

local obs = require('openmw_aux.obscript.runtime')

obs.bind('GetSecondsPassed', function()
    return obs._dt or 0
end)

local function setEnabled(ref, enabled)
    if ref == nil then
        core.sendGlobalEvent('ObScriptSetEnabled', { object = self.object, enabled = enabled })
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
    if type(ref) == 'string' and ref:lower() == 'player' then
        return types.Player.objectIsInstance(actor) and 1 or 0
    end
    return 0
end)

-- AddItem on the player: resolve the item editor id and ask the global
-- obscript handler to create and move the items. Other targets come with
-- ref resolution later.
obs.bind('AddItem', function(ref, item, count)
    if type(ref) == 'string' and ref:lower() == 'player' and type(item) == 'string' then
        core.sendGlobalEvent('ObScriptAddItem', { item = item, count = count or 1 })
    end
    return 0
end)

return obs
