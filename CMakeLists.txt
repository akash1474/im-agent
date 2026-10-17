# ImAgent: a debug-only MCP server inside a Dear ImGui app, so AI agents can inspect and drive the running UI.
# See README.md. Use it with add_subdirectory(); an `imgui` target (GLFW + OpenGL3 backends) must already exist.
#
#   add_subdirectory(src/agent)
#   imagent_configure(imgui IMGUI_DIR ${IMGUI_DIR})
#   target_link_libraries(my_app PRIVATE ImAgent::ImAgent)

cmake_minimum_required(VERSION 3.16)

option(AGENT_BRIDGE "Compile ImAgent into Debug configurations (Release builds never contain it)" ON)
set(IMAGENT_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/packages" CACHE PATH
    "Directory that holds cpp-httplib/httplib.h and nlohmann/json.hpp")
set(IMAGENT_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/imgui-agent-hooks.patch" CACHE INTERNAL "")

if(NOT TARGET imgui)
    message(FATAL_ERROR "ImAgent needs an 'imgui' target; add it before add_subdirectory(src/agent).")
endif()

# What every imgui translation unit must see: these defines switch on the hooks added by the imgui patch,
# so imagent_configure() attaches them to the imgui target.
add_library(imagent_config INTERFACE)
if(AGENT_BRIDGE)
    target_compile_definitions(imagent_config INTERFACE $<$<CONFIG:Debug>:AGENT_BRIDGE=1>)
    target_compile_definitions(imagent_config INTERFACE $<$<CONFIG:Debug>:IMGUI_AGENT_HOOKS>)
    target_compile_definitions(imagent_config INTERFACE $<$<CONFIG:Debug>:IMGUI_AGENT_TEXT_HOOK>)
endif()

if(AGENT_BRIDGE)
    foreach(_header cpp-httplib/httplib.h nlohmann/json.hpp)
        if(NOT EXISTS "${IMAGENT_THIRD_PARTY_DIR}/${_header}")
            message(FATAL_ERROR "ImAgent: ${IMAGENT_THIRD_PARTY_DIR}/${_header} not found. Set IMAGENT_THIRD_PARTY_DIR.")
        endif()
    endforeach()
    find_package(Threads REQUIRED)
    find_package(OpenGL REQUIRED)

    add_library(imagent STATIC
        src/AgentActions.cpp
        src/AgentBridge.cpp
        src/AgentTools.cpp
        src/Batch.cpp
        src/Changes.cpp
        src/Input.cpp
        src/Lint.cpp
        src/Metrics.cpp
        src/Recorder.cpp
        src/Screenshot.cpp
    )
    target_compile_features(imagent PRIVATE cxx_std_17)
    target_include_directories(imagent
        PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include
        PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src
    )
    # Vendored headers: keep their warnings out of our build.
    target_include_directories(imagent SYSTEM PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/third_party/stb
        ${IMAGENT_THIRD_PARTY_DIR}
    )
    target_link_libraries(imagent PUBLIC imagent_config imgui PRIVATE OpenGL::GL Threads::Threads)

    # Debug builds would otherwise run the per-item imgui hooks and the screenshot resampling at -O0.
    if(NOT MSVC)
        set_source_files_properties(src/Recorder.cpp src/Screenshot.cpp PROPERTIES COMPILE_OPTIONS "-O2")
    endif()
    if(APPLE)
        target_compile_definitions(imagent PRIVATE GL_SILENCE_DEPRECATION)
    endif()
else()
    # Nothing to build: hosts still link ImAgent::ImAgent and call the no-op API in AgentBridge.h.
    add_library(imagent INTERFACE)
    target_include_directories(imagent INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
    target_link_libraries(imagent INTERFACE imagent_config imgui)
endif()
add_library(ImAgent::ImAgent ALIAS imagent)

# Wires imgui to ImAgent: the shared config. imgui calls hooks that imagent implements, so link imagent
# into the final executable too (linking ImAgent::ImAgent does that).
function(imagent_configure imgui_target)
    cmake_parse_arguments(ARG "" "IMGUI_DIR" "" ${ARGN})
    if(NOT ARG_IMGUI_DIR)
        message(FATAL_ERROR "imagent_configure(<target> IMGUI_DIR <path to imgui sources>)")
    endif()

    # Without the hooks the build succeeds but get_ui_tree and get_text silently return nothing, so fail early.
    if(AGENT_BRIDGE)
        file(STRINGS ${ARG_IMGUI_DIR}/imgui_draw.cpp _text_hook REGEX "ImGuiAgentHook_Text")
        file(STRINGS ${ARG_IMGUI_DIR}/imgui_internal.h _item_hook REGEX "ImGuiAgentHook_ItemAdd")
        if(NOT _text_hook OR NOT _item_hook)
            message(FATAL_ERROR
                "${ARG_IMGUI_DIR} lacks the agent hooks. Apply them with:\n"
                "  patch -p1 -d ${ARG_IMGUI_DIR} < ${IMAGENT_PATCH}\n"
                "or configure with -DAGENT_BRIDGE=OFF.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ARG_IMGUI_DIR}/imgui_draw.cpp ${ARG_IMGUI_DIR}/imgui_internal.h)
    endif()

    target_link_libraries(${imgui_target} PUBLIC imagent_config)
endfunction()
