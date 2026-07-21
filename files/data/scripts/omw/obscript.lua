-- Global-context handler for world mutations requested by transpiled
-- ObScript local scripts (see openmw_aux/obscript/bindings.lua).
return {
    eventHandlers = {
        ObScriptSetEnabled = function(data)
            data.object.enabled = data.enabled
        end,
    },
}