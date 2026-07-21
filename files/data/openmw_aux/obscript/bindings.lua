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

-- Reference-less Enable/Disable act on the script's own object. Visibility is
-- world state, so the change is requested from the obscript global handler.
obs.bind('Enable', function(ref)
    if ref == nil then
        core.sendGlobalEvent('ObScriptSetEnabled', { object = self.object, enabled = true })
    end
    return 0
end)

obs.bind('Disable', function(ref)
    if ref == nil then
        core.sendGlobalEvent('ObScriptSetEnabled', { object = self.object, enabled = false })
    end
    return 0
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

return obs