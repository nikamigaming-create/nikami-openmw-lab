local core = require('openmw.core')
local ui = require('openmw.ui')
local util = require('openmw.util')

-- Fallout-family content does not ship Morrowind's FontColor GMST records.
-- Keep the stock OpenMW UI scripts usable when a content stack legitimately
-- omits them instead of aborting the entire MWUI bootstrap (which also takes
-- the settings, camera, cursor, and menu scripts down with it).
local function gmstColor(name, fallback)
    local value = core.getGMST(name)
    if value == nil or value == '' then
        value = fallback
    end
    return util.color.commaString(value)
end

return {
    textNormalSize = ui._getDefaultFontSize(),
    textHeaderSize = ui._getDefaultFontSize(),
    headerColor = gmstColor("FontColor_color_header", "223,201,159"),
    normalColor = gmstColor("FontColor_color_normal", "202,165,96"),
    border = 2,
    thickBorder = 4,
    padding = 2,
    whiteTexture = ui.texture { path = 'white' },
}
