# asmjit

set(ASMJIT_STATIC ON CACHE INTERNAL BOOL "")
add_subdirectory(asmjit)
set_target_properties(asmjit PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:asmjit,INTERFACE_INCLUDE_DIRECTORIES>)
