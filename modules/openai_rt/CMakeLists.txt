# ============================================================================
# modules/openai_rt/CMakeLists.txt
# ============================================================================
project(openai_rt)

# Use the built-in FindCURL module instead of custom one
find_package(CURL REQUIRED)
find_package(WEBSOCKETS REQUIRED)

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(SRCS openai_rt.c)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

# Handle include directories
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    ${CURL_INCLUDE_DIRS}
    ${WEBSOCKETS_INCLUDE_DIRS})

# Handle library directories (only if they exist)
if(CURL_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${CURL_LIBRARY_DIRS})
endif()

if(WEBSOCKETS_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${WEBSOCKETS_LIBRARY_DIRS})
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${CURL_LIBRARIES}
    ${WEBSOCKETS_LIBRARIES})

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wno-pedantic
    -D_GNU_SOURCE
)