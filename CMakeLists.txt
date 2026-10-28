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

set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "Vcpkg triplet")
include("/opt/vcpkg/scripts/buildsystems/vcpkg.cmake")

# --- GStreamer через pkg-config ---
find_package(PkgConfig REQUIRED)

# Базовые библиотеки GStreamer
pkg_search_module(glib REQUIRED IMPORTED_TARGET glib-2.0)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_search_module(gstreamer-base REQUIRED IMPORTED_TARGET gstreamer-base-1.0)
pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0)

# КРИТИЧНО для статической сборки: регистрация статических плагинов
pkg_search_module(gstcoreelements REQUIRED IMPORTED_TARGET gstcoreelements)
pkg_search_module(gstvideo4linux2 REQUIRED IMPORTED_TARGET gstvideo4linux2)
pkg_search_module(gstvideoparsersbad REQUIRED IMPORTED_TARGET gstvideoparsersbad)
pkg_search_module(gstrtp REQUIRED IMPORTED_TARGET gstrtp)
pkg_search_module(gstrtpmanager REQUIRED IMPORTED_TARGET gstrtpmanager)
pkg_search_module(gstwebrtc REQUIRED IMPORTED_TARGET gstwebrtc)
pkg_search_module(gstsrtp REQUIRED IMPORTED_TARGET gstsrtp)
pkg_search_module(gstdtls REQUIRED IMPORTED_TARGET gstdtls)
pkg_search_module(gstdebug REQUIRED IMPORTED_TARGET gstdebug)
pkg_search_module(gstapp REQUIRED IMPORTED_TARGET gstapp)
pkg_search_module(gstvideoconvertscale REQUIRED IMPORTED_TARGET gstvideoconvertscale)
pkg_search_module(gstnice REQUIRED IMPORTED_TARGET gstnice)
pkg_search_module(gstudp REQUIRED IMPORTED_TARGET gstudp)

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)

# Интерфейсный таргет для группировки всех GStreamer-зависимостей
add_library(gstreamer_deps INTERFACE)
target_link_libraries(gstreamer_deps INTERFACE
        PkgConfig::glib
        PkgConfig::gstreamer
        PkgConfig::gstreamer-base
        PkgConfig::gstreamer-video
        PkgConfig::gstreamer-app
        PkgConfig::gstcoreelements
        PkgConfig::gstvideo4linux2
        PkgConfig::gstvideoparsersbad
        PkgConfig::gstrtp
        PkgConfig::gstrtpmanager
        PkgConfig::gstwebrtc
        PkgConfig::gstsrtp
        PkgConfig::gstdtls
        PkgConfig::gstdebug
        PkgConfig::gstapp
        PkgConfig::gstvideoconvertscale
        PkgConfig::gstnice
        PkgConfig::gstudp
)

add_executable(webrtc)
target_link_libraries(webrtc PRIVATE
        gstreamer_deps
        Boost::system
        nlohmann_json::nlohmann_json
)

add_subdirectory(src)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/index.html"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")