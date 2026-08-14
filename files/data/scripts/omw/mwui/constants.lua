<<<<<<< HEAD
local core = require('openmw.core')
=======
>>>>>>> origin/main
local ui = require('openmw.ui')
local util = require('openmw.util')

return {
<<<<<<< HEAD
    textNormalSize = ui._getDefaultFontSize(),
    textHeaderSize = ui._getDefaultFontSize(),
    headerColor = util.color.commaString(core.getGMST("FontColor_color_header")),
    normalColor = util.color.commaString(core.getGMST("FontColor_color_normal")),
=======
    textNormalSize = 16,
    textHeaderSize = 16,
    headerColor = util.color.rgb(223 / 255, 201 / 255, 159 / 255),
    normalColor = util.color.rgb(202 / 255, 165 / 255, 96 / 255),
>>>>>>> origin/main
    border = 2,
    thickBorder = 4,
    padding = 2,
    whiteTexture = ui.texture { path = 'white' },
}