local self = require('openmw.self')
local core = require('openmw.core')
local types = require('openmw.types')
local Actor = types.Actor

return {
    eventHandlers = {
        ModifyStat = function(data)
            local stat = Actor.stats.dynamic[data.stat](self)
            stat.current = stat.current + data.amount
        end,
        PlaySound3d = function(data)
            if data.sound then
                core.sound.playSound3d(data.sound, self, data.options)
            else
                core.sound.playSoundFile3d(data.file, self, data.options)
            end
        end,
        BreakInvisibility = function(data)
            Actor.activeEffects(self):remove(core.magic.EFFECT_TYPE.Invisibility)
        end,
        Unequip = function(data)
            local equipment = Actor.getEquipment(self)
            if data.item then
                for slot, item in pairs(equipment) do
                    if item == data.item then
                        equipment[slot] = nil
                    end
                end
            elseif data.slot then
                equipment[slot] = nil
            end
            Actor.setEquipment(self, equipment)
        end,
        ObScriptEquipItem = function(data)
            local item = Actor.inventory(self):find(data.recordId)
            if item == nil then
                print('[obscript] EquipItem: item is not in actor inventory: ' .. tostring(data.recordId))
                return
            end
            local equipment = Actor.getEquipment(self)
            -- The engine redirects this request to the first authored slot
            -- allowed by the item when CarriedRight is not valid (ammo and
            -- apparel included).
            equipment[Actor.EQUIPMENT_SLOT.CarriedRight] = item
            Actor.setEquipment(self, equipment)
        end,
        ObScriptUnequipItem = function(data)
            local equipment = Actor.getEquipment(self)
            for slot, item in pairs(equipment) do
                if item.recordId == data.recordId then
                    equipment[slot] = nil
                end
            end
            Actor.setEquipment(self, equipment)
        end,
    },
}
