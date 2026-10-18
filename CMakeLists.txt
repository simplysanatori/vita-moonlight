cmake_minimum_required(VERSION 3.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
	message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()
add_compile_options(-g3 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare -Wno-switch)


project(moonlight)

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
# NOTE: To update the version shown in the UI and VPK, edit the following lines:
#   set(VERSION_MAJOR "0")
#   set(VERSION_MINOR "13")
#   set(VERSION_PATCH "2")
#
set(TITLE_ID "XYZZ00002")
set(TITLE "Moonlight")
set(VERSION_MAJOR "0")
set(VERSION_MINOR "13")
set(VERSION_PATCH "2")

# APP_VER is MM.mm, zero-padded. Built in CMake because a bare `python` is not
# available everywhere, and a failed call silently left APP_VER empty
foreach(part MAJOR MINOR)
	if(VERSION_${part} LESS 10)
		set(APP_VER_${part} "0${VERSION_${part}}")
	else()
		set(APP_VER_${part} "${VERSION_${part}}")
	endif()
endforeach()
set(APP_VER "${APP_VER_MAJOR}.${APP_VER_MINOR}")

add_definitions(-DVERSION_MAJOR=${VERSION_MAJOR})
add_definitions(-DVERSION_MINOR=${VERSION_MINOR})
add_definitions(-DVERSION_PATCH=${VERSION_PATCH})

# This replaces CMAKE_C_FLAGS outright and no CMAKE_BUILD_TYPE is used, so the
# optimisation level must be set here or everything is built at -O0
set(VITA_OPT_LEVEL "-O2" CACHE STRING "Optimisation flag for all C sources, e.g. -O0 for debugging")
set(CMAKE_C_FLAGS "-Wl,-q -g ${VITA_OPT_LEVEL} -std=gnu99 -D__vita__")



include_directories(
	include/
	src/
	third_party/moonlight-common-c/src/
	third_party/moonlight-common-c/reedsolomon/
	libgamestream/
	third_party/libuuid/src/
	third_party/h264bitstream/
	third_party/enet/include/
	third_party/inih/
	third_party/mdnsniff/
)


add_executable(${PROJECT_NAME}.elf
	src/config.c
	src/input/mapping.c
	src/connection.c
	src/debug.c
	src/main.c
	src/platform.c
	src/stats.c
	src/bench.c
	src/netcount.c
	src/settings.c
	src/session.c
	src/hosts.c
	src/ui_ops.c
	src/devshot.c
	src/util.c
	src/device.c
	src/check_dir.c
	src/wake_on_lan.c
	src/audio/vita.c
	src/video/vita.c
	src/input/vita.c
	src/input/shortcuts.c
	src/power/vita.c
	src/input/motion.c
	src/graphics.c
	src/font.c
	src/gui/guilib.c
	src/gui/ime.c
	src/gui/ui.c
	src/gui/ui_settings.c
	src/gui/ui_recommended.c
	src/gui/ui_boxart.c
	src/gui/ui_connect.c
	src/gui/ui_device.c
	src/gui/ui_check.c

	libgamestream/client.c
	libgamestream/http.c
	libgamestream/mkcert.c
	libgamestream/sps.c
	libgamestream/xml.c

	src/keyboardsystem.c
	src/input/keyboardkeys.c
	src/gui/ui_keyboard.c
	src/input/touchabsolute.c
	src/check_host.c
)


# Por defecto, solo enlazamos curl y el resto de libs estándar:
target_link_libraries(${PROJECT_NAME}.elf
	# Room for vita-elf-create's module tables before the data segment (sce_gap.ld)
	-Wl,-T,${CMAKE_SOURCE_DIR}/sce_gap.ld
	# Lets the benchmark time the Vita's send path (src/bench.c)
	-Wl,--wrap=enet_peer_send
	-Wl,--wrap=enet_socket_send
	# Counts video packets as they arrive (src/netcount.c)
	-Wl,--wrap=recvUdpSocket
	# Logs the receive buffer granted to the video socket (src/netcount.c)
	-Wl,--wrap=bindUdpSocket
	-lexpat
	-lcurl
	moonlight-common-c
	enet
	h264bitstream
	inih
	libuuid
	mdnsniff
	-lz
	-lzstd
	-lssl
	-lcrypto
	-lopus
	-lbz2
	-lpthread
	-lSceDisplay_stub
	-lSceCtrl_stub
	-lSceTouch_stub
	-lSceNet_stub
	-lSceNetCtl_stub
	-lSceRegistryMgr_stub
	-lScePower_stub
	-lSceVideodec_stub
	-lSceCommonDialog_stub
	-lSceShellSvc_stub
	-lSceAppMgr_stub
	-lSceAudio_stub
	-lSceIme_stub
	-lSceMotion_stub
	-lSceAppUtil_stub
	-lvita2d
	-lSceGxm_stub
	-lScePgf_stub
	-lSceCommonDialog_stub
	-lfreetype
	-lpng
	-ljpeg
	-lz
	-lm
	-lc
)
# Passed through -Wl, the linker script isn't a dependency by itself, so
# editing it would not relink
set_target_properties(${PROJECT_NAME}.elf PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/sce_gap.ld)

add_custom_target(${PROJECT_NAME}.velf ALL
	COMMAND vita-elf-create ${PROJECT_NAME}.elf ${PROJECT_NAME}.velf
)

add_custom_target(param.sfo ALL   
	COMMAND vita-mksfoex -s TITLE_ID=${TITLE_ID} -s APP_VER=${APP_VER} "${TITLE}" param.sfo
)

# Actualizar versión en template.xml automáticamente antes de empaquetar el VPK (solo la línea visual)
add_custom_target(update_template_version ALL
	COMMAND sed -i '/<str size=\"18.*>v[0-9]*\.[0-9]*\.[0-9]*<\\/str>/s/v[0-9]*\.[0-9]*\.[0-9]*/v${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}/' sce_sys/livearea/contents/template.xml
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(${PROJECT_NAME}.vpk ALL
	COMMAND vita-make-fself -a 0x2800000000000001 -at 0x0E -c ${PROJECT_NAME}.velf eboot.bin
	COMMAND vita-pack-vpk -s param.sfo -b eboot.bin
			--add ../sce_sys/icon0.png=sce_sys/icon0.png
			--add ../sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png
			--add ../sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png
			--add ../sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml
			--add ../assets/nerdfont.ttf=assets/nerdfont.ttf
			${PROJECT_NAME}.vpk
)

add_dependencies(${PROJECT_NAME}.velf ${PROJECT_NAME}.elf)
add_dependencies(${PROJECT_NAME}.vpk ${PROJECT_NAME}.velf param.sfo update_template_version)

add_subdirectory(third_party)

option(USE_DIR_UMA0 "Forzar uso de uma0:data/moonlight como directorio de claves/config" OFF)
if(USE_DIR_UMA0)
  add_definitions(-DUSE_DIR_UMA0)
endif()


