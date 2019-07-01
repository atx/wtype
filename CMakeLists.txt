cmake_minimum_required (VERSION 3.6)
project (wtype)

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package (Wayland REQUIRED)
find_package (WaylandScanner REQUIRED)
find_package (PkgConfig REQUIRED)
pkg_search_module (XKBCOMMON REQUIRED xkbcommon)

option (WITH_ASAN "Enable ASan" OFF)

if (WITH_ASAN)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif ()

ecm_add_wayland_client_protocol (
	VIRTUAL_KEYBOARD_SRC
	PROTOCOL virtual-keyboard-unstable-v1.xml
	BASENAME virtual-keyboard
)

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ggdb")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused")

include_directories (nuklear/)
include_directories ("${CMAKE_BINARY_DIR}")
include_directories ("${XKBCOMMON_INCLUDEDIR}")

add_executable (wtype main.c ${VIRTUAL_KEYBOARD_SRC})
target_link_libraries (wtype rt ${Wayland_LIBRARIES} ${XKBCOMMON_LIBRARIES})

install (TARGETS wtype RUNTIME DESTINATION bin COMPONENT bin)
