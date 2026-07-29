local core = require('openmw.core')
local nearby = require('openmw.nearby')
local self = require('openmw.self')
local types = require('openmw.types')
local ui = require('openmw.ui')

local cell = nil
local autodoors = {}
local activeAutodoors = {}

local function onCellChange()
    autodoors = {}
    activeAutodoors = {}
    for _, door in ipairs(nearby.doors) do
        if door.type == types.ESM4Door and types.ESM4Door.record(door).isAutomatic then
            autodoors[#autodoors + 1] = door
        end
    end
end

local autodoorActivationDist = 300
local autodoorResetDist = 380

local lastAutoActivation = 0
local function processAutomaticDoors()
    local now = core.getRealTime()
    for _, door in ipairs(autodoors) do
        local doorKey = door.id
        local distance = (door.position - self.position):length()
        if activeAutodoors[doorKey] and distance > autodoorResetDist then
            activeAutodoors[doorKey] = nil
        end
        if door.enabled and not activeAutodoors[doorKey] and distance < autodoorActivationDist
            and now - lastAutoActivation >= 2 then
            print('Automatic activation of', door)
            door:activateBy(self)
            activeAutodoors[doorKey] = true
            lastAutoActivation = now
        end
    end
end

local function onUpdate(dt)
    if dt <= 0 then
        return
    end

    if self.cell ~= cell then
        cell = self.cell
        onCellChange()
    end
    processAutomaticDoors()
end

return {
    engineHandlers = {
        onUpdate = onUpdate,
    },

    eventHandlers = {
        ShowMessage = function(data)
            if data.message then ui.showMessage(data.message) end
        end
    },
}
