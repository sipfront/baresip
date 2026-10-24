#
# sf-voiceai: the openai_rt voice-AI implementation as a standalone sound device.
#
# Built from the module's own sources (websocket.c, openai.c, gemini.c, ai_model.c, utils.c,
# trace.c are compiled unchanged); only the baresip-bound layers are replaced: audio_pulse.c
# (PulseAudio/PipeWire in and out instead of ausrc/auplay/aufilt), calls_cli.c (tool calls and
# transcript turns become JSON events on stdout instead of baresip call operations) and main.c
# (config, lifecycle, signals) with compat.c providing the four baresip symbols the shared
# sources use (info, warning, conf_cur, conf_path_get).
#
# Needs libre (3.x, which includes rem), libwebsockets, json-c, libpulse-simple and the baresip
# headers of this checkout (types only, nothing of libbaresip is linked).
#
#   cmake -B build -S modules/openai_rt/standalone && cmake --build build
#   see README.md next to this file for the container build that matches the agent image
#
cmake_minimum_required(VERSION 3.18...4.0)
project(sf-voiceai C)

set(CMAKE_C_STANDARD 11)
set(MODDIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BARESIP_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

# libre: include dir and library via pkg-config or the usual prefixes (self-contained, so this
# directory builds on its own, without baresip's cmake/ helpers); the compile definitions from
# libre's own cmake config when it is installed
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBRE QUIET libre)
find_path(RE_INCLUDE_DIR NAMES re.h
  HINTS ${LIBRE_INCLUDEDIR} ${LIBRE_INCLUDE_DIRS} ../../../../re/include
  PATHS /usr/local/include/re /usr/include/re)
find_library(RE_LIBRARY NAMES re libre
  HINTS ${LIBRE_LIBDIR} ${LIBRE_LIBRARY_DIRS} ../../../../re/build
  PATHS /usr/local/lib64 /usr/local/lib /usr/lib64 /usr/lib)
if(NOT RE_INCLUDE_DIR OR NOT RE_LIBRARY)
  message(FATAL_ERROR "libre not found (re.h / libre.so): install libre 3.x or point LIBRE_* at it")
endif()
message(STATUS "libre: ${RE_LIBRARY}, headers ${RE_INCLUDE_DIR}")
set(RE_INCLUDE_DIRS ${RE_INCLUDE_DIR})
set(RE_LIBRARIES ${RE_LIBRARY})
find_package(re CONFIG QUIET HINTS ../../../../re/cmake /usr/local/lib/cmake/re)
pkg_check_modules(JSON_C REQUIRED json-c)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(PULSE_SIMPLE REQUIRED libpulse-simple)

add_executable(${PROJECT_NAME}
  main.c
  compat.c
  audio_pulse.c
  calls_cli.c
  ${MODDIR}/websocket.c
  ${MODDIR}/openai.c
  ${MODDIR}/gemini.c
  ${MODDIR}/ai_model.c
  ${MODDIR}/utils.c
  ${MODDIR}/trace.c)

target_include_directories(${PROJECT_NAME} PRIVATE
  ${MODDIR}
  ${BARESIP_INCLUDE}
  ${RE_INCLUDE_DIRS}
  ${JSON_C_INCLUDE_DIRS}
  ${LWS_INCLUDE_DIRS}
  ${PULSE_SIMPLE_INCLUDE_DIRS})

target_compile_definitions(${PROJECT_NAME} PRIVATE ${RE_DEFINITIONS} _GNU_SOURCE)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(${PROJECT_NAME} PRIVATE
  ${RE_LIBRARIES} ${RE_LIBS}
  ${JSON_C_LIBRARIES}
  ${LWS_LIBRARIES}
  ${PULSE_SIMPLE_LIBRARIES}
  ssl crypto z m pthread)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
