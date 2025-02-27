cmake_minimum_required(VERSION 3.20)

project(slang-rhi)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

# Add the cmake directory to the module path.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakeDependentOption)
include(FetchPackage)
include(DetermineTargetArchitecture)

# Determine the target architecture we build for.
# CMAKE_SYSTEM_PROCESSOR is not a reliable way to determine the target architecture.
determine_target_architecture(SLANG_RHI_ARCHITECTURE)

# set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)

# Check if this project is the master cmake project (i.e. not included via add_subdirectory).
set(SLANG_RHI_MASTER_PROJECT OFF)

if(${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
    set(SLANG_RHI_MASTER_PROJECT ON)
endif()

if(NOT DEFINED SLANG_RHI_BINARY_DIR)
    if(CMAKE_CONFIGURATION_TYPES)
        set(SLANG_RHI_BINARY_DIR ${CMAKE_BINARY_DIR}/$<CONFIG>)
    else()
        set(SLANG_RHI_BINARY_DIR ${CMAKE_BINARY_DIR})
    endif()
endif()

# Configuration options
option(SLANG_RHI_BUILD_SHARED "Build shared library" OFF)
option(SLANG_RHI_BUILD_TESTS "Build tests" ON)
option(SLANG_RHI_BUILD_EXAMPLES "Build examples" ON)

# Determine available backends
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(SLANG_RHI_HAS_D3D11 ON)
    set(SLANG_RHI_HAS_D3D12 ON)
    set(SLANG_RHI_HAS_VULKAN ON)
    set(SLANG_RHI_HAS_METAL OFF)
    set(SLANG_RHI_HAS_CUDA ON)
    set(SLANG_RHI_HAS_WGPU ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(SLANG_RHI_HAS_D3D11 OFF)
    set(SLANG_RHI_HAS_D3D12 OFF)
    set(SLANG_RHI_HAS_VULKAN ON)
    set(SLANG_RHI_HAS_METAL OFF)
    set(SLANG_RHI_HAS_CUDA ON)
    set(SLANG_RHI_HAS_WGPU ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(SLANG_RHI_HAS_D3D11 OFF)
    set(SLANG_RHI_HAS_D3D12 OFF)
    set(SLANG_RHI_HAS_VULKAN ON)
    set(SLANG_RHI_HAS_METAL ON)
    set(SLANG_RHI_HAS_CUDA OFF)
    set(SLANG_RHI_HAS_WGPU ON)
endif()

# Backend options
option(SLANG_RHI_ENABLE_CPU "Enable CPU backend" ON)
cmake_dependent_option(SLANG_RHI_ENABLE_D3D11 "Enable D3D11 backend" ON "SLANG_RHI_HAS_D3D11" OFF)
cmake_dependent_option(SLANG_RHI_ENABLE_D3D12 "Enable D3D12 backend" ON "SLANG_RHI_HAS_D3D12" OFF)
cmake_dependent_option(SLANG_RHI_ENABLE_VULKAN "Enable Vulkan backend" ON "SLANG_RHI_HAS_VULKAN" OFF)
cmake_dependent_option(SLANG_RHI_ENABLE_METAL "Enable Metal backend" ON "SLANG_RHI_HAS_METAL" OFF)
cmake_dependent_option(SLANG_RHI_ENABLE_CUDA "Enable CUDA backend" ON "SLANG_RHI_HAS_CUDA" OFF)
cmake_dependent_option(SLANG_RHI_ENABLE_WGPU "Enable WebGPU backend" ON "SLANG_RHI_HAS_WGPU" OFF)
option(SLANG_RHI_FETCH_DXC "Fetch dxc (DirectX Shader Compiler)" ON)
option(SLANG_RHI_FETCH_AGILITY_SDK "Fetch Agility SDK" ON)

if(SLANG_RHI_BUILD_SHARED)
    add_library(slang-rhi SHARED)
else()
    add_library(slang-rhi STATIC)
endif()

set(SLANG_RHI_COPY_FILES "")

# Function to copy a file to the binary output directory
macro(copy_file IN_FILE OUT_DIR)
    if(EXISTS ${IN_FILE})
        get_filename_component(FILENAME ${IN_FILE} NAME)
        set(OUT_FILE_1 "${CMAKE_CURRENT_BINARY_DIR}/${OUT_DIR}/${FILENAME}")
        set(OUT_FILE_2 "${SLANG_RHI_BINARY_DIR}/${OUT_DIR}/${FILENAME}")

        if (UNIX)
            add_custom_command(
                OUTPUT ${OUT_FILE_2} DEPENDS ${IN_FILE} ${ARGN}
                COMMAND ${CMAKE_COMMAND} -E copy ${IN_FILE} ${OUT_FILE_2}
                COMMENT "Copying ${FILENAME}"
            )
            list(APPEND SLANG_RHI_COPY_FILES ${OUT_FILE_2})
        else()
            # add_custom_command does not support generator expressions in OUTPUT argument
            # make a dummy copy and then depend on that
            add_custom_command(
                OUTPUT ${OUT_FILE_1} DEPENDS ${IN_FILE} ${ARGN}
                COMMAND ${CMAKE_COMMAND} -E copy ${IN_FILE} ${OUT_FILE_1}
                COMMAND ${CMAKE_COMMAND} -E copy ${IN_FILE} ${OUT_FILE_2}
                COMMENT "Copying ${FILENAME}"
            )
            list(APPEND SLANG_RHI_COPY_FILES ${OUT_FILE_1})
        endif()
    endif()
endmacro()

# Fetch slang binary release if this is the master project
if(SLANG_RHI_MASTER_PROJECT)
    set(SLANG_VERSION "2024.14")
    set(SLANG_URL "https://github.com/shader-slang/slang/releases/download/v${SLANG_VERSION}/slang-${SLANG_VERSION}")

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if(SLANG_RHI_ARCHITECTURE MATCHES "x86_64")
            set(SLANG_URL "${SLANG_URL}-windows-x86_64.zip")
        elseif(SLANG_RHI_ARCHITECTURE MATCHES "aarch64|arm64")
            set(SLANG_URL "${SLANG_URL}-windows-aarch64.zip")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(SLANG_RHI_ARCHITECTURE MATCHES "x86_64")
            set(SLANG_URL "${SLANG_URL}-linux-x86_64-glibc-2.17.tar.gz")
        elseif(SLANG_RHI_ARCHITECTURE MATCHES "aarch64|arm64")
            set(SLANG_URL "${SLANG_URL}-linux-aarch64.tar.gz")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_APPLE_SILICON_PROCESSOR MATCHES "x86_64")
            set(SLANG_URL "${SLANG_URL}-macos-x86_64.zip")
        else()
            set(SLANG_URL "${SLANG_URL}-macos-aarch64.zip")
        endif()
    endif()

    FetchPackage(slang URL ${SLANG_URL})
    set(SLANG_RHI_SLANG_INCLUDE_DIR ${slang_SOURCE_DIR}/include)
    set(SLANG_RHI_SLANG_BINARY_DIR ${slang_SOURCE_DIR})
endif()

set(SLANG_RHI_SLANG_INCLUDE_DIR ${SLANG_RHI_SLANG_INCLUDE_DIR} CACHE STRING "Slang include directory")
set(SLANG_RHI_SLANG_BINARY_DIR ${SLANG_RHI_SLANG_BINARY_DIR} CACHE STRING "Slang binary directory")

# Use the variables from the cache.
unset(SLANG_RHI_SLANG_INCLUDE_DIR)
unset(SLANG_RHI_SLANG_BINARY_DIR)

# Fetch dxc
if(SLANG_RHI_FETCH_DXC AND SLANG_RHI_ENABLE_D3D12 AND (CMAKE_SYSTEM_NAME STREQUAL "Windows"))
    set(DXC_VERSION "1.8.2407")
    set(DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2407/dxc_2024_07_31.zip")
    FetchPackage(dxc URL ${DXC_URL})
    if(SLANG_RHI_ARCHITECTURE MATCHES "x86_64")
        copy_file(${dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll .)
        copy_file(${dxc_SOURCE_DIR}/bin/x64/dxil.dll .)
    elseif(SLANG_RHI_ARCHITECTURE MATCHES "aarch64|arm64")
        copy_file(${dxc_SOURCE_DIR}/bin/arm64/dxcompiler.dll .)
        copy_file(${dxc_SOURCE_DIR}/bin/arm64/dxil.dll .)
    endif()
endif()

# Fetch Agility SDK
if(SLANG_RHI_FETCH_AGILITY_SDK AND SLANG_RHI_ENABLE_D3D12 AND (CMAKE_SYSTEM_NAME STREQUAL "Windows"))
    set(AGILITY_SDK_VERSION "1.614.1")
    FetchPackage(agility_sdk URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${AGILITY_SDK_VERSION}")
    if(SLANG_RHI_ARCHITECTURE MATCHES "x86_64")
        copy_file(${agility_sdk_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll D3D12)
        copy_file(${agility_sdk_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll D3D12)
    elseif(SLANG_RHI_ARCHITECTURE MATCHES "aarch64|arm64")
        copy_file(${agility_sdk_SOURCE_DIR}/build/native/bin/arm64/D3D12Core.dll D3D12)
        copy_file(${agility_sdk_SOURCE_DIR}/build/native/bin/arm64/d3d12SDKLayers.dll D3D12)
    endif()
    add_library(slang-rhi-agility-sdk INTERFACE)
    target_include_directories(slang-rhi-agility-sdk INTERFACE ${agility_sdk_SOURCE_DIR}/build/native/include)
    target_link_libraries(slang-rhi PRIVATE slang-rhi-agility-sdk)
    set(SLANG_RHI_ENABLE_AGILITY_SDK ON)
else()
    set(SLANG_RHI_ENABLE_AGILITY_SDK OFF)
endif()

# Fetch NVAPI
if((SLANG_RHI_ENABLE_D3D11 OR SLANG_RHI_ENABLE_D3D12) AND (CMAKE_SYSTEM_NAME STREQUAL "Windows") AND (SLANG_RHI_ARCHITECTURE MATCHES "x86_64"))
    # NVAPI
    set(NVAPI_VERSION "4ba3384657149d63aa193f5a34e20efe1e42bf31")
    FetchPackage(nvapi URL "https://github.com/NVIDIA/nvapi/archive/${NVAPI_VERSION}.zip")
    set(NVAPI_ROOT_DIR ${nvapi_SOURCE_DIR})

    add_library(slang-rhi-nvapi INTERFACE)
    target_include_directories(slang-rhi-nvapi INTERFACE ${NVAPI_ROOT_DIR})
    target_link_libraries(slang-rhi-nvapi INTERFACE ${NVAPI_ROOT_DIR}/amd64/nvapi64.lib)
    target_link_libraries(slang-rhi PRIVATE slang-rhi-nvapi)
    set(SLANG_RHI_ENABLE_NVAPI ON)
else()
    set(SLANG_RHI_ENABLE_NVAPI OFF)
endif()

# Fetch and setup Google Dawn library (WebGPU implementation)
if(SLANG_RHI_ENABLE_WGPU)
    set(DAWN_VERSION "131.0.6738.0")

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(DAWN_OS "windows")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(DAWN_OS "linux")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(DAWN_OS "macos")
    endif()

    if(SLANG_RHI_ARCHITECTURE MATCHES "x86_64")
        set(DAWN_ARCH "x86_64")
    elseif(SLANG_RHI_ARCHITECTURE MATCHES "aarch64|arm64")
        set(DAWN_ARCH "aarch64")
    else()
        message(FATAL_ERROR "Unsupported processor architecture")
    endif()

    set(DAWN_URL "https://github.com/shader-slang/webgpu-dawn-binaries/releases/download/v${DAWN_VERSION}/webgpu-dawn-${DAWN_VERSION}-${DAWN_OS}-${DAWN_ARCH}.zip")
    FetchPackage(dawn URL ${DAWN_URL})
    set(SLANG_RHI_DAWN_INCLUDE_DIR ${dawn_SOURCE_DIR}/include CACHE STRING "Dawn include directory")
    set(SLANG_RHI_DAWN_LIB_DIR ${dawn_SOURCE_DIR}/lib CACHE STRING "Dawn lib directory")
    set(SLANG_RHI_DAWN_BIN_DIR ${dawn_SOURCE_DIR}/bin CACHE STRING "Dawn bin directory")

    # Use the variables from the cache.
    unset(SLANG_RHI_DAWN_INCLUDE_DIR)
    unset(SLANG_RHI_DAWN_LIB_DIR)
    unset(SLANG_RHI_DAWN_BIN_DIR)

    add_library(slang-rhi-dawn SHARED IMPORTED GLOBAL)

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(slang-rhi-dawn PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_DAWN_INCLUDE_DIR}
            IMPORTED_IMPLIB ${SLANG_RHI_DAWN_LIB_DIR}/dawn.lib
            IMPORTED_LOCATION ${SLANG_RHI_DAWN_BIN_DIR}/dawn.dll
        )
        copy_file(${SLANG_RHI_DAWN_BIN_DIR}/dawn.dll .)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(slang-rhi-dawn PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_DAWN_INCLUDE_DIR}
            IMPORTED_LOCATION ${SLANG_RHI_DAWN_LIB_DIR}64/libdawn.so
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set_target_properties(slang-rhi-dawn PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_DAWN_INCLUDE_DIR}
            IMPORTED_LOCATION ${SLANG_RHI_DAWN_LIB_DIR}/libdawn.dylib
        )
    endif()

    target_link_libraries(slang-rhi PRIVATE slang-rhi-dawn)
endif()


if(SLANG_RHI_ENABLE_CUDA)
    # Find Optix
    set(Optix_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/optix CACHE STRING "Optix root directory")
    find_package(OptiX)
    set(SLANG_RHI_ENABLE_OPTIX ${OptiX_FOUND})
    if(${SLANG_RHI_ENABLE_OPTIX})
        add_library(slang-rhi-optix INTERFACE)
        target_include_directories(slang-rhi-optix INTERFACE ${OptiX_INCLUDE_DIRS})
        target_link_libraries(slang-rhi PRIVATE slang-rhi-optix)
    endif()
else()
    set(SLANG_RHI_ENABLE_OPTIX OFF)
endif()

# Fetch glfw
if(SLANG_RHI_BUILD_EXAMPLES)
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG master)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-assume -Wno-switch")
endif()

if(SLANG_RHI_MASTER_PROJECT)
    add_library(slang SHARED IMPORTED GLOBAL)

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(slang PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_SLANG_INCLUDE_DIR}
            IMPORTED_IMPLIB ${SLANG_RHI_SLANG_BINARY_DIR}/lib/slang.lib
            IMPORTED_LOCATION ${SLANG_RHI_SLANG_BINARY_DIR}/bin/slang.dll
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(slang PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_SLANG_INCLUDE_DIR}
            IMPORTED_LOCATION ${SLANG_RHI_SLANG_BINARY_DIR}/lib/libslang.so
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set_target_properties(slang PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SLANG_RHI_SLANG_INCLUDE_DIR}
            IMPORTED_LOCATION ${SLANG_RHI_SLANG_BINARY_DIR}/lib/libslang.dylib
        )
    endif()

    target_link_libraries(slang-rhi PUBLIC slang)

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        copy_file(${SLANG_RHI_SLANG_BINARY_DIR}/bin/slang.dll .)
        copy_file(${SLANG_RHI_SLANG_BINARY_DIR}/bin/slang-glslang.dll .)
        copy_file(${SLANG_RHI_SLANG_BINARY_DIR}/bin/slang-llvm.dll .)
        copy_file(${SLANG_RHI_SLANG_BINARY_DIR}/bin/slang-rt.dll .)
    endif()
else()
    target_include_directories(slang-rhi PUBLIC ${SLANG_RHI_SLANG_INCLUDE_DIR})
endif()

# target_compile_options(slang-rhi PRIVATE $<$<CXX_COMPILER_ID:AppleClang>:-Wall>) # -Wextra -Wpedantic -Wno-unused-parameter -Wno-missing-field-initializer

target_sources(slang-rhi PRIVATE
    src/enum-strings.cpp
    src/flag-combiner.cpp
    src/immediate-device.cpp
    src/resource-desc-utils.cpp
    src/rhi.cpp
    src/rhi-shared.cpp
    src/core/assert.cpp
    src/core/blob.cpp
    src/core/platform.cpp
    src/debug-layer/debug-buffer.cpp
    src/debug-layer/debug-command-buffer.cpp
    src/debug-layer/debug-command-encoder.cpp
    src/debug-layer/debug-command-queue.cpp
    src/debug-layer/debug-device.cpp
    src/debug-layer/debug-fence.cpp
    src/debug-layer/debug-helper-functions.cpp
    src/debug-layer/debug-pipeline.cpp
    src/debug-layer/debug-query.cpp
    src/debug-layer/debug-sampler.cpp
    src/debug-layer/debug-shader-object.cpp
    src/debug-layer/debug-shader-program.cpp
    src/debug-layer/debug-surface.cpp
    src/debug-layer/debug-texture-view.cpp
    src/debug-layer/debug-texture.cpp
    src/debug-layer/debug-transient-heap.cpp
)

if(APPLE)
    target_sources(slang-rhi PRIVATE
        src/cocoa-util.mm
    )
endif()

if(SLANG_RHI_ENABLE_CPU)
    target_sources(slang-rhi PRIVATE
        src/cpu/cpu-buffer.cpp
        src/cpu/cpu-device.cpp
        src/cpu/cpu-helper-functions.cpp
        src/cpu/cpu-pipeline.cpp
        src/cpu/cpu-query.cpp
        src/cpu/cpu-shader-object-layout.cpp
        src/cpu/cpu-shader-object.cpp
        src/cpu/cpu-texture-view.cpp
        src/cpu/cpu-texture.cpp
    )
endif()

if(SLANG_RHI_ENABLE_D3D11 OR SLANG_RHI_HAS_D3D12)
    target_sources(slang-rhi PRIVATE
        src/d3d/d3d-util.cpp
        src/nvapi/nvapi-util.cpp
    )
endif()

if(SLANG_RHI_ENABLE_D3D11)
    target_sources(slang-rhi PRIVATE
        src/d3d11/d3d11-buffer.cpp
        src/d3d11/d3d11-device.cpp
        src/d3d11/d3d11-helper-functions.cpp
        src/d3d11/d3d11-pipeline.cpp
        src/d3d11/d3d11-query.cpp
        src/d3d11/d3d11-scopeNVAPI.cpp
        src/d3d11/d3d11-shader-object-layout.cpp
        src/d3d11/d3d11-shader-object.cpp
        src/d3d11/d3d11-shader-program.cpp
        src/d3d11/d3d11-surface.cpp
        src/d3d11/d3d11-texture.cpp
    )
endif()

if(SLANG_RHI_ENABLE_D3D12)
    target_sources(slang-rhi PRIVATE
        src/d3d12/d3d12-acceleration-structure.cpp
        src/d3d12/d3d12-buffer.cpp
        src/d3d12/d3d12-command-buffer.cpp
        src/d3d12/d3d12-command-encoder.cpp
        src/d3d12/d3d12-command-queue.cpp
        src/d3d12/d3d12-descriptor-heap.cpp
        src/d3d12/d3d12-device.cpp
        src/d3d12/d3d12-fence.cpp
        src/d3d12/d3d12-helper-functions.cpp
        src/d3d12/d3d12-pipeline.cpp
        src/d3d12/d3d12-posix-synchapi.cpp
        src/d3d12/d3d12-query.cpp
        src/d3d12/d3d12-resource.cpp
        src/d3d12/d3d12-sampler.cpp
        src/d3d12/d3d12-shader-object-layout.cpp
        src/d3d12/d3d12-shader-object.cpp
        src/d3d12/d3d12-shader-program.cpp
        src/d3d12/d3d12-shader-table.cpp
        src/d3d12/d3d12-submitter.cpp
        src/d3d12/d3d12-surface.cpp
        src/d3d12/d3d12-texture-view.cpp
        src/d3d12/d3d12-texture.cpp
        src/d3d12/d3d12-transient-heap.cpp
    )
endif()

if(SLANG_RHI_ENABLE_VULKAN)
    target_sources(slang-rhi PRIVATE
        src/vulkan/vk-acceleration-structure.cpp
        src/vulkan/vk-api.cpp
        src/vulkan/vk-buffer.cpp
        src/vulkan/vk-command-buffer.cpp
        src/vulkan/vk-command-encoder.cpp
        src/vulkan/vk-command-queue.cpp
        src/vulkan/vk-descriptor-allocator.cpp
        src/vulkan/vk-device-queue.cpp
        src/vulkan/vk-device.cpp
        src/vulkan/vk-fence.cpp
        src/vulkan/vk-helper-functions.cpp
        src/vulkan/vk-module.cpp
        src/vulkan/vk-pipeline.cpp
        src/vulkan/vk-query.cpp
        src/vulkan/vk-sampler.cpp
        src/vulkan/vk-shader-object-layout.cpp
        src/vulkan/vk-shader-object.cpp
        src/vulkan/vk-shader-program.cpp
        src/vulkan/vk-shader-table.cpp
        src/vulkan/vk-surface.cpp
        src/vulkan/vk-texture-view.cpp
        src/vulkan/vk-texture.cpp
        src/vulkan/vk-transient-heap.cpp
        src/vulkan/vk-util.cpp
    )
    add_library(slang-rhi-vulkan-headers INTERFACE)
    target_include_directories(slang-rhi-vulkan-headers INTERFACE external/vulkan-headers/include)
    target_link_libraries(slang-rhi PRIVATE slang-rhi-vulkan-headers)
endif()

if(SLANG_RHI_ENABLE_METAL)
    target_sources(slang-rhi PRIVATE
        src/metal/metal-acceleration-structure.cpp
        src/metal/metal-api.cpp
        src/metal/metal-buffer.cpp
        src/metal/metal-command-buffer.cpp
        src/metal/metal-command-encoder.cpp
        src/metal/metal-command-queue.cpp
        src/metal/metal-device.cpp
        src/metal/metal-fence.cpp
        src/metal/metal-helper-functions.cpp
        src/metal/metal-input-layout.cpp
        src/metal/metal-pipeline.cpp
        src/metal/metal-query.cpp
        src/metal/metal-sampler.cpp
        src/metal/metal-shader-object-layout.cpp
        src/metal/metal-shader-object.cpp
        src/metal/metal-shader-program.cpp
        src/metal/metal-shader-table.cpp
        src/metal/metal-surface.cpp
        src/metal/metal-texture-view.cpp
        src/metal/metal-texture.cpp
        src/metal/metal-transient-heap.cpp
        src/metal/metal-util.cpp
    )
    add_library(slang-rhi-metal-cpp INTERFACE)
    target_include_directories(slang-rhi-metal-cpp INTERFACE external/metal-cpp)
    target_link_libraries(slang-rhi-metal-cpp INTERFACE "-framework Foundation" "-framework Metal" "-framework QuartzCore")
    target_link_libraries(slang-rhi PRIVATE slang-rhi-metal-cpp)
endif()

if(SLANG_RHI_ENABLE_CUDA)
    target_sources(slang-rhi PRIVATE
        src/cuda/cuda-acceleration-structure.cpp
        src/cuda/cuda-api.cpp
        src/cuda/cuda-buffer.cpp
        src/cuda/cuda-command-buffer.cpp
        src/cuda/cuda-command-encoder.cpp
        src/cuda/cuda-command-queue.cpp
        src/cuda/cuda-device.cpp
        src/cuda/cuda-helper-functions.cpp
        src/cuda/cuda-pipeline.cpp
        src/cuda/cuda-query.cpp
        src/cuda/cuda-shader-object-layout.cpp
        src/cuda/cuda-shader-object.cpp
        src/cuda/cuda-shader-program.cpp
        src/cuda/cuda-texture.cpp
    )
endif()

if(SLANG_RHI_ENABLE_WGPU)
    target_sources(slang-rhi PRIVATE
        src/wgpu/wgpu-api.cpp
        src/wgpu/wgpu-buffer.cpp
        src/wgpu/wgpu-command-buffer.cpp
        src/wgpu/wgpu-command-encoder.cpp
        src/wgpu/wgpu-command-queue.cpp
        src/wgpu/wgpu-device.cpp
        src/wgpu/wgpu-fence.cpp
        src/wgpu/wgpu-helper-functions.cpp
        src/wgpu/wgpu-input-layout.cpp
        src/wgpu/wgpu-pipeline.cpp
        src/wgpu/wgpu-query.cpp
        src/wgpu/wgpu-sampler.cpp
        src/wgpu/wgpu-shader-object-layout.cpp
        src/wgpu/wgpu-shader-object.cpp
        src/wgpu/wgpu-shader-program.cpp
        src/wgpu/wgpu-surface.cpp
        src/wgpu/wgpu-texture.cpp
        src/wgpu/wgpu-transient-resource-heap.cpp
        src/wgpu/wgpu-util.cpp
    )
endif()

target_include_directories(slang-rhi PUBLIC include)
target_include_directories(slang-rhi PRIVATE src)
target_compile_definitions(slang-rhi
    PRIVATE
    SLANG_RHI_ENABLE_CPU=$<BOOL:${SLANG_RHI_ENABLE_CPU}>
    SLANG_RHI_ENABLE_D3D11=$<BOOL:${SLANG_RHI_ENABLE_D3D11}>
    SLANG_RHI_ENABLE_D3D12=$<BOOL:${SLANG_RHI_ENABLE_D3D12}>
    SLANG_RHI_ENABLE_VULKAN=$<BOOL:${SLANG_RHI_ENABLE_VULKAN}>
    SLANG_RHI_ENABLE_METAL=$<BOOL:${SLANG_RHI_ENABLE_METAL}>
    SLANG_RHI_ENABLE_CUDA=$<BOOL:${SLANG_RHI_ENABLE_CUDA}>
    SLANG_RHI_ENABLE_WGPU=$<BOOL:${SLANG_RHI_ENABLE_WGPU}>
    SLANG_RHI_ENABLE_AGILITY_SDK=$<BOOL:${SLANG_RHI_ENABLE_AGILITY_SDK}>
    SLANG_RHI_ENABLE_NVAPI=$<BOOL:${SLANG_RHI_ENABLE_NVAPI}>
    SLANG_RHI_ENABLE_OPTIX=$<BOOL:${SLANG_RHI_ENABLE_OPTIX}>
    $<$<PLATFORM_ID:Windows>:NOMINMAX> # do not define min/max macros
    $<$<PLATFORM_ID:Windows>:UNICODE> # force character map to unicode
)
target_compile_features(slang-rhi PRIVATE cxx_std_17)
set_target_properties(slang-rhi PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SLANG_RHI_BUILD_TESTS)
    add_library(doctest INTERFACE)
    target_include_directories(doctest INTERFACE external/doctest)

    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE external/stb)

    add_executable(slang-rhi-tests)
    target_sources(slang-rhi-tests PRIVATE
        tests/main.cpp
        tests/test-buffer-barrier.cpp
        tests/test-clear-texture.cpp
        tests/test-compute-smoke.cpp
        tests/test-compute-trivial.cpp
        tests/test-copy-texture.cpp
        tests/test-create-buffer-from-handle.cpp
        tests/test-existing-device-handle.cpp
        tests/test-formats.cpp
        tests/test-instanced-draw.cpp
        tests/test-link-time-constant.cpp
        tests/test-link-time-default.cpp
        tests/test-link-time-options.cpp
        tests/test-link-time-type.cpp
        tests/test-mutable-shader-object.cpp
        tests/test-native-handle.cpp
        tests/test-nested-parameter-block.cpp
        tests/test-precompiled-module-2.cpp
        tests/test-precompiled-module-cache.cpp
        tests/test-precompiled-module.cpp
        tests/test-ray-tracing.cpp
        tests/test-resolve-resource-tests.cpp
        tests/test-resource-states.cpp
        tests/test-root-mutable-shader-object.cpp
        tests/test-root-shader-parameter.cpp
        tests/test-sampler-array.cpp
        tests/test-shader-cache.cpp
        tests/test-shared-buffer.cpp
        tests/test-shared-texture.cpp
        tests/test-swapchain.cpp
        tests/test-texture-types.cpp
        tests/test-uint16-structured-buffer.cpp
        tests/testing.cpp
        tests/texture-utils.cpp
    )
    target_compile_definitions(slang-rhi-tests
        PRIVATE
        $<$<PLATFORM_ID:Windows>:NOMINMAX> # do not define min/max macros
        $<$<PLATFORM_ID:Windows>:UNICODE> # force character map to unicode
    )
    target_compile_features(slang-rhi-tests PRIVATE cxx_std_17)
    target_include_directories(slang-rhi-tests PRIVATE tests)
    target_link_libraries(slang-rhi-tests PRIVATE doctest stb slang slang-rhi)
endif()

if(SLANG_RHI_BUILD_EXAMPLES)
    function(add_example name source)
        add_executable(${name} ${source})
        target_compile_features(${name} PRIVATE cxx_std_17)
        target_include_directories(${name} PRIVATE examples/base)
        target_link_libraries(${name} PRIVATE slang slang-rhi glfw)
    endfunction()

    add_example(example-surface examples/surface/example-surface.cpp)
endif()

add_custom_target(slang-rhi-copy-files ALL DEPENDS ${SLANG_RHI_COPY_FILES})
