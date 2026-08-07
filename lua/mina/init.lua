-- The ergonomic layer. Everything under lua/ is plain Lua, ships beside the
-- binary, and needs no C++ rebuild to change.
--
-- The host calls this with the table it built in C++ - mina.raw,
-- mina.signatures and mina.on_event, the primitives listed under "The seam"
-- in CONTRIBUTING.md - and whatever comes back is what mods see as `mina`.
--
-- Note what is NOT in that table: there are no receiver methods on handles and
-- no `defines`. Both are planned, both are C++-side work, and neither can be
-- faked from here. See CONTRIBUTING.md before writing against either.
--
-- Nothing here yet. Add modules under lua/mina/ and require them below:
--
--     mina.vec = require("mina.vec")

local mina = ...

return mina
