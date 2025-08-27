project(openai_rt)

find_package(CURL REQUIRED)
find_package(WEBSOCKETS REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(SNDFILE REQUIRED)

# Find json-c library
pkg_check_modules(JSON_C REQUIRED json-c)

# Find libre
find_library(RE_LIBRARY NAMES re PATHS /usr/lib /usr/local/lib)

if(NOT RE_LIBRARY)
    message(FATAL_ERROR "libre not found")
endif()

if(NOT SNDFILE_FOUND)
  message(FATAL_ERROR "libsndfile not found")
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

# Source files
set(SRCS
    openai_rt.c
    audio.c
    websocket.c
    utils.c
    calls.c
    dump.c)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

# Handle include directories
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${CURL_INCLUDE_DIRS}
    ${WEBSOCKETS_INCLUDE_DIRS}
    ${JSON_C_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIR})

# Handle library directories (only if they exist)
if(CURL_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${CURL_LIBRARY_DIRS})
endif()

if(WEBSOCKETS_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${WEBSOCKETS_LIBRARY_DIRS})
endif()

if(JSON_C_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${JSON_C_LIBRARY_DIRS})
endif()

# Link libraries - IMPORTANT: Add baresip core libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${RE_LIBRARY}
    ${CURL_LIBRARIES}
    ${WEBSOCKETS_LIBRARIES}
    ${JSON_C_LIBRARIES}
    ${SNDFILE_LIBRARIES})

# Add additional system libraries for MODULE builds
if(NOT STATIC)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ssl
        crypto
        z
        m
        pthread
    )
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wno-pedantic
    -D_GNU_SOURCE
    -DBARESIP_MODULE)

# ---- test harness ----
add_executable(openai_rt_test
    test.c
    websocket.c
    dump.c
    utils.c
    test_shim.c     # <--- add the shim
)

# Headers the test needs
target_include_directories(openai_rt_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include         # openai_rt.h, baresip.h, etc.
    ${SNDFILE_INCLUDE_DIR}              # from find_package(SNDFILE)
    ${JSON_C_INCLUDE_DIRS}              # from pkg_check_modules(JSON_C)
    ${WEBSOCKETS_INCLUDE_DIRS}
)

# Make sure the linker sees the right libraries (order matters a bit)
# Use the variables discovered by find_package / pkg_check_modules
target_link_libraries(openai_rt_test PRIVATE
    ${RE_LIBRARY}               # libre, discovered above
    baresip                     # the core baresip lib (already built in tree)
    ${WEBSOCKETS_LIBRARIES}     # libwebsockets
    ${JSON_C_LIBRARIES}         # json-c
    ${SNDFILE_LIBRARIES}        # <-- system libsndfile (sf_open, etc.)
    ssl crypto z pthread m
)

# Get the GNU prototype for pthread_timedjoin_np to silence the warning
target_compile_options(openai_rt_test PRIVATE -D_GNU_SOURCE)