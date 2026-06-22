local async = require('openmw.async')
local core = require('openmw.core')
local types = require('openmw.types')
local world = require('openmw.world')
local auxUtil = require('openmw_aux.util')

local EnableObject = async:registerTimerCallback('EnableObject', function(obj) obj.enabled = true end)

local function formatVec3(v)
    if v == nil then
        return '<nil>'
    end
    return string.format('(%.3f,%.3f,%.3f)', v.x, v.y, v.z)
end

local function objectCellText(obj)
    if obj == nil or obj.cell == nil then
        return '<none>'
    end
    return tostring(obj.cell)
end

local function ESM4DoorActivation(door, actor)
    -- TODO: Implement lockpicking minigame
    -- TODO: Play door opening animation
    local Door4 = types.ESM4Door
    local record = Door4.record(door)
    core.sound.playSound3d(record.openSound, actor)
    if Door4.isTeleport(door) then
        local destCell = Door4.destCell(door)
        local destPosition = Door4.destPosition(door)
        local destRotation = Door4.destRotation(door)
        print('FNV/ESM4 door diag: activating teleport door',
            tostring(door),
            'base=' .. tostring(record.id),
            'actor=' .. tostring(actor),
            'fromCell=' .. objectCellText(actor),
            'destCell=' .. tostring(destCell),
            'destPosition=' .. formatVec3(destPosition))
        actor:teleport(destCell, destPosition, destRotation)
        print('FNV/ESM4 door diag: completed teleport door',
            tostring(door),
            'actor=' .. tostring(actor),
            'toCell=' .. objectCellText(actor),
            'toPosition=' .. formatVec3(actor.position))
    else
        print('FNV/ESM4 door diag: activating non-teleport door',
            tostring(door),
            'base=' .. tostring(record.id),
            'actor=' .. tostring(actor),
            'cell=' .. objectCellText(actor))
        door.enabled = false
        async:newSimulationTimer(5, EnableObject, door)
    end
    return false -- disable activation handling in C++ mwmechanics code
end

local function ESM4BookActivation(book, actor)
    if actor.type == types.Player then
        actor:sendEvent('AddUiMode', { mode = 'Book', target = book })
    end
end

local handlersPerObject = {}
local handlersPerType = {}

handlersPerType[types.ESM4Book] = { ESM4BookActivation }
handlersPerType[types.ESM4Door] = { ESM4DoorActivation }

local function onActivate(obj, actor)
    if world.isWorldPaused() then
        return
    end
    if obj.parentContainer then
        return
    end
    local handled = auxUtil.callMultipleEventHandlers({ handlersPerObject[obj.id], handlersPerType[obj.type] }, obj, actor)
    if handled then
        return
    end
    types.Actor.activeEffects(actor):remove('invisibility')
    world._runStandardActivationAction(obj, actor)
end

return {
    interfaceName = 'Activation',
    ---
    -- @module Activation
    -- @context global
    -- @usage require('openmw.interfaces').Activation
    interface = {
        --- Interface version
        -- @field [parent=#Activation] #number version
        version = 0,

        --- Add a new activation handler for a specific object.
        -- If `handler(object, actor)` returns false, other handlers for
        -- the same object (including type handlers) will be skipped.
        -- @function [parent=#Activation] addHandlerForObject
        -- @param openmw.core#GameObject obj The object.
        -- @param #function handler The handler.
        addHandlerForObject = function(obj, handler)
            local handlers = handlersPerObject[obj.id]
            if handlers == nil then
                handlers = {}
                handlersPerObject[obj.id] = handlers
            end
            handlers[#handlers + 1] = handler
        end,

        --- Add a new activation handler for a type of object.
        -- If `handler(object, actor)` returns false, other handlers for
        -- the same object (including type handlers) will be skipped.
        -- @function [parent=#Activation] addHandlerForType
        -- @param #any type A type from the `openmw.types` package.
        -- @param #function handler The handler.
        addHandlerForType = function(type, handler)
            local handlers = handlersPerType[type]
            if handlers == nil then
                handlers = {}
                handlersPerType[type] = handlers
            end
            handlers[#handlers + 1] = handler
        end,
    },
    engineHandlers = { onActivate = onActivate },
}
