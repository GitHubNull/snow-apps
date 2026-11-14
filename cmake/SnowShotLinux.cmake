# The Rust static archive does not propagate Cargo's native link metadata to
# CMake. The x64-linux vcpkg triplet builds FFmpeg as static archives, so the
# pkg-config lookup must request the static dependency closure.
find_package(PkgConfig REQUIRED)
set(_snow_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${SNOW_FFMPEG_ROOT}/lib/pkgconfig")
set(_snow_saved_pkg_config_argn "${PKG_CONFIG_ARGN}")
set(PKG_CONFIG_ARGN --static)
pkg_check_modules(SNOW_SHOT_FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
    libavfilter libavformat libavcodec libswresample libswscale libavutil)
# The x264-enabled build references zlib directly; the pkg-config closure does
# not always surface it after link-order reordering.
find_package(ZLIB REQUIRED)
set(PKG_CONFIG_ARGN "${_snow_saved_pkg_config_argn}")
set(ENV{PKG_CONFIG_PATH} "${_snow_saved_pkg_config_path}")
unset(_snow_saved_pkg_config_argn)
unset(_snow_saved_pkg_config_path)
# The Rust audio recorder binds libpulse through libpulse-simple; Cargo's
# native link metadata does not reach the final CMake link of the staticlib.
pkg_check_modules(SNOW_SHOT_PULSE REQUIRED IMPORTED_TARGET GLOBAL libpulse-simple libpulse)

if(TARGET snow_shot_rust_ffi_bundle)
    foreach(_snow_rust_bundle IN ITEMS
            snow_shot_rust_ffi_bundle snow_shot_full_rust_ffi_bundle)
        if(TARGET ${_snow_rust_bundle})
            target_link_libraries(${_snow_rust_bundle} INTERFACE
                PkgConfig::SNOW_SHOT_FFMPEG PkgConfig::SNOW_SHOT_PULSE ZLIB::ZLIB)
            if(X11_xcb_FOUND)
                # x11rb's FD-passing SHM capture links libxcb; interface
                # libraries follow the Rust archive so --as-needed keeps it.
                target_link_libraries(${_snow_rust_bundle} INTERFACE X11::xcb)
            endif()
        endif()
    endforeach()
    unset(_snow_rust_bundle)
else()
    target_link_libraries(snow_recording_c INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
    target_link_libraries(snow_capture_c INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
    if(X11_xcb_FOUND)
        target_link_libraries(snow_recording_c INTERFACE X11::xcb)
        target_link_libraries(snow_capture_c INTERFACE X11::xcb)
    endif()
endif()
target_link_libraries(snow_shot PRIVATE PkgConfig::SNOW_SHOT_FFMPEG PkgConfig::SNOW_SHOT_PULSE ZLIB::ZLIB)
