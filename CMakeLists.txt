cmake_minimum_required(VERSION 3.22)

project(
    advaitmusic_cpp
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "AdvaitMusic C++ port — Telegram voice-chat music bot"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(MoldLinker)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

find_package(nlohmann_json 3.10 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(ADVAIT_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(ADVAIT_NTG_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ntgcalls")

find_library(NTGCALLS_LIB NAMES ntgcalls libntgcalls
    PATHS ${ADVAIT_LIB_DIR} ${CMAKE_CURRENT_SOURCE_DIR} NO_DEFAULT_PATH)
find_library(TDJSON_LIB NAMES tdjson libtdjson
    PATHS ${ADVAIT_LIB_DIR} /usr/local/lib /usr/lib NO_DEFAULT_PATH)

add_library(advait_core STATIC
    src/core/config.cpp
    src/core/logger.cpp
    src/core/locales.cpp
    src/core/utils.cpp
    src/core/models.cpp
    src/core/room.cpp
    src/core/buttons.cpp
    src/core/assistant.cpp
)
add_library(advait::core ALIAS advait_core)
target_include_directories(advait_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_definitions(advait_core PUBLIC
    ADVAIT_LOCALES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/locales"
)
target_link_libraries(advait_core PUBLIC
    Threads::Threads
    nlohmann_json::nlohmann_json
)
set_project_warnings(advait_core)

add_library(advait_database STATIC src/database/database.cpp)
add_library(advait::database ALIAS advait_database)
target_include_directories(advait_database PUBLIC include)
target_link_libraries(advait_database PUBLIC advait::core)
set_project_warnings(advait_database)

add_library(advait_platforms STATIC
    src/platforms/registry.cpp
    src/platforms/youtube.cpp
        src/platforms/iota.cpp
        src/platforms/yuki.cpp
        src/platforms/artwork.cpp
)
add_library(advait::platforms ALIAS advait_platforms)
target_include_directories(advait_platforms PUBLIC include)
target_link_libraries(advait_platforms PUBLIC advait::core CURL::libcurl)
set_project_warnings(advait_platforms)

add_library(advait_ntg STATIC
    src/ntgcalls/client.cpp
    src/ubot/context.cpp
)
add_library(advait::ntg ALIAS advait_ntg)
target_include_directories(advait_ntg PUBLIC include ${ADVAIT_NTG_INCLUDE})
target_link_libraries(advait_ntg PUBLIC advait::core advait::mtproto)
if(NTGCALLS_LIB)
    # libntgcalls bundles its own BoringSSL; force its crypto objects into the
    # executable so WebRTC never binds ASN1_*/d2i_* etc. to the mismatched
    # system libcrypto ABI. The trimmed archive lacks those members to avoid
    # duplicate definitions with the forced subset.
    get_filename_component(_ntg_dir "${NTGCALLS_LIB}" DIRECTORY)
    set(_ntg_trimmed "${_ntg_dir}/libntgcalls_trimmed.a")
    set(_ntg_forced "${_ntg_dir}/libntgcalls_boringssl_forced.a")
    if(EXISTS "${_ntg_trimmed}" AND EXISTS "${_ntg_forced}")
        target_link_libraries(advait_ntg PUBLIC "${_ntg_trimmed}" m z resolv ${CMAKE_DL_LIBS})
        set_property(TARGET advait_ntg PROPERTY LINK_DEPENDS "${_ntg_trimmed}")
        set_property(TARGET advait_ntg PROPERTY LINK_DEPENDS "${_ntg_forced}")
        target_link_options(advait_ntg INTERFACE
            "-Wl,--whole-archive" "${_ntg_forced}" "-Wl,--no-whole-archive")
    else()
        target_link_libraries(advait_ntg PUBLIC "${NTGCALLS_LIB}" m z resolv ${CMAKE_DL_LIBS})
    endif()
    target_compile_definitions(advait_ntg PUBLIC ADVAIT_WITH_NTGCALLS)
endif()
set_project_warnings(advait_ntg)

add_library(advait_telegram STATIC
    src/telegram/tdlib.cpp
    src/telegram/dispatcher.cpp
)
add_library(advait_mtproto STATIC
    src/mtproto/session.cpp
    src/mtproto/client.cpp
    src/mtproto/groupcall.cpp
)
add_library(advait::mtproto ALIAS advait_mtproto)
target_include_directories(advait_mtproto PUBLIC include)
target_link_libraries(advait_mtproto PUBLIC
    advait::core
    OpenSSL::Crypto
    ZLIB::ZLIB
)
set_project_warnings(advait_mtproto)
add_library(advait::telegram ALIAS advait_telegram)
target_include_directories(advait_telegram PUBLIC include)
target_link_libraries(advait_telegram PUBLIC
    advait::core
    advait::database
    advait::platforms
    advait::ntg
    ${CMAKE_DL_LIBS}
)
set_project_warnings(advait_telegram)

add_executable(mtproto_probe
    tools/mtproto_probe.cpp
    src/ntgcalls/resolver_compat.c
)
# ntgcalls first: its bundled BoringSSL ASN1_* must be defined in the
# executable so WebRTC does not interpose against system libcrypto.
target_link_libraries(mtproto_probe PRIVATE advait::ntg advait::mtproto advait::core)
set_project_warnings(mtproto_probe)

add_executable(ubot_smoke
    tools/ubot_smoke.cpp
    src/ntgcalls/resolver_compat.c
)
target_link_libraries(ubot_smoke PRIVATE advait::ntg advait::core)
set_project_warnings(ubot_smoke)


add_library(advait_modules STATIC
    src/modules/handlers.cpp
    src/modules/start.cpp
    src/modules/ping.cpp
    src/modules/help.cpp
    src/modules/help_texts.cpp
    src/modules/play.cpp
    src/modules/chat_state.cpp
    src/modules/group_admin.cpp
    src/modules/moderation.cpp
    src/modules/sticker.cpp
    src/modules/skip.cpp
    src/modules/pause.cpp
    src/modules/queue.cpp
    src/modules/stop.cpp
    src/modules/vc_control.cpp
    src/modules/admin_lists.cpp
    src/modules/admin_system.cpp
    src/modules/playlists.cpp
    src/modules/monitor.cpp
)
add_library(advait::modules ALIAS advait_modules)
target_include_directories(advait_modules PUBLIC include)
target_link_libraries(advait_modules PUBLIC advait::telegram)
set_project_warnings(advait_modules)

add_executable(advait_bot
    src/main.cpp
    src/ntgcalls/resolver_compat.c
)
target_link_libraries(advait_bot PRIVATE
    advait::core
    advait::database
    advait::platforms
    advait::ntg
    advait::telegram
    advait::modules
)
set_project_warnings(advait_bot)
set_target_properties(advait_bot PROPERTIES
    OUTPUT_NAME "advait_bot"
    BUILD_RPATH "$ORIGIN:$ORIGIN/../lib:${ADVAIT_LIB_DIR}:/usr/local/lib:/usr/lib"
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib:/usr/local/lib:/usr/lib"
)

add_custom_command(TARGET advait_bot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/locales
        $<TARGET_FILE_DIR:advait_bot>/locales
    COMMENT "Copy locale YAML next to advait_bot"
)

if(TDJSON_LIB)
    add_custom_command(TARGET advait_bot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TDJSON_LIB} $<TARGET_FILE_DIR:advait_bot>/libtdjson.so
    )
endif()

install(TARGETS advait_bot RUNTIME DESTINATION bin)
install(DIRECTORY locales DESTINATION share/advaitmusic)
