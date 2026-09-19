cmake_minimum_required(VERSION 3.24)
project(monado_wine_hello_xr LANGUAGES C CXX)

include(FetchContent)

set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(INSTALL_TO_ARCHITECTURE_PREFIXES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	openxr_sdk_source
	URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/211e2f4f0b79932300a845f3a8ed75026dbfa4a3.tar.gz
	URL_HASH SHA256=44747d4b90e0e4d5793da90ec7af9d38aa856b3c78d7fbdca0faad8029460325
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(openxr_sdk_source)

if(MINGW)
	# OpenXR-SDK 1.1.61 tests this Windows partition macro with
	# -Werror=undef, while MinGW does not define it.
	target_compile_definitions(openxr_loader PRIVATE WINAPI_PARTITION_SYSTEM=0)
endif()

FetchContent_Declare(
	directxmath
	URL https://github.com/microsoft/DirectXMath/archive/93e6399d6d15e1e57f80b5cea04d5a8eb892d693.tar.gz
	URL_HASH SHA256=5df86719dff4f5c10418de4ed3b2e8a77e41ccf8aeab3f74d2810a45e0935df7
	DOWNLOAD_EXTRACT_TIMESTAMP FALSE
	SOURCE_SUBDIR monado-no-cmake-project
)
FetchContent_MakeAvailable(directxmath)

set(KHR_HELLO_XR_DIR "${openxr_sdk_source_SOURCE_DIR}/src/tests/hello_xr")

add_executable(
	khr_hello_xr_d3d11
	"${KHR_HELLO_XR_DIR}/d3d_common.cpp"
	"${KHR_HELLO_XR_DIR}/graphicsplugin_d3d11.cpp"
	"${KHR_HELLO_XR_DIR}/graphicsplugin_factory.cpp"
	"${KHR_HELLO_XR_DIR}/logger.cpp"
	"${KHR_HELLO_XR_DIR}/main.cpp"
	"${KHR_HELLO_XR_DIR}/openxr_program.cpp"
	"${KHR_HELLO_XR_DIR}/pch.cpp"
	"${KHR_HELLO_XR_DIR}/platformplugin_factory.cpp"
	"${KHR_HELLO_XR_DIR}/platformplugin_win32.cpp"
	"${KHR_HELLO_XR_DIR}/swapchain_image_data.cpp"
)

target_include_directories(
	khr_hello_xr_d3d11
	PRIVATE
		"${directxmath_SOURCE_DIR}/Inc"
		"${CMAKE_CURRENT_SOURCE_DIR}/compat"
		"${KHR_HELLO_XR_DIR}"
		"${openxr_sdk_source_SOURCE_DIR}/src"
		"${openxr_sdk_source_SOURCE_DIR}/src/common"
		"${openxr_sdk_source_SOURCE_DIR}/src/external/span-lite/include"
)

target_compile_definitions(
	khr_hello_xr_d3d11
	PRIVATE
		XR_USE_PLATFORM_WIN32
		XR_USE_GRAPHICS_API_D3D11
		_XM_NO_INTRINSICS_
)

target_compile_features(khr_hello_xr_d3d11 PRIVATE cxx_std_17)
target_compile_options(khr_hello_xr_d3d11 PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(
	khr_hello_xr_d3d11
	PRIVATE
		OpenXR::openxr_loader
		ole32
		d3d11
		d3dcompiler
		dxgi
		dxguid
)

target_link_options(khr_hello_xr_d3d11 PRIVATE -static -static-libgcc -static-libstdc++)
