require 'bz2'

local data = ('1234'):rep(1000)
print(#data)

local benc = bz2.open('out.bz2', 'w')
benc:write(data)
print(benc:close()) -- returns uncopressed and compressed length

local bdec = bz2.open('out.bz2', 'r')
local data2 = bdec:read(#data)
bdec:close()

assert(data == data2, "compressed and uncompressed data differ")
