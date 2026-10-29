# An application that consumes DAF the way Dusk Studio does: add_subdirectory, daf__add_dgl_opengl3 and
# DGL's public API only. Configured with -DDGL_BACKEND=both it checks that a runtime-backend DGL links into
# an application and opens a window on whichever backend the environment offers.
cmake_minimum_required(VERSION 3.15)
project(DafRuntimeBackendConsumer LANGUAGES CXX)
enable_testing()
get_filename_component(DAF_ROOT "../.." ABSOLUTE)
set(DAF_LIBRARIES OFF CACHE BOOL "" FORCE)
set(DAF_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DAF_WIDGETS_GALLERY OFF CACHE BOOL "" FORCE)
add_subdirectory("${DAF_ROOT}" daf)
daf__add_dgl_opengl3(TRUE TRUE FALSE)
add_executable(runtime-backend-consumer "${DAF_ROOT}/tests/RuntimeBackendConsumer.cpp")
target_compile_features(runtime-backend-consumer PRIVATE cxx_std_11)
target_compile_definitions(runtime-backend-consumer PRIVATE DONT_SET_USING_DGL_NAMESPACE)
target_include_directories(runtime-backend-consumer PRIVATE "${DAF_ROOT}")
target_link_libraries(runtime-backend-consumer PRIVATE
    dgl-opengl3 dgl-opengl3-definitions dgl-system-libs-definitions)
add_test(NAME runtime-backend-consumer COMMAND runtime-backend-consumer)
set_tests_properties(runtime-backend-consumer PROPERTIES TIMEOUT 30)
