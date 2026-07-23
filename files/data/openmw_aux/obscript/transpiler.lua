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
    local out = name:gsub('[^%w_]', '_')
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
        lines = {},
        depth = 0,
    }, Emitter)
    for _, v in ipairs(scriptAst.variables) do
        self.locals[v.name:lower()] = true
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
            elseif s.kind == 'If' then
                for _, c in ipairs(s.clauses) do
                    walk(c.body)
                end
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
    if k == 'BinOp' then
        local o = n.op
        local left, right = self:expr(n.left), self:expr(n.right)
        if o == '&&' or o == '||' then
            return ('(obs.b(%s) %s obs.b(%s))'):format(left, OPMAP[o], right)
        end
        return ('(%s %s %s)'):format(left, OPMAP[o], right)
    end
    if k == 'Member' then
        return ('obs.mv(%s, %s)'):format(self:exprRef(n.base), luaString(memberName(n.member)))
    end
    if k == 'Call' then
        return self:call(n)
    end
    error('unhandled expr node ' .. tostring(k))
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
    for i, a in ipairs(args) do
        parts[i] = self:arg(a)
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
    -- bare names in argument position are editor IDs / actor values / anim
    -- groups far more often than variables; pass strings, but prefer locals.
    if n.kind == 'Name' then
        local name = n.value
        if self.locals[name:lower()] then
            return 'S.' .. safeIdent(name)
        end
        return luaString(name)
    end
    return self:expr(n)
end

-- statements

function Emitter:stmt(n)
    local k = n.kind
    if k == 'VarDecl' or k == 'JunkLine' or k == 'StrayKeyword' then
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

function Emitter:emit()
    self:collectBlockLocals()
    local name = self.ast.name or 'anonymous'
    self:out('-- transpiled from ObScript: ' .. name)
    self:out("local obs = require('openmw_aux.obscript.runtime')")
    self:out(('local S = obs.locals(%s)'):format(luaString(name)))
    self:out()
    for _, block in ipairs(self.ast.blocks) do
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
    if #self.ast.stray > 0 then
        self:out("obs.on('__stray', function()")
        self.depth = self.depth + 1
        for _, s in ipairs(self.ast.stray) do
            self:stmt(s)
        end
        self.depth = self.depth - 1
        self:out('end)')
    end
    self:out('return obs.makeLocalScript()')
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

return transpiler
