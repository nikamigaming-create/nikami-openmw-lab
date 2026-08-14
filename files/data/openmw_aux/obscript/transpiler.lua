---
-- `openmw_aux.obscript.transpiler` translates an ObScript AST (produced by
-- `openmw_aux.obscript.parser`) into Lua source. Script locals become fields
-- on a persistent table `S`; each Begin-block becomes a handler registered by
-- event name; engine commands become calls into the `obs` runtime:
--
--  * `obs.f(name, ...)` — free function call: SetStage, ShowMessage, ...
--  * `obs.m(base, name, ...)` — member call: player.AddItem, Ref.Say, ...
--  * `obs.v(name)` — value of a bare name (global/editor id/0-arg function)
--  * `obs.mv(base, name)` — cross-script variable read: Quest.var
--  * `obs.setv(name, value)` — assignment to a non-local name
--  * `obs.msetv(base, name, value)` — assignment to Quest.var / Ref.var
--  * `obs.b(x)` — ObScript truthiness (nonzero) as a Lua boolean
--
-- Unknown functions can be stubbed in the runtime, so every transpiled
-- script loads regardless of implementation coverage.
-- Implementation can be found in `resources/vfs/openmw_aux/obscript/transpiler.lua`.
-- @module transpiler
-- @context global|menu|local
-- @usage
-- local parser = require('openmw_aux.obscript.parser')
-- local transpiler = require('openmw_aux.obscript.transpiler')
-- local luaSource = transpiler.transpile(parser.parse(text))

local transpiler = {}

local OPMAP = {
    ['&&'] = 'and', ['||'] = 'or', ['=='] = '==', ['!='] = '~=',
    ['<'] = '<', ['>'] = '>', ['<='] = '<=', ['>='] = '>=',
    ['+'] = '+', ['-'] = '-', ['*'] = '*', ['/'] = '/', ['%'] = '%',
}

local COMPARISON_OPS = {
    ['=='] = true, ['!='] = true, ['<'] = true, ['>'] = true,
    ['<='] = true, ['>='] = true,
}

local OUTPUT_ARGUMENTS = {
    -- JohnnyGuitar: WorldToScreen fX fY fZ dX dY dZ mode target
    worldtoscreen = { [1] = true, [2] = true, [3] = true },
}

local function fmtFloat(v)
    -- deterministic float formatting, kept in sync with the reference
    -- implementation ('%d.0' for integral values, '%.14g' otherwise)
    if v == math.floor(v) and math.abs(v) < 2 ^ 53 then
        return ('%d.0'):format(v)
    end
    return ('%.14g'):format(v)
end

local function luaString(s)
    return '"' .. s:gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
end

local function safeIdent(name)
    -- ObScript/xNVSE identifiers are case-insensitive. Canonicalizing every
    -- generated local to lowercase keeps direct script access, Quest.var
    -- access, save/load state, and compatibility-interface mutation on the
    -- same storage slot.
    local out = name:lower():gsub('[^%w_]', '_')
    if out:sub(1, 1):match('%d') then
        out = '_' .. out
    end
    return out
end

local function memberName(n)
    if n.kind == 'Name' then
        return n.value
    end
    if n.kind == 'Int' then
        return ('%d'):format(n.value)
    end
    if n.kind == 'Member' then -- chained a.b.c - flatten rightmost
        return memberName(n.member)
    end
    return tostring(n.value == nil and '?' or n.value)
end

local Emitter = {}
Emitter.__index = Emitter

function Emitter.new(scriptAst)
    local self = setmetatable({
        ast = scriptAst,
        locals = {},
        localTypes = {},
        lines = {},
        depth = 0,
        loopStack = {},
        lambdaIndex = 0,
    }, Emitter)
    for _, v in ipairs(scriptAst.variables) do
        self.locals[v.name:lower()] = true
        self.localTypes[v.name:lower()] = v.type
    end
    return self
end

-- helpers

function Emitter:out(text)
    self.lines[#self.lines + 1] = ('    '):rep(self.depth) .. (text or '')
end

function Emitter:collectBlockLocals()
    -- mid-block VarDecls also become script locals
    local function walk(stmts)
        for _, s in ipairs(stmts) do
            if s.kind == 'VarDecl' then
                self.locals[s.name:lower()] = true
                self.localTypes[s.name:lower()] = s.type
            elseif s.kind == 'If' then
                for _, c in ipairs(s.clauses) do
                    walk(c.body)
                end
            elseif s.kind == 'While' then
                walk(s.body)
            end
        end
    end
    for _, b in ipairs(self.ast.blocks) do
        walk(b.body)
    end
    walk(self.ast.stray)
end

-- expressions

function Emitter:expr(n)
    local k = n.kind
    if k == 'Int' then
        return ('%d'):format(n.value)
    end
    if k == 'Float' then
        return fmtFloat(n.value)
    end
    if k == 'Str' then
        return luaString(n.value)
    end
    if k == 'Name' then
        local name = n.value
        if self.locals[name:lower()] then
            return 'S.' .. safeIdent(name)
        end
        return ('obs.v(%s)'):format(luaString(name))
    end
    if k == 'Missing' then
        -- missing comparison operand (`x >= 0 && <10`); vanilla evaluates
        -- the absent side as 0
        return '0'
    end
    if k == 'Neg' then
        return ('-(%s)'):format(self:expr(n.operand))
    end
    if k == 'Not' then
        return ('obs.boolnum(not obs.b(%s))'):format(self:expr(n.operand))
    end
    if k == 'StringCoerce' then
        return ('obs.str(%s)'):format(self:expr(n.operand))
    end
    if k == 'Deref' then
        return ('obs.deref(%s)'):format(self:expr(n.operand))
    end
    if k == 'BinOp' then
        local o = n.op
        local left, right = self:expr(n.left), self:expr(n.right)
        if o == '&&' or o == '||' then
            -- ObScript logical expressions are numeric 0/1 values. Keep the
            -- Lua `and`/`or` inside boolnum so the right side still
            -- short-circuits before conversion.
            return ('obs.boolnum(obs.b(%s) %s obs.b(%s))'):format(left, OPMAP[o], right)
        end
        if o == '==' then
            return ('obs.boolnum(obs.eq(%s, %s))'):format(left, right)
        end
        if o == '!=' then
            return ('obs.boolnum(not obs.eq(%s, %s))'):format(left, right)
        end
        if COMPARISON_OPS[o] then
            return ('obs.boolnum((%s %s %s))'):format(left, OPMAP[o], right)
        end
        if o == '&' or o == '|' then
            return ('obs.bit(%s, %s, %s)'):format(luaString(o), left, right)
        end
        if o == '+' and (self:mightString(n.left) or self:mightString(n.right)) then
            return ('obs.add(%s, %s)'):format(left, right)
        end
        return ('(%s %s %s)'):format(left, OPMAP[o], right)
    end
    if k == 'Member' then
        return ('obs.mv(%s, %s)'):format(self:exprRef(n.base), luaString(memberName(n.member)))
    end
    if k == 'Call' then
        return self:call(n)
    end
    if k == 'Index' then
        return ('obs.index(%s, %s)'):format(self:expr(n.base), self:expr(n.index))
    end
    if k == 'Pair' then
        return ('obs.pair(%s, %s)'):format(self:expr(n.key), self:expr(n.value))
    end
    if k == 'AssignExpr' then
        return self:assignmentExpr(n)
    end
    if k == 'Lambda' then
        return self:lambdaExpr(n)
    end
    error('unhandled expr node ' .. tostring(k))
end

function Emitter:mightString(n)
    if n.kind == 'Str' or n.kind == 'StringCoerce' then
        return true
    end
    if n.kind == 'Name' then
        return self.localTypes[n.value:lower()] == 'string_var'
    end
    if n.kind == 'BinOp' and n.op == '+' then
        return self:mightString(n.left) or self:mightString(n.right)
    end
    return false
end

function Emitter:exprRef(n)
    -- base of a member access: a local, or a name handle
    if n.kind == 'Name' then
        local name = n.value
        if self.locals[name:lower()] then
            return 'S.' .. safeIdent(name)
        end
        return luaString(name)
    end
    return self:expr(n)
end

function Emitter:call(n)
    local callee, args = n.callee, n.args
    local parts = {}
    local commandName
    if callee.kind == 'Name' then
        commandName = callee.value:lower()
    elseif callee.kind == 'Member' then
        commandName = memberName(callee.member):lower()
    end
    local outputArguments = commandName and OUTPUT_ARGUMENTS[commandName]
    for i, a in ipairs(args) do
        if outputArguments and outputArguments[i] and a.kind == 'Name'
                and self.locals[a.value:lower()] then
            parts[i] = ('obs.out(S, %s)'):format(
                luaString(safeIdent(a.value)))
        else
            parts[i] = self:arg(a)
        end
    end
    local luaArgs = table.concat(parts, ', ')
    local tail = luaArgs ~= '' and (', ' .. luaArgs) or ''
    if callee.kind == 'Member' then
        local base = self:exprRef(callee.base)
        local name = luaString(memberName(callee.member))
        return ('obs.m(%s, %s%s)'):format(base, name, tail)
    end
    if callee.kind == 'Name' then
        return ('obs.f(%s%s)'):format(luaString(callee.value), tail)
    end
    -- call on an arbitrary expression (rare; parenthesised)
    return ('obs.fx(%s%s)'):format(self:expr(callee), tail)
end

function Emitter:arg(n)
    -- Bare names in argument position can be script locals, globals, editor
    -- IDs, actor values, or animation groups. Defer nonlocals to the runtime:
    -- it substitutes a numeric global when one exists and otherwise preserves
    -- the name token for bindings that consume an editor ID or enum name.
    if n.kind == 'Name' then
        local name = n.value
        if self.locals[name:lower()] then
            return 'S.' .. safeIdent(name)
        end
        return ('obs.arg(%s)'):format(luaString(name))
    end
    return self:expr(n)
end

function Emitter:assignmentExpr(n)
    local target = n.target
    local value = self:expr(n.value)
    local op = n.op
    if op ~= '=' and op ~= ':=' then
        local current = self:expr(target)
        if op == '+=' then
            value = ('obs.add(%s, %s)'):format(current, value)
        elseif op == '-=' then
            value = ('(%s - %s)'):format(current, value)
        elseif op == '*=' then
            value = ('(%s * %s)'):format(current, value)
        elseif op == '/=' then
            value = ('(%s / %s)'):format(current, value)
        elseif op == '%=' then
            value = ('(%s %% %s)'):format(current, value)
        end
    end

    if target.kind == 'Name' then
        local name = target.value
        if self.locals[name:lower()] then
            return ('obs.setlocal(S, %s, %s)'):format(luaString(safeIdent(name)), value)
        end
        return ('obs.setvexpr(%s, %s)'):format(luaString(name), value)
    end
    if target.kind == 'Member' then
        return ('obs.msetvexpr(%s, %s, %s)'):format(
            self:exprRef(target.base), luaString(memberName(target.member)), value)
    end
    if target.kind == 'Index' then
        return ('obs.setindex(%s, %s, %s)'):format(
            self:expr(target.base), self:expr(target.index), value)
    end
    error('unhandled assignment-expression target ' .. tostring(target.kind))
end

function Emitter:lambdaExpr(n)
    self.lambdaIndex = self.lambdaIndex + 1
    local label = (self.ast.name or 'anonymous') .. '#lambda' .. self.lambdaIndex
    local luaParameters = {}
    for index, parameter in ipairs(n.parameters or {}) do
        self.locals[parameter:lower()] = true
        luaParameters[index] = '__obsArg' .. index
    end

    local previousLines, previousDepth = self.lines, self.depth
    self.lines, self.depth = {}, 0
    self:out(('obs.lambda(%s, function(%s)'):format(
        luaString(label), table.concat(luaParameters, ', ')))
    self.depth = self.depth + 1
    for index, parameter in ipairs(n.parameters or {}) do
        self:out(('S.%s = __obsArg%d'):format(safeIdent(parameter), index))
    end
    if n.expression ~= nil then
        self:out('return ' .. self:expr(n.expression))
    else
        for _, statement in ipairs(n.body or {}) do
            self:stmt(statement)
        end
    end
    self.depth = self.depth - 1
    self:out('end)')
    local result = table.concat(self.lines, '\n')
    self.lines, self.depth = previousLines, previousDepth
    return result
end

-- statements

function Emitter:stmt(n)
    local k = n.kind
    if k == 'VarDecl' or k == 'JunkLine' or k == 'IgnoredLine' or k == 'StrayKeyword' then
        return -- declarations hoisted; junk dropped
    end
    if k == 'Return' then
        self:out('do return end')
        return
    end
    if k == 'Set' then
        local target = n.target
        local value = self:expr(n.value)
        if target.kind == 'Name' then
            local name = target.value
            if self.locals[name:lower()] then
                self:out(('S.%s = %s'):format(safeIdent(name), value))
            else
                self:out(('obs.setv(%s, %s)'):format(luaString(name), value))
            end
        elseif target.kind == 'Member' then
            local base = self:exprRef(target.base)
            local member = luaString(memberName(target.member))
            self:out(('obs.msetv(%s, %s, %s)'):format(base, member, value))
        elseif target.kind == 'Index' then
            self:out(('obs.setindex(%s, %s, %s)'):format(
                self:expr(target.base), self:expr(target.index), value))
        else
            error('unhandled set target')
        end
        return
    end
    if k == 'If' then
        local first = true
        for _, clause in ipairs(n.clauses) do
            if clause.cond == nil then
                self:out('else')
            else
                local kw = first and 'if' or 'elseif'
                self:out(('%s obs.b(%s) then'):format(kw, self:expr(clause.cond)))
            end
            first = false
            self.depth = self.depth + 1
            if #clause.body == 0 then
                self:out('-- empty')
            end
            for _, s in ipairs(clause.body) do
                self:stmt(s)
            end
            self.depth = self.depth - 1
        end
        self:out('end')
        return
    end
    if k == 'While' then
        local loopId = #self.loopStack + 1
        local breakFlag = '__obsBreak' .. loopId
        self:out(('while obs.b(%s) do'):format(self:expr(n.cond)))
        self.depth = self.depth + 1
        self:out(('local %s = false'):format(breakFlag))
        self:out('repeat')
        self.depth = self.depth + 1
        self.loopStack[#self.loopStack + 1] = breakFlag
        for _, statement in ipairs(n.body) do
            self:stmt(statement)
        end
        self.loopStack[#self.loopStack] = nil
        self.depth = self.depth - 1
        self:out('until true')
        self:out(('if %s then break end'):format(breakFlag))
        self.depth = self.depth - 1
        self:out('end')
        return
    end
    if k == 'Continue' then
        if #self.loopStack > 0 then
            self:out('break')
        end
        return
    end
    if k == 'Break' then
        local breakFlag = self.loopStack[#self.loopStack]
        if breakFlag ~= nil then
            self:out(('%s = true; break'):format(breakFlag))
        end
        return
    end
    if k == 'ExprStatement' then
        local e = n.expr
        if e.kind == 'Call' then
            self:out(self:call(e))
        elseif e.kind == 'Member' then
            -- zero-arg member command in statement position: Ref.Enable
            local base = self:exprRef(e.base)
            self:out(('obs.m(%s, %s)'):format(base, luaString(memberName(e.member))))
        elseif e.kind == 'Name' and not self.locals[e.value:lower()] then
            -- zero-arg command in statement position: Disable, evp, ...
            self:out(('obs.f(%s)'):format(luaString(e.value)))
        else
            -- genuinely value-only line (comparison used as statement, etc.)
            self:out(('obs.discard(%s)'):format(self:expr(e)))
        end
        return
    end
    error('unhandled stmt node ' .. tostring(k))
end

-- top level

function Emitter:emit(includeFooter)
    self:collectBlockLocals()
    local name = self.ast.name or 'anonymous'
    self:out('-- transpiled from ObScript: ' .. name)
    self:out("local obs = require('openmw_aux.obscript.runtime')")
    self:out(('local S = obs.locals(%s)'):format(luaString(name)))
    self:out()
    for _, block in ipairs(self.ast.blocks) do
        if block.event:lower() == 'function' then
            local functionArgs = {}
            for index, parameter in ipairs(block.parameters or {}) do
                self.locals[parameter:lower()] = true
                functionArgs[index] = '__obsArg' .. index
            end
            self:out(('obs.udf(%s, function(%s)'):format(
                luaString(name), table.concat(functionArgs, ', ')))
            self.depth = self.depth + 1
            for index, parameter in ipairs(block.parameters or {}) do
                self:out(('S.%s = __obsArg%d'):format(safeIdent(parameter), index))
            end
            if #block.body == 0 then
                self:out('-- empty')
            end
            for _, s in ipairs(block.body) do
                self:stmt(s)
            end
            self.depth = self.depth - 1
            self:out('end)')
            self:out()
        else
            local parts = {}
            for i, a in ipairs(block.args) do
                parts[i] = self:arg(a)
            end
            local args = table.concat(parts, ', ')
            self:out(('obs.on(%s, function()'):format(luaString(block.event)))
            self.depth = self.depth + 1
            if #block.body == 0 then
                self:out('-- empty')
            end
            for _, s in ipairs(block.body) do
                self:stmt(s)
            end
            self.depth = self.depth - 1
            self:out(('end%s)'):format(args ~= '' and (', ' .. args) or ''))
            self:out()
        end
    end
    if #self.ast.stray > 0 then
        self:out("obs.on('__stray', function()")
        self.depth = self.depth + 1
        for _, s in ipairs(self.ast.stray) do
            self:stmt(s)
        end
        self.depth = self.depth - 1
        self:out('end)')
    end
    if includeFooter ~= false then
        self:out('return obs.makeLocalScript()')
    end
    return table.concat(self.lines, '\n') .. '\n'
end

---
-- Translate a `Script` AST node into Lua source text.
-- @function [parent=#transpiler] transpile
-- @param #Node ast `Script` node from @{parser#parser.parse}
-- @return #string Lua source
function transpiler.transpile(ast)
    return Emitter.new(ast):emit()
end

---
-- Translate a `Script` AST into registration-only Lua source. This form is
-- used by the ESM4 quest host to load every quest and UDF from one player
-- sandbox before returning a single shared engine-handler table.
-- @function [parent=#transpiler] transpileRegistration
-- @param #Node ast `Script` node from @{parser#parser.parse}
-- @return #string Lua source without a `return` footer
function transpiler.transpileRegistration(ast)
    return Emitter.new(ast):emit(false)
end

return transpiler
