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
set(PKG_CONFIG_ARGN "${_snow_saved_pkg_config_argn}")
set(ENV{PKG_CONFIG_PATH} "${_snow_saved_pkg_config_path}")
unset(_snow_saved_pkg_config_argn)
unset(_snow_saved_pkg_config_path)

if(TARGET snow_shot_rust_ffi_bundle)
    foreach(_snow_rust_bundle IN ITEMS
            snow_shot_rust_ffi_bundle snow_shot_full_rust_ffi_bundle)
        if(TARGET ${_snow_rust_bundle})
            target_link_libraries(${_snow_rust_bundle} INTERFACE
                PkgConfig::SNOW_SHOT_FFMPEG)
        endif()
    endforeach()
    unset(_snow_rust_bundle)
else()
    target_link_libraries(snow_recording_c INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
    target_link_libraries(snow_capture_c INTERFACE PkgConfig::SNOW_SHOT_FFMPEG)
endif()
target_link_libraries(snow_shot PRIVATE PkgConfig::SNOW_SHOT_FFMPEG)
