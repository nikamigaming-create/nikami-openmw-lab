-- Shared compatibility runtime for legacy MWSE-Lua mods.
--
-- This is deliberately a contract adapter, not a per-mod port.  The generated
-- C++ host discovers every MWSE main.lua and loads them in this one sandbox so
-- globals, require caches, custom events, and inter-mod libraries behave like a
-- single MWSE process.

local core = require('openmw.core')
local ambient = require('openmw.ambient')
local animation = require('openmw.animation')
local camera = require('openmw.camera')
local ui = require('openmw.ui')
local util = require('openmw.util')
local input = require('openmw.input')
local nearby = require('openmw.nearby')
local self = require('openmw.self')
local types = require('openmw.types')

local runtime = {}
local state = nil

local baseMath = math
local baseOs = os
local baseString = string
local baseTable = table

local function countKeys(value)
    local result = 0
    for _ in pairs(value or {}) do
        result = result + 1
    end
    return result
end

local function formatMessage(format, ...)
    if select('#', ...) == 0 then
        return tostring(format)
    end
    local ok, message = pcall(baseString.format, tostring(format), ...)
    if ok then
        return message
    end
    local values = { tostring(format) }
    for i = 1, select('#', ...) do
        values[#values + 1] = tostring(select(i, ...))
    end
    return baseTable.concat(values, ' ')
end

local function noteUnsupported(name)
    if state == nil or state.unsupported[name] then
        return
    end
    state.unsupported[name] = true
    print('MWSE compat: state=unsupported api=' .. name)
end

local function noteError(stage, message)
    if state == nil then
        return
    end
    local row = stage .. ': ' .. tostring(message)
    state.errorSerial = state.errorSerial + 1
    local count = (state.errorCounts[row] or 0) + 1
    state.errorCounts[row] = count
    state.lastError = row
    if count == 1 then
        if #state.errors < state.errorUniqueLimit then
            state.errors[#state.errors + 1] = row
        else
            state.errorOverflow = state.errorOverflow + 1
        end
        if state.errorPrinted < state.errorPrintLimit then
            print('MWSE compat: state=error stage=' .. stage .. ' error=' .. tostring(message))
            state.errorPrinted = state.errorPrinted + 1
        elseif not state.errorPrintLimitReported then
            print('MWSE compat: state=error-limit limit=' .. state.errorPrintLimit
                .. ' further runtime diagnostics suppressed')
            state.errorPrintLimitReported = true
        end
    elseif count == 2 and state.errorPrinted < state.errorPrintLimit then
        print('MWSE compat: state=error-suppressed stage=' .. stage
            .. ' repeats=all further occurrences error=' .. tostring(message))
        state.errorPrinted = state.errorPrinted + 1
    end
end

local function numericValue(value, fallback, context)
    local valueType = type(value)
    if valueType == 'number' then
        return value
    end
    if valueType == 'string' then
        local parsed = tonumber(value)
        if parsed ~= nil then
            return parsed
        end
    elseif valueType == 'table' then
        for _, key in ipairs({ 'current', 'value', 'base' }) do
            local candidate = rawget(value, key)
            if type(candidate) == 'number' then
                return candidate
            elseif type(candidate) == 'string' then
                local parsed = tonumber(candidate)
                if parsed ~= nil then
                    return parsed
                end
            end
        end
    end
    noteUnsupported('numeric:' .. tostring(context))
    return fallback
end

local function copyTable(source, seen)
    if type(source) ~= 'table' then
        return source
    end
    seen = seen or {}
    if seen[source] ~= nil then
        return seen[source]
    end
    local result = {}
    seen[source] = result
    for key, value in pairs(source) do
        result[copyTable(key, seen)] = copyTable(value, seen)
    end
    return result
end

local function installStandardExtensions()
    local mutableMath = {}
    for key, value in pairs(baseMath) do
        mutableMath[key] = value
    end
    mutableMath.clamp = function(value, minimum, maximum)
        local minValue = numericValue(minimum, 0, 'math.clamp.minimum')
        local maxValue = numericValue(maximum, minValue, 'math.clamp.maximum')
        local inputValue = numericValue(value, minValue, 'math.clamp.value')
        return baseMath.max(minValue, baseMath.min(maxValue, inputValue))
    end
    mutableMath.round = function(value)
        value = numericValue(value, 0, 'math.round.value')
        if value >= 0 then
            return baseMath.floor(value + 0.5)
        end
        return baseMath.ceil(value - 0.5)
    end
    mutableMath.remap = function(value, inMinimum, inMaximum, outMinimum, outMaximum)
        value = numericValue(value, 0, 'math.remap.value')
        inMinimum = numericValue(inMinimum, 0, 'math.remap.inMinimum')
        inMaximum = numericValue(inMaximum, inMinimum, 'math.remap.inMaximum')
        outMinimum = numericValue(outMinimum, 0, 'math.remap.outMinimum')
        outMaximum = numericValue(outMaximum, outMinimum, 'math.remap.outMaximum')
        if inMaximum == inMinimum then
            return outMinimum
        end
        return outMinimum + (value - inMinimum) * (outMaximum - outMinimum) / (inMaximum - inMinimum)
    end
    mutableMath.isclose = function(left, right, tolerance)
        left = numericValue(left, 0, 'math.isclose.left')
        right = numericValue(right, 0, 'math.isclose.right')
        tolerance = numericValue(tolerance, 1e-5, 'math.isclose.tolerance')
        return math.abs(left - right) <= (tolerance or 1e-5)
    end

    local mutableString = {}
    for key, value in pairs(baseString) do
        mutableString[key] = value
    end
    mutableString.startswith = function(value, prefix)
        return value:sub(1, #prefix) == prefix
    end
    mutableString.endswith = function(value, suffix)
        return suffix == '' or value:sub(-#suffix) == suffix
    end
    mutableString.split = function(value, separator)
        separator = separator or '%s+'
        local result = {}
        if separator == '' then
            for i = 1, #value do
                result[i] = value:sub(i, i)
            end
            return result
        end
        local start = 1
        while true do
            local first, last = value:find(separator, start)
            if first == nil then
                result[#result + 1] = value:sub(start)
                return result
            end
            result[#result + 1] = value:sub(start, first - 1)
            start = last + 1
        end
    end

    local mutableTable = {}
    for key, value in pairs(baseTable) do
        mutableTable[key] = value
    end
    mutableTable.copy = function(source, destination)
        destination = destination or {}
        for key, value in pairs(source or {}) do
            destination[key] = value
        end
        return destination
    end
    mutableTable.deepcopy = copyTable
    mutableTable.copymissing = function(destination, source)
        for key, value in pairs(source or {}) do
            if destination[key] == nil then
                destination[key] = copyTable(value)
            end
        end
        return destination
    end
    mutableTable.size = countKeys
    mutableTable.keys = function(value)
        local result = {}
        for key in pairs(value or {}) do
            result[#result + 1] = key
        end
        return result
    end
    mutableTable.find = function(value, needle)
        for key, item in pairs(value or {}) do
            if item == needle then
                return key
            end
        end
        return nil
    end
    mutableTable.choice = function(value)
        if value == nil or #value == 0 then
            return nil
        end
        return value[baseMath.random(1, #value)]
    end
    mutableTable.get = function(value, key, defaultValue)
        local result = value and value[key]
        if result == nil then
            return defaultValue
        end
        return result
    end
    mutableTable.traverse = function(value)
        local keys = mutableTable.keys(value)
        local index = 0
        return function()
            index = index + 1
            local key = keys[index]
            if key ~= nil then
                return key, value[key]
            end
        end
    end

    local mutableOs = {}
    for key, value in pairs(baseOs or {}) do
        mutableOs[key] = value
    end
    -- OpenMW intentionally omits the process-level Lua OS library.  MWSE's
    -- os.clock contract is monotonic elapsed time, which maps directly to
    -- OpenMW's steady real-time clock without exposing host process access.
    mutableOs.clock = function()
        return core.getRealTime()
    end

    _G.math = mutableMath
    _G.os = mutableOs
    _G.string = mutableString
    _G.table = mutableTable
end

local function loggerWrite(level, name, format, ...)
    print(baseString.format('[%s] [%s] %s', level, name, formatMessage(format, ...)))
end

local Logger = {}
Logger.__index = Logger
local loggerLevels = {
    NONE = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4,
    TRACE = 5,
    FATAL = 1,
}
for _, level in ipairs({ 'trace', 'debug', 'info', 'warn', 'error', 'fatal' }) do
    Logger[level] = function(self, format, ...)
        if self:doLog(level) then
            loggerWrite(level:upper(), self.name, format, ...)
        end
    end
end
function Logger:setLogLevel(level)
    self.logLevel = level
end
function Logger:doLog(level)
    local configured = self.logLevel
    local configuredLevel = type(configured) == 'number'
        and configured
        or loggerLevels[tostring(configured or 'INFO'):upper()]
        or loggerLevels.INFO
    local requestedLevel = type(level) == 'number'
        and level
        or loggerLevels[tostring(level or 'INFO'):upper()]
        or loggerLevels.INFO
    return configuredLevel >= requestedLevel
end
function Logger:getLevelString(level)
    local numericLevel = tonumber(level) or tonumber(self.logLevel)
    if numericLevel ~= nil then
        for name, value in pairs(loggerLevels) do
            if value == numericLevel and name ~= 'FATAL' then
                return name
            end
        end
    end
    return tostring(level or self.logLevel or 'INFO'):upper()
end
function Logger:assert(condition, format, ...)
    if not condition then
        error(formatMessage(format, ...), 2)
    end
    return condition
end

runtime.logging = {
    new = function(options)
        options = options or {}
        return setmetatable({
            name = options.name or 'MWSE mod',
            logLevel = options.logLevel or 'INFO',
        }, Logger)
    end,
}

runtime.https = {
    request = function(url)
        noteUnsupported('ssl.https.request')
        return nil, 0, {}, 'network disabled by compatibility host: ' .. tostring(url)
    end,
}

runtime.inspect = function(value)
    local valueType = type(value)
    if valueType ~= 'table' then
        return tostring(value)
    end
    local parts = {}
    local count = 0
    for key, item in pairs(value) do
        count = count + 1
        if count > 16 then
            parts[#parts + 1] = '...'
            break
        end
        parts[#parts + 1] = tostring(key) .. '=' .. tostring(item)
    end
    return '{' .. table.concat(parts, ', ') .. '}'
end

local function makeEnumTable(prefix)
    return setmetatable({}, {
        __index = function(value, key)
            local result = prefix .. '.' .. tostring(key)
            rawset(value, key, result)
            return result
        end,
    })
end

local proxyMetatable
local function makeProxy(name, initial)
    local result = initial or {}
    result.__mwseCompatName = name
    return setmetatable(result, proxyMetatable)
end

proxyMetatable = {
    __index = function(value, key)
        local name = rawget(value, '__mwseCompatName') or 'proxy'
        local childName = name .. '.' .. tostring(key)
        noteUnsupported(childName)
        local child = makeProxy(childName)
        rawset(value, key, child)
        return child
    end,
    __call = function(value, ...)
        noteUnsupported((rawget(value, '__mwseCompatName') or 'proxy') .. '()')
        return nil
    end,
    __tostring = function(value)
        return '<' .. (rawget(value, '__mwseCompatName') or 'MWSE proxy') .. '>'
    end,
    __len = function()
        return 0
    end,
    __add = function()
        return 0
    end,
    __sub = function()
        return 0
    end,
    __mul = function()
        return 0
    end,
    __div = function()
        return 0
    end,
    __lt = function()
        return false
    end,
    __le = function()
        return false
    end,
}

local vector3Methods = {}
local vector3Metatable = { __index = vector3Methods }

local function makeVector3(x, y, z)
    if type(x) == 'table' and y == nil and z == nil then
        y = x.y or x[2]
        z = x.z or x[3]
        x = x.x or x[1]
    end
    return setmetatable({
        x = tonumber(x) or 0,
        y = tonumber(y) or 0,
        z = tonumber(z) or 0,
    }, vector3Metatable)
end

function vector3Methods:copy()
    return makeVector3(self.x, self.y, self.z)
end
function vector3Methods:length()
    return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)
end
function vector3Methods:normalize()
    local length = self:length()
    if length > 0 then
        self.x, self.y, self.z = self.x / length, self.y / length, self.z / length
    end
    return self
end
function vector3Methods:normalized()
    return self:copy():normalize()
end
function vector3Methods:dot(other)
    return self.x * other.x + self.y * other.y + self.z * other.z
end
function vector3Methods:cross(other)
    return makeVector3(
        self.y * other.z - self.z * other.y,
        self.z * other.x - self.x * other.z,
        self.x * other.y - self.y * other.x)
end
function vector3Methods:distance(other)
    return (self - other):length()
end
vector3Metatable.__add = function(left, right)
    return makeVector3(left.x + right.x, left.y + right.y, left.z + right.z)
end
vector3Metatable.__sub = function(left, right)
    return makeVector3(left.x - right.x, left.y - right.y, left.z - right.z)
end
vector3Metatable.__unm = function(value)
    return makeVector3(-value.x, -value.y, -value.z)
end
vector3Metatable.__mul = function(left, right)
    if type(left) == 'number' then
        return makeVector3(left * right.x, left * right.y, left * right.z)
    elseif type(right) == 'number' then
        return makeVector3(left.x * right, left.y * right, left.z * right)
    end
    return makeVector3(left.x * right.x, left.y * right.y, left.z * right.z)
end
vector3Metatable.__div = function(left, right)
    return makeVector3(left.x / right, left.y / right, left.z / right)
end

local matrix33Methods = {}
local matrix33Metatable = { __index = matrix33Methods }
local function makeMatrix33(...)
    local values = { ... }
    local result = { values = values }
    if #values == 3 and type(values[1]) == 'table' then
        result.x = makeVector3(values[1])
        result.y = makeVector3(values[2])
        result.z = makeVector3(values[3])
    elseif #values >= 9 then
        result.x = makeVector3(values[1], values[2], values[3])
        result.y = makeVector3(values[4], values[5], values[6])
        result.z = makeVector3(values[7], values[8], values[9])
    else
        result.x = makeVector3(1, 0, 0)
        result.y = makeVector3(0, 1, 0)
        result.z = makeVector3(0, 0, 1)
    end
    return setmetatable(result, matrix33Metatable)
end
function matrix33Methods:copy()
    local result = makeMatrix33(self.x, self.y, self.z)
    result.euler = self.euler and self.euler:copy() or nil
    return result
end
function matrix33Methods:fromEulerXYZ(x, y, z)
    self.euler = makeVector3(x, y, z)
    return self
end
function matrix33Methods:fromEulerZYX(z, y, x)
    self.euler = makeVector3(x, y, z)
    return self
end
function matrix33Methods:toEulerXYZ()
    return self.euler and self.euler:copy() or makeVector3(0, 0, 0)
end
function matrix33Methods:toRotation()
    return self
end
matrix33Metatable.__add = function()
    return makeMatrix33()
end
matrix33Metatable.__mul = function(_, right)
    if type(right) == 'table' and right.x ~= nil then
        return makeVector3(right)
    end
    return makeMatrix33()
end

local function copyPath(path)
    local result = {}
    for index, value in ipairs(path or {}) do
        result[index] = value
    end
    return result
end

local function appendPath(path, value)
    local result = copyPath(path)
    result[#result + 1] = tostring(value)
    return result
end

local function pathStartsWith(path, prefix)
    if #prefix > #path then
        return false
    end
    for index, value in ipairs(prefix) do
        if path[index] ~= value then
            return false
        end
    end
    return true
end

local function quaternionFromMatrix(rotation)
    local euler = type(rotation) == 'table' and rotation.euler
    if type(euler) ~= 'table' then
        return { x = 0, y = 0, z = 0, w = 1 }
    end
    local x = tonumber(euler.x) or 0
    local y = tonumber(euler.y) or 0
    local z = tonumber(euler.z) or 0
    if math.abs(x) > math.pi * 2 or math.abs(y) > math.pi * 2 or math.abs(z) > math.pi * 2 then
        x = math.rad(x)
        y = math.rad(y)
        z = math.rad(z)
    end
    local cx, sx = math.cos(x * 0.5), math.sin(x * 0.5)
    local cy, sy = math.cos(y * 0.5), math.sin(y * 0.5)
    local cz, sz = math.cos(z * 0.5), math.sin(z * 0.5)
    return {
        x = sx * cy * cz - cx * sy * sz,
        y = cx * sy * cz + sx * cy * sz,
        z = cx * cy * sz - sx * sy * cz,
        w = cx * cy * cz + sx * sy * sz,
    }
end

local function findSceneAttachment(targetKey, parentPath, nodeName)
    if state == nil then
        return nil
    end
    local exact
    for _, attachment in pairs(state.sceneAttachments) do
        if attachment.targetKey == targetKey
            and attachment.nodeName == nodeName
            and pathStartsWith(attachment.parentPath, parentPath)
        then
            if #attachment.parentPath == #parentPath then
                return attachment
            end
            exact = exact or attachment
        end
    end
    return exact
end

local function findSceneAttachments(targetKey)
    local result = {}
    for _, attachment in pairs(state.sceneAttachments) do
        if attachment.targetKey == targetKey then
            result[#result + 1] = attachment
        end
    end
    table.sort(result, function(left, right)
        return left.sequence > right.sequence
    end)
    return result
end

local makeSceneNode
makeSceneNode = function(options)
    options = options or {}
    local node = {
        __mwseSceneNode = true,
        __scenePath = copyPath(options.path),
        __attachmentId = options.attachmentId,
        __containerAttachmentId = options.containerAttachmentId,
        __openmwObject = options.openmwObject,
        __targetKey = tostring(options.targetKey or 'pending'),
        __isSelfScene = options.isSelf == true,
        __meshModel = options.model,
        __clearRootTransform = options.clearRootTransform == true,
        name = options.name or '',
        translation = options.translation or makeVector3(),
        rotation = options.rotation or makeMatrix33(),
        scale = tonumber(options.scale) or 1,
        appCulled = options.appCulled == true,
    }

    node.children = setmetatable({}, {
        __index = function(_, index)
            if type(index) ~= 'number' or index ~= 1 then
                return nil
            end
            return makeSceneNode {
                -- MWSE's NiNode collection retains a child handle.  Named
                -- descent can safely search the owned subtree from here,
                -- which also tolerates OpenMW's extra NiSwitchNode wrapper.
                path = node.__scenePath,
                containerAttachmentId = node.__containerAttachmentId,
                openmwObject = node.__openmwObject,
                targetKey = node.__targetKey,
                isSelf = node.__isSelfScene,
            }
        end,
    })

    node.getObjectByName = function(nodeSelf, name)
        local attachment = findSceneAttachment(
            nodeSelf.__targetKey, nodeSelf.__scenePath, tostring(name))
        local containerAttachment
        if nodeSelf.__containerAttachmentId ~= nil then
            containerAttachment = state.sceneAttachments[nodeSelf.__containerAttachmentId]
        end
        local nextPath = appendPath(nodeSelf.__scenePath, name)
        if attachment == nil
            and containerAttachment == nil
            and nodeSelf.__openmwObject ~= nil
        then
            local ok, exists = pcall(animation._mwseHasNode, nodeSelf.__openmwObject, {
                parentAttachment = '',
                path = nextPath,
            })
            if ok and exists == false then
                -- MWSE mutates the scene immediately, while OpenMW applies
                -- scene actions after this Lua frame.  Inspect each queued
                -- mesh instance synchronously so nested lookups can retain
                -- the correct owner without hard-coding a mod or node name.
                for _, candidate in ipairs(findSceneAttachments(nodeSelf.__targetKey)) do
                    local modelOk, meshContainsNode = pcall(
                        animation._mwseMeshHasNode,
                        nodeSelf.__openmwObject,
                        { model = candidate.model, path = nextPath })
                    if modelOk and meshContainsNode then
                        containerAttachment = candidate
                        break
                    end
                end
                if containerAttachment == nil then
                    return nil
                end
            end
        end
        return makeSceneNode {
            path = nextPath,
            attachmentId = attachment and attachment.id or nil,
            containerAttachmentId = attachment and attachment.id
                or containerAttachment and containerAttachment.id or nil,
            openmwObject = nodeSelf.__openmwObject,
            targetKey = nodeSelf.__targetKey,
            isSelf = nodeSelf.__isSelfScene,
            name = tostring(name),
        }
    end

    node.clone = function(nodeSelf)
        return makeSceneNode {
            path = nodeSelf.__scenePath,
            attachmentId = nodeSelf.__attachmentId,
            containerAttachmentId = nodeSelf.__containerAttachmentId,
            openmwObject = nodeSelf.__openmwObject,
            targetKey = nodeSelf.__targetKey,
            isSelf = nodeSelf.__isSelfScene,
            model = nodeSelf.__meshModel,
            clearRootTransform = nodeSelf.__clearRootTransform,
            name = nodeSelf.name,
            translation = nodeSelf.translation and nodeSelf.translation:copy() or makeVector3(),
            rotation = nodeSelf.rotation and nodeSelf.rotation:copy() or makeMatrix33(),
            scale = nodeSelf.scale,
            appCulled = nodeSelf.appCulled,
        }
    end

    node.clearTransforms = function(nodeSelf)
        nodeSelf.translation = makeVector3()
        nodeSelf.rotation = makeMatrix33()
        nodeSelf.scale = 1
        nodeSelf.__clearRootTransform = true
    end

    node.attachChild = function(nodeSelf, child)
        if nodeSelf.__openmwObject == nil or type(child) ~= 'table' or child.__meshModel == nil then
            noteUnsupported('scene.attachChild:missingTargetOrMesh')
            return child
        end
        local nodeName = child.name ~= '' and child.name or child.__meshModel
        local attachmentPath = appendPath(nodeSelf.__scenePath, nodeName)
        local id = nodeSelf.__targetKey .. '/' .. table.concat(attachmentPath, '/')
        animation._mwseAttachMesh(nodeSelf.__openmwObject, {
            id = id,
            model = child.__meshModel,
            parentAttachment = nodeSelf.__containerAttachmentId or '',
            parentPath = nodeSelf.__scenePath,
            nodeName = nodeName,
            translation = child.translation or makeVector3(),
            rotation = quaternionFromMatrix(child.rotation),
            scale = tonumber(child.scale) or 1,
            clearRootTransform = child.__clearRootTransform == true,
        })
        child.__attachmentId = id
        child.__containerAttachmentId = id
        child.__openmwObject = nodeSelf.__openmwObject
        child.__targetKey = nodeSelf.__targetKey
        child.__scenePath = attachmentPath
        state.sceneAttachments[id] = {
            id = id,
            targetKey = nodeSelf.__targetKey,
            parentPath = copyPath(nodeSelf.__scenePath),
            parentAttachment = nodeSelf.__containerAttachmentId,
            nodeName = nodeName,
            model = child.__meshModel,
            sequence = state.nextSceneAttachmentSequence,
        }
        state.nextSceneAttachmentSequence = state.nextSceneAttachmentSequence + 1
        state.sceneAttachmentCount = countKeys(state.sceneAttachments)
        return child
    end

    node.detachChild = function(nodeSelf, child)
        local id = type(child) == 'table' and child.__attachmentId
        if id ~= nil then
            animation._mwseDetachMesh(nodeSelf.__openmwObject, id)
            state.sceneAttachments[id] = nil
            state.sceneAttachmentCount = countKeys(state.sceneAttachments)
        end
    end
    node.detachChildAt = function() end

    node.update = function() end
    node.updateNodeEffects = function() end

    return setmetatable(node, {
        __newindex = function(nodeSelf, key, value)
            if key == 'switchIndex' and nodeSelf.__openmwObject ~= nil then
                rawset(nodeSelf, '__switchIndex', value)
                animation._mwseSetSwitch(nodeSelf.__openmwObject, {
                    parentAttachment = nodeSelf.__containerAttachmentId or '',
                    path = nodeSelf.__scenePath,
                    index = tonumber(value) or 0,
                })
            else
                rawset(nodeSelf, key, value)
            end
        end,
        __index = function(nodeSelf, key)
            if key == 'switchIndex' then
                return rawget(nodeSelf, '__switchIndex')
            end
            return nil
        end,
    })
end

local function makeMobilePlayer()
    return makeProxy('tes3.mobilePlayer', {
        health = { current = 100, base = 100 },
        magicka = { current = 100, base = 100 },
        fatigue = { current = 100, base = 100 },
        encumbrance = { current = 0, currentRaw = 0, base = 300 },
        level = 1,
        inCombat = false,
        inJail = false,
        controlsDisabled = false,
        torchSlot = false,
        sleeping = false,
        waiting = false,
        underwater = false,
        resistFrost = 0,
        resistFire = 0,
        isAffectedByObject = function()
            return false
        end,
    })
end

local function makePlayer()
    return makeProxy('tes3.player', {
        id = 'player',
        data = {},
        tempData = {},
        object = makeProxy('tes3.player.object', {
            id = 'player',
            name = 'Player',
            female = false,
            race = {
                id = 'player_race',
                name = 'Player Race',
                weight = { male = 1, female = 1 },
                height = { male = 1, female = 1 },
            },
            inventory = {},
            equipment = {},
        }),
        position = makeVector3(),
        orientation = makeVector3(),
        stackSize = 1,
        attachments = { variables = { count = 1 } },
        sceneNode = makeSceneNode {
            isSelf = true,
            openmwObject = self,
            targetKey = 'player',
        },
    })
end

local recordTypes = {
    { name = 'Activator', api = types.Activator },
    { name = 'Apparatus', api = types.Apparatus },
    { name = 'Armor', api = types.Armor },
    { name = 'Book', api = types.Book },
    { name = 'Clothing', api = types.Clothing },
    { name = 'Container', api = types.Container },
    { name = 'Creature', api = types.Creature },
    { name = 'Door', api = types.Door },
    { name = 'Ingredient', api = types.Ingredient },
    { name = 'Light', api = types.Light },
    { name = 'Lockpick', api = types.Lockpick },
    { name = 'Miscellaneous', api = types.Miscellaneous },
    { name = 'NPC', api = types.NPC },
    { name = 'Potion', api = types.Potion },
    { name = 'Probe', api = types.Probe },
    { name = 'Repair', api = types.Repair },
    { name = 'Static', api = types.Static },
    { name = 'Weapon', api = types.Weapon },
}

local mwseObjectTypeByRecordType = {
    Activator = 'tes3.objectType.activator',
    Apparatus = 'tes3.objectType.apparatus',
    Armor = 'tes3.objectType.armor',
    Book = 'tes3.objectType.book',
    Clothing = 'tes3.objectType.clothing',
    Container = 'tes3.objectType.container',
    Creature = 'tes3.objectType.creature',
    Door = 'tes3.objectType.door',
    Ingredient = 'tes3.objectType.ingredient',
    Light = 'tes3.objectType.light',
    Lockpick = 'tes3.objectType.lockpick',
    Miscellaneous = 'tes3.objectType.miscItem',
    NPC = 'tes3.objectType.npc',
    Potion = 'tes3.objectType.alchemy',
    Probe = 'tes3.objectType.probe',
    Repair = 'tes3.objectType.repairItem',
    Static = 'tes3.objectType.static',
    Weapon = 'tes3.objectType.weapon',
}

local function readRecordProperty(record, key)
    local ok, value = pcall(function()
        return record[key]
    end)
    if ok then
        return value
    end
    return nil
end

local function findOpenMwRecord(id)
    local normalizedId = tostring(id)
    for _, candidate in ipairs(recordTypes) do
        local ok, record = pcall(candidate.api.record, normalizedId)
        if ok and record ~= nil then
            return record, candidate.name
        end
    end
    return nil, nil
end

local function makeObject(id)
    local normalizedId = tostring(id)
    local cacheKey = normalizedId:lower()
    if state ~= nil and state.objectCache[cacheKey] ~= nil then
        return state.objectCache[cacheKey]
    end
    local record, recordType = findOpenMwRecord(normalizedId)
    local result = {
        id = normalizedId,
        name = normalizedId,
        value = 0,
        weight = 0,
        objectType = mwseObjectTypeByRecordType[recordType] or 'tes3.objectType.unknown',
        openmwRecordType = recordType,
        equipment = {},
        inventory = {},
        parts = {
            makeProxy('tes3.object[' .. normalizedId .. '].parts[1]', {
                type = 0,
            }),
        },
    }

    if record ~= nil then
        for _, key in ipairs({
            'id', 'name', 'model', 'icon', 'weight', 'value', 'type', 'enchant',
            'enchantCapacity', 'mwscript', 'race', 'class', 'baseDisposition',
            'baseGold', 'isMale', 'isEssential', 'isRespawning',
        }) do
            local value = readRecordProperty(record, key)
            if value ~= nil then
                result[key] = value
            end
        end

        -- MWSE names this field "mesh"; OpenMW's record API calls the same
        -- canonical VFS path "model".
        result.mesh = result.model or ''
        result.openmwRecord = record
        if type(result.race) == 'string' then
            result.race = {
                id = result.race,
                name = result.race,
            }
        end
        if type(result.class) == 'string' then
            result.class = {
                id = result.class,
                name = result.class,
            }
        end

        if state ~= nil and not state.recordMappingIds[normalizedId] then
            state.recordMappingIds[normalizedId] = true
            state.recordMappings = state.recordMappings + 1
            print('MWSE compat: state=record-mapped id=' .. normalizedId
                .. ' type=' .. tostring(recordType)
                .. ' model=' .. tostring(result.model or ''))
        end
    end

    local facade = makeProxy('tes3.object[' .. normalizedId .. ']', result)
    facade.baseObject = facade
    if state ~= nil then
        state.objectCache[cacheKey] = facade
    end
    return facade
end

local function getItemId(item)
    if type(item) == 'string' then
        return item:lower()
    end
    if type(item) == 'table' and item.id ~= nil then
        return tostring(item.id):lower()
    end
    return tostring(item or ''):lower()
end

local function makeInventoryFacade()
    local inventory = {}
    local ok, openmwInventory = pcall(types.Actor.inventory, self)
    if ok and openmwInventory ~= nil then
        local allOk, all = pcall(function()
            return openmwInventory:getAll()
        end)
        if allOk and all ~= nil then
            for _, item in ipairs(all) do
                local recordId = readRecordProperty(item, 'recordId')
                if recordId ~= nil then
                    inventory[#inventory + 1] = {
                        object = makeObject(recordId),
                        count = tonumber(readRecordProperty(item, 'count')) or 1,
                        itemData = {},
                        openmwObject = item,
                    }
                end
            end
        end
    end
    return setmetatable(inventory, {
        -- MWSE exposes inventory as userdata: pairs() yields stacks, while
        -- helper members such as iterator are not themselves inventory rows.
        __index = function(_, key)
            if key == 'getItemCount' then
                return function(_, item)
                    local id = getItemId(item)
                    local countOk, count = pcall(function()
                        return types.Actor.inventory(self):countOf(id)
                    end)
                    return countOk and tonumber(count) or 0
                end
            end
            if key == 'iterator' then
                return function()
                    local index = 0
                    return function()
                        index = index + 1
                        return inventory[index]
                    end
                end
            end
            if key == 'calculateWeight' then
                return function()
                    local total = 0
                    for _, stack in ipairs(inventory) do
                        total = total
                            + (tonumber(stack.object and stack.object.weight) or 0)
                                * (tonumber(stack.count) or 0)
                    end
                    return total
                end
            end
            return nil
        end,
    })
end

local function syncPlayerInventory(player)
    player.object.inventory = makeInventoryFacade()
    local inventoryWeight = player.object.inventory:calculateWeight()
    if player.mobile ~= nil and type(player.mobile.encumbrance) == 'table' then
        player.mobile.encumbrance.current = inventoryWeight
        player.mobile.encumbrance.currentRaw = inventoryWeight
    end
    local equipment = {}
    local ok, openmwEquipment = pcall(types.Actor.getEquipment, self)
    if ok and type(openmwEquipment) == 'table' then
        for slot, item in pairs(openmwEquipment) do
            local recordId = readRecordProperty(item, 'recordId')
            if recordId ~= nil then
                equipment[#equipment + 1] = {
                    object = makeObject(recordId),
                    count = tonumber(readRecordProperty(item, 'count')) or 1,
                    itemData = {},
                    slot = slot,
                    openmwObject = item,
                }
            end
        end
    end
    for id in pairs(state.pseudoEquipped) do
        local alreadyPresent = false
        for _, stack in ipairs(equipment) do
            if stack.object.id:lower() == id then
                alreadyPresent = true
                break
            end
        end
        if not alreadyPresent then
            equipment[#equipment + 1] = {
                object = makeObject(id),
                count = 1,
                itemData = {},
                slot = state.pseudoEquipped[id],
            }
        end
    end
    player.object.equipment = equipment
end

local function ensurePlayerRecordFacade(player)
    local object = player.object
    object.female = object.female == true or object.isMale == false
    if type(object.race) ~= 'table' then
        object.race = {
            id = tostring(object.race or 'player_race'),
            name = tostring(object.race or 'Player Race'),
        }
    end
    object.race.weight = type(object.race.weight) == 'table'
        and object.race.weight
        or { male = 1, female = 1 }
    object.race.height = type(object.race.height) == 'table'
        and object.race.height
        or { male = 1, female = 1 }
    object.race.weight.male = tonumber(object.race.weight.male) or 1
    object.race.weight.female = tonumber(object.race.weight.female) or object.race.weight.male
    object.race.height.male = tonumber(object.race.height.male) or 1
    object.race.height.female = tonumber(object.race.height.female) or object.race.height.male
    object.inventory = object.inventory or {}
    object.equipment = object.equipment or {}
end

local function requestedTypeMatches(recordType, requestedType)
    if requestedType == nil then
        return true
    end
    local requested = tostring(requestedType):lower()
    if requested:sub(1, 16) == 'tes3.objecttype.' then
        requested = requested:sub(17)
    end
    if requested == 'item' then
        return recordType ~= 'Activator'
            and recordType ~= 'Container'
            and recordType ~= 'Creature'
            and recordType ~= 'Door'
            and recordType ~= 'NPC'
            and recordType ~= 'Static'
    end
    local aliases = {
        alchemy = 'potion',
        miscitem = 'miscellaneous',
        repairitem = 'repair',
    }
    requested = aliases[requested] or requested
    return tostring(recordType or ''):lower() == requested
end

local function makeReferenceFromOpenMw(object, cell)
    local objectId = readRecordProperty(object, 'id') or tostring(object)
    local cacheKey = tostring(objectId)
    if state.referenceCache[cacheKey] ~= nil then
        return state.referenceCache[cacheKey]
    end

    local recordId = readRecordProperty(object, 'recordId') or cacheKey
    local position = readRecordProperty(object, 'position')
    local recordFacade = makeObject(recordId)
    local stackSize = tonumber(readRecordProperty(object, 'count')) or 1
    local reference = makeProxy('tes3.reference[' .. cacheKey .. ']', {
        id = cacheKey,
        object = recordFacade,
        baseObject = recordFacade,
        position = position and makeVector3(position.x, position.y, position.z) or makeVector3(),
        orientation = makeVector3(),
        cell = cell,
        data = {},
        tempData = {},
        disabled = readRecordProperty(object, 'enabled') == false,
        supportsLuaData = true,
        stackSize = stackSize,
        attachments = { variables = { count = stackSize } },
        light = false,
        mobile = {
            health = { current = 100, base = 100 },
            magicka = { current = 100, base = 100 },
            fatigue = { current = 100, base = 100 },
            underwater = false,
        },
        openmwObject = object,
        sceneNode = makeSceneNode {
            openmwObject = object,
            targetKey = cacheKey,
        },
        updateSceneGraph = function() end,
        deleteDynamicLightAttachment = function() end,
    })
    state.referenceCache[cacheKey] = reference
    state.referenceMappings = state.referenceMappings + 1
    return reference
end

local function makePendingReference(requestId, recordId, options, cell)
    local targetKey = 'pending-' .. tostring(requestId)
    local position = options.position or makeVector3()
    local orientation = options.orientation or makeVector3()
    local stackSize = tonumber(options.count) or 1
    return makeProxy('tes3.reference[' .. targetKey .. ']', {
        id = targetKey,
        object = makeObject(recordId),
        baseObject = makeObject(recordId),
        position = makeVector3(position.x, position.y, position.z),
        orientation = makeVector3(orientation.x, orientation.y, orientation.z),
        scale = tonumber(options.scale) or 1,
        cell = cell,
        data = {},
        tempData = {},
        disabled = false,
        supportsLuaData = true,
        stackSize = stackSize,
        attachments = { variables = { count = stackSize } },
        light = false,
        openmwObject = nil,
        sceneNode = makeSceneNode {
            targetKey = targetKey,
        },
        updateSceneGraph = function() end,
        deleteDynamicLightAttachment = function() end,
    })
end

local function collectNearbyReferences(cell, requestedType)
    local result = {}
    local seen = {}
    local lists = {
        nearby.activators,
        nearby.actors,
        nearby.containers,
        nearby.doors,
        nearby.items,
        nearby.players,
    }
    for _, list in ipairs(lists) do
        for _, object in ipairs(list) do
            local objectId = readRecordProperty(object, 'id') or tostring(object)
            if not seen[objectId] then
                seen[objectId] = true
                local recordId = readRecordProperty(object, 'recordId')
                local _, recordType = findOpenMwRecord(recordId or '')
                if requestedTypeMatches(recordType, requestedType) then
                    result[#result + 1] = makeReferenceFromOpenMw(object, cell)
                end
            end
        end
    end
    return result
end

local function makeCurrentCell()
    local openmwCell = self.cell
    local id = readRecordProperty(openmwCell, 'id') or 'OpenMW current cell'
    local name = readRecordProperty(openmwCell, 'name') or tostring(id)
    local isExterior = readRecordProperty(openmwCell, 'isExterior') == true
    local hasWater = readRecordProperty(openmwCell, 'hasWater') == true
    local waterLevel = readRecordProperty(openmwCell, 'waterLevel')
    local cell
    cell = makeProxy('tes3.cell[' .. tostring(id) .. ']', {
        id = tostring(id),
        name = tostring(name),
        isExterior = isExterior,
        isInterior = not isExterior,
        behavesAsExterior = isExterior,
        region = { id = '', name = '' },
        hasWater = hasWater,
        waterLevel = tonumber(waterLevel) or 0,
        openmwCell = openmwCell,
        iterateReferences = function(_, requestedType)
            local references = collectNearbyReferences(cell, requestedType)
            local index = 0
            return function()
                index = index + 1
                return references[index]
            end
        end,
    })
    return cell
end

local function makeMcmNode(kind, options, parent)
    options = type(options) == 'table' and options or { label = tostring(options or kind) }
    local node = {
        kind = kind,
        label = options.label or options.name or options.text or kind,
        options = options,
        parent = parent,
        children = {},
    }
    return setmetatable(node, {
        __index = function(value, key)
            if key == 'sidebar' then
                local sidebar = makeMcmNode('sidebar', { label = 'Sidebar' }, value)
                rawset(value, key, sidebar)
                return sidebar
            end
            if key == 'register' then
                return function(template)
                    template.registered = true
                    state.mcmRegistered = state.mcmRegistered + 1
                    print('MWSE compat: state=mcm-registered template=' .. tostring(template.label))
                    return template
                end
            end
            if key == 'saveOnClose' then
                return function(container)
                    return container
                end
            end
            if type(key) == 'string' and key:sub(1, 6) == 'create' then
                return function(container, childOptions)
                    local child = makeMcmNode(key:sub(7), childOptions, container)
                    container.children[#container.children + 1] = child
                    state.mcmControls = state.mcmControls + 1
                    if container.kind == 'Template' and key:find('Page') then
                        state.mcmPages[#state.mcmPages + 1] = child.label
                    end
                    return child
                end
            end
            return nil
        end,
    })
end

runtime.mcm = {}
runtime.mcm.createTemplate = function(options)
    local template = makeMcmNode('Template', options)
    state.mcmTemplates[#state.mcmTemplates + 1] = template
    return template
end
runtime.mcm.createTableVariable = function(options)
    options = options or {}
    return setmetatable(options, {
        __index = function(value, key)
            if key == 'value' and value.table ~= nil then
                return value.table[value.id]
            end
            return nil
        end,
        __newindex = function(value, key, newValue)
            if key == 'value' and value.table ~= nil then
                value.table[value.id] = newValue
            else
                rawset(value, key, newValue)
            end
        end,
    })
end

runtime.common = {
    isReference = function(value)
        return type(value) == 'table' and value.object ~= nil
    end,
    isMobileActor = function(value)
        return type(value) == 'table' and (value.reference ~= nil or value.health ~= nil)
    end,
    isActor = function(value)
        return type(value) == 'table' and (value.mobile ~= nil or value.reference ~= nil)
    end,
    getRelatedReference = function(value)
        if type(value) == 'string' then
            return tes3 and tes3.getReference and tes3.getReference(value) or nil
        end
        return value and (value.reference or value)
    end,
    getRelatedMobileActor = function(value)
        if type(value) == 'string' then
            value = tes3 and tes3.getReference and tes3.getReference(value) or nil
        end
        return value and (value.mobile or value)
    end,
    resolveDynamicValue = function(value, ...)
        if type(value) == 'function' then
            return value(...)
        end
        return value
    end,
}
runtime.common.resolveDynamicText = function(value, ...)
    return tostring(runtime.common.resolveDynamicValue(value, ...))
end

local eventApi = {}

local function normalizeEventId(eventId)
    return tostring(eventId)
end

function eventApi.register(eventId, callback, options)
    local id = normalizeEventId(eventId)
    local handlers = state.handlers[id]
    if handlers == nil then
        handlers = {}
        state.handlers[id] = handlers
    end
    options = options or {}
    handlers[#handlers + 1] = {
        callback = callback,
        priority = tonumber(options.priority) or 0,
        filter = options.filter,
        doOnce = options.doOnce == true,
    }
    table.sort(handlers, function(left, right)
        return left.priority > right.priority
    end)
    state.registrations = state.registrations + 1
end

function eventApi.unregister(eventId, callback, options)
    local handlers = state.handlers[normalizeEventId(eventId)] or {}
    local filter = options and options.filter
    for index = #handlers, 1, -1 do
        local row = handlers[index]
        if row.callback == callback and (filter == nil or row.filter == filter) then
            table.remove(handlers, index)
        end
    end
end

function eventApi.isRegistered(eventId, callback)
    for _, row in ipairs(state.handlers[normalizeEventId(eventId)] or {}) do
        if row.callback == callback then
            return true
        end
    end
    return false
end

function eventApi.clear(eventId)
    if eventId == nil then
        state.handlers = {}
    else
        state.handlers[normalizeEventId(eventId)] = {}
    end
end

function eventApi.trigger(eventId, payload, options)
    local id = normalizeEventId(eventId)
    payload = payload or {}
    local requestedFilter = options and options.filter
    local handlers = state.handlers[id] or {}
    local remove = {}
    local errorsBefore = state.errorSerial
    for index, row in ipairs(handlers) do
        local filterMatches = row.filter == nil or requestedFilter == nil or row.filter == requestedFilter
        if filterMatches then
            local ok, result = pcall(row.callback, payload)
            if not ok then
                noteError('event:' .. id, result)
            elseif result == true then
                payload.block = true
                payload.claim = true
            end
            if row.doOnce then
                remove[#remove + 1] = index
            end
            if payload.claim then
                break
            end
        end
    end
    for index = #remove, 1, -1 do
        table.remove(handlers, remove[index])
    end
    return payload, state.errorSerial == errorsBefore
end

local timers = {}
local timerApi = {
    real = 'real',
    simulate = 'simulate',
    game = 'game',
}

local function addTimer(options, callback)
    if type(options) == 'number' then
        options = { duration = options, callback = callback }
    end
    options = options or {}
    local timer = {
        duration = tonumber(options.duration) or 0,
        remaining = tonumber(options.duration) or 0,
        callback = options.callback or callback,
        iterations = tonumber(options.iterations) or 1,
        data = options.data,
    }
    timers[#timers + 1] = timer
    return timer
end

timerApi.start = addTimer
timerApi.delayOneFrame = function(callback)
    return addTimer({ duration = 0, callback = callback })
end
timerApi.frame = {
    delayOneFrame = timerApi.delayOneFrame,
}

local function updateTimers(dt)
    for index = #timers, 1, -1 do
        local timer = timers[index]
        timer.remaining = timer.remaining - dt
        if timer.remaining <= 0 then
            local ok, message = pcall(timer.callback, timer)
            if not ok then
                noteError('timer', message)
            end
            if timer.iterations == -1 or timer.iterations > 1 then
                if timer.iterations > 1 then
                    timer.iterations = timer.iterations - 1
                end
                timer.remaining = timer.duration
            else
                table.remove(timers, index)
            end
        end
    end
end

local function simpleJsonEncode(value, seen)
    local valueType = type(value)
    if valueType == 'nil' then
        return 'null'
    elseif valueType == 'boolean' or valueType == 'number' then
        return tostring(value)
    elseif valueType == 'string' then
        return baseString.format('%q', value)
    elseif valueType ~= 'table' then
        return baseString.format('%q', tostring(value))
    end
    seen = seen or {}
    if seen[value] then
        return '"<cycle>"'
    end
    seen[value] = true
    local parts = {}
    local array = #value > 0
    for key, item in pairs(value) do
        if array then
            parts[#parts + 1] = simpleJsonEncode(item, seen)
        else
            parts[#parts + 1] = simpleJsonEncode(tostring(key), seen) .. ':' .. simpleJsonEncode(item, seen)
        end
    end
    seen[value] = nil
    if array then
        return '[' .. table.concat(parts, ',') .. ']'
    end
    return '{' .. table.concat(parts, ',') .. '}'
end

-- MWSE UI elements are retained-mode objects.  This virtual tree preserves
-- their identity, hierarchy, mutable properties, and lookup semantics even
-- though OpenMW uses a different declarative UI system.  Rendering adapters
-- can consume the same tree without any mod-specific source changes.
local uiElementMethods = {}
local uiElementMetatable = {
    __index = function(value, key)
        return uiElementMethods[key] or rawget(value, key)
    end,
    __newindex = function(value, key, item)
        rawset(value, key, item)
        if state ~= nil then
            state.uiDirty = true
        end
    end,
}

local function makeUiElement(kind, options, parent)
    options = type(options) == 'table' and options or {}
    local element = {
        kind = kind,
        id = options.id and tostring(options.id) or nil,
        parent = parent,
        children = {},
        events = {},
        visible = options.visible ~= false,
        width = tonumber(options.width) or 0,
        height = tonumber(options.height) or 0,
        text = tostring(options.text or ''),
        color = options.color,
        contentPath = options.path and tostring(options.path) or nil,
        widget = {
            current = tonumber(options.current) or 0,
            max = tonumber(options.max) or 100,
            fillColor = options.fillColor,
            showText = options.showText ~= false,
        },
    }
    setmetatable(element, uiElementMetatable)
    if parent ~= nil then
        parent.children[#parent.children + 1] = element
    end
    if state ~= nil then
        state.uiElementCount = state.uiElementCount + 1
        state.uiDirty = true
        if element.id ~= nil then
            state.uiById[element.id] = element
        end
    end
    return element
end

function uiElementMethods:findChild(id)
    id = tostring(id)
    for _, child in ipairs(self.children) do
        if tostring(child.id or '') == id then
            return child
        end
        local nested = child:findChild(id)
        if nested ~= nil then
            return nested
        end
    end
    return nil
end

function uiElementMethods:createBlock(options)
    return makeUiElement('block', options, self)
end

function uiElementMethods:createThinBorder(options)
    return makeUiElement('thinBorder', options, self)
end

function uiElementMethods:createRect(options)
    return makeUiElement('rect', options, self)
end

function uiElementMethods:createLabel(options)
    return makeUiElement('label', options, self)
end

function uiElementMethods:createImage(options)
    return makeUiElement('image', options, self)
end

function uiElementMethods:createFillBar(options)
    local fillBar = makeUiElement('fillBar', options, self)
    local colorBar = makeUiElement('rect', {
        id = 'PartFillbar_colorbar_ptr',
        color = fillBar.widget.fillColor,
    }, fillBar)
    colorBar.width = fillBar.width
    colorBar.height = fillBar.height
    makeUiElement('label', {
        id = 'PartFillbar_text_ptr',
        text = '',
        visible = false,
    }, fillBar)
    return fillBar
end

function uiElementMethods:register(eventId, callback)
    local id = tostring(eventId)
    self.events[id] = self.events[id] or {}
    self.events[id][#self.events[id] + 1] = callback
    return true
end

function uiElementMethods:updateLayout()
    if state ~= nil then
        state.uiDirty = true
    end
    return true
end

function uiElementMethods:reorderChildren()
    if state ~= nil then
        state.uiDirty = true
    end
    return true
end

function uiElementMethods:destroy()
    if self.parent ~= nil then
        for index, child in ipairs(self.parent.children) do
            if child == self then
                table.remove(self.parent.children, index)
                break
            end
        end
    end
    self.visible = false
    return true
end

function uiElementMethods:destroyChildren()
    for _, child in ipairs(self.children) do
        child.visible = false
    end
    self.children = {}
    if state ~= nil then
        state.uiDirty = true
    end
    return true
end

local function createCompatUiMenus()
    local menuMulti = makeUiElement('menu', { id = 'MenuMulti' })
    menuMulti.width = 1280
    menuMulti.height = 720

    local function addStatBars(menu)
        for _, id in ipairs({
            'MenuStat_health_fillbar',
            'MenuStat_magic_fillbar',
            'MenuStat_fatigue_fillbar',
        }) do
            local bar = menu:createFillBar { id = id, current = 100, max = 100 }
            bar.width = 110
            bar.height = 12
        end
    end
    addStatBars(menuMulti)

    -- Reproduce the stable ancestor relationship MWSE mods use to anchor
    -- widgets beside the vanilla weapon layout.
    local hudAnchor = menuMulti:createBlock { id = 'MenuMulti_compat_hud_anchor' }
    local shellA = hudAnchor:createBlock()
    local shellB = shellA:createBlock()
    shellB:createBlock { id = 'MenuMulti_weapon_layout' }

    state.uiMenus.MenuMulti = menuMulti

    -- Ashfall extends both of these vanilla menus.  Supplying their stable
    -- element IDs lets its untouched uiCreated/uiActivated handlers build the
    -- real retained-mode needs widgets against the compatibility surface.
    local menuStat = makeUiElement('menu', { id = 'MenuStat', visible = false })
    menuStat.width = 420
    menuStat.height = 560
    addStatBars(menuStat)
    state.uiMenus.MenuStat = menuStat

    local menuInventory = makeUiElement('menu', { id = 'MenuInventory', visible = false })
    menuInventory.width = 560
    menuInventory.height = 620
    local inventoryLayout = menuInventory:createBlock {
        id = 'MenuInventory_compat_layout',
    }
    inventoryLayout:createBlock { id = 'MenuInventory_character_box' }
    state.uiMenus.MenuInventory = menuInventory
end

local function closeMessageMenu()
    state.messageMenu = nil
    if state.messageMenuOverlay ~= nil then
        pcall(function()
            state.messageMenuOverlay:destroy()
        end)
        state.messageMenuOverlay = nil
    end
end

local function messageButtonState(button)
    if type(button) ~= 'table' or type(button.enableRequirements) ~= 'function' then
        return true
    end
    local ok, enabled = pcall(button.enableRequirements)
    if not ok then
        noteError('message-menu-requirement', enabled)
        return false
    end
    return enabled ~= false
end

local function messageMenuText(menu)
    local message = tostring(menu.message or '')
    if message == '' then
        message = 'Cooking Pot'
    end
    local lines = {
        'ASHFALL COOKING POT',
        message,
        '',
    }
    for index, button in ipairs(menu.buttons or {}) do
        local enabled = messageButtonState(button)
        lines[#lines + 1] = baseString.format(
            '%d. %s%s',
            index,
            tostring(button.text or ('Option ' .. index)),
            enabled and '' or '  [UNAVAILABLE]')
    end
    lines[#lines + 1] = ''
    lines[#lines + 1] = 'LIVE MENU FROM ASHFALL'
    return baseTable.concat(lines, '\n')
end

local function showMessageMenu(options)
    options = type(options) == 'table' and options or { message = options }
    closeMessageMenu()
    state.messageMenu = {
        message = options.message,
        buttons = options.buttons or {},
        cancels = options.cancels == true,
        cancelCallback = options.cancelCallback,
    }
    local text = messageMenuText(state.messageMenu)
    local backgroundTexture = ui.texture { path = 'white' }
    state.messageMenuOverlay = ui.create {
        layer = 'Windows',
        type = ui.TYPE.Widget,
        props = {
            position = util.vector2(800, 150),
            size = util.vector2(470, 320),
        },
        content = ui.content {
            {
                type = ui.TYPE.Image,
                props = {
                    position = util.vector2(0, 0),
                    size = util.vector2(470, 320),
                    resource = backgroundTexture,
                    color = util.color.rgb(0.025, 0.025, 0.02),
                    alpha = 0.82,
                    visible = true,
                },
            },
            {
                type = ui.TYPE.Text,
                props = {
                    position = util.vector2(22, 18),
                    text = text,
                    textSize = 22,
                    textColor = util.color.rgb(1.0, 0.86, 0.42),
                    textShadow = true,
                    textShadowColor = util.color.rgb(0, 0, 0),
                    multiline = true,
                    visible = true,
                },
            },
        },
    }
    local names = {}
    for _, button in ipairs(state.messageMenu.buttons) do
        names[#names + 1] = tostring(button.text or '')
    end
    print('MWSE compat: state=message-menu-shown message='
        .. tostring(state.messageMenu.message or '')
        .. ' buttons=' .. baseTable.concat(names, '|'))
    return state.messageMenu
end

local function selectMessageButton(selector)
    local menu = state.messageMenu
    if menu == nil then
        print('MWSE compat: state=message-menu-select-missed reason=no-menu')
        return false
    end
    local selected
    local selectedIndex
    if type(selector) == 'number' then
        selectedIndex = math.floor(selector)
        selected = menu.buttons[selectedIndex]
    else
        local needle = tostring(selector or ''):lower()
        for index, button in ipairs(menu.buttons) do
            local text = tostring(button.text or ''):lower()
            if text == needle or text:find(needle, 1, true) ~= nil then
                selected = button
                selectedIndex = index
                break
            end
        end
    end
    if selected == nil or not messageButtonState(selected) then
        print('MWSE compat: state=message-menu-select-missed selector='
            .. tostring(selector) .. ' reason=missing-or-disabled')
        return false
    end

    local label = tostring(selected.text or selectedIndex)
    closeMessageMenu()
    local ok, result = pcall(selected.callback or function() end)
    if not ok then
        noteError('message-menu-callback:' .. label, result)
        return false
    end
    print('MWSE compat: state=message-menu-selected index='
        .. tostring(selectedIndex) .. ' text=' .. label)
    return true
end

local function installGlobals(mods)
    installStandardExtensions()

    local player = makePlayer()
    local currentCell = makeCurrentCell()
    state.currentCell = currentCell
    player.cell = currentCell
    local mobilePlayer = makeMobilePlayer()
    local playerRecordId = readRecordProperty(self, 'recordId') or 'player'
    player.object = makeObject(playerRecordId)
    ensurePlayerRecordFacade(player)
    player.openmwObject = self
    player.mobile = mobilePlayer
    mobilePlayer.reference = player
    mobilePlayer.openmwObject = self
    local werewolfOk, isWerewolf = pcall(types.NPC.isWerewolf, self)
    mobilePlayer.werewolf = werewolfOk and isWerewolf == true
    state.player = player
    syncPlayerInventory(player)
    local tes3 = {
        player = player,
        mobilePlayer = mobilePlayer,
        game = makeProxy('tes3.game', { playerTarget = nil }),
        worldController = makeProxy('tes3.worldController', {
            deltaTime = 0,
            hour = { value = 12 },
            daysPassed = { value = 1 },
            weatherController = { hoursBetweenWeatherChanges = 3 },
            worldCamera = { cameraRoot = nil },
        }),
    }

    for _, enumName in ipairs({
        'activeBodyPart', 'actorType', 'aiPackage', 'animationGroup', 'animationStartFlag',
        'armorSlot', 'attribute', 'clothingSlot', 'creatureType', 'effect', 'effectRange',
        'event', 'gmst', 'keybind', 'niType', 'objectType', 'physicalAttackType', 'scanCode',
        'skill', 'specialization', 'spellState', 'spellType', 'weaponType', 'weather',
    }) do
        tes3[enumName] = makeEnumTable('tes3.' .. enumName)
    end

    tes3.isModActive = function(name)
        return core.contentFiles ~= nil and core.contentFiles.has ~= nil and core.contentFiles.has(tostring(name))
    end
    tes3.getLanguage = function()
        return 'eng'
    end
    local function getGlobalValue(id)
        local ok, value = pcall(function()
            return core.obscript.getGlobalVariable(tostring(id))
        end)
        if ok and value ~= nil then
            return tonumber(value) or 0
        end
        return 0
    end
    tes3.getGlobal = function(id)
        return getGlobalValue(id)
    end
    tes3.setGlobal = function(id, value)
        local ok = pcall(function()
            core.obscript.setGlobalVariable(tostring(id), tonumber(value) or 0)
        end)
        return ok
    end
    tes3.findGlobal = function(id)
        return {
            id = tostring(id),
            value = getGlobalValue(id),
        }
    end
    tes3.messageBox = function(format, ...)
        ui.showMessage(formatMessage(format, ...), { showInDialogue = false })
    end
    tes3.playSound = function(options)
        options = type(options) == 'table' and options or { sound = options }
        local sound = options.sound or options.id
        if sound == nil then
            return false
        end
        local ok, message = pcall(ambient.playSound, tostring(sound))
        if not ok then
            noteError('tes3.playSound:' .. tostring(sound), message)
        end
        return ok
    end
    tes3.removeSound = function()
        return true
    end
    tes3.findGMST = function(id)
        local requestedId = tostring(id)
        local gmstId = requestedId:gsub('^tes3%.gmst%.', '')
        local ok, value = pcall(core.getGMST, gmstId)
        return { id = gmstId, value = ok and value or gmstId }
    end
    tes3.loadMesh = function(model)
        local normalizedModel = tostring(model):gsub('\\', '/')
        if normalizedModel:lower():match('^meshes/') == nil then
            normalizedModel = 'meshes/' .. normalizedModel
        end
        return makeSceneNode {
            model = normalizedModel,
            isSelf = true,
        }
    end
    tes3.addClothingSlot = function()
        return true
    end
    tes3.addArmorSlot = function()
        return true
    end
    tes3.getObject = makeObject
    tes3.createReference = function(options)
        options = options or {}
        local recordId = getItemId(options.object or options.item)
        if recordId == '' then
            return nil
        end
        local requestId = state.nextReferenceRequestId
        state.nextReferenceRequestId = requestId + 1
        local reference = makePendingReference(
            requestId, recordId, options, options.cell or currentCell)
        state.pendingReferences[tostring(requestId)] = reference
        core.sendGlobalEvent('MWSECompatCreateReference', {
            requestId = tostring(requestId),
            recordId = recordId,
            count = tonumber(options.count) or 1,
            position = {
                x = tonumber(reference.position.x) or 0,
                y = tonumber(reference.position.y) or 0,
                z = tonumber(reference.position.z) or 0,
            },
            rotationZ = tonumber(reference.orientation.z) or 0,
            scale = tonumber(reference.scale) or 1,
            onGround = options.onGround == true,
            playerIndex = 1,
        })
        print('MWSE compat: state=reference-create-request request='
            .. tostring(requestId) .. ' record=' .. recordId)
        return reference
    end
    tes3.getItemCount = function(options)
        options = options or {}
        local id = getItemId(options.item or options.object)
        if id == '' then
            return 0
        end
        local ok, count = pcall(function()
            return types.Actor.inventory(self):countOf(id)
        end)
        return ok and tonumber(count) or 0
    end
    tes3.addItem = function(options)
        options = options or {}
        local id = getItemId(options.item or options.object)
        local count = math.max(1, math.floor(tonumber(options.count) or 1))
        if id == '' then
            return false
        end
        animation._mwseAddItem(self, id, count)
        state.inventoryDirty = true
        return true
    end
    tes3.removeItem = function(options)
        options = options or {}
        local id = getItemId(options.item or options.object)
        local count = math.max(1, math.floor(tonumber(options.count) or 1))
        if id == '' then
            return 0
        end
        animation._mwseRemoveItem(self, id, count)
        state.inventoryDirty = true
        return count
    end
    tes3.equip = function(options)
        options = options or {}
        local item = options.item or options.object
        local id = getItemId(item)
        if id == '' then
            return false
        end
        local object = makeObject(id)
        state.pseudoEquipped[id] = tonumber(options.slot)
            or tonumber(object.slot)
            or 11
        animation._mwseEquipItem(self, id)
        state.inventoryDirty = true
        -- OpenMW applies queued inventory actions after this Lua slice.  The
        -- next-frame dispatch gives MWSE handlers the same post-equip view.
        timerApi.delayOneFrame(function()
            syncPlayerInventory(player)
            state.inventoryDirty = false
            eventApi.trigger('equipped', {
                reference = player,
                item = object,
                itemData = options.itemData or {},
            })
        end)
        return true
    end
    tes3.unequip = function(options)
        options = options or {}
        local object = makeObject(getItemId(options.item or options.object))
        state.pseudoEquipped[object.id:lower()] = nil
        syncPlayerInventory(player)
        eventApi.trigger('unequipped', {
            reference = player,
            item = object,
            itemData = options.itemData or {},
        })
        return true
    end
    tes3.getEquippedItem = function(options)
        options = options or {}
        local actor = options.actor or options.reference or player
        local equipment = actor.object and actor.object.equipment or {}
        local wantedSlot = tonumber(options.slot)
        local wantedType = options.objectType and tostring(options.objectType):lower()
        for _, stack in pairs(equipment) do
            local slotMatches = wantedSlot == nil or tonumber(stack.slot) == wantedSlot
            local stackType = tostring(stack.object.objectType or ''):lower()
            local typeMatches = wantedType == nil or stackType == wantedType
            if slotMatches and typeMatches then
                return stack
            end
        end
        return nil
    end
    tes3.getReference = function(id)
        local object = makeObject(id)
        return makeProxy('tes3.reference[' .. tostring(id) .. ']', {
            id = tostring(id),
            object = object,
            baseObject = object,
            cell = currentCell,
            data = {},
            tempData = {},
        })
    end
    tes3.getPlayerEyePosition = function()
        local position = camera.getPosition()
        return makeVector3(position.x, position.y, position.z)
    end
    tes3.getPlayerEyeVector = function()
        local direction = camera.viewportToWorldVector(util.vector2(0.5, 0.5))
        return makeVector3(direction.x, direction.y, direction.z):normalize()
    end
    tes3.getPlayerActivationDistance = function()
        local ok, value = pcall(core.getGMST, 'iMaxActivateDist')
        return ok and tonumber(value) or 192
    end
    tes3.iterateObjects = function()
        return function()
            return nil
        end
    end
    tes3.iterate = function(source)
        if type(source) == 'function' then
            local ok, produced = pcall(source)
            if not ok then
                noteError('tes3.iterate', produced)
                return tes3.iterateObjects()
            end
            if type(produced) == 'function' then
                return produced
            end
            local firstPending = true
            return function()
                if firstPending then
                    firstPending = false
                    return produced
                end
                return source()
            end
        end
        if type(source) == 'table' then
            local key
            return function()
                key = next(source, key)
                if key ~= nil then
                    return source[key]
                end
                return nil
            end
        end
        return tes3.iterateObjects()
    end
    tes3.getActiveCells = function()
        return { currentCell }
    end
    tes3.getSimulationTimestamp = function()
        return core.getSimulationTime and core.getSimulationTime() or 0
    end
    tes3.menuMode = function()
        return state.messageMenu ~= nil
    end
    tes3.getEffectMagnitude = function(options)
        options = options or {}
        local effectName = tostring(options.effect or ''):match('([^.]+)$') or ''
        local effectTypeNames = {
            fortifyHealth = 'FortifyHealth',
            fortifyMagicka = 'FortifyMagicka',
            fortifyFatigue = 'FortifyFatigue',
            drainHealth = 'DrainHealth',
            drainMagicka = 'DrainMagicka',
            drainFatigue = 'DrainFatigue',
            restoreHealth = 'RestoreHealth',
            restoreMagicka = 'RestoreMagicka',
            restoreFatigue = 'RestoreFatigue',
        }
        local typeOk, effectType = pcall(function()
            return core.magic.EFFECT_TYPE[effectTypeNames[effectName] or effectName]
        end)
        if not typeOk or effectType == nil then
            return 0
        end
        local reference = options.reference
        local openmwObject = type(reference) == 'table' and reference.openmwObject or self
        local ok, effect = pcall(function()
            return types.Actor.activeEffects(openmwObject):getEffect(effectType)
        end)
        if ok and effect ~= nil then
            return tonumber(effect.magnitude) or 0
        end
        return 0
    end
    tes3.setStatistic = function(options)
        options = options or {}
        local statistic = mobilePlayer[tostring(options.name or '')]
        if type(statistic) == 'table' then
            if options.base ~= nil then
                statistic.base = tonumber(options.base) or statistic.base
            end
            if options.current ~= nil then
                statistic.current = tonumber(options.current) or statistic.current
            end
        end
        return true
    end
    tes3.getCurrentWeather = function()
        local openmwCell = self.cell
        local ok, weather = pcall(core.weather.getCurrent, openmwCell)
        if not ok or weather == nil then
            return {
                index = 0,
                id = 'clear',
                name = 'Clear',
            }
        end
        local recordId = tostring(readRecordProperty(weather, 'recordId') or ''):lower()
        local weatherIndices = {
            clear = 0,
            cloudy = 1,
            foggy = 2,
            overcast = 3,
            rain = 4,
            thunder = 5,
            ash = 6,
            blight = 7,
            snow = 8,
            blizzard = 9,
        }
        local index = 0
        for name, value in pairs(weatherIndices) do
            if recordId:find(name, 1, true) then
                index = value
                break
            end
        end
        return {
            index = index,
            id = recordId,
            name = readRecordProperty(weather, 'name') or recordId,
            openmwWeather = weather,
        }
    end
    tes3.getPlayerCell = function()
        return currentCell
    end
    tes3.getCell = function(id)
        if tostring(id):lower() == tostring(currentCell.id):lower()
            or tostring(id):lower() == tostring(currentCell.name):lower() then
            return currentCell
        end
        return makeProxy('tes3.cell[' .. tostring(id) .. ']', {
            id = tostring(id),
            name = tostring(id),
            isExterior = false,
            isInterior = true,
            iterateReferences = function()
                return function()
                    return nil
                end
            end,
        })
    end

    setmetatable(tes3, {
        __index = function(value, key)
            local apiName = 'tes3.' .. tostring(key)
            noteUnsupported(apiName)
            local stub = function()
                return nil
            end
            rawset(value, key, stub)
            return stub
        end,
    })

    createCompatUiMenus()
    local tes3ui = {}
    tes3ui.showMessageMenu = showMessageMenu
    tes3ui.updateInventoryTiles = function() end
    tes3ui.updateContentsMenuTiles = function() end
    tes3ui.updateBarterMenuTiles = function() end
    tes3ui.forcePlayerInventoryUpdate = function() end
    tes3ui.menuMode = function()
        return state.messageMenu ~= nil
    end
    tes3ui._mwseCompatSelectMessageButton = selectMessageButton
    tes3ui._mwseCompatCloseMessageMenu = closeMessageMenu
    tes3ui._mwseCompatSetProofStage = function(text)
        state.proofStage = tostring(text or '')
        state.uiDirty = true
        return true
    end
    tes3ui._mwseCompatSetProofStagePosition = function(x, y)
        state.proofStagePosition = {
            x = tonumber(x) or state.proofStagePosition.x,
            y = tonumber(y) or state.proofStagePosition.y,
        }
        if state.stageOverlay ~= nil then
            state.stageOverlay.layout.props.position = util.vector2(
                state.proofStagePosition.x,
                state.proofStagePosition.y)
            state.stageOverlay:update()
        end
        return true
    end
    tes3ui._mwseCompatSetBridgeVisible = function(visible)
        state.bridgeVisible = visible ~= false
        if state.overlay ~= nil then
            state.overlay.layout.props.visible = state.bridgeVisible
            state.overlay:update()
        end
        return true
    end
    tes3ui.registerID = function(id)
        return tostring(id)
    end
    tes3ui.findMenu = function(id)
        return state.uiMenus[tostring(id)]
    end
    tes3ui.getPalette = function(id)
        local palettes = {
            black_color = { 0.02, 0.02, 0.02 },
            header_color = { 0.86, 0.72, 0.36 },
            negative_color = { 0.95, 0.25, 0.20 },
            positive_color = { 0.25, 0.95, 0.35 },
            normal_color = { 0.90, 0.90, 0.90 },
        }
        return palettes[tostring(id)] or { 1, 1, 1 }
    end
    setmetatable(tes3ui, {
        __index = function(value, key)
            local apiName = 'tes3ui.' .. tostring(key)
            noteUnsupported(apiName)
            local stub = function()
                return nil
            end
            rawset(value, key, stub)
            return stub
        end,
    })

    local mwse = {
        mcm = runtime.mcm,
        log = function(format, ...)
            loggerWrite('INFO', 'MWSE', format, ...)
        end,
        loadConfig = function(path, defaults)
            if state.configs[path] == nil then
                state.configs[path] = copyTable(defaults or {})
            end
            return state.configs[path]
        end,
        saveConfig = function(path, value)
            state.configs[path] = copyTable(value or {})
            return true
        end,
    }
    setmetatable(mwse, {
        __index = function(value, key)
            local apiName = 'mwse.' .. tostring(key)
            noteUnsupported(apiName)
            local stub = function()
                return nil
            end
            rawset(value, key, stub)
            return stub
        end,
    })

    local metadataByName = {}
    for _, mod in ipairs(mods) do
        metadataByName[tostring(mod.name):lower()] = mod
        metadataByName[tostring(mod.module):lower()] = mod
    end

    _G.event = eventApi
    _G.timer = timerApi
    _G.tes3 = tes3
    _G.tes3ui = tes3ui
    _G.mwse = mwse
    _G.mwscript = {
        addSpell = function(options)
            options = options or {}
            local id = getItemId(options.spell or options.object)
            local reference = options.reference
            local actor = type(reference) == 'table' and reference.openmwObject or self
            local ok, message = pcall(function()
                types.Actor.spells(actor):add(id)
            end)
            if not ok then
                noteError('mwscript.addSpell:' .. id, message)
            end
            return ok
        end,
        removeSpell = function(options)
            options = options or {}
            local id = getItemId(options.spell or options.object)
            local reference = options.reference
            local actor = type(reference) == 'table' and reference.openmwObject or self
            local ok, message = pcall(function()
                types.Actor.spells(actor):remove(id)
            end)
            if not ok then
                noteError('mwscript.removeSpell:' .. id, message)
            end
            return ok
        end,
    }
    local function moduleFromMwsePath(path)
        local normalized = tostring(path):gsub('\\', '/')
        local lowered = normalized:lower()
        for _, prefix in ipairs({
            'data files/mwse/mods/',
            'data files/mwse/lib/',
            'mwse/mods/',
            'mwse/lib/',
        }) do
            if lowered:sub(1, #prefix) == prefix then
                normalized = normalized:sub(#prefix + 1)
                break
            end
        end
        normalized = normalized:gsub('^%./', ''):gsub('%.lua$', '')
        return normalized:gsub('/', '.')
    end
    _G.dofile = function(path)
        return require(moduleFromMwsePath(path))
    end
    _G.loadfile = function(path)
        local module = moduleFromMwsePath(path)
        return function()
            return require(module)
        end
    end
    _G.include = function(module)
        local ok, result = pcall(require, module)
        if ok then
            return result
        end
        if tostring(result):find('module not found:', 1, true) then
            noteUnsupported('include:' .. tostring(module))
            return nil
        end
        error(result)
    end
    _G.json = {
        encode = simpleJsonEncode,
        decode = function()
            noteUnsupported('json.decode')
            return nil
        end,
    }
    _G.toml = {
        loadMetadata = function(name)
            local mod = metadataByName[tostring(name):lower()] or {}
            return {
                package = {
                    name = mod.name or tostring(name),
                    version = mod.version ~= '' and mod.version or 'unknown',
                    homepage = mod.homepage or '',
                    repository = mod.repository or '',
                    description = mod.description or '',
                },
                tools = { mwse = { ['lua-mod'] = mod.module or '' } },
            }
        end,
    }
    _G.lfs = {
        currentdir = function()
            return '.'
        end,
        attributes = function()
            return nil
        end,
        dir = function(path)
            local normalized = tostring(path):gsub('\\', '/'):lower()
            normalized = normalized:gsub('^data files/', '')
            if normalized:sub(-1) ~= '/' then
                normalized = normalized .. '/'
            end
            local entries = state.virtualDirectories[normalized]
            if entries == nil then
                noteUnsupported('lfs.dir:' .. tostring(path))
                entries = {}
            end
            local index = 0
            return function()
                index = index + 1
                return entries[index]
            end
        end,
    }
    _G.tes3fader = {
        new = function()
            return makeProxy('tes3fader')
        end,
    }
    _G.tes3vector3 = { new = makeVector3 }
    _G.tes3matrix33 = { new = makeMatrix33 }
    _G.niSourceTexture = {
        createFromPath = function(path)
            return makeProxy('niSourceTexture[' .. tostring(path) .. ']', { fileName = tostring(path) })
        end,
    }
    _G.mgeShadersConfig = {
        load = function(options)
            local name = type(options) == 'table' and options.name or options
            noteUnsupported('mgeShadersConfig.load:' .. tostring(name or 'unnamed'))
            return makeProxy('mgeShader[' .. tostring(name or 'unnamed') .. ']', {
                enabled = false,
                temperature = 0,
            })
        end,
    }
    state.tes3 = tes3
end

local function loadEntrypoints(mods)
    for _, mod in ipairs(mods) do
        local ok, result = pcall(require, mod.module)
        mod.loaded = ok
        mod.result = result
        if ok then
            state.loaded = state.loaded + 1
            print('MWSE compat: state=entrypoint-loaded module=' .. mod.module
                .. ' name=' .. tostring(mod.name) .. ' version=' .. tostring(mod.version))
        else
            noteError('entrypoint:' .. mod.module, result)
        end
    end
end

local function pageSummary()
    if #state.mcmPages == 0 then
        return '(no page declarations captured)'
    end
    local pages = {}
    for index = 1, math.min(#state.mcmPages, 6) do
        pages[#pages + 1] = state.mcmPages[index]
    end
    return table.concat(pages, ' | ')
end

local function primaryMod()
    for _, mod in ipairs(state.mods) do
        if tostring(mod.name):lower() == 'ashfall' or tostring(mod.module):lower():find('ashfall') then
            return mod
        end
    end
    return state.mods[1] or {}
end

local function discoverSurvivalData()
    local root = state.player and state.player.data
    if type(root) ~= 'table' then
        return nil, nil
    end
    local bestName
    local best
    local bestScore = 0
    for name, candidate in pairs(root) do
        if type(candidate) == 'table' then
            local score = 0
            for _, key in ipairs({ 'hunger', 'thirst', 'tiredness', 'temp', 'wetness' }) do
                if type(candidate[key]) == 'number' then
                    score = score + 1
                end
            end
            if score > bestScore then
                bestName = tostring(name)
                best = candidate
                bestScore = score
            end
        end
    end
    if bestScore < 3 then
        return nil, nil
    end
    return bestName, best
end

local function conditionLabel(data, key)
    local states = type(data.currentStates) == 'table' and data.currentStates or {}
    local label = tostring(states[key] or '')
    label = label:gsub('(%l)(%u)', '%1 %2')
    return label:upper()
end

local function proofText()
    local mod = primaryMod()
    local namespace, survival = discoverSurvivalData()
    local header = baseString.format(
        'MWSE-LUA COMPAT | %s %s | UNMODIFIED MOD TREE',
        tostring(mod.name or mod.module or 'MWSE mod'),
        tostring(mod.version or 'unknown'))
    local bridge = baseString.format(
        '%d/%d ENTRYPOINTS | %d UI ELEMENTS | %d WORLD REFS | %d SCENE ATTACHMENTS',
        state.loaded,
        #state.mods,
        state.uiElementCount,
        state.referenceMappings,
        state.sceneAttachmentCount)
    if survival ~= nil then
        return baseString.format(
            '%s\n%s\nHUNGER %d%% (%s) | THIRST %d%% (%s) | TIRED %d%% (%s)\n'
                .. 'TEMP %d (%s) | WETNESS %d%% (%s) | LIVE %s PLAYER DATA',
            header,
            bridge,
            tonumber(survival.hunger) or 0,
            conditionLabel(survival, 'hunger'),
            tonumber(survival.thirst) or 0,
            conditionLabel(survival, 'thirst'),
            tonumber(survival.tiredness) or 0,
            conditionLabel(survival, 'tiredness'),
            tonumber(survival.temp) or 0,
            conditionLabel(survival, 'temp'),
            tonumber(survival.wetness) or 0,
            conditionLabel(survival, 'wetness'),
            tostring(namespace):upper())
    end
    return header .. '\n' .. bridge
end

local function needsText()
    local _, survival = discoverSurvivalData()
    if survival == nil then
        return 'ASHFALL SURVIVAL\nWAITING FOR LIVE NEEDS DATA'
    end
    return baseString.format(
        'ASHFALL SURVIVAL\n'
            .. '[FOOD]  %3d%%  %s\n'
            .. '[WATER] %3d%%  %s\n'
            .. '[REST]  %3d%%  %s\n'
            .. '[TEMP]  %3d    %s\n'
            .. '[WET]   %3d%%  %s',
        tonumber(survival.hunger) or 0,
        conditionLabel(survival, 'hunger'),
        tonumber(survival.thirst) or 0,
        conditionLabel(survival, 'thirst'),
        tonumber(survival.tiredness) or 0,
        conditionLabel(survival, 'tiredness'),
        tonumber(survival.temp) or 0,
        conditionLabel(survival, 'temp'),
        tonumber(survival.wetness) or 0,
        conditionLabel(survival, 'wetness'))
end

local function updateOverlay()
    local text = proofText()
    if state.overlay == nil then
        state.overlay = ui.create {
            layer = 'HUD',
            type = ui.TYPE.Text,
            props = {
                position = util.vector2(28, 28),
                text = text,
                textSize = 16,
                textColor = util.color.rgb(0.96, 0.78, 0.20),
                textShadow = true,
                textShadowColor = util.color.rgb(0, 0, 0),
                multiline = true,
                visible = state.bridgeVisible,
            },
        }
        state.lastOverlayText = text
    elseif state.lastOverlayText ~= text then
        state.overlay.layout.props.text = text
        state.overlay.layout.props.textColor = state.initializedOk
            and util.color.rgb(0.25, 1.0, 0.35)
            or util.color.rgb(0.96, 0.78, 0.20)
        state.overlay:update()
        state.lastOverlayText = text
    end

    local currentNeedsText = needsText()
    if state.needsBackdrop == nil then
        state.needsBackdrop = ui.create {
            layer = 'HUD',
            type = ui.TYPE.Image,
            props = {
                position = util.vector2(942, 18),
                size = util.vector2(420, 142),
                resource = ui.texture { path = 'white' },
                color = util.color.rgb(0.015, 0.02, 0.025),
                alpha = 0.68,
                visible = true,
            },
        }
    end
    if state.needsOverlay == nil then
        state.needsOverlay = ui.create {
            layer = 'HUD',
            type = ui.TYPE.Text,
            props = {
                position = util.vector2(960, 28),
                text = currentNeedsText,
                textSize = 18,
                textColor = util.color.rgb(0.42, 0.92, 1.0),
                textShadow = true,
                textShadowColor = util.color.rgb(0, 0, 0),
                multiline = true,
                visible = true,
            },
        }
        state.lastNeedsText = currentNeedsText
    elseif state.lastNeedsText ~= currentNeedsText then
        state.needsOverlay.layout.props.text = currentNeedsText
        state.needsOverlay:update()
        state.lastNeedsText = currentNeedsText
    end

    local currentStage = tostring(state.proofStage or '')
    if state.stageOverlay == nil then
        state.stageOverlay = ui.create {
            layer = 'Windows',
            type = ui.TYPE.Text,
            props = {
                position = util.vector2(
                    state.proofStagePosition.x,
                    state.proofStagePosition.y),
                text = currentStage,
                textSize = 23,
                textColor = util.color.rgb(1.0, 0.82, 0.28),
                textShadow = true,
                textShadowColor = util.color.rgb(0, 0, 0),
                multiline = true,
                visible = currentStage ~= '',
            },
        }
        state.lastProofStage = currentStage
    elseif state.lastProofStage ~= currentStage then
        state.stageOverlay.layout.props.text = currentStage
        state.stageOverlay.layout.props.visible = currentStage ~= ''
        state.stageOverlay:update()
        state.lastProofStage = currentStage
    end
end

function runtime.bootstrap(mods, virtualDirectories)
    state = {
        mods = mods,
        virtualDirectories = virtualDirectories or {},
        loaded = 0,
        handlers = {},
        registrations = 0,
        unsupported = {},
        errors = {},
        errorCounts = {},
        errorSerial = 0,
        errorOverflow = 0,
        errorUniqueLimit = 128,
        errorPrinted = 0,
        errorPrintLimit = 256,
        errorPrintLimitReported = false,
        lastError = nil,
        initializationError = nil,
        configs = {},
        mcmTemplates = {},
        mcmPages = {},
        mcmControls = 0,
        mcmRegistered = 0,
        recordMappings = 0,
        recordMappingIds = {},
        objectCache = {},
        pseudoEquipped = {},
        inventoryDirty = false,
        inventorySyncElapsed = 0,
        player = nil,
        tes3 = nil,
        currentCell = nil,
        pendingReferences = {},
        nextReferenceRequestId = 1,
        referenceMappings = 0,
        referenceCache = {},
        sceneAttachments = {},
        sceneAttachmentCount = 0,
        nextSceneAttachmentSequence = 1,
        uiMenus = {},
        uiById = {},
        uiElementCount = 0,
        uiDirty = false,
        messageMenu = nil,
        messageMenuOverlay = nil,
        initializedOk = false,
        bridgeVisible = true,
        overlay = nil,
        lastOverlayText = nil,
        needsOverlay = nil,
        needsBackdrop = nil,
        lastNeedsText = nil,
        proofStage = 'INITIALIZING ASHFALL COMPATIBILITY',
        proofStagePosition = { x = 365, y = 112 },
        stageOverlay = nil,
        lastProofStage = nil,
        overlayElapsed = 0,
    }
    runtime.state = state
    installGlobals(mods)
    loadEntrypoints(mods)

    local function onInit()
        eventApi.trigger('modConfigReady', {})
        local errorsBeforeInitialized = #state.errors
        local _, initializedOk = eventApi.trigger('initialized', {})
        state.initializedOk = initializedOk
        if not initializedOk then
            state.initializationError = state.errors[errorsBeforeInitialized + 1] or state.lastError
        end
        eventApi.trigger('load', {})
        eventApi.trigger('loaded', {})
        for _, menuId in ipairs({ 'MenuInventory', 'MenuStat', 'MenuMulti' }) do
            local menu = state.uiMenus[menuId]
            if menuId == 'MenuInventory' then
                eventApi.trigger('uiCreated', {
                    element = menu,
                    newlyCreated = true,
                }, { filter = menuId })
            end
            local _, uiActivatedOk = eventApi.trigger('uiActivated', {
                element = menu,
                newlyCreated = true,
            }, { filter = menuId })
            eventApi.trigger('uiRefreshed', {
                element = menu,
                newlyCreated = true,
            }, { filter = menuId })
            print('MWSE compat: state=ui-surface-ready menu=' .. menuId
                .. ' elements=' .. state.uiElementCount
                .. ' callbacks=' .. tostring(uiActivatedOk))
        end
        updateOverlay()
        print('MWSE compat: state=host-ready loaded=' .. state.loaded .. '/' .. #state.mods
            .. ' registrations=' .. state.registrations
            .. ' mcmRegistered=' .. state.mcmRegistered
            .. ' records=' .. state.recordMappings
            .. ' references=' .. state.referenceMappings
            .. ' unsupported=' .. countKeys(state.unsupported)
            .. ' errors=' .. #state.errors)
    end

    local function onFrame(dt)
        dt = math.clamp(tonumber(dt) or 0, 0, 0.25)
        if state.player ~= nil then
            local position = self.position
            state.player.position = makeVector3(position.x, position.y, position.z)
            local rotationOk, yaw = pcall(function()
                return self.rotation:getYaw()
            end)
            if rotationOk then
                state.player.orientation = makeVector3(0, 0, yaw)
            end
        end
        state.inventorySyncElapsed = state.inventorySyncElapsed + dt
        if state.player ~= nil
            and (state.inventoryDirty or state.inventorySyncElapsed >= 0.25)
        then
            state.inventorySyncElapsed = 0
            syncPlayerInventory(state.player)
            state.inventoryDirty = false
        end
        if state.initializedOk then
            updateTimers(dt)
            local timestamp = core.getSimulationTime and core.getSimulationTime() or 0
            local menuMode = state.messageMenu ~= nil
            eventApi.trigger('enterFrame', {
                delta = dt,
                timestamp = timestamp,
                menuMode = menuMode,
            })
            eventApi.trigger('simulate', {
                delta = dt,
                timestamp = timestamp,
                menuMode = menuMode,
            })
        end
        state.overlayElapsed = state.overlayElapsed + dt
        if state.overlayElapsed >= 0.25 then
            state.overlayElapsed = 0
            updateOverlay()
        end
    end

    local function onReferenceCreated(data)
        data = data or {}
        local requestId = tostring(data.requestId or '')
        local reference = state.pendingReferences[requestId]
        local object = data.object
        if reference == nil or object == nil then
            noteError('reference-created', 'unknown request or missing object: ' .. requestId)
            return
        end
        state.pendingReferences[requestId] = nil
        local targetKey = tostring(readRecordProperty(object, 'id') or reference.id)
        local position = readRecordProperty(object, 'position')
        reference.id = targetKey
        reference.openmwObject = object
        reference.cell = state.currentCell
        if position ~= nil then
            reference.position = makeVector3(position.x, position.y, position.z)
        end
        reference.sceneNode = makeSceneNode {
            openmwObject = object,
            targetKey = targetKey,
        }
        state.referenceCache[targetKey] = reference
        state.referenceMappings = state.referenceMappings + 1
        eventApi.trigger('referenceActivated', { reference = reference })
        eventApi.trigger('Ashfall:UpdateAttachNodes', { reference = reference })
        print('MWSE compat: state=reference-ready request=' .. requestId
            .. ' id=' .. targetKey
            .. ' record=' .. tostring(reference.object.id))
    end

    return {
        eventHandlers = {
            MWSECompatReferenceCreated = onReferenceCreated,
        },
        engineHandlers = {
            onInit = onInit,
            onFrame = onFrame,
            onLoad = function()
                eventApi.trigger('load', {})
                eventApi.trigger('loaded', {})
            end,
            onSave = function()
                return {
                    loaded = state.loaded,
                    registrations = state.registrations,
                    unsupported = countKeys(state.unsupported),
                    errors = #state.errors,
                }
            end,
        },
    }
end

return runtime
