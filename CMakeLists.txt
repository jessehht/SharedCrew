cmake_minimum_required(VERSION 3.20)
project(SharedCrew VERSION 0.4.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C runtime statically so users don't need an extra Visual C++ install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(XPSDK "${CMAKE_SOURCE_DIR}/third_party/XPSDK" CACHE PATH "X-Plane SDK folder")
if(NOT EXISTS "${XPSDK}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR "X-Plane SDK not found in ${XPSDK}. Run tools/get-sdk.ps1 first.")
endif()

add_library(SharedCrew SHARED
    src/main.cpp
    src/code.cpp
    src/nat.cpp
    src/log.cpp
    src/net.cpp
    src/profile.cpp
    src/profile_files.cpp
    src/session.cpp
    src/settings.cpp
    src/ui.cpp
)

target_include_directories(SharedCrew PRIVATE
    "${XPSDK}/CHeaders/XPLM"
    "${XPSDK}/CHeaders/Widgets"
)

# SDK 4.0 = X-Plane 12.0 and newer.
target_compile_definitions(SharedCrew PRIVATE
    XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM302=1 XPLM303=1 XPLM400=1
    SHAREDCREW_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_compile_definitions(SharedCrew PRIVATE IBM=1 _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(SharedCrew PRIVATE
        "${XPSDK}/Libraries/Win/XPLM_64.lib"
        "${XPSDK}/Libraries/Win/XPWidgets_64.lib"
        ws2_32
        opengl32
    )
    set(PLATFORM_DIR win_x64)
    target_compile_options(SharedCrew PRIVATE /W3 /utf-8)
elseif(APPLE)
    target_compile_definitions(SharedCrew PRIVATE APL=1)
    target_link_libraries(SharedCrew PRIVATE
        "${XPSDK}/Libraries/Mac/XPLM.framework"
        "${XPSDK}/Libraries/Mac/XPWidgets.framework"
    )
    set(PLATFORM_DIR mac_x64)
else()
    target_compile_definitions(SharedCrew PRIVATE LIN=1)
    set_target_properties(SharedCrew PROPERTIES LINK_FLAGS "-Wl,--unresolved-symbols=ignore-all")
    set(PLATFORM_DIR lin_x64)
endif()

# Output a ready-to-copy plugin folder: dist/SharedCrew/<platform>/SharedCrew.xpl
set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/SharedCrew")
set_target_properties(SharedCrew PROPERTIES
    PREFIX ""
    SUFFIX ".xpl"
    OUTPUT_NAME "SharedCrew"
    RUNTIME_OUTPUT_DIRECTORY "${DIST_DIR}/${PLATFORM_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${DIST_DIR}/${PLATFORM_DIR}"
)
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(SharedCrew PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG} "${DIST_DIR}/${PLATFORM_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG} "${DIST_DIR}/${PLATFORM_DIR}"
    )
endforeach()

# Test peer: a fake second pilot for testing on one PC (Windows console app).
if(WIN32)
    add_executable(sc-testpeer tools/testpeer/main.cpp src/net.cpp src/nat.cpp src/code.cpp src/profile_files.cpp)
    target_compile_definitions(sc-testpeer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(sc-testpeer PRIVATE /W3 /utf-8)
    target_link_libraries(sc-testpeer PRIVATE ws2_32)
    set_target_properties(sc-testpeer PROPERTIES OUTPUT_NAME "SharedCrew-TestPeer")
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${cfg} CFG)
        set_target_properties(sc-testpeer PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} "${CMAKE_BINARY_DIR}/dist/TestPeer")
    endforeach()
    add_custom_command(TARGET sc-testpeer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/tools/testpeer/Test via server.bat"
            "${CMAKE_SOURCE_DIR}/tools/testpeer/Test via relay.bat"
            "${CMAKE_BINARY_DIR}/dist/TestPeer/"
    )
endif()

add_custom_command(TARGET SharedCrew POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/profiles" "${DIST_DIR}/profiles"
)
