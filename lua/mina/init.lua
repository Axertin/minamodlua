-- The ergonomic layer. Everything under lua/ is plain Lua, ships beside the
-- binary, and needs no C++ rebuild to change.
--
-- The host calls this with the table it built in C++
-- mina.raw and the primitives listed in CONTRIBUTING and whatever comes back is what mods see
-- as `mina`
--
-- Nothing here yet. Add modules under lua/mina/ and require them below:
--
--     mina.vec = require("mina.vec")

local mina = ...

return mina
