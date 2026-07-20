---
-- `openmw_aux.lexer` is a general purpose lexical analyzer implemented in Lua.
-- Implementation can be found in `resources/vfs/openmw_aux/lexer.lua`.
-- @module lexer
-- @context global|menu|local
-- @usage
-- local lexer = require('openmw_aux.lexer')
-- local rules = {
--     { name = 'NUMBER', pattern = '%d+' },
--     { name = 'NAME', pattern = '[%a_][%w_]*' },
--     { name = 'WS', pattern = '[ \t]+', skip = true },
-- }
-- local tokens = lexer.tokenize('foo 42', rules)

local lexer = {}

---
-- A token produced by @{lexer#lexer.tokenize}.
-- @type Token
-- @field #string kind Name of the matched rule (possibly remapped by the rule's `map` function)
-- @field #string value Matched text
-- @field #number line 1-based line number where the token starts

---
-- A lexical rule.
-- @type Rule
-- @field #string name Token kind produced by this rule
-- @field #string pattern Lua pattern matched at the current position (anchored automatically)
-- @field #boolean skip If true, matches are consumed but produce no token (whitespace, comments)
-- @field #function map Optional function(value) -> kind, value; allows keyword
--   recognition or other post-processing of the match
-- @field #string notFollowedBy Optional character class (e.g. `'[%a_]'`); the
--   rule is rejected if the character immediately after the match belongs to
--   the class. Provides negative lookahead, which Lua patterns lack.

local function countNewlines(s)
    local n = 0
    for _ in s:gmatch('\n') do
        n = n + 1
    end
    return n
end

---
-- Split `text` into tokens according to `rules`.
-- Rules are tried in order at each position; the first rule that matches wins
-- (so longer/more specific rules must precede shorter/general ones).
-- A rule with `skip = true` consumes text without emitting a token.
-- If no rule matches, an error with the offending line number is raised.
-- Two tokens are always appended at the end of the stream: a trailing
-- NEWLINE and an EOF token.
-- @function [parent=#lexer] tokenize
-- @param #string text Input text
-- @param #list<#Rule> rules Lexical rules, tried in order
-- @return #list<#Token>
function lexer.tokenize(text, rules)
    local tokens = {}
    local pos = 1
    local line = 1
    local len = #text
    -- precompile anchored patterns
    local anchored = {}
    for i, rule in ipairs(rules) do
        anchored[i] = '^(' .. rule.pattern .. ')'
    end
    while pos <= len do
        local matched = false
        for i, rule in ipairs(rules) do
            local value = text:sub(pos):match(anchored[i])
            if value ~= nil and #value > 0
                and not (rule.notFollowedBy
                    and text:sub(pos + #value, pos + #value):match(rule.notFollowedBy)) then
                if not rule.skip then
                    local kind = rule.name
                    if rule.map then
                        kind, value = rule.map(value)
                    end
                    tokens[#tokens + 1] = { kind = kind, value = value, line = line }
                end
                line = line + countNewlines(value)
                pos = pos + #value
                matched = true
                break
            end
        end
        if not matched then
            error(('line %d: unexpected character %q'):format(line, text:sub(pos, pos)))
        end
    end
    tokens[#tokens + 1] = { kind = 'NEWLINE', value = '\n', line = line }
    tokens[#tokens + 1] = { kind = 'EOF', value = '', line = line }
    return tokens
end

return lexer