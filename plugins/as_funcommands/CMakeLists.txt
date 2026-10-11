# as_funcommands — Plugify C++ plugin: port of the Pisex cs2-admin_system module "FunCommands".
#
#   plugify-as_funcommands  the plugin shared library (entry "bin/as_funcommands" ->
#                           bin/libas_funcommands.so)

set(AS_FUNCOMMANDS_VERSION "1.1.1")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*        (s2sdk_bindings)
#   __admin_system_* (admin_system consumer bindings, generated into plugins/admin_system/include)
set(AS_FUNCOMMANDS_BINDING_OBJECTS "")
if(TARGET admin_system_bindings_obj)
    list(APPEND AS_FUNCOMMANDS_BINDING_OBJECTS $<TARGET_OBJECTS:admin_system_bindings_obj>)
else()
    message(WARNING "as_funcommands: admin_system_bindings_obj is missing (build plugins/admin_system first)")
endif()

add_library(plugify-as_funcommands SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${AS_FUNCOMMANDS_BINDING_OBJECTS}
)
target_include_directories(plugify-as_funcommands PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated admin_system/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-as_funcommands SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_funcommands PRIVATE
        pisexcompat
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
