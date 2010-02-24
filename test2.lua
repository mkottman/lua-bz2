require 'bz2'

local data = ('1234'):rep(1000)

local benc = bz2.open('out.bz2', 'w')
benc:write(data)
print(benc:close()) -- returns uncopressed and compressed length

local bdec = bz2.open('out.bz2', 'r')
local data2 = bdec:read(#data)
bdec:close()

assert(data == data2, "compressed and uncompressed data differ")

local d3 = assert(bz2.compress(data))
local d4 = assert(bz2.decompress(d3))
assert(d4 == data, "low-level compressed and uncompressed data differ")
