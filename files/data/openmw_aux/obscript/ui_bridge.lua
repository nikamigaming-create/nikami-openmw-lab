---
-- Native OpenMW projection of Gamebryo UI trait writes used by xNVSE mods.
--
-- The bridge is deliberately state-driven: authored SetUIFloatAlt calls
-- mutate the same trait table that controls the rendered OpenMW widgets and
-- produces proof telemetry. There is no separately painted proof overlay.
-- @module ui_bridge
-- @context local

local core = require('openmw.core')
local camera = require('openmw.camera')
local ui = require('openmw.ui')
local util = require('openmw.util')

local bridge = {}

local traits = {
    ['hudmainmenu\\reticlecenter\\reticle_center\\alpha'] = 255,
    ['hudmainmenu\\reticlecenter\\reticle_center\\systemcolor'] = 1,
    ['hudmainmenu\\reticlecenter\\reticle_center\\visible'] = 1,
    ['hudmainmenu\\jdc\\_jdcalpharc'] = 255,
    ['hudmainmenu\\jdc\\_jdcdynamicoffset'] = 1,
    ['hudmainmenu\\jdc\\_jdcdynamiclength'] = 0,
    ['hudmainmenu\\jdc\\_jdclengthmin'] = 24,
    ['hudmainmenu\\jdc\\_jdclengthmax'] = 72,
    ['hudmainmenu\\jdc\\_jdcoffsetmin'] = 0,
    ['hudmainmenu\\jdc\\_jdcoffsetmax'] = 256,
    ['hudmainmenu\\jdc\\_jdcwidthbase'] = 8,
    ['hudmainmenu\\jdc\\_jdcspread'] = 0.1,
    ['hudmainmenu\\jdc\\_jdcvisible'] = 1,
    ['hudmainmenu\\jdc\\_jdcvisibledot'] = 0,
    ['hudmainmenu\\jdc\\_jdcvisiblereticle'] = 0,
    ['hudmainmenu\\jdc\\systemcolor'] = 1,
    ['hudmainmenu\\jdc\\red'] = 255,
    ['hudmainmenu\\jdc\\green'] = 255,
    ['hudmainmenu\\jdc\\blue'] = 255,
    ['hudmainmenu\\jlm\\_jlmaction1'] = 1,
    ['hudmainmenu\\jlm\\_jlmaction2'] = 7,
    ['hudmainmenu\\jlm\\_jlmaction3'] = 5,
}
local function gameSettingString(name, fallback)
    if core.obscript.getStringGameSetting ~= nil then
        local ok, value = pcall(core.obscript.getStringGameSetting, name)
        if ok and value ~= nil and value ~= '' then
            return value
        end
    end
    return fallback
end

-- Gamebryo resolves these XML entities when JWH.xml is loaded. OpenMW's
-- projection keeps the same trait paths and sources their localized text
-- from FalloutNV.esm so GetUIString observes the authored values.
local strings = {
    ['hudmainmenu\\jwh\\_jwhdam']
        = gameSettingString('sInventoryDamage', 'DAM'),
    ['hudmainmenu\\jwh\\_jwhdps']
        = gameSettingString('sInventoryDamagePerSecond', 'DPS'),
    ['hudmainmenu\\jwh\\_jwhstr']
        = gameSettingString('sInventoryStrReq', 'STR'),
    ['hudmainmenu\\jwh\\_jwhcnd']
        = gameSettingString('sInventoryCondition', 'CND'),
    ['hudmainmenu\\jwh\\_jwhdr']
        = gameSettingString('sInventoryDamageResistance', 'DR'),
    ['hudmainmenu\\jwh\\_jwhdt']
        = gameSettingString('sInventoryDamageThreshold', 'DT'),
    ['hudmainmenu\\jwh\\_jwhwg']
        = gameSettingString('sInventoryWeightUpper', 'WG'),
    ['hudmainmenu\\jwh\\_jwhval']
        = gameSettingString('sInventoryValue', 'VAL'),
    ['hudmainmenu\\jwh\\_jwhhp']
        = gameSettingString('sHitPointsShort', 'HP'),
    ['hudmainmenu\\jlm\\_jlmkeystring1'] = 'E)',
    ['hudmainmenu\\jlm\\_jlmkeystring2'] = 'R)',
    ['hudmainmenu\\jlm\\_jlmkeystring3'] = 'T)',
}
local components = {}
local gameplayState = {
    jbt = 0,
    jhb = 0,
    jvs = 0,
    jwh = 0,
    jlm = 0,
}
local nativeReticleContext = {
    promptVisible = false,
    promptText = '',
}
local externalProofOverlay = false

local function normalize(path)
    return tostring(path or '')
        :gsub('/', '\\')
        :gsub('\\\\+', '\\')
        :lower()
end

local function normalizeUiTexturePath(path)
    local value = tostring(path or '')
        :gsub('\\', '/')
        :gsub('^%s+', '')
        :gsub('%s+$', '')
    if value == '' or value == '0' then return nil end
    if not value:lower():match('^textures/') then
        value = 'textures/' .. value
    end
    return value
end

local function itemInfo(value)
    if core.obscript.getItemInfo == nil then return nil end
    local recordId = value
    if type(value) == 'userdata' then
        local ok, id = pcall(function() return value.recordId end)
        if ok and id ~= nil then recordId = tostring(id) end
    elseif type(value) == 'string'
            and core.obscript.resolveItemEditorId ~= nil then
        recordId = core.obscript.resolveItemEditorId(value) or value
    end
    if recordId == nil or recordId == 0 then return nil end
    local ok, info = pcall(core.obscript.getItemInfo, tostring(recordId))
    return ok and info or nil
end

local function displayName(value)
    local info = itemInfo(value)
    if info ~= nil and info.name ~= nil and tostring(info.name) ~= '' then
        return tostring(info.name)
    end
    return tostring(value or '')
end

local keyNames = {
    [1] = 'ESC', [2] = '1', [3] = '2', [4] = '3', [5] = '4',
    [6] = '5', [7] = '6', [8] = '7', [9] = '8', [10] = '9',
    [11] = '0', [14] = 'BACKSPACE', [15] = 'TAB', [16] = 'Q',
    [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T', [21] = 'Y',
    [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P', [28] = 'ENTER',
    [29] = 'CTRL', [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F',
    [34] = 'G', [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
    [42] = 'SHIFT', [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V',
    [48] = 'B', [49] = 'N', [50] = 'M', [57] = 'SPACE',
}

local function keyName(value)
    local code = math.floor(tonumber(value) or -1)
    return keyNames[code] or tostring(value or '')
end

local function number(path, fallback)
    local value = tonumber(traits[path])
    if value == nil then
        return fallback
    end
    return value
end

local function clamp(value, minimum, maximum)
    return math.max(minimum, math.min(maximum, value))
end

local function lerp(minimum, maximum, amount)
    return minimum + (maximum - minimum) * amount
end

local armTexture = ui.texture {
    -- Gamebryo rotates JDCDefault.dds once for each arm. Lua UI images do not
    -- expose that tile rotation, and a one-pixel DDS atlas crop is unstable
    -- under MyGUI's texture-coordinate transform (it sampled the transparent
    -- black edge on this backend). Resolve the authored white bar to OpenMW's
    -- built-in tintable white UI primitive; the original asset remains the
    -- source/provenance reported by the compatibility bridge.
    path = 'white',
}
local dotTexture = ui.texture { path = 'textures/Interface/JDC/JDCDot.dds' }
local dotBigTexture = ui.texture { path = 'textures/Interface/JDC/JDCDotBig.dds' }
local circleTexture = ui.texture { path = 'textures/Interface/JDC/JDCCircle.dds' }
local vanillaTexture = ui.texture { path = 'textures/Interface/JDC/JDCVanilla.dds' }
local hitIndicatorTexture = ui.texture { path = 'textures/Interface/JHI/JHIHitIndicator.dds' }
local objectiveTexture = ui.texture { path = 'textures/Interface/JVO/JVOClassic.dds' }
local wheelBackgroundTexture = ui.texture { path = 'textures/Interface/JWH/JWHBackGround.dds' }
local wheelForegroundTexture = ui.texture { path = 'textures/Interface/JWH/JWHForeGround.dds' }
local wheelSliceTextures = {}
for index = 1, 11 do
    wheelSliceTextures[index]
        = ui.texture { path = ('textures/Interface/JWH/JWHSlice%d.dds'):format(index) }
end

local function image(resource)
    return {
        type = ui.TYPE.Image,
        props = {
            resource = resource,
            position = util.vector2(0, 0),
            size = util.vector2(1, 1),
            visible = false,
            color = util.color.rgb(1, 1, 1),
            alpha = 1,
            rotation = 0,
            rotationCenter = util.vector2(0, 0),
        },
    }
end

local function arm()
    -- JDCDefault.dds has a two-pixel white core, a half-alpha shoulder, and a
    -- faint outer edge. These three tintable layers preserve that authored
    -- cross-section for both orientations without requiring tile rotation.
    return {
        outer = image(armTexture),
        middle = image(armTexture),
        core = image(armTexture),
    }
end

local arms = {
    up = arm(),
    down = arm(),
    left = arm(),
    right = arm(),
}
local center = image(circleTexture)
local dot = image(dotTexture)
local promptText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(520, 44),
        text = '',
        textSize = 18,
        textColor = util.color.rgb(0.95, 0.95, 0.95),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        textAlignH = ui.ALIGNMENT.Center,
        visible = false,
    },
}
-- JHMHitMarker.dds is an 8x24 antialiased bar that Gamebryo rotates four
-- times to form the hit-marker X. Use the same generic white primitive as
-- JDC and build each diagonal from overlapping samples; this preserves the
-- authored geometry without the unstable narrow-DDS sampling path.
local hitMarkerSegments = {}
for index = 1, 40 do
    hitMarkerSegments[index] = image(armTexture)
end
local hitIndicator = image(hitIndicatorTexture)
local objectiveMarker = image(objectiveTexture)
local wheelBackground = image(wheelBackgroundTexture)
local wheelSlice = image(wheelSliceTextures[11])
local wheelForeground = image(wheelForegroundTexture)
local wheelItemImages = {}
local wheelItemTexturePaths = {}
local wheelItemTextureValid = {}
for index = 1, 8 do
    wheelItemImages[index] = image(armTexture)
end
local wheelNameText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(300, 48),
        text = '',
        textSize = 19,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        textAlignH = ui.ALIGNMENT.Center,
        multiline = true,
        visible = false,
    },
}
local wheelStatsText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(300, 116),
        text = '',
        textSize = 15,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        textAlignH = ui.ALIGNMENT.Center,
        multiline = true,
        visible = false,
    },
}
local lootBackdrop = image(armTexture)
local lootSelection = image(armTexture)
local lootBorders = {
    top = image(armTexture),
    bottom = image(armTexture),
    left = image(armTexture),
    right = image(armTexture),
}
local lootTitleText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(390, 40),
        text = '',
        textSize = 22,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        visible = false,
    },
}
local lootText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(390, 280),
        text = '',
        textSize = 20,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        multiline = true,
        visible = false,
    },
}
local lootControlsText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(390, 34),
        text = '',
        textSize = 15,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        textAlignH = ui.ALIGNMENT.Center,
        visible = false,
    },
}
local featureText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(20, 130),
        size = util.vector2(450, 150),
        text = '',
        textSize = 18,
        textColor = util.color.rgb(1, 0.82, 0.25),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        multiline = true,
        visible = false,
    },
}
local proofText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(470, 132),
        text = 'JAM execution bridge: waiting for authored UI dispatch',
        textSize = 16,
        textColor = util.color.rgb(0.25, 1, 0.42),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        multiline = true,
        visible = true,
    },
}

local elementContent = {
    arms.up.outer,
    arms.up.middle,
    arms.up.core,
    arms.down.outer,
    arms.down.middle,
    arms.down.core,
    arms.left.outer,
    arms.left.middle,
    arms.left.core,
    arms.right.outer,
    arms.right.middle,
    arms.right.core,
    center,
    dot,
    promptText,
}
for _, segment in ipairs(hitMarkerSegments) do
    elementContent[#elementContent + 1] = segment
end
for _, widget in ipairs({
    hitIndicator,
    objectiveMarker,
    wheelBackground,
    wheelSlice,
}) do
    elementContent[#elementContent + 1] = widget
end
for _, widget in ipairs(wheelItemImages) do
    elementContent[#elementContent + 1] = widget
end
for _, widget in ipairs({
    wheelForeground,
    wheelNameText,
    wheelStatsText,
    lootBackdrop,
    lootSelection,
    lootBorders.top,
    lootBorders.bottom,
    lootBorders.left,
    lootBorders.right,
    lootTitleText,
    lootText,
    lootControlsText,
    featureText,
    proofText,
}) do
    elementContent[#elementContent + 1] = widget
end

local element = ui.create {
    layer = 'HUD',
    type = ui.TYPE.Container,
    props = {
        relativeSize = util.vector2(1, 1),
        visible = true,
    },
    content = ui.content(elementContent),
}

local lastProofState = ''
local lastExtentBucket = -1
local updateCount = 0
local callbackState = {
    active = false,
    name = 'none',
    delay = 0,
}
local hitMarkerSeconds = 0
local hitMarkerMode = 0
local hitMarkerSystemColor = 1
local hitIndicatorSeconds = 0
local hitIndicatorAngle = 0
local hitIndicatorMode = 0
local hitIndicatorSystemColor = 1
local hitIndicatorX = 0
local hitIndicatorY = 0
local objectiveScreenX = -1
local objectiveScreenY = -1
local selectedWeaponWheelSlice = 11
local lootMenuRowCount = 0
local lootMenuTitle = ''
local lastModuleState = {}

local function selectedColor()
    local systemColor = math.floor(number('hudmainmenu\\jdc\\systemcolor', 1))
    if systemColor == 2 then
        return util.color.rgb(1, 0.18, 0.12)
    end
    local red = clamp(number('hudmainmenu\\jdc\\red', 255) / 255, 0, 1)
    local green = clamp(number('hudmainmenu\\jdc\\green', 255) / 255, 0, 1)
    local blue = clamp(number('hudmainmenu\\jdc\\blue', 255) / 255, 0, 1)
    return util.color.rgb(red, green, blue)
end

local function emitModuleState(module, state, details)
    local signature = tostring(state)
    if lastModuleState[module] == signature then
        return
    end
    lastModuleState[module] = signature
    print(('[obscript-compat] state=native-effect scenarioId=%s sourceScript=%s '
            .. 'provider=xnvse-core command=GamebryoUI enginePath=openmw.ui '
            .. 'asset=%s visualState=%s details=%s')
        :format(module, tostring(bridge._lastSourceScript or '__runtime__'),
            tostring(bridge._lastAsset or 'authored-JAM-UI'),
            tostring(state), tostring(details or ''):gsub('%s+', '_')))
end

local function compactText(value)
    local text = tostring(value or '')
        :gsub('^%s+', '')
        :gsub('%s+$', '')
    if text == '..' then return '' end
    return text
end

local function updateWheelItemTexture(index, path)
    local normalizedPath = normalizeUiTexturePath(path)
    if wheelItemTexturePaths[index] == normalizedPath then
        return wheelItemTextureValid[index] == true
    end
    wheelItemTexturePaths[index] = normalizedPath
    wheelItemTextureValid[index] = false
    if normalizedPath == nil then return false end
    local ok, resource = pcall(ui.texture, { path = normalizedPath })
    if not ok or resource == nil then return false end
    wheelItemImages[index].props.resource = resource
    wheelItemTextureValid[index] = true
    return true
end

local function wheelStatLine(index)
    local base = ('hudmainmenu\\jwh\\_jwhbox%d'):format(index)
    local visibility = math.floor(number(base .. 'visible', 0))
    if visibility == 0 then return nil end
    local left = compactText(strings[base .. 'textleft'])
    local right = compactText(strings[base .. 'textright'])
    if visibility == 4 then
        right = ('%d%%'):format(math.floor(
            clamp(number(base .. 'value', 0), 0, 1) * 100 + 0.5))
        if left == '' then left = 'CND' end
    end
    if left == '' then return right ~= '' and right or nil end
    if right == '' then return left end
    return left .. '  ' .. right
end

local lootActionNames = {
    [1] = gameSettingString('sTargetTypeTake', 'TAKE'),
    [2] = gameSettingString('sSteal', 'STEAL'),
    [3] = gameSettingString('sInventoryEquip', 'EQUIP'),
    [4] = gameSettingString('sSteal', 'STEAL'),
    [5] = gameSettingString('sTakeAll', 'TAKE ALL'),
    [6] = gameSettingString('sTakeAll', 'TAKE ALL'),
    [7] = gameSettingString('sTargetTypeOpen', 'OPEN'),
    [8] = gameSettingString('sTargetTypeOpen', 'OPEN'),
}

local function lootControlText(index)
    local key = compactText(strings[
        ('hudmainmenu\\jlm\\_jlmkeystring%d'):format(index)])
        :gsub('%)$', '')
    local action = lootActionNames[math.floor(number(
        ('hudmainmenu\\jlm\\_jlmaction%d'):format(index), 0))]
    if key == '' or action == nil or action == '' then return nil end
    return key .. '  ' .. action
end

local function refreshModules(screen)
    local centerX, centerY = screen.x * 0.5, screen.y * 0.5

    local hitMarkerVisible = hitMarkerSeconds > 0
    local hitMarkerColor
    if hitMarkerSystemColor == 2 then
        hitMarkerColor = util.color.rgb(1, 0.18, 0.12)
    else
        hitMarkerColor = util.color.rgb(
            clamp(number('hudmainmenu\\jhm\\red', 255) / 255, 0, 1),
            clamp(number('hudmainmenu\\jhm\\green', 255) / 255, 0, 1),
            clamp(number('hudmainmenu\\jhm\\blue', 255) / 255, 0, 1))
    end
    local hitMarkerScale = hitMarkerMode == 4 and 1.18 or 1
    local hitMarkerGap = (hitMarkerMode == 3 and 6 or 8) * hitMarkerScale
    local hitMarkerStep = 2 * hitMarkerScale
    local hitMarkerSize = (hitMarkerMode == 4 and 4 or 3) * hitMarkerScale
    local hitMarkerAlpha = clamp(hitMarkerSeconds / 0.35, 0, 1)
    local segmentIndex = 1
    for _, direction in ipairs({
        { x = 1, y = 1 },
        { x = 1, y = -1 },
        { x = -1, y = -1 },
        { x = -1, y = 1 },
    }) do
        for step = 0, 9 do
            local distance = hitMarkerGap + step * hitMarkerStep
            local segment = hitMarkerSegments[segmentIndex]
            segmentIndex = segmentIndex + 1
            segment.props.position = util.vector2(
                centerX + direction.x * distance - hitMarkerSize / 2,
                centerY + direction.y * distance - hitMarkerSize / 2)
            segment.props.size = util.vector2(hitMarkerSize, hitMarkerSize)
            segment.props.visible = hitMarkerVisible
            segment.props.alpha = hitMarkerAlpha
            segment.props.color = hitMarkerColor
        end
    end

    -- Match JHI.xml: the 256x256 authored image is offset 24 pixels above the
    -- HUD centre and rotated around (width/2, height/2 + offset).
    local indicatorRadians = math.rad(hitIndicatorAngle)
    local indicatorWidth = math.max(
        32, number('hudmainmenu\\jhi\\_jhiwidth', 256))
    local indicatorHeight = math.max(
        32, number('hudmainmenu\\jhi\\_jhiheight', 256))
    local indicatorOffset = number('hudmainmenu\\jhi\\_jhioffset', 24)
    local indicatorRadius = indicatorHeight * 0.373 + indicatorOffset
    hitIndicatorX = centerX + math.sin(indicatorRadians) * indicatorRadius
    hitIndicatorY = centerY - math.cos(indicatorRadians) * indicatorRadius
    hitIndicator.props.position = util.vector2(
        centerX - indicatorWidth / 2,
        centerY - indicatorHeight / 2 - indicatorOffset)
    hitIndicator.props.size = util.vector2(indicatorWidth, indicatorHeight)
    -- MyGUI's screen-space rotation direction is opposite Gamebryo's
    -- rotateangle convention, so the positive bridge heading maps directly
    -- to the visual compass direction here.
    hitIndicator.props.rotation = indicatorRadians
    hitIndicator.props.rotationCenter = util.vector2(
        indicatorWidth / 2, indicatorHeight / 2 + indicatorOffset)
    hitIndicator.props.visible = hitIndicatorSeconds > 0
    hitIndicator.props.alpha = clamp(hitIndicatorSeconds / 0.35, 0, 1)
    hitIndicator.props.color = hitIndicatorSystemColor == 2
        and util.color.rgb(1, 0.18, 0.12)
        or util.color.rgb(
            clamp(number('hudmainmenu\\jhi\\red', 255) / 255, 0, 1),
            clamp(number('hudmainmenu\\jhi\\green', 255) / 255, 0, 1),
            clamp(number('hudmainmenu\\jhi\\blue', 255) / 255, 0, 1))

    local jvoVisible = number('hudmainmenu\\jvo\\_jvovisible', 0) ~= 0
    local objectiveX = number('hudmainmenu\\jvo\\jvoplayermarker\\_x', screen.x * 0.68)
    local objectiveY = number('hudmainmenu\\jvo\\jvoplayermarker\\_y', screen.y * 0.38)
    if objectiveX >= 0 and objectiveX <= 1 then objectiveX = objectiveX * screen.x end
    if objectiveY >= 0 and objectiveY <= 1 then objectiveY = objectiveY * screen.y end
    objectiveScreenX = objectiveX
    objectiveScreenY = objectiveY
    objectiveMarker.props.position = util.vector2(objectiveX - 28, objectiveY - 28)
    objectiveMarker.props.size = util.vector2(56, 56)
    objectiveMarker.props.visible = jvoVisible and objectiveX >= 0 and objectiveY >= 0

    local wheelVisible = gameplayState.jwh ~= 0
        or number('hudmainmenu\\jwh\\_jwhvisible', 0) ~= 0
    local wheelSize = math.min(560, screen.y * 0.78)
    local wheelPosition = util.vector2(centerX - wheelSize / 2, centerY - wheelSize / 2)
    for _, widget in ipairs({ wheelBackground, wheelSlice, wheelForeground }) do
        widget.props.position = wheelPosition
        widget.props.size = util.vector2(wheelSize, wheelSize)
        widget.props.visible = wheelVisible
    end
    local selectedSlice = math.floor(number('hudmainmenu\\jwh\\_jwhslice', 11))
    selectedSlice = clamp(selectedSlice, 1, 11)
    selectedWeaponWheelSlice = selectedSlice
    wheelSlice.props.resource = wheelSliceTextures[selectedSlice]

    -- JWH.xml places the eight authored inventory icons at these exact
    -- wheel-relative coordinates. The filenames and visibility flags remain
    -- driven by the untouched JWH quest scripts.
    local wheelItemOffsets = {
        { 0.233333338, -0.230769225 },
        { 0.3151041663, -0.080000004 },
        { 0.3151041663, 0.080000004 },
        { 0.233333338, 0.230769225 },
        { -0.233333338, 0.230769225 },
        { -0.3151041663, 0.080000004 },
        { -0.3151041663, -0.080000004 },
        { -0.233333338, -0.230769225 },
    }
    local wheelItemSize = wheelSize / 6
    for index, offset in ipairs(wheelItemOffsets) do
        local widget = wheelItemImages[index]
        local base = ('hudmainmenu\\jwh\\jwhimage%d\\'):format(index)
        local textureValid = updateWheelItemTexture(
            index, strings[base .. 'filename'])
        widget.props.position = util.vector2(
            centerX + offset[1] * wheelSize - wheelItemSize / 2,
            centerY + offset[2] * wheelSize - wheelItemSize / 2)
        widget.props.size = util.vector2(wheelItemSize, wheelItemSize)
        widget.props.visible = wheelVisible and textureValid
            and number(base .. 'visible', 0) ~= 0
        widget.props.color = util.color.rgb(1, 1, 1)
        widget.props.alpha = 1
    end

    local wheelName = compactText(strings['hudmainmenu\\jwh\\_jwhtext'])
    wheelNameText.props.position = util.vector2(
        centerX - wheelSize * 0.22, centerY - wheelSize * 0.31)
    wheelNameText.props.size = util.vector2(wheelSize * 0.44, 52)
    wheelNameText.props.text = wheelName
    wheelNameText.props.visible = wheelVisible and wheelName ~= ''

    local wheelStats = {}
    for index = 1, 8 do
        local line = wheelStatLine(index)
        if line ~= nil then wheelStats[#wheelStats + 1] = line end
    end
    wheelStatsText.props.position = util.vector2(
        centerX - wheelSize * 0.22, centerY + wheelSize * 0.105)
    wheelStatsText.props.size = util.vector2(wheelSize * 0.44, wheelSize * 0.18)
    wheelStatsText.props.text = table.concat(wheelStats, '\n')
    wheelStatsText.props.visible = wheelVisible and #wheelStats > 0

    -- iLootMenu means the authored subsystem is enabled; it is not the
    -- per-target visibility flag.  JLM owns _JLMVisible and only raises it
    -- while a valid crosshair container/corpse is being rendered.
    local lootVisible = number('hudmainmenu\\jlm\\_jlmvisible', 0) ~= 0
    local title = compactText(strings['hudmainmenu\\jlm\\_jlmtitle'])
    if title == '' then title = 'Container' end
    local itemLines = {}
    for index = 0, 9 do
        local value = strings[('hudmainmenu\\jlm\\jlmrect\\item%d\\string'):format(index)]
        if value ~= nil and value ~= '' then
            itemLines[#itemLines + 1] = (index == number(
                'hudmainmenu\\jlm\\_jlmindex', 0) and '> ' or '  ') .. value
        end
    end
    lootMenuRowCount = #itemLines
    if lootMenuRowCount == 0 then
        itemLines[1] = '  waiting for authored inventory rows'
    end
    lootMenuTitle = title

    local panelWidth = 430
    local rowHeight = 28
    local panelHeight = 96 + #itemLines * rowHeight
    local panelX = math.max(20, centerX - 500)
    local panelY = centerY - panelHeight / 2
    local lootColor = number('hudmainmenu\\jlm\\_jlmsystemcolor', 1) == 2
        and util.color.rgb(1, 0.2, 0.14)
        or util.color.rgb(0.35, 1, 0.45)

    lootBackdrop.props.position = util.vector2(panelX, panelY)
    lootBackdrop.props.size = util.vector2(panelWidth, panelHeight)
    lootBackdrop.props.color = util.color.rgb(0.025, 0.035, 0.025)
    lootBackdrop.props.alpha = 0.84
    lootBackdrop.props.visible = lootVisible

    local selectedRow = clamp(math.floor(number(
        'hudmainmenu\\jlm\\_jlmindex', 0)), 0, #itemLines - 1)
    lootSelection.props.position = util.vector2(
        panelX + 10, panelY + 52 + selectedRow * rowHeight)
    lootSelection.props.size = util.vector2(panelWidth - 20, rowHeight)
    lootSelection.props.color = lootColor
    lootSelection.props.alpha = 0.22
    lootSelection.props.visible = lootVisible

    local borderWidth = 2
    for _, border in pairs(lootBorders) do
        border.props.color = lootColor
        border.props.alpha = 0.9
        border.props.visible = lootVisible
    end
    lootBorders.top.props.position = util.vector2(panelX, panelY)
    lootBorders.top.props.size = util.vector2(panelWidth, borderWidth)
    lootBorders.bottom.props.position = util.vector2(
        panelX, panelY + panelHeight - borderWidth)
    lootBorders.bottom.props.size = util.vector2(panelWidth, borderWidth)
    lootBorders.left.props.position = util.vector2(panelX, panelY)
    lootBorders.left.props.size = util.vector2(borderWidth, panelHeight)
    lootBorders.right.props.position = util.vector2(
        panelX + panelWidth - borderWidth, panelY)
    lootBorders.right.props.size = util.vector2(borderWidth, panelHeight)

    lootTitleText.props.position = util.vector2(panelX + 18, panelY + 12)
    lootTitleText.props.size = util.vector2(panelWidth - 36, 36)
    lootTitleText.props.text = title
    lootTitleText.props.textColor = lootColor
    lootTitleText.props.visible = lootVisible

    lootText.props.position = util.vector2(panelX + 20, panelY + 52)
    lootText.props.size = util.vector2(
        panelWidth - 40, #itemLines * rowHeight + 4)
    lootText.props.text = table.concat(itemLines, '\n')
    lootText.props.textColor = lootColor
    lootText.props.visible = lootVisible

    local controls = {}
    for index = 1, 3 do
        local control = lootControlText(index)
        if control ~= nil then controls[#controls + 1] = control end
    end
    lootControlsText.props.position = util.vector2(
        panelX + 16, panelY + panelHeight - 34)
    lootControlsText.props.size = util.vector2(panelWidth - 32, 28)
    lootControlsText.props.text = table.concat(controls, '    ')
    lootControlsText.props.textColor = lootColor
    lootControlsText.props.visible = lootVisible and #controls > 0

    local featureLines = {}
    if gameplayState.jbt ~= 0 then
        featureLines[#featureLines + 1] = 'JBT  BULLET TIME ACTIVE'
    end
    if gameplayState.jhb ~= 0 then
        featureLines[#featureLines + 1] = 'JHB  HOLD BREATH ACTIVE'
    end
    if gameplayState.jvs ~= 0 then
        featureLines[#featureLines + 1] = 'JVS  SPRINT ACTIVE'
    end
    featureText.props.text = table.concat(featureLines, '\n')
    featureText.props.visible = #featureLines > 0

    emitModuleState('JHM.hit-marker', hitMarkerVisible and 'visible' or 'hidden',
        ('seconds=%.3f'):format(hitMarkerSeconds))
    emitModuleState('JHI.hit-indicator', hitIndicator.props.visible and 'visible' or 'hidden',
        ('seconds=%.3f_angle=%.2f_x=%.1f_y=%.1f')
            :format(hitIndicatorSeconds, hitIndicatorAngle, hitIndicatorX, hitIndicatorY))
    emitModuleState('JLM.loot-menu', lootVisible and 'visible' or 'hidden',
        ('rows=%d'):format(#itemLines))
    emitModuleState('JVO.visual-objectives',
        objectiveMarker.props.visible and 'visible' or 'hidden',
        ('x=%.1f_y=%.1f'):format(objectiveX, objectiveY))
    emitModuleState('JWH.weapon-wheel', wheelVisible and 'visible' or 'hidden',
        ('slice=%d'):format(selectedSlice))
    emitModuleState('JBT.bullet-time', gameplayState.jbt ~= 0 and 'active' or 'inactive')
    emitModuleState('JHB.hold-breath', gameplayState.jhb ~= 0 and 'active' or 'inactive')
    emitModuleState('JVS.sprint', gameplayState.jvs ~= 0 and 'active' or 'inactive')
end

local function refresh(sourceScript, command)
    local screen = ui.screenSize()
    local centerX, centerY = screen.x * 0.5, screen.y * 0.5
    local spread = clamp(number('hudmainmenu\\jdc\\_jdcspread', 0), 0, 1)
    local minimumLength = math.max(1, number('hudmainmenu\\jdc\\_jdclengthmin', 24))
    local maximumLength = math.max(minimumLength, number('hudmainmenu\\jdc\\_jdclengthmax', 72))
    local width = math.max(1, number('hudmainmenu\\jdc\\_jdcwidthbase', 8))
    local minimumOffset = math.max(0, number('hudmainmenu\\jdc\\_jdcoffsetmin', 0)) + width / 8
    local maximumOffset = math.max(minimumOffset, number('hudmainmenu\\jdc\\_jdcoffsetmax', 256)) + width / 8
    local dynamicLength = number('hudmainmenu\\jdc\\_jdcdynamiclength', 0) ~= 0
    local dynamicOffset = number('hudmainmenu\\jdc\\_jdcdynamicoffset', 0) ~= 0
    local length = dynamicLength and lerp(minimumLength, maximumLength, spread) or minimumLength
    local offset = dynamicOffset and lerp(minimumOffset, maximumOffset, spread) or minimumOffset
    local reticleMode = math.floor(number('hudmainmenu\\jdc\\_jdcvisiblereticle', 0))
    local dotMode = math.floor(number('hudmainmenu\\jdc\\_jdcvisibledot', 0))
    local bridgeVisible = number('hudmainmenu\\jdc\\_jdcvisible', 1) > 0
    local armsVisible = bridgeVisible and reticleMode == 1
    local centerVisible = bridgeVisible and (reticleMode == 2 or reticleMode == 3)
    local dotVisible = bridgeVisible and dotMode > 0
    local alphaRaw = number('hudmainmenu\\jdc\\_jdcalpharc', 255)
    local alpha = clamp(alphaRaw > 1 and alphaRaw / 255 or alphaRaw, 0, 1)
    local color = selectedColor()

    local function updateArm(layers, startX, startY, vertical)
        local layerData = {
            { image = layers.outer, thickness = width, inset = 0, opacity = 0.16 },
            {
                image = layers.middle,
                thickness = math.max(1, math.floor(width * 0.5 + 0.5)),
                inset = 0.5,
                opacity = 0.52,
            },
            {
                image = layers.core,
                thickness = math.max(1, math.floor(width * 0.25 + 0.5)),
                inset = 1,
                opacity = 1,
            },
        }
        for _, layer in ipairs(layerData) do
            local layerLength = math.max(1, length - layer.inset * 2)
            if vertical then
                layer.image.props.position = util.vector2(
                    centerX - layer.thickness / 2, startY + layer.inset)
                layer.image.props.size = util.vector2(layer.thickness, layerLength)
            else
                layer.image.props.position = util.vector2(
                    startX + layer.inset, centerY - layer.thickness / 2)
                layer.image.props.size = util.vector2(layerLength, layer.thickness)
            end
            layer.image.props.visible = armsVisible
            layer.image.props.alpha = alpha * layer.opacity
            layer.image.props.color = color
        end
    end

    updateArm(arms.up, centerX, centerY - offset - length, true)
    updateArm(arms.down, centerX, centerY + offset, true)
    updateArm(arms.left, centerX - offset - length, centerY, false)
    updateArm(arms.right, centerX + offset, centerY, false)

    local centerSize = reticleMode == 3 and 64 or 48
    center.props.resource = reticleMode == 3 and vanillaTexture or circleTexture
    center.props.position = util.vector2(centerX - centerSize / 2, centerY - centerSize / 2)
    center.props.size = util.vector2(centerSize, centerSize)
    center.props.visible = centerVisible
    center.props.alpha = alpha
    center.props.color = color

    local dotSize = dotMode == 2 and 16 or 8
    dot.props.resource = dotMode == 2 and dotBigTexture or dotTexture
    dot.props.position = util.vector2(centerX - dotSize / 2, centerY - dotSize / 2)
    dot.props.size = util.vector2(dotSize, dotSize)
    dot.props.visible = dotVisible
    dot.props.alpha = alpha
    dot.props.color = color

    promptText.props.position = util.vector2(centerX - 260, centerY + 58)
    promptText.props.text = nativeReticleContext.promptText
    promptText.props.visible = nativeReticleContext.promptVisible

    local nativeVisible = number('hudmainmenu\\reticlecenter\\reticle_center\\visible', 1) ~= 0
    local pixelExtent = offset + length
    camera.showCrosshair(nativeVisible)
    proofText.props.position = util.vector2(math.max(12, screen.x - 490), 22)
    proofText.props.text = ('LIVE COMPAT EXECUTION (not a post-process label)\n'
            .. 'JAM ESP/UDF  %s\n'
            .. 'JIP LN      SetGameMainLoopCallback -> %s (active=%d, delay=%d)\n'
            .. 'xNVSE core  %s -> Gamebryo traits\n'
            .. 'OpenMW UI   JDCDefault.dds  spread %.4f  extent %.1f px')
        :format(tostring(sourceScript or '__no_active_script'),
            callbackState.name, callbackState.active and 1 or 0, callbackState.delay,
            tostring(command or 'SetUIFloatAlt'), spread, pixelExtent)
    proofText.props.visible = not externalProofOverlay
    refreshModules(screen)
    element:update()
    updateCount = updateCount + 1

    local state = ('reticle=%d dot=%d native=%d'):format(
        reticleMode, dotMode, nativeVisible and 1 or 0)
    local extentBucket = math.floor(pixelExtent / 4)
    if state ~= lastProofState or extentBucket ~= lastExtentBucket then
        lastProofState = state
        lastExtentBucket = extentBucket
        print(('[obscript-compat] state=native-effect scenarioId=JDC.dynamic-crosshair '
                .. 'sourceScript=%s provider=xnvse-core command=%s enginePath=openmw.ui '
                .. 'asset=textures/Interface/JDC/JDCDefault.dds spread=%.4f '
                .. 'pixelExtent=%.2f reticleMode=%d dotMode=%d nativeVisible=%d')
            :format(tostring(sourceScript or '__no_active_script'),
                tostring(command or 'SetUIFloatAlt'), spread, pixelExtent,
                reticleMode, dotMode, nativeVisible and 1 or 0))
    end
end

function bridge.setCallbackState(name, enabled, delay)
    callbackState.name = tostring(name or 'none')
    callbackState.active = enabled and true or false
    callbackState.delay = math.max(0, math.floor(tonumber(delay) or 0))
end

function bridge.setFloat(path, value, sourceScript, command)
    local key = normalize(path)
    bridge._lastSourceScript = sourceScript
    traits[key] = tonumber(value) or 0
    if key:find('_jhmmode', 1, true) then
        hitMarkerMode = tonumber(value) or 0
    elseif key:find('hudmainmenu\\jhm\\', 1, true)
            and key:sub(-#'systemcolor') == 'systemcolor' then
        hitMarkerSystemColor = tonumber(value) or 1
    end
    if key:find('_jhirotateangle', 1, true) then
        hitIndicatorAngle = tonumber(value) or 0
    elseif key:find('_jhimode', 1, true) then
        hitIndicatorMode = tonumber(value) or 0
    elseif key:find('hudmainmenu\\jhi\\', 1, true)
            and key:sub(-#'systemcolor') == 'systemcolor' then
        hitIndicatorSystemColor = tonumber(value) or 1
    end
    if key:find('hudmainmenu\\jhm\\', 1, true) then
        bridge._lastAsset = 'textures/Interface/JHM/JHMHitMarker.dds'
    elseif key:find('hudmainmenu\\jhi\\', 1, true) then
        bridge._lastAsset = 'textures/Interface/JHI/JHIHitIndicator.dds'
    elseif key:find('hudmainmenu\\jvo\\', 1, true) then
        bridge._lastAsset = 'textures/Interface/JVO/JVOClassic.dds'
    elseif key:find('hudmainmenu\\jwh\\', 1, true) then
        bridge._lastAsset = 'textures/Interface/JWH/JWHBackGround.dds'
    elseif key:find('hudmainmenu\\jdc\\', 1, true) then
        bridge._lastAsset = 'textures/Interface/JDC/JDCDefault.dds'
    end
    if key:find('hudmainmenu\\', 1, true) then
        refresh(sourceScript, command)
    end
    return 1
end

function bridge.getFloat(path)
    local key = normalize(path)
    local value = traits[key]
    if value == nil and key:find('\\*\\', 1, true) then
        value = traits[key:gsub('\\%*\\', '\\')]
    end
    return value or 0
end

local function renderString(formatValue, ...)
    local formatText = tostring(formatValue or '')
    local args = { ... }
    formatText = formatText:gsub('%%e', '')
    formatText = formatText:gsub('%%r', '\n')
    if formatText:find('%%k%)') then
        formatText = formatText:gsub('%%k%)',
            function() return keyName(args[1]) .. ')' end, 1)
        table.remove(args, 1)
    end
    if formatText:find('%%g%)') then
        formatText = formatText:gsub('%%g%)',
            function() return tostring(args[1] or '') .. ')' end, 1)
        table.remove(args, 1)
    end
    if formatText:find('%%n') then
        formatText = formatText:gsub('%%n',
            function() return displayName(args[1]) end, 1)
        table.remove(args, 1)
    end
    if formatText:find('%%c') then
        formatText = formatText:gsub('%%c',
            function() return displayName(args[1]) end, 1)
        table.remove(args, 1)
        if #args > 0 and tonumber(args[1]) ~= nil then
            table.remove(args, 1)
        end
    end
    local ok, result = pcall(string.format, formatText, unpack(args))
    return ok and result or formatText
end

function bridge.setString(path, value, sourceScript, command, ...)
    local key = normalize(path)
    bridge._lastSourceScript = sourceScript
    strings[key] = renderString(value, ...)
    refresh(sourceScript, command or 'SetUIStringAlt')
    return 1
end

function bridge.getString(path)
    return strings[normalize(path)] or ''
end

function bridge.setFloatGradual(path, startValue, endValue, seconds, mode, sourceScript)
    local key = normalize(path)
    traits[key] = tonumber(endValue) or 0
    bridge._lastSourceScript = sourceScript
    if key:find('hudmainmenu\\jhm\\', 1, true) then
        hitMarkerSeconds = math.max(hitMarkerSeconds, tonumber(seconds) or 0.5)
        bridge._lastAsset = 'textures/Interface/JHM/JHMHitMarker.dds'
    elseif key:find('hudmainmenu\\jhi\\', 1, true) then
        hitIndicatorSeconds = math.max(hitIndicatorSeconds, tonumber(seconds) or 0.5)
        bridge._lastAsset = 'textures/Interface/JHI/JHIHitIndicator.dds'
    end
    refresh(sourceScript, 'SetUIFloatGradual')
    return 1
end

function bridge.addTile(path, sourceScript)
    local key = normalize(path)
    components[key] = true
    bridge._lastSourceScript = sourceScript
    refresh(sourceScript, 'AddTileFromTemplate')
    return 1
end

function bridge.unload(path, sourceScript)
    local key = normalize(path)
    for component in pairs(components) do
        if component:sub(1, #key) == key then components[component] = nil end
    end
    for trait in pairs(traits) do
        if trait:sub(1, #key) == key then traits[trait] = nil end
    end
    for stringPath in pairs(strings) do
        if stringPath:sub(1, #key) == key then strings[stringPath] = nil end
    end
    if key:find('hudmainmenu\\jhm\\', 1, true) then hitMarkerSeconds = 0 end
    if key:find('hudmainmenu\\jhi\\', 1, true) then
        hitIndicatorSeconds = 0
        -- Keep the last rendered direction for diagnostics after the
        -- short-lived authored tile unloads. The component and its traits
        -- are still removed above, so this does not keep the UI visible or
        -- change GetUIFloatAlt behavior.
    end
    refresh(sourceScript, 'UnloadUIComponent')
    return 1
end

function bridge.isComponentLoaded(path)
    local key = normalize(path)
    if components[key] then return 1 end
    for component in pairs(components) do
        if component:sub(1, #key) == key then return 1 end
    end
    return 0
end

function bridge.setGameplayState(state)
    for key, value in pairs(state or {}) do
        if gameplayState[key] ~= nil then
            gameplayState[key] = tonumber(value) or 0
        end
    end
end

function bridge.setNativeReticleContext(context)
    context = context or {}
    nativeReticleContext.promptVisible = context.promptVisible and true or false
    nativeReticleContext.promptText = tostring(context.promptText or '')
    traits['hudmainmenu\\reticlecenter\\reticle_center\\systemcolor']
        = math.floor(tonumber(context.systemColor) or 1)
    refresh('__native_crosshair__', 'native-reticle-context')
end

function bridge.setExternalProofOverlay(active)
    externalProofOverlay = active and true or false
    proofText.props.visible = not externalProofOverlay
    element:update()
end

function bridge.onFrame(dt)
    local elapsed = math.max(0, tonumber(dt) or 0)
    hitMarkerSeconds = math.max(0, hitMarkerSeconds - elapsed)
    hitIndicatorSeconds = math.max(0, hitIndicatorSeconds - elapsed)
    refresh('__runtime__', 'frame')
end

function bridge.getStatus()
    local spread = clamp(number('hudmainmenu\\jdc\\_jdcspread', 0), 0, 1)
    local minimumLength = math.max(1, number('hudmainmenu\\jdc\\_jdclengthmin', 24))
    local maximumLength = math.max(minimumLength,
        number('hudmainmenu\\jdc\\_jdclengthmax', 72))
    local width = math.max(1, number('hudmainmenu\\jdc\\_jdcwidthbase', 8))
    local minimumOffset = math.max(0,
        number('hudmainmenu\\jdc\\_jdcoffsetmin', 0)) + width / 8
    local maximumOffset = math.max(minimumOffset,
        number('hudmainmenu\\jdc\\_jdcoffsetmax', 256)) + width / 8
    local dynamicLength = number('hudmainmenu\\jdc\\_jdcdynamiclength', 0) ~= 0
    local dynamicOffset = number('hudmainmenu\\jdc\\_jdcdynamicoffset', 0) ~= 0
    local length = dynamicLength and lerp(minimumLength, maximumLength, spread)
        or minimumLength
    local offset = dynamicOffset and lerp(minimumOffset, maximumOffset, spread)
        or minimumOffset
    return {
        spread = spread,
        pixelExtent = offset + length,
        reticleMode = math.floor(number('hudmainmenu\\jdc\\_jdcvisiblereticle', 0)),
        dotMode = math.floor(number('hudmainmenu\\jdc\\_jdcvisibledot', 0)),
        nativeVisible =
            number('hudmainmenu\\reticlecenter\\reticle_center\\visible', 1) ~= 0,
        systemColor = math.floor(number('hudmainmenu\\jdc\\systemcolor', 1)),
        red = number('hudmainmenu\\jdc\\red', 255),
        green = number('hudmainmenu\\jdc\\green', 255),
        blue = number('hudmainmenu\\jdc\\blue', 255),
        interactablePromptVisible = nativeReticleContext.promptVisible,
        promptText = nativeReticleContext.promptText,
        hitMarkerSeconds = hitMarkerSeconds,
        hitMarkerMode = hitMarkerMode,
        hitMarkerSystemColor = hitMarkerSystemColor,
        hitIndicatorSeconds = hitIndicatorSeconds,
        hitIndicatorAngle = hitIndicatorAngle,
        hitIndicatorMode = hitIndicatorMode,
        hitIndicatorSystemColor = hitIndicatorSystemColor,
        hitIndicatorX = hitIndicatorX,
        hitIndicatorY = hitIndicatorY,
        objectiveVisible =
            number('hudmainmenu\\jvo\\_jvovisible', 0) ~= 0,
        objectiveScreenX = objectiveScreenX,
        objectiveScreenY = objectiveScreenY,
        weaponWheelVisible = gameplayState.jwh ~= 0
            or number('hudmainmenu\\jwh\\_jwhvisible', 0) ~= 0,
        selectedWeaponWheelSlice = selectedWeaponWheelSlice,
        lootMenuVisible =
            number('hudmainmenu\\jlm\\_jlmvisible', 0) ~= 0,
        lootMenuRowCount = lootMenuRowCount,
        lootMenuTitle = lootMenuTitle,
        bulletTimeActive = gameplayState.jbt ~= 0,
        holdBreathActive = gameplayState.jhb ~= 0,
        sprintActive = gameplayState.jvs ~= 0,
        updates = updateCount,
        asset = 'textures/Interface/JDC/JDCDefault.dds',
    }
end

return bridge
