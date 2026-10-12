cmake_minimum_required(VERSION 3.25)
project(image_slicer LANGUAGES CXX)

include(FetchContent)

# Pin Glaze before saucer/CPM declare it (FetchContent uses the first declaration).
# Glaze's default SYSTEM includes can lose to -I/usr/local/include, so use
# ordinary include paths and explicitly prioritize them for both consumers.
set(CPM_DOWNLOAD_glaze ON)
set(CPM_USE_LOCAL_PACKAGES OFF)
set(CPM_LOCAL_PACKAGES_ONLY OFF)
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
set(glaze_INCLUDES_WITH_SYSTEM OFF CACHE BOOL "Use ordinary Glaze includes" FORCE)

FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v5.7.2
    GIT_SHALLOW TRUE)

FetchContent_Declare(saucer
    GIT_REPOSITORY https://github.com/saucer/saucer.git
    GIT_TAG v7.0.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(saucer)

FetchContent_GetProperties(glaze SOURCE_DIR slicer_glaze_source)
if(NOT slicer_glaze_source OR
   NOT EXISTS "${slicer_glaze_source}/include/glaze/core/opts.hpp")
    message(FATAL_ERROR "Pinned Glaze source was not populated. Use a fresh build directory.")
endif()
get_target_property(slicer_glaze_target glaze::glaze ALIASED_TARGET)
if(NOT slicer_glaze_target)
    message(FATAL_ERROR "Expected the source-built glaze::glaze alias target.")
endif()
set_property(TARGET "${slicer_glaze_target}" PROPERTY SYSTEM FALSE)
set_property(TARGET "${slicer_glaze_target}" PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
set(slicer_glaze_include "${slicer_glaze_source}/include")
target_include_directories(saucer BEFORE PUBLIC
    "$<BUILD_INTERFACE:${slicer_glaze_include}>")
message(STATUS "Image Slicer: prioritized Glaze 5.7.2 headers: ${slicer_glaze_include}")

FetchContent_Declare(desktop
    GIT_REPOSITORY https://github.com/saucer/desktop.git
    GIT_TAG v4.2.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(desktop)

add_executable(slicer slicer.cpp)
target_compile_features(slicer PRIVATE cxx_std_23)
set_target_properties(slicer PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(slicer BEFORE PRIVATE "${slicer_glaze_include}")
target_link_libraries(slicer PRIVATE saucer::saucer saucer::desktop)
