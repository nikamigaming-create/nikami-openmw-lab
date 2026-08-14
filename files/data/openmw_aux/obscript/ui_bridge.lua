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
    -- This one-pixel crop is sampled from JAM's untouched JDCDefault.dds.
    -- Stretching the authored white pixel lets OpenMW render both horizontal
    -- and vertical arms without substituting a generated texture.
    path = 'textures/Interface/JDC/JDCDefault.dds',
    offset = util.vector2(3, 16),
    size = util.vector2(1, 1),
}
local dotTexture = ui.texture { path = 'textures/Interface/JDC/JDCDot.dds' }
local dotBigTexture = ui.texture { path = 'textures/Interface/JDC/JDCDotBig.dds' }
local circleTexture = ui.texture { path = 'textures/Interface/JDC/JDCCircle.dds' }
local vanillaTexture = ui.texture { path = 'textures/Interface/JDC/JDCVanilla.dds' }
local hitMarkerTexture = ui.texture { path = 'textures/Interface/JHM/JHMHitMarker.dds' }
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
        },
    }
end

local arms = {
    up = image(armTexture),
    down = image(armTexture),
    left = image(armTexture),
    right = image(armTexture),
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
local hitMarker = image(hitMarkerTexture)
local hitIndicator = image(hitIndicatorTexture)
local objectiveMarker = image(objectiveTexture)
local wheelBackground = image(wheelBackgroundTexture)
local wheelSlice = image(wheelSliceTextures[11])
local wheelForeground = image(wheelForegroundTexture)
local lootText = {
    type = ui.TYPE.Text,
    props = {
        position = util.vector2(0, 0),
        size = util.vector2(390, 360),
        text = '',
        textSize = 20,
        textColor = util.color.rgb(0.35, 1, 0.45),
        textShadow = true,
        textShadowColor = util.color.rgb(0, 0, 0),
        multiline = true,
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

local element = ui.create {
    layer = 'HUD',
    type = ui.TYPE.Container,
    props = {
        relativeSize = util.vector2(1, 1),
        visible = true,
    },
    content = ui.content {
        arms.up,
        arms.down,
        arms.left,
        arms.right,
        center,
        dot,
        promptText,
        hitMarker,
        hitIndicator,
        objectiveMarker,
        wheelBackground,
        wheelSlice,
        wheelForeground,
        lootText,
        featureText,
        proofText,
    },
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

local function refreshModules(screen)
    local centerX, centerY = screen.x * 0.5, screen.y * 0.5

    hitMarker.props.position = util.vector2(centerX - 48, centerY - 48)
    hitMarker.props.size = util.vector2(96, 96)
    hitMarker.props.visible = hitMarkerSeconds > 0

    -- Gamebryo rotates JHI's authored tile around the HUD centre. OpenMW's
    -- image widget has no equivalent tile-rotation trait, so preserve the
    -- gameplay meaning by placing the untouched indicator texture on the
    -- corresponding point of the same compass. Zero degrees is in front,
    -- positive ninety is right, and one-eighty is behind.
    local indicatorRadians = math.rad(hitIndicatorAngle)
    local indicatorRadius = math.min(210, screen.y * 0.29)
    hitIndicatorX = centerX + math.sin(indicatorRadians) * indicatorRadius
    hitIndicatorY = centerY - math.cos(indicatorRadians) * indicatorRadius
    hitIndicator.props.position = util.vector2(hitIndicatorX - 72, hitIndicatorY - 36)
    hitIndicator.props.size = util.vector2(144, 72)
    hitIndicator.props.visible = hitIndicatorSeconds > 0

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

    -- iLootMenu means the authored subsystem is enabled; it is not the
    -- per-target visibility flag.  JLM owns _JLMVisible and only raises it
    -- while a valid crosshair container/corpse is being rendered.
    local lootVisible = number('hudmainmenu\\jlm\\_jlmvisible', 0) ~= 0
    local title = strings['hudmainmenu\\jlm\\_jlmtitle'] or 'Container'
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
    lootText.props.position = util.vector2(math.max(20, centerX - 500), centerY - 170)
    lootText.props.text = ('JUST LOOT MENU\n%s\n%s'):format(title, table.concat(itemLines, '\n'))
    lootText.props.visible = lootVisible

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

    emitModuleState('JHM.hit-marker', hitMarker.props.visible and 'visible' or 'hidden',
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

    arms.up.props.position = util.vector2(centerX - width / 2, centerY - offset - length)
    arms.up.props.size = util.vector2(width, length)
    arms.down.props.position = util.vector2(centerX - width / 2, centerY + offset)
    arms.down.props.size = util.vector2(width, length)
    arms.left.props.position = util.vector2(centerX - offset - length, centerY - width / 2)
    arms.left.props.size = util.vector2(length, width)
    arms.right.props.position = util.vector2(centerX + offset, centerY - width / 2)
    arms.right.props.size = util.vector2(length, width)
    for _, arm in pairs(arms) do
        arm.props.visible = armsVisible
        arm.props.alpha = alpha
        arm.props.color = color
    end

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
    if formatText:find('%%k%)') then
        formatText = formatText:gsub('%%k%)', tostring(args[1] or '') .. ')')
        table.remove(args, 1)
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
        hitIndicatorAngle = 0
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
