local core = require('openmw.core')
local ui = require('openmw.ui')
local util = require('openmw.util')

-- ESM4 games do not carry Morrowind's FontColor_* GMST records.  The generic
-- MWUI module is still loaded for shared UI services, so use the engine's
-- stock fallback colours when a content file does not provide a GMST.
local function colorSetting(id, fallback)
    return util.color.commaString(core.getGMST(id) or fallback)
end

return {
    textNormalSize = ui._getDefaultFontSize(),
    textHeaderSize = ui._getDefaultFontSize(),
    headerColor = colorSetting("FontColor_color_header", "223,201,159"),
    normalColor = colorSetting("FontColor_color_normal", "202,165,96"),
    border = 2,
    thickBorder = 4,
    padding = 2,
    whiteTexture = ui.texture { path = 'white' },
}
