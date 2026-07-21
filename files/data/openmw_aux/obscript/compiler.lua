---
-- `openmw_aux.obscript.compiler` combines `openmw_aux.obscript.parser` and
-- `openmw_aux.obscript.transpiler` into a single entry point that translates
-- ObScript source text into Lua source text.
-- The engine runs it at content load to compile SCPT records;
-- it can equally be used by tools.
-- Implementation can be found in `resources/vfs/openmw_aux/obscript/compiler.lua`.
-- @module compiler
-- @context global|menu|local
-- @usage
-- local compiler = require('openmw_aux.obscript.compiler')
-- local luaSource = compiler.compile('scn MyScript\nbegin GameMode\nend')

local parser = require('openmw_aux.obscript.parser')
local transpiler = require('openmw_aux.obscript.transpiler')

local compiler = {}

---
-- Translate ObScript source text into Lua source text.
-- Raises an error with a line number if the text cannot be parsed.
-- @function [parent=#compiler] compile
-- @param #string text ObScript source
-- @return #string Lua source
function compiler.compile(text)
    return transpiler.transpile(parser.parse(text))
end

return compiler
