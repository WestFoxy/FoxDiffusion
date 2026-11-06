cmake_minimum_required(VERSION 3.24)

project(FoxDiffusion VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the app and its bundled targets independent of the VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Bootstrap missing upstream checkouts. Existing checkouts are managed with Git.
function(use_local_dependency name)
    string(TOUPPER "${name}" upper_name)
    set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}/lib/${name}")
    if(EXISTS "${source_dir}/.git")
        set(FETCHCONTENT_SOURCE_DIR_${upper_name} "${source_dir}" PARENT_SCOPE)
    endif()
endfunction()


set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/glfw"
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    # Official GLFW 3.4 tag, pinned to its verified commit.
    GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c
    GIT_SHALLOW TRUE
)
use_local_dependency(glfw)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui"
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    # Official Dear ImGui v1.92.9b-docking tag, pinned for reproducibility.
    GIT_TAG 9acdfbf46810c0c74ab281ce04122c4149ae8bd1
    GIT_SHALLOW TRUE
)
use_local_dependency(imgui)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    stb
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/stb"
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
    GIT_SHALLOW TRUE
)
use_local_dependency(stb)
FetchContent_MakeAvailable(stb)

# Engine and every GGML backend are part of the executable.
set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SD_BUILD_SHARED_GGML_LIB OFF CACHE BOOL "" FORCE)
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_WEBP OFF CACHE BOOL "" FORCE)
set(SD_WEBM OFF CACHE BOOL "" FORCE)
set(SD_CUDA ON CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_STATIC ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(CMAKE_CUDA_RUNTIME_LIBRARY Static)
find_package(CUDAToolkit 12.8 REQUIRED)
FetchContent_Declare(
    stable-diffusion.cpp
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/stable-diffusion.cpp"
    GIT_REPOSITORY https://github.com/leejet/stable-diffusion.cpp.git
    GIT_TAG c678dfe704a2230342376b46add9c8ca736a653d
    GIT_SUBMODULES ggml
)
use_local_dependency(stable-diffusion.cpp)
FetchContent_MakeAvailable(stable-diffusion.cpp)
set(engine_include "${stable-diffusion.cpp_SOURCE_DIR}/include")

# The downloadable runtime is pinned to CUDA 12.8 Update 1.
if(NOT CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8" OR
   NOT CUDAToolkit_VERSION VERSION_LESS "12.9")
    message(FATAL_ERROR "Fox Diffusion requires CUDA Toolkit 12.8 for its pinned portable runtime")
endif()
add_library(fox-engine INTERFACE)
target_link_libraries(fox-engine INTERFACE stable-diffusion delayimp winhttp bcrypt)
target_link_options(fox-engine INTERFACE /DELAYLOAD:cublas64_12.dll /DELAYLOAD:cublasLt64_12.dll /DELAYLOAD:nvcuda.dll)
target_sources(fox-engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/cuda_runtime.cpp)

find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

add_executable(fox-diffusion WIN32
    src/main.cpp
    src/app.cpp
    src/app.hpp
    src/image_io.cpp
    src/image_io.hpp
    src/backend_process.cpp
    src/backend_process.hpp
    src/raster.cpp
    src/img2img_pipeline.cpp
    src/img2img_editor.cpp
    src/parameters.cpp
)
target_include_directories(fox-diffusion PRIVATE ${stb_SOURCE_DIR} ${engine_include})
target_link_libraries(fox-diffusion PRIVATE imgui_lib fox-engine)

if(WIN32)
    target_compile_definitions(fox-diffusion PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(fox-diffusion PRIVATE ole32 shell32 gdi32)
endif()

set_target_properties(fox-diffusion PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:fox-diffusion>"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(cuda-runtime-smoke tests/cuda_runtime_smoke.cpp)
    target_include_directories(cuda-runtime-smoke PRIVATE src)
    target_compile_definitions(cuda-runtime-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(cuda-runtime-smoke PRIVATE fox-engine)
    add_test(NAME cuda-deferred-startup COMMAND cuda-runtime-smoke)
    add_executable(img2img-tests tests/img2img_tests.cpp src/raster.cpp src/image_io.cpp src/img2img_pipeline.cpp src/parameters.cpp)
    target_include_directories(img2img-tests PRIVATE src ${stb_SOURCE_DIR})
    target_compile_definitions(img2img-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(img2img-tests PRIVATE gdi32)
    add_test(NAME img2img-processing COMMAND img2img-tests)
    add_executable(img2img-smoke tests/img2img_smoke.cpp src/raster.cpp src/image_io.cpp src/img2img_pipeline.cpp src/backend_process.cpp src/parameters.cpp)
    target_include_directories(img2img-smoke PRIVATE src ${stb_SOURCE_DIR} ${engine_include})
    target_compile_definitions(img2img-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(img2img-smoke PRIVATE fox-engine gdi32)
endif()
