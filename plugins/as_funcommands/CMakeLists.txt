# as_funcommands — Plugify C++ plugin: port of the Pisex admin_system module "FunCommands".
#
#   plugify-as_funcommands  the plugin shared library (entry "bin/as_funcommands")
#   as_funcommands_tests    engine-free fake-runtime tests (build/tests/as_funcommands_tests)
#
# Pure s2sdk: no pisexcompat and no s2sguard (docs/PLUGIN_STANDARD.md §7). The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself - the B1/B2 map gate, the C2
# client/slot/handle checks, the C6/C8 raw schema offsets, the C9 deadline accumulators, the C10
# native menu lifecycle and the C11 command ownership rules. The plugin has no config, translation
# or database file of its own; phrases come from the admin_system API.
#
# The .so compiles the binding globals the plugify module fills at load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper include/admin_api.hpp,
#   * the optional hudmenu plugin API (__hudmenu_*, the original's per-player "!menu" type).

set(AS_FUNCOMMANDS_VERSION "1.2.0")

# In this monorepo plugins/admin_system provides admin_system_bindings_obj. A standalone export of
# this plugin ships only the admin_system headers (tools/export/export_plugin.py copies include/ and
# no CMakeLists.txt for a consumed plugin), so the target is absent there; build a private,
# uniquely named object library from the checked-in generated bindings instead of failing. The name
# must stay unique: another plugin may need the same fallback in the standalone tree, and two object
# targets with one name cannot coexist.
if(TARGET admin_system_bindings_obj)
    set(AS_FUNCOMMANDS_ADMIN_BINDINGS_OBJECTS $<TARGET_OBJECTS:admin_system_bindings_obj>)
else()
    set(AS_FUNCOMMANDS_ADMIN_BINDINGS_CPP
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    if(NOT EXISTS "${AS_FUNCOMMANDS_ADMIN_BINDINGS_CPP}")
        message(FATAL_ERROR "as_funcommands needs the generated admin_system consumer bindings (${AS_FUNCOMMANDS_ADMIN_BINDINGS_CPP})")
    endif()
    add_library(as_funcommands_admin_system_bindings_obj OBJECT "${AS_FUNCOMMANDS_ADMIN_BINDINGS_CPP}")
    target_include_directories(as_funcommands_admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_funcommands_admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_FUNCOMMANDS_ADMIN_BINDINGS_OBJECTS $<TARGET_OBJECTS:as_funcommands_admin_system_bindings_obj>)
endif()

# The optional hudmenu consumer bindings (GetClientMenuType: the original's per-player "!menu"
# type). The monorepo provides the hudmenu_bindings object library at the top level; a standalone
# export carries only sdk/include, so the fallback compiles the checked-in generated hudmenu.cpp
# itself. The target name is plugin-specific so another consumer's fallback cannot collide with it.
if(TARGET hudmenu_bindings)
    set(AS_FUNCOMMANDS_HUDMENU_BINDINGS $<TARGET_OBJECTS:hudmenu_bindings>)
else()
    set(AS_FUNCOMMANDS_HUDMENU_BINDINGS_CPP "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    if(NOT EXISTS "${AS_FUNCOMMANDS_HUDMENU_BINDINGS_CPP}")
        message(FATAL_ERROR "as_funcommands needs the generated hudmenu consumer bindings (${AS_FUNCOMMANDS_HUDMENU_BINDINGS_CPP})")
    endif()
    add_library(as_funcommands_hudmenu_bindings OBJECT "${AS_FUNCOMMANDS_HUDMENU_BINDINGS_CPP}")
    target_include_directories(as_funcommands_hudmenu_bindings SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_funcommands_hudmenu_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_FUNCOMMANDS_HUDMENU_BINDINGS $<TARGET_OBJECTS:as_funcommands_hudmenu_bindings>)
endif()

add_library(plugify-as_funcommands SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${AS_FUNCOMMANDS_ADMIN_BINDINGS_OBJECTS}
        ${AS_FUNCOMMANDS_HUDMENU_BINDINGS}
)
target_include_directories(plugify-as_funcommands PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/admin_system/hudmenu/plg headers and admin_api.hpp are not ours to
# fix; their warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_funcommands SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
)
target_link_libraries(plugify-as_funcommands PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_funcommands PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_funcommands PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_funcommands"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_funcommands PROPERTIES OUTPUT_NAME as_funcommands)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_funcommands PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_funcommands.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_funcommands.pplugin"
)

# --- tests -------------------------------------------------------------------------
# as_funcommands_tests: engine-free. src/plugin.cpp is compiled into the test TU (its handlers live
# in an anonymous namespace) and every __s2sdk_* / __admin_system_* / __hudmenu_* binding slot is a
# recording fake: all twelve commands and their contexts, the permission and admin-readiness gate,
# signed CEntityHandle identities, the raw schema field writes, both timed commands through the
# OnPluginUpdate accumulators (including slot reuse and map change), the three menus with the
# pisexcompat menu-type table, the admin item registration/retirement, the map gate, the paired
# binding rules and the A7 unload/reload cycle. Written into build/tests so ../test.sh runs it with
# the rest (*_tests convention).
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone release build) is already in the cache and
# still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_funcommands_tests
            tests/as_funcommands_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            ${AS_FUNCOMMANDS_ADMIN_BINDINGS_OBJECTS}
            ${AS_FUNCOMMANDS_HUDMENU_BINDINGS}
    )
    target_include_directories(as_funcommands_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_funcommands_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
    )
    target_link_libraries(as_funcommands_tests PRIVATE plugify_sdk -static-libstdc++ -static-libgcc)
    target_compile_options(as_funcommands_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_funcommands_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_funcommands"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
    )
    set_target_properties(as_funcommands_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
