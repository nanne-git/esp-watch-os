# Lua 5.4.7 (https://www.lua.org) - MIT licence, see LICENSE.
# Only the core + safe libraries are built (no io/os/package/debug): watch apps run sandboxed.
file(GLOB LUA_SRCS "src/*.c")
idf_component_register(SRCS ${LUA_SRCS} INCLUDE_DIRS include)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-maybe-uninitialized)
