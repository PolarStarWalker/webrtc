cmake_minimum_required(VERSION 4.0)
project(webrtc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-Wall -Wextra -Wvla -Wno-psabi -freflection)
link_libraries(-static-libstdc++ -static-libgcc -lstdc++exp)

set(BROADCASTER_DEBUG_FLAGS "-ggdb -O0 -fno-lto -fno-omit-frame-pointer -fsanitize=undefined,address,leak,bounds")
set(BROADCASTER_RELEASE_FLAGS "-O3 -flto=auto -fwhole-program -fhardened")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${BROADCASTER_DEBUG_FLAGS} -Werror")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${BROADCASTER_RELEASE_FLAGS}")


cmake_host_system_information(RESULT HOST_CPU QUERY OS_PLATFORM)
if (${HOST_CPU} STREQUAL x86_64)
    set(VCPKG_TARGET_TRIPLET "x64-linux")
    add_compile_options(-fcf-protection)
elseif (${HOST_CPU} STREQUAL aarch64)
    set(VCPKG_TARGET_TRIPLET "arm64-linux")
    add_compile_options(-mbranch-protection=standard)
else ()
    message(FATAL_ERROR "INVALID PLATFORM ${HOST_CPU}")
endif ()

include("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake")

# ── GStreamer через pkg-config ────────────────────────────────
find_package(PkgConfig REQUIRED)

# Ядро GStreamer + SDP + WebRTC
pkg_search_module(glib REQUIRED IMPORTED_TARGET glib-2.0)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_search_module(gstreamer_base REQUIRED IMPORTED_TARGET gstreamer-base-1.0)
pkg_search_module(gstreamer_video REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
pkg_search_module(gstreamer_app REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
pkg_search_module(gstreamer_sdp REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0)
pkg_search_module(gstreamer_webrtc REQUIRED IMPORTED_TARGET gstreamer-webrtc-1.0)

# Статические плагины
pkg_search_module(gstcoreelements REQUIRED IMPORTED_TARGET gstcoreelements)
pkg_search_module(gstvideo4linux2 REQUIRED IMPORTED_TARGET gstvideo4linux2)
pkg_search_module(gstvideoconvertscale REQUIRED IMPORTED_TARGET gstvideoconvertscale)
pkg_search_module(gstvideoparsersbad REQUIRED IMPORTED_TARGET gstvideoparsersbad)
pkg_search_module(gstrtp REQUIRED IMPORTED_TARGET gstrtp)
pkg_search_module(gstrtpmanager REQUIRED IMPORTED_TARGET gstrtpmanager)
pkg_search_module(gstwebrtc REQUIRED IMPORTED_TARGET gstwebrtc)
pkg_search_module(gstnice REQUIRED IMPORTED_TARGET gstnice)
pkg_search_module(gstdtls REQUIRED IMPORTED_TARGET gstdtls)
pkg_search_module(gstsrtp REQUIRED IMPORTED_TARGET gstsrtp)
pkg_search_module(gstudp REQUIRED IMPORTED_TARGET gstudp)
pkg_search_module(gstapp REQUIRED IMPORTED_TARGET gstapp)
pkg_search_module(gstlibav REQUIRED IMPORTED_TARGET gstlibav)
pkg_search_module(gstdebug REQUIRED IMPORTED_TARGET gstdebug)


# ── Группировка GStreamer-зависимостей ────────────────────────
add_library(gstreamer_static_plugins INTERFACE)
target_link_libraries(gstreamer_static_plugins INTERFACE
        PkgConfig::glib
        PkgConfig::gstreamer
        PkgConfig::gstreamer_base
        PkgConfig::gstreamer_video
        PkgConfig::gstreamer_app
        PkgConfig::gstreamer_sdp
        PkgConfig::gstreamer_webrtc
        PkgConfig::gstcoreelements
        PkgConfig::gstvideo4linux2
        PkgConfig::gstvideoconvertscale
        PkgConfig::gstvideoparsersbad
        PkgConfig::gstrtp
        PkgConfig::gstrtpmanager
        PkgConfig::gstwebrtc
        PkgConfig::gstnice
        PkgConfig::gstdtls
        PkgConfig::gstsrtp
        PkgConfig::gstudp
        PkgConfig::gstapp
        PkgConfig::gstlibav
        PkgConfig::gstdebug
)

find_package(Drogon CONFIG REQUIRED)

add_executable(webrtc)
target_link_libraries(webrtc PRIVATE
        gstreamer_static_plugins
        Drogon::Drogon
)

find_package(glfw3 CONFIG REQUIRED)
find_package(GLEW CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)

add_executable(client)
target_link_libraries(client PRIVATE
        gstreamer_static_plugins
        imgui::imgui
        glfw
        GLEW::GLEW
        OpenGL::GL
        nlohmann_json::nlohmann_json
        ixwebsocket::ixwebsocket
)

add_subdirectory(src)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/index.html"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")