cmake_minimum_required(VERSION 3.16)
project(obs-vlc-by-kubo VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Check if building within OBS Studio source tree
if(TARGET OBS::libobs)
  set(STANDALONE_BUILD OFF)
elseif(TARGET libobs)
  add_library(OBS::libobs ALIAS libobs)
  set(STANDALONE_BUILD OFF)
else()
  set(STANDALONE_BUILD ON)
endif()

# Determine plugin target name
set(TARGET_NAME "vlc-video")

add_library(${TARGET_NAME} MODULE
  vlc-video-plugin.c
  vlc-video-plugin.h
  vlc-video-source.c
  mxf-audio-pairing.c
  mxf-audio-pairing.h
  mxf-timeline-aligner.c
  mxf-timeline-aligner.h
)

target_include_directories(${TARGET_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/obs-headers"
)

# LibVLC headers
find_path(LibVLC_INCLUDE_DIR
  NAMES vlc/libvlc.h
  PATHS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    /usr/include
    /usr/local/include
)
if(LibVLC_INCLUDE_DIR)
  target_include_directories(${TARGET_NAME} PRIVATE "${LibVLC_INCLUDE_DIR}")
endif()

if(NOT STANDALONE_BUILD)
  target_link_libraries(${TARGET_NAME} PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)
  if(OS_WINDOWS)
    configure_file(cmake/windows/obs-module.rc.in vlc-video.rc)
    target_sources(${TARGET_NAME} PRIVATE vlc-video.rc)
  endif()
  if(COMMAND set_target_properties_obs)
    set_target_properties_obs(${TARGET_NAME} PROPERTIES FOLDER plugins PREFIX "")
  else()
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
  endif()
else()
  # Standalone build
  # Check for system libobs first
  find_package(libobs QUIET)
  if(TARGET OBS::libobs)
    target_link_libraries(${TARGET_NAME} PRIVATE OBS::libobs)
  else()
    # Try finding system headers
    find_path(LIBOBS_SYS_INCLUDE_DIR
      NAMES obs/obs-module.h obs-module.h
      PATHS /usr/include/obs /usr/local/include/obs
    )
    if(LIBOBS_SYS_INCLUDE_DIR)
      target_include_directories(${TARGET_NAME} PRIVATE "${LIBOBS_SYS_INCLUDE_DIR}")
    endif()

    if(WIN32)
      target_compile_definitions(${TARGET_NAME} PRIVATE _WIN32 UNICODE _UNICODE LIBOBS_EXPORTS)
      # Locate obs.dll / obs.lib if path provided, e.g. -DOBS_DIR="C:/Program Files/obs-studio"
      if(OBS_DIR)
        target_link_directories(${TARGET_NAME} PRIVATE "${OBS_DIR}/bin/64bit" "${OBS_DIR}/lib")
      else()
        target_link_directories(${TARGET_NAME} PRIVATE "C:/Program Files/obs-studio/bin/64bit")
      endif()
      target_link_libraries(${TARGET_NAME} PRIVATE obs)
      if(MINGW)
        target_link_options(${TARGET_NAME} PRIVATE -static-libgcc)
      endif()
    elseif(APPLE)
      # On macOS, runtime dynamic lookup allows plugins to load cleanly into OBS host
      target_link_options(${TARGET_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
    else()
      # Linux
      target_link_libraries(${TARGET_NAME} PRIVATE obs)
    endif()
  endif()

  set_target_properties(${TARGET_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "vlc-video"
  )
endif()
