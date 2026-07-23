---
-- `openmw_aux.parser` parses ObScript (Fallout 3 / New Vegas script) source
-- text into an abstract syntax tree. Together with `openmw_aux.lexer` it
-- forms the front end of the ObScript-to-Lua transpiler.
-- Implementation can be found in `resources/vfs/openmw_aux/obscript/parser.lua`.
-- @module parser
-- @context global|menu|local
-- @usage
-- local parser = require('openmw_aux.parser')
-- local ast = parser.parse('scn MyScript\nbegin GameMode\nend')
-- print(ast.name) -- 'MyScript'

local lexer = require('openmw_aux.lexer')

local parser = {}

---
-- An AST node. Every node is a table with a `kind` field naming the node
-- type; the remaining fields depend on the kind:
--
--  * `Script` — `name` (string or nil), `variables`, `blocks`, `stray` (lists)
--  * `VarDecl` — `type` ('short'|'int'|'long'|'float'|'ref'), `name`
--  * `Block` — `event` (string), `args` (list), `body` (list)
--  * `Set` — `target`, `value`
--  * `If` — `clauses` (list of `Clause`)
--  * `Clause` — `cond` (expression or nil for `else`), `body` (list)
--  * `Return` — no fields
--  * `ExprStatement` — `expr`
--  * `StrayKeyword` — `keyword` (unmatched `endif`/`elseif`/`else`)
--  * `JunkLine` — decorative line the vanilla compiler ignores
--  * `BinOp` — `op`, `left`, `right`
--  * `Neg` — `operand`
--  * `Call` — `callee`, `args` (list)
--  * `Member` — `base`, `member`
--  * `Name` — `value`; `Int`/`Float` — `value` (number); `Str` — `value`
-- @type Node
-- @field #string kind Node type

local KEYWORDS = {
    scn = true, scriptname = true, begin = true, ['end'] = true,
    ['if'] = true, ['elseif'] = true, ['else'] = true, endif = true,
    set = true, to = true, ['return'] = true,
    short = true, int = true, long = true, float = true, ref = true,
}

---
-- Lexical rules describing ObScript tokens, in the format accepted by @{lexer#lexer.tokenize}.
-- Comments (`;`), whitespace and decoration characters are skipped;
-- keywords are recognized case-insensitively and emitted as KEYWORD tokens with a lowercased value;
-- digit-led identifiers (`12GaCoinShot`) are emitted as NAME tokens.
-- @field [parent=#parser] #list<#Rule> rules
parser.rules = {
    { name = 'COMMENT', pattern = ';[^\n]*', skip = true },
    { name = 'NEWLINE', pattern = '\r?\n',
      map = function(_) return 'NEWLINE', '\n' end },
    { name = 'WS', pattern = '[ \t]+', skip = true },
    { name = 'FLOAT', pattern = '%d+%.%d*', notFollowedBy = '[%a_]' },
    { name = 'FLOAT', pattern = '%.%d+', notFollowedBy = '[%a_]' },
    { name = 'NAME', pattern = '%d+[%a_][%w_]*' }, -- digit-led identifier
    { name = 'INT', pattern = '%d+' },
    { name = 'STRING', pattern = '"[^"\n]*"' },
    -- multi-character operators must precede the single-character ones and
    -- the JUNK rule (so `!=` is an operator while a lone `!` is decoration)
    { name = 'OP', pattern = '&&' },
    { name = 'OP', pattern = '||' },
    { name = 'OP', pattern = '==' },
    { name = 'OP', pattern = '!=' },
    { name = 'OP', pattern = '<=' },
    { name = 'OP', pattern = '>=' },
    { name = 'OP', pattern = ':=' },
    { name = 'OP', pattern = '[%(%)%+%-%*/%%,%.<>=]' },
    -- prose/decoration characters vanilla ignores
    { name = 'JUNK', pattern = "[`'!:?@#%$%^&|%[%]{}~\\]+", skip = true },
    { name = 'NAME', pattern = '[%a_][%w_]*',
      map = function(value)
          local lower = value:lower()
          if KEYWORDS[lower] then
              return 'KEYWORD', lower
          end
          return 'NAME', value
      end },
}

local function node(kind, fields)
    local n = fields or {}
    n.kind = kind
    return n
end

-- ObScript recursive-descent parser over a token stream.
local Parser = {}
Parser.__index = Parser

function Parser.new(tokens)
    return setmetatable({ toks = tokens, i = 1 }, Parser)
end

-- token helpers

function Parser:peek(offset)
    return self.toks[math.min(self.i + (offset or 0), #self.toks)]
end

function Parser:next()
    local t = self.toks[self.i]
    if t.kind ~= 'EOF' then
        self.i = self.i + 1
    end
    return t
end

function Parser:accept(kind, value)
    local t = self:peek()
    if t.kind == kind and (value == nil or t.value:lower() == value) then
        return self:next()
    end
    return nil
end

function Parser:expect(kind, value)
    local t = self:accept(kind, value)
    if t == nil then
        local got = self:peek()
        error(('line %d: expected %s, got %s %q')
            :format(got.line, value or kind, got.kind, got.value))
    end
    return t
end

function Parser:skipNewlines()
    while self:accept('NEWLINE') do end
end

function Parser:skipRestOfLine()
    while self:peek().kind ~= 'NEWLINE' and self:peek().kind ~= 'EOF' do
        self:next()
    end
end

function Parser:endOfLine()
    -- Everything in ObScript is line-based; require and consume end of line.
    if self:peek().kind == 'EOF' then
        return
    end
    self:expect('NEWLINE')
    self:skipNewlines()
end

-- grammar

function Parser:parseScript()
    self:skipNewlines()
    local name = nil
    local t = self:peek()
    if t.kind == 'KEYWORD' and (t.value == 'scn' or t.value == 'scriptname') then
        self:next()
        name = self:expect('NAME').value
        self:skipRestOfLine() -- vanilla ignores trailing junk on the header
        self:endOfLine()
    end
    local variables = {}
    local blocks = {}
    local stray = {} -- statements outside blocks (rare but happens in the wild)
    while self:peek().kind ~= 'EOF' do
        self:skipNewlines()
        t = self:peek()
        if t.kind == 'EOF' then
            break
        end
        if t.kind == 'KEYWORD' and (t.value == 'short' or t.value == 'int'
                or t.value == 'long' or t.value == 'float' or t.value == 'ref') then
            self:next()
            local varname = self:expect('NAME').value
            variables[#variables + 1] = node('VarDecl', { type = t.value, name = varname })
            self:skipRestOfLine() -- semicolon-less comments after declarations
            self:endOfLine()
        elseif t.kind == 'KEYWORD' and t.value == 'begin' then
            blocks[#blocks + 1] = self:parseBlock()
        else
            stray[#stray + 1] = self:parseStatement()
        end
    end
    return node('Script', { name = name, variables = variables, blocks = blocks, stray = stray })
end

function Parser:parseBlock()
    self:expect('KEYWORD', 'begin')
    local event
    if self:peek().kind == 'NAME' then
        event = self:expect('NAME').value
    else
        event = self:expect('KEYWORD').value
    end
    -- optional block arguments: e.g. "begin OnActivate", "begin MenuMode 1017",
    -- "begin OnTriggerEnter player"
    local args = {}
    while self:peek().kind ~= 'NEWLINE' and self:peek().kind ~= 'EOF' do
        -- commas may separate block arguments: begin OnAlarm 3, player
        if not self:accept('OP', ',') then
            args[#args + 1] = self:parseExpression()
        end
    end
    self:endOfLine()
    local body = self:parseStatements({ ['end'] = true })
    self:expect('KEYWORD', 'end')
    self:skipRestOfLine() -- trailing junk after 'end' is tolerated by vanilla
    self:endOfLine()
    return node('Block', { event = event, args = args, body = body })
end

function Parser:parseStatements(stopKeywords)
    local stmts = {}
    while true do
        self:skipNewlines()
        local t = self:peek()
        if t.kind == 'EOF' then
            break
        end
        if t.kind == 'KEYWORD' and stopKeywords[t.value] then
            break
        end
        stmts[#stmts + 1] = self:parseStatement()
    end
    return stmts
end

function Parser:parseStatement()
    local t = self:peek()
    if t.kind == 'KEYWORD' then
        if t.value == 'endif' or t.value == 'elseif' or t.value == 'else' then
            -- Vanilla tolerates unmatched block terminators; skip the line.
            self:next()
            self:skipRestOfLine()
            self:endOfLine()
            return node('StrayKeyword', { keyword = t.value })
        end
        if t.value == 'set' then
            return self:parseSet()
        end
        if t.value == 'if' then
            return self:parseIf()
        end
        if t.value == 'return' then
            self:next()
            self:skipRestOfLine()
            self:endOfLine()
            return node('Return')
        end
        if t.value == 'short' or t.value == 'int' or t.value == 'long'
                or t.value == 'float' or t.value == 'ref' then
            -- local declarations can appear mid-block in the wild
            self:next()
            local varname = self:expect('NAME').value
            self:skipRestOfLine()
            self:endOfLine()
            return node('VarDecl', { type = t.value, name = varname })
        end
    end
    if t.kind == 'OP' and t.value ~= '(' then
        -- decorative separator lines (=====, -----) the vanilla compiler ignores
        self:skipRestOfLine()
        self:endOfLine()
        return node('JunkLine')
    end
    if t.kind == 'NAME'
            and self:peek(1).kind == 'OP' and self:peek(1).value == '.'
            and self:peek(2).kind == 'KEYWORD' and self:peek(2).value == 'set' then
        -- `SomeRef.Set Var to Expr` - set with explicit reference prefix
        local base = node('Name', { value = self:next().value })
        self:next() -- '.'
        self:next() -- 'set'
        local target = node('Member', { base = base, member = self:parsePostfix() })
        self:expect('KEYWORD', 'to')
        local value = self:parseExpression()
        self:endOfLine()
        return node('Set', { target = target, value = value })
    end
    -- otherwise: a command/expression line (possibly ref.Command arg arg)
    local expr = self:parseCommandLine()
    self:endOfLine()
    return node('ExprStatement', { expr = expr })
end

function Parser:parseSet()
    self:expect('KEYWORD', 'set')
    local target = self:parsePostfix() -- allows quest.var / ref.var
    self:expect('KEYWORD', 'to')
    local value = self:parseExpression()
    self:endOfLine()
    return node('Set', { target = target, value = value })
end

function Parser:parseIf()
    self:expect('KEYWORD', 'if')
    local cond = self:parseExpression()
    self:endOfLine()
    local stops = { ['elseif'] = true, ['else'] = true, endif = true, ['end'] = true }
    local thenBody = self:parseStatements(stops)
    local clauses = { node('Clause', { cond = cond, body = thenBody }) }
    while true do
        local t = self:peek()
        if t.kind == 'KEYWORD' and t.value == 'elseif' then
            self:next()
            cond = self:parseExpression()
            self:endOfLine()
            local body = self:parseStatements(stops)
            clauses[#clauses + 1] = node('Clause', { cond = cond, body = body })
        elseif t.kind == 'KEYWORD' and t.value == 'else' then
            self:next()
            if self:peek().kind == 'KEYWORD' and self:peek().value == 'if' then
                -- 'else if' split across two words: treat as elseif
                self:next()
                cond = self:parseExpression()
                self:endOfLine()
                local body = self:parseStatements(stops)
                clauses[#clauses + 1] = node('Clause', { cond = cond, body = body })
            else
                self:skipRestOfLine() -- vanilla ignores junk conditions after else
                self:endOfLine()
                local body = self:parseStatements({ endif = true, ['end'] = true })
                clauses[#clauses + 1] = node('Clause', { cond = nil, body = body })
            end
        else
            break
        end
    end
    if self:peek().kind == 'KEYWORD' and self:peek().value == 'endif' then
        self:next()
        self:skipRestOfLine() -- tolerate junk after endif
        self:endOfLine()
    end
    -- else: unterminated if closed by the block's 'end' (or EOF);
    -- vanilla tolerates the missing endif, so leave 'end' unconsumed.
    return node('If', { clauses = clauses })
end

function Parser:parseCommandLine()
    -- A statement line is a full expression (commands with space-separated
    -- args are handled by parseCallExpr); extra comma-separated arguments
    -- after the expression are folded into a call.
    local expr = self:parseExpression()
    local extra = {}
    while self:peek().kind ~= 'NEWLINE' and self:peek().kind ~= 'EOF' do
        if not self:accept('OP', ',') then
            extra[#extra + 1] = self:parseExpression()
        end
    end
    if #extra > 0 then
        if expr.kind == 'Call' then
            for _, arg in ipairs(extra) do
                expr.args[#expr.args + 1] = arg
            end
        else
            expr = node('Call', { callee = expr, args = extra })
        end
    end
    return expr
end

-- expressions (precedence climbing)

function Parser:parseExpression()
    return self:parseOr()
end

function Parser:parseOr()
    local left = self:parseAnd()
    while self:accept('OP', '||') do
        left = node('BinOp', { op = '||', left = left, right = self:parseAnd() })
    end
    return left
end

function Parser:parseAnd()
    local left = self:parseCmp()
    while self:accept('OP', '&&') do
        left = node('BinOp', { op = '&&', left = left, right = self:parseCmp() })
    end
    return left
end

local CMP_OPS = { ['=='] = true, ['!='] = true, ['<'] = true,
                  ['>'] = true, ['<='] = true, ['>='] = true }

function Parser:parseCmp()
    local left
    local first = self:peek()
    if first.kind == 'OP' and CMP_OPS[first.value] then
        -- vanilla tolerates a missing left operand in chained comparisons:
        -- `if x >= 0 && <10`; represent it explicitly and let the
        -- transpiler decide the semantics (vanilla treats it as 0).
        left = node('Missing')
    else
        left = self:parseAdd()
    end
    while true do
        local t = self:peek()
        if t.kind == 'OP' and CMP_OPS[t.value] then
            self:next()
            left = node('BinOp', { op = t.value, left = left, right = self:parseAdd() })
        else
            return left
        end
    end
end

function Parser:parseAdd()
    local left = self:parseMul()
    while true do
        local t = self:peek()
        if t.kind == 'OP' and (t.value == '+' or t.value == '-') then
            self:next()
            left = node('BinOp', { op = t.value, left = left, right = self:parseMul() })
        else
            return left
        end
    end
end

function Parser:parseMul()
    local left = self:parseUnary()
    while true do
        local t = self:peek()
        if t.kind == 'OP' and (t.value == '*' or t.value == '/' or t.value == '%') then
            self:next()
            left = node('BinOp', { op = t.value, left = left, right = self:parseUnary() })
        else
            return left
        end
    end
end

function Parser:parseUnary()
    if self:accept('OP', '-') then
        return node('Neg', { operand = self:parseUnary() })
    end
    return self:parseCallExpr()
end

local ARG_KINDS = { NAME = true, INT = true, FLOAT = true, STRING = true }

function Parser:parseCallExpr()
    -- Commands take space-separated arguments with no parentheses:
    -- `if player.GetItemCount Caps001 >= 10`. Names and literals after
    -- a command are its arguments; anything else ends the list.
    local expr = self:parsePostfix()
    local args = {}
    while true do
        local t = self:peek()
        if t.kind == 'OP' and t.value == ',' and #args > 0 then
            -- commas may separate bare arguments: PlaceAtMe Foo 1, 0, 0
            self:next()
        elseif ARG_KINDS[t.kind] then
            args[#args + 1] = self:parsePostfix()
        else
            break
        end
    end
    if #args > 0 then
        return node('Call', { callee = expr, args = args })
    end
    return expr
end

function Parser:parsePostfix()
    local expr = self:parsePrimary()
    while self:accept('OP', '.') do
        local t = self:peek()
        local member
        if t.kind == 'KEYWORD' then
            -- keywords can appear as member names (e.g. `Ref.Set`)
            self:next()
            member = node('Name', { value = t.value })
        else
            member = self:parsePrimary()
        end
        expr = node('Member', { base = expr, member = member })
    end
    return expr
end

function Parser:parsePrimary()
    local t = self:peek()
    if t.kind == 'INT' then
        self:next()
        return node('Int', { value = tonumber(t.value) })
    end
    if t.kind == 'FLOAT' then
        self:next()
        return node('Float', { value = tonumber(t.value) })
    end
    if t.kind == 'STRING' then
        self:next()
        return node('Str', { value = t.value:sub(2, -2) })
    end
    if t.kind == 'OP' and t.value == '(' then
        self:next()
        local inner = self:parseExpression()
        -- inside parens, calls can have space- or comma-separated args too:
        local args = {}
        while self:peek().kind ~= 'NEWLINE' and self:peek().kind ~= 'EOF'
                and not (self:peek().kind == 'OP' and self:peek().value == ')') do
            if not self:accept('OP', ',') then
                args[#args + 1] = self:parseExpression()
            end
        end
        self:expect('OP', ')')
        if #args > 0 then
            inner = node('Call', { callee = inner, args = args })
        end
        return inner
    end
    if t.kind == 'NAME' then
        self:next()
        return node('Name', { value = t.value })
    end
    if t.kind == 'KEYWORD' and t.value == 'to' then
        -- 'to' appearing as identifier (it happens); treat as name
        self:next()
        return node('Name', { value = t.value })
    end
    error(('line %d: unexpected %s %q'):format(t.line, t.kind, t.value))
end

---
-- Tokenize ObScript source text using @{#parser.rules}.
-- @function [parent=#parser] tokenize
-- @param #string text ObScript source
-- @return #list<#Token>
function parser.tokenize(text)
    return lexer.tokenize(text, parser.rules)
end

---
-- Parse a token stream produced by @{#parser.tokenize} into a `Script` node.
-- @function [parent=#parser] parseTokens
-- @param #list<#Token> tokens
-- @return #Node `Script` AST node
function parser.parseTokens(tokens)
    return Parser.new(tokens):parseScript()
end

---
-- Parse ObScript source text into a `Script` AST node.
-- @function [parent=#parser] parse
-- @param #string text ObScript source
-- @return #Node `Script` AST node
function parser.parse(text)
    return parser.parseTokens(parser.tokenize(text))
end

return parser