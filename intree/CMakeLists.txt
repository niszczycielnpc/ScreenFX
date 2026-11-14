# Built as a subdirectory of the OnionHEN source tree (see ../build.sh): the
# renderer and the loader reuse OnionHEN's remote ELF loader and process
# helpers, which are not part of the public plugin SDK.
cmake_minimum_required(VERSION 3.20)
project(screenfx C CXX ASM)

set(ONION_PLUGIN_ID SCFX00001)
set(ONION_PLUGIN_VERSION 0.30)
set(ONION_PLUGIN_NAME "ScreenFX")

get_filename_component(SCREENFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT ONIONHEN_PLUGIN_SDK_SOURCE)
	message(FATAL_ERROR "Set ONIONHEN_PLUGIN_SDK_SOURCE to an onionHEN-plugin-sdk checkout")
endif()

onion_module_prologue(C_STANDARD 11 CXX_STANDARD 20)
onion_output_dirs()

set(SCREENFX_FLAGS "${ONION_PS5_TARGET_FLAGS} -fPIC -fPIE -march=znver2 -Wall -Werror -fstack-protector-all -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS "${SCREENFX_FLAGS}")
set(CMAKE_CXX_FLAGS "${SCREENFX_FLAGS} -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

# ---- renderer: loaded into SceShellUI ---------------------------------------
add_executable(screenfx_renderer "${SCREENFX_DIR}/renderer/renderer.cpp")
set_target_properties(screenfx_renderer PROPERTIES OUTPUT_NAME "screenfx_renderer.elf")
target_link_options(screenfx_renderer PRIVATE "-Wl,--gc-sections" "-Wl,--icf=safe")
add_dependencies(screenfx_renderer onion_proc onion_platform onion_ready)
target_include_directories(screenfx_renderer PRIVATE
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/include"
	"${SCREENFX_DIR}/include")
target_link_directories(screenfx_renderer PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(screenfx_renderer PUBLIC onion_proc onion_ready onion_platform)
onion_strip_debug(screenfx_renderer)

# ---- plugin: the ELF OnionHEN starts from /data/OnionHEN/plugins ------------
configure_file("${SCREENFX_DIR}/include/plugin_config.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/plugin_config.h" @ONLY)

set(_sdk_runtime "${ONIONHEN_PLUGIN_SDK_SOURCE}/runtime")
add_executable(screenfx
	"${SCREENFX_DIR}/source/main.c"
	"${SCREENFX_DIR}/source/plugin_descriptor.c"
	"${SCREENFX_DIR}/source/fx_settings.c"
	"${SCREENFX_DIR}/source/fx_state.c"
	"${SCREENFX_DIR}/source/fx_ui.c"
	"${SCREENFX_DIR}/source/fx_inject.c"
	"${_sdk_runtime}/host_api.c" "${_sdk_runtime}/status.c" "${_sdk_runtime}/transport.c"
	"${_sdk_runtime}/services.c" "${_sdk_runtime}/runtime.c" "${_sdk_runtime}/event_bus.c"
	"${_sdk_runtime}/client.c" "${_sdk_runtime}/ui.c" "${_sdk_runtime}/ui_client.c"
	"${_sdk_runtime}/ui_event.c")
set_target_properties(screenfx PROPERTIES OUTPUT_NAME "${ONION_PLUGIN_ID}.elf" LINKER_LANGUAGE CXX)
target_link_options(screenfx PRIVATE "-Wl,--gc-sections")

# .incbin is invisible to the dependency scanner.
set_source_files_properties("${SCREENFX_DIR}/source/fx_inject.c" PROPERTIES
	OBJECT_DEPENDS "${ONIONHEN_OUT_BIN}/screenfx_renderer.elf")
target_compile_definitions(screenfx PRIVATE
	FX_RENDERER_ELF="${ONIONHEN_OUT_BIN}/screenfx_renderer.elf")

add_dependencies(screenfx screenfx_renderer NineS onion_elfldr onion_proc onion_platform onion_ready)
target_include_directories(screenfx PRIVATE
	"${SCREENFX_DIR}/include"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
	"${ONIONHEN_PLUGIN_SDK_SOURCE}/include"
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/include")
target_link_directories(screenfx PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(screenfx PUBLIC
	NineS onion_elfldr onion_proc onion_ready onion_platform
	SceNotification SceSystemService kernel_sys)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET screenfx POST_BUILD
	COMMAND ${CMAKE_OBJCOPY} --strip-unneeded --keep-section=.onion_plugin $<TARGET_FILE:screenfx>
	COMMAND "${Python3_EXECUTABLE}" "${ONIONHEN_PLUGIN_SDK_SOURCE}/tools/inspect_plugin.py"
		$<TARGET_FILE:screenfx> --expect-id ${ONION_PLUGIN_ID}
		--expect-version ${ONION_PLUGIN_VERSION} --quiet
	COMMENT "Stripping and validating ${ONION_PLUGIN_ID}.elf"
	VERBATIM)
