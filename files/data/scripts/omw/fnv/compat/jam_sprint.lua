-- Engine-native compatibility provider for the sprint subsystem in the
-- untouched Just Assorted Mods plugin. This consumes JAM's winning globals
-- and OpenMW APIs; it does not load or emulate an xNVSE DLL.

local core = require('openmw.core')

if not (core.contentFiles and core.contentFiles.has
        and core.contentFiles.has('JustAssortedMods.esp')) then
    return {}
end

local input = require('openmw.input')
local I = require('openmw.interfaces')
local self = require('openmw.self')
local ui = require('openmw.ui')
local util = require('openmw.util')

local directInput = require('scripts.omw.fnv.compat.directinput')

local ACTION_POINTS = 12
local ENDURANCE = 7
local proofDrive = os ~= nil and os.getenv ~= nil
    and os.getenv('OPENMW_FNV_JAM_PROOF_DRIVE') == '1'
local fullProofDrive = os ~= nil and os.getenv ~= nil
    and os.getenv('OPENMW_FNV_JAM_FULL_PROOF') == '1'
local proofAutoMove = proofDrive and os ~= nil and os.getenv ~= nil
    and os.getenv('OPENMW_FNV_JAM_PROOF_AUTOMOVE') == '1'
local proofYawDegrees = os ~= nil and os.getenv ~= nil
    and tonumber(os.getenv('OPENMW_FNV_JAM_PROOF_YAW_DEGREES') or '') or nil
if fullProofDrive then proofYawDegrees = nil end
local proofBaselineSeconds = 2
local proofSprintSeconds = 5

local function clamp(value, minimum, maximum)
    return math.max(minimum, math.min(maximum, value))
end

local function global(name, fallback)
    local value = core.obscript.getGlobalVariable(name)
    if value == nil then
        return fallback
    end
    return tonumber(value) or fallback
end

local function readConfig()
    local minimumDrain = math.max(0, global('JVSAPDrainMin', 14))
    local maximumDrain = math.max(minimumDrain, global('JVSAPDrainMax', 20))
    local directInputCode = math.floor(global('JVSKey', 42))
    return {
        enabled = global('JVSEnabled', 0) ~= 0,
        toggle = global('JVSToggle', 0) ~= 0,
        directInputCode = directInputCode,
        key = directInput[directInputCode],
        speedMultiplier = clamp(1 + 0.01 * global('JVSSpeedMult', 75), 1, 5),
        minimumDrain = minimumDrain,
        maximumDrain = maximumDrain,
        enduranceBuff = global('JVSEnduranceBuff', 0.5),
    }
end

local function actorValue(index, fallback)
    local value = core.obscript.getPlayerActorValue(index)
    if value == nil then
        return fallback
    end
    return tonumber(value) or fallback
end

local function apDrainPerSecond(config, current, maximum)
    local ratio = maximum > 0 and clamp(current / maximum, 0, 1) or 0
    local drain
    if ratio >= 0.75 then
        drain = config.maximumDrain
    elseif ratio >= 0.5 then
        drain = (2 * config.maximumDrain + config.minimumDrain) / 3
    elseif ratio >= 0.25 then
        drain = (config.maximumDrain + 2 * config.minimumDrain) / 3
    else
        drain = config.minimumDrain
    end
    local endurance = actorValue(ENDURANCE, 5)
    local enduranceFactor = math.max(0, 1 - 0.1 * config.enduranceBuff * (endurance - 1))
    return drain * enduranceFactor
end

local overlay
local active = false
local toggleLatched = false
local keyWasDown = false
local elapsed = 0
local distance = 0
local apAtStart = 0
local baselineElapsed = 0
local baselineDistance = 0
local readySeconds = 5
local lastPosition = self.position
local measuredSpeed = 0
local lastText = ''
local lastVisible = true
local announced = false
local unsupportedKeyAnnounced = false
local frameAnnounced = false
local hudFailed = false
local proofSprintRequested = false
local proofSprintElapsed = 0
local proofSprintComplete = false
local proofYawApplied = false
local compatKeyState = {}
local compatBridgeAnnounced = false
local proofCrosshairConfigured = false
local proofAutoMoveElapsed = 0
local lastJbtActive = false
local lastJhbActive = false

local function compatBridge()
    local ok, bridge = pcall(function() return I.FNVObScriptCompat end)
    if ok then return bridge end
    return nil
end

local function dispatchCompatKey(bridge, dik, pressed)
    if bridge == nil or bridge.dispatchKey == nil then
        return
    end
    local previous = compatKeyState[dik]
    if previous == pressed then
        return
    end
    compatKeyState[dik] = pressed
    bridge.dispatchKey(dik, pressed)
end

local function configureProofCrosshair(bridge)
    if not proofDrive or proofCrosshairConfigured or bridge == nil
        or bridge.setGlobalVariable == nil or bridge.setScriptVariable == nil then
        return
    end
    proofCrosshairConfigured = true
    for name, value in pairs {
        JDCEnabled = 1,
        JDCDynamic = 3,
        JDCModeOut1st = 3,
        JDCModeOut3rd = 3,
        JDCModeSighting1st = 4,
        JDCModeSighting3rd = 4,
        JDCModeScope = 0,
        JDCModeHolstered = 1,
    } do
        bridge.setGlobalVariable(name, value)
    end
    bridge.setScriptVariable('JDCScript', 'iSettingUpdate', 1)
    if core.obscript.setPlayerWeaponOut ~= nil then
        core.obscript.setPlayerWeaponOut(true)
    end
    print(
        '[obscript-compat] state=proof-config scenarioId=JDC.dynamic-crosshair '
            .. 'source=matched-JAM-configuration JDCModeOut1st=3 JDCDynamic=3')
end

local function jamState(bridge, script, variable)
    if bridge == nil or bridge.getScriptVariable == nil then return false end
    return tonumber(bridge.getScriptVariable(script, variable)) ~= 0
end

local function tieredDrain(minimum, maximum, current, maximumValue)
    local ratio = maximumValue > 0 and clamp(current / maximumValue, 0, 1) or 0
    if ratio >= 0.75 then return maximum end
    if ratio >= 0.5 then return (2 * maximum + minimum) / 3 end
    if ratio >= 0.25 then return (maximum + 2 * minimum) / 3 end
    return minimum
end

local function applyCompanionResourceEffects(
        bridge, dt, currentAp, maximumAp)
    local jbtActive = jamState(bridge, 'JBT', 'iBulletTime')
    local jhbActive = jamState(bridge, 'JHB', 'iHoldBreath')
    local drain = 0
    if jbtActive then
        drain = drain + math.max(0, global('JBTAPDrain', 10))
    end
    if jhbActive then
        local minimum = math.max(0, global('JHBAPDrainMin', 21))
        local maximum = math.max(minimum, global('JHBAPDrainMax', 30))
        local endurance = actorValue(ENDURANCE, 5)
        local enduranceFactor = math.max(
            0, 1 - 0.1 * global('JHBEnduranceBuff', 0.5)
                * (endurance - 1))
        drain = drain + tieredDrain(
            minimum, maximum, currentAp, maximumAp) * enduranceFactor
    end
    if drain > 0 then
        core.obscript.modPlayerActorValue(ACTION_POINTS, -drain * dt)
        currentAp = actorValue(ACTION_POINTS, currentAp)
    end
    if jbtActive ~= lastJbtActive then
        lastJbtActive = jbtActive
        print(('[obscript-compat] state=native-effect '
                .. 'scenarioId=JBT.bullet-time sourceScript=JBTMainLoopEventHandler '
                .. 'provider=xnvse-core command=DamageAV '
                .. 'enginePath=openmw.fnv.playerActorValues visualState=%s ap=%.3f')
            :format(jbtActive and 'active' or 'restored', currentAp))
    end
    if jhbActive ~= lastJhbActive then
        lastJhbActive = jhbActive
        print(('[obscript-compat] state=native-effect '
                .. 'scenarioId=JHB.hold-breath sourceScript=JHBMainLoopEventHandler '
                .. 'provider=xnvse-core command=GetPerkModifier '
                .. 'enginePath=openmw.fnv.perkEntryPointSpread visualState=%s ap=%.3f')
            :format(jhbActive and 'active' or 'restored', currentAp))
    end
    return currentAp
end

local function logReady(config)
    if announced then
        return
    end
    announced = true
    print(string.format(
        'FNV mod compat: provider=JAM subsystem=sprint state=ready plugin=JustAssortedMods.esp '
            .. 'keyDIK=%d keyMapped=%d speedMultiplier=%.3f apDrainMin=%.3f apDrainMax=%.3f '
            .. 'proofDrive=%d',
        config.directInputCode, config.key and 1 or 0, config.speedMultiplier,
        config.minimumDrain, config.maximumDrain, proofDrive and 1 or 0))
    print(
        'FNV mod compat: provider=JAM subsystem=sprint state=attestation '
            .. 'engine=OpenMW xnvseCore=global-dispatch '
            .. 'jipBridge=DIK42-event-AP jamUdf=JVSOnKeyDownEventHandler '
            .. 'johnnyGuitar=not-called-by-sprint knvse=animation-out-of-scope')
end

local function setActive(nextActive, config, currentAp)
    if nextActive == active then
        return
    end
    active = nextActive
    if active then
        elapsed = 0
        distance = 0
        apAtStart = currentAp
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=start ap=%.3f speedMultiplier=%.3f',
            currentAp, config.speedMultiplier))
    else
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=stop elapsed=%.3f distance=%.3f '
                .. 'averageSpeed=%.3f apBefore=%.3f apAfter=%.3f',
            elapsed, distance, elapsed > 0 and distance / elapsed or 0, apAtStart, currentAp))
    end
end

local function finishBaseline()
    if baselineElapsed >= 1 then
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=baseline elapsed=%.3f distance=%.3f '
                .. 'averageSpeed=%.3f',
            baselineElapsed, baselineDistance, baselineDistance / baselineElapsed))
    end
    baselineElapsed = 0
    baselineDistance = 0
end

local function updateOverlay(config, moving, currentAp, maximumAp)
    if fullProofDrive then
        if overlay ~= nil and lastVisible ~= false then
            overlay.layout.props.visible = false
            overlay:update()
            lastVisible = false
        end
        return
    end
    if overlay == nil then
        overlay = ui.create {
            layer = 'HUD',
            type = ui.TYPE.Text,
            props = {
                position = util.vector2(24, 24),
                text = 'JAM → OpenMW native sprint: loading',
                textSize = 18,
                textColor = util.color.rgb(0.95, 0.85, 0.2),
                textShadow = true,
                textShadowColor = util.color.rgb(0, 0, 0),
                multiline = true,
                visible = true,
            },
        }
    end
    local visible = active or moving or readySeconds > 0
    local state = active and 'SPRINT ACTIVE' or (moving and 'BASELINE RUN' or 'SPRINT READY')
    local text = string.format(
        'JAM 4.6 → OPENMW NATIVE COMPAT\n%s  •  %.2fx  •  SPEED %.1f u/s  •  AP %.1f/%.1f',
        state, active and config.speedMultiplier or 1, measuredSpeed, currentAp, maximumAp)
    text = string.format(
        'HELLO FROM OPENMW NATIVE ENGINE\n'
            .. 'HELLO xNVSE CORE COMPAT: GLOBALS + COMMAND DISPATCH\n'
            .. 'HELLO JIP LN COMPAT: DIK 42 + EVENT + ACTION POINTS\n'
            .. 'JohnnyGuitar / kNVSE: NOT CALLED BY THIS SPRINT SLICE\n'
            .. 'HELLO JAM 4.6 EXACT ESP: %s\n'
            .. 'LIVE  %.2fx  |  SPEED %.1f u/s  |  AP %.1f/%.1f',
        state, active and config.speedMultiplier or 1, measuredSpeed, currentAp, maximumAp)
    if text ~= lastText or visible ~= lastVisible then
        overlay.layout.props.text = text
        overlay.layout.props.visible = visible
        overlay.layout.props.textColor = active
            and util.color.rgb(0.25, 1, 0.35)
            or util.color.rgb(0.95, 0.85, 0.2)
        overlay:update()
        lastText = text
        lastVisible = visible
    end
end

local function onFrame(dt)
    dt = clamp(tonumber(dt) or 0, 0, 0.25)
    readySeconds = math.max(0, readySeconds - dt)
    if proofAutoMove then
        proofAutoMoveElapsed = proofAutoMoveElapsed + dt
        if proofAutoMoveElapsed >= 1 and proofAutoMoveElapsed <= 11 then
            self.controls.movement = 1
            self.controls.run = true
        end
    end

    local config = readConfig()
    logReady(config)
    if not proofYawApplied and proofYawDegrees ~= nil then
        self.controls.yawChange = math.rad(proofYawDegrees)
        proofYawApplied = true
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=proof-yaw '
                .. 'source=save-relative degrees=%.3f',
            proofYawDegrees))
    end
    if config.key == nil and not unsupportedKeyAnnounced then
        unsupportedKeyAnnounced = true
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=unsupported-key keyDIK=%d',
            config.directInputCode))
    end

    local position = self.position
    if dt > 0 then
        measuredSpeed = (position - lastPosition):length() / dt
        if measuredSpeed > 5000 then
            measuredSpeed = 0
        end
    end
    lastPosition = position

    local movement = self.controls.movement
    local moving = movement > 0.1
    local currentAp = actorValue(ACTION_POINTS, 0)
    local maximumAp = core.obscript.getPlayerMaxActionPoints() or math.max(currentAp, 1)
    local keyDown = config.key ~= nil and input.isKeyPressed(config.key)
    if proofDrive and moving and not proofSprintRequested and not proofSprintComplete
        and baselineElapsed >= proofBaselineSeconds then
        proofSprintRequested = true
        proofSprintElapsed = 0
        print(
            'FNV mod compat: provider=JAM subsystem=sprint state=proof-key-down '
                .. 'source=engine-proof-driver keyDIK=42')
    end
    if proofDrive and proofSprintRequested and not proofSprintComplete then
        proofSprintElapsed = proofSprintElapsed + dt
        keyDown = proofSprintElapsed <= proofSprintSeconds
        if not keyDown then
            proofSprintRequested = false
            proofSprintComplete = true
            print(
                'FNV mod compat: provider=JAM subsystem=sprint state=proof-key-up '
                    .. 'source=engine-proof-driver keyDIK=42')
        end
    end
    local bridge = compatBridge()
    configureProofCrosshair(bridge)
    if bridge ~= nil and bridge.getRegisteredKeys ~= nil then
        if not compatBridgeAnnounced then
            compatBridgeAnnounced = true
            print(
                'FNV mod compat: provider=JAM subsystem=sprint state=script-bridge '
                    .. 'source=JVSOnKeyDownEventHandler effect=openmw.controls')
        end
        for _, dik in ipairs(bridge.getRegisteredKeys()) do
            local mapped = directInput[dik]
            local pressed = mapped ~= nil and input.isKeyPressed(mapped) or false
            if dik == config.directInputCode and proofDrive then
                pressed = keyDown
            end
            dispatchCompatKey(bridge, dik, pressed)
        end
    end
    currentAp = applyCompanionResourceEffects(
        bridge, dt, currentAp, maximumAp)
    local keyPressed = keyDown and not keyWasDown
    keyWasDown = keyDown

    local eligible = config.enabled and config.key ~= nil and moving
        and not self.controls.sneak and not core.isWorldPaused() and currentAp > 0
    if config.toggle then
        if keyPressed and eligible then
            toggleLatched = not toggleLatched
        end
        if not eligible then
            toggleLatched = false
        end
    else
        toggleLatched = false
    end
    local requested = config.toggle and toggleLatched or keyDown
    local scriptActive = bridge ~= nil and bridge.getScriptVariable ~= nil
        and tonumber(bridge.getScriptVariable('JVS', 'iSprint')) ~= 0
    local nextActive
    if bridge ~= nil and bridge.getScriptVariable ~= nil then
        nextActive = scriptActive
    else
        nextActive = eligible and requested
    end
    if nextActive and not active then
        finishBaseline()
    end
    setActive(nextActive, config, currentAp)

    self.controls.speedMultiplier = active and config.speedMultiplier or 1
    if active then
        self.controls.run = true
        elapsed = elapsed + dt
        distance = distance + measuredSpeed * dt
        local drain = apDrainPerSecond(config, currentAp, maximumAp)
        core.obscript.modPlayerActorValue(ACTION_POINTS, -drain * dt)
        currentAp = actorValue(ACTION_POINTS, currentAp)
    elseif moving then
        baselineElapsed = baselineElapsed + dt
        baselineDistance = baselineDistance + measuredSpeed * dt
    elseif baselineElapsed > 0 then
        finishBaseline()
    end

    if not frameAnnounced then
        frameAnnounced = true
        print(string.format(
            'FNV mod compat: provider=JAM subsystem=sprint state=live-frame movement=%.3f ap=%.3f maxAp=%.3f',
            movement, currentAp, maximumAp))
    end
    if not hudFailed then
        local hudOk, hudError = pcall(updateOverlay, config, moving, currentAp, maximumAp)
        if not hudOk then
            hudFailed = true
            print('FNV mod compat: provider=JAM subsystem=sprint state=hud-error error=' .. tostring(hudError))
            overlay = nil
        end
    end
end

local function onLoad()
    self.controls.speedMultiplier = 1
end

local function onSave()
    return {
        toggleLatched = toggleLatched,
    }
end

return {
    engineHandlers = {
        onFrame = onFrame,
        onLoad = onLoad,
        onSave = onSave,
    },
}
