cmake_minimum_required(VERSION 3.25)
project(parallax LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ffmpeg INTERFACE)
foreach(name avformat avcodec avutil)
  find_path(FFMPEG_${name}_INCLUDE_DIR lib${name}/${name}.h)
  find_library(FFMPEG_${name}_LIBRARY ${name})
  if(NOT FFMPEG_${name}_INCLUDE_DIR OR NOT FFMPEG_${name}_LIBRARY)
    message(FATAL_ERROR
      "FFmpeg lib${name} not found. Run scripts/setup-windows.ps1, then configure with a "
      "preset (cmake --preset win-release) so the vcpkg toolchain file is used.")
  endif()
  target_include_directories(ffmpeg SYSTEM INTERFACE ${FFMPEG_${name}_INCLUDE_DIR})
  target_link_libraries(ffmpeg INTERFACE ${FFMPEG_${name}_LIBRARY})
endforeach()

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
  /W4 /permissive- /utf-8 /Zc:__cplusplus /external:W0)

add_library(demux STATIC src/demux.cc)
target_include_directories(demux PUBLIC src)
target_link_libraries(demux PUBLIC ffmpeg PRIVATE project_warnings)

add_executable(probe src/probe.cc)
target_link_libraries(probe PRIVATE demux project_warnings)
