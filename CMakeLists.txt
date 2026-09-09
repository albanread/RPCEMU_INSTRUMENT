# RPCEmu - headless build
#
# Builds rpcemu-headless: the emulator core plus a frontend with no GUI
# toolkit, for agent-driven and automated use. See RPCEMU-AGENT.md.
#
# The interpreter is the only CPU built here. It is the one that can be
# stepped and breakpointed; the recompiler stays with the Qt build.
#
# Configure and build (from a shell with clang on PATH):
#
#     cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
#     cmake --build build
#
# Run it from the directory holding rpc.cfg, roms/ and hostfs/.

cmake_minimum_required(VERSION 3.20)
project(rpcemu-headless C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
	message(FATAL_ERROR "The headless frontend is Windows-only for now")
endif()

add_executable(rpcemu-headless
	# CPU: interpreter only
	src/arm.c
	src/arm_common.c
	src/codegen_null.c
	src/cp15.c
	src/fpa.c

	# Memory and ROM
	src/mem.c
	src/romload.c

	# Machine
	src/rpcemu.c
	src/iomd.c
	src/superio.c
	src/i8042.c
	src/keyboard.c
	src/cmos.c
	src/vidc20.c
	src/sound.c
	src/podules.c
	src/podulerom.c

	# Storage
	src/fdc.c
	src/disc.c
	src/disc_adf.c
	src/disc_hfe.c
	src/disc_mfm_common.c
	src/ide.c
	src/icside.c
	src/cdrom-iso.c

	# HostFS
	src/hostfs.c
	src/hostfs-win.c

	# Platform
	src/rpc-machdep.c
	src/win/rpc-win.c
	src/win/cdrom-ioctl.c

	# Headless frontend
	src/headless/main_headless.c
	src/headless/plt_headless.c
	src/headless/config_headless.c
	src/headless/png_write.c
	src/headless/keyboard_type.c
	src/headless/window_win.c

	# Debug core
	src/debug/dbg_vdu.c
	src/debug/dbg_cpu.c
	src/debug/dbg_break.c
	src/debug/dbg_sym.c
	src/debug/dbg_watch.c
	src/debug/dbg_trace.c
	src/debug/dbg_analyse.c
	src/debug/dbg_rpc.c
	src/debug/json.c

	# MinGW-isms the Microsoft CRT lacks
	src/headless/compat/dirent_win.c
)

target_include_directories(rpcemu-headless PRIVATE
	src
	src/debug
	src/headless
	src/headless/compat
)

target_compile_definitions(rpcemu-headless PRIVATE
	WIN32
	NDEBUG
	RPCEMU_NO_NETWORKING
	RPCEMU_DEBUG_HOOKS
	_CRT_SECURE_NO_WARNINGS
	_CRT_NONSTDC_NO_DEPRECATE
)

# Inject the CRT compatibility shims ahead of every translation unit, so the
# upstream sources compile unmodified.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
	target_compile_options(rpcemu-headless PRIVATE
		-include "${CMAKE_CURRENT_SOURCE_DIR}/src/headless/compat/win_compat.h")
elseif(MSVC)
	target_compile_options(rpcemu-headless PRIVATE
		/FI"${CMAKE_CURRENT_SOURCE_DIR}/src/headless/compat/win_compat.h")
endif()

target_link_libraries(rpcemu-headless PRIVATE winmm user32 gdi32)

# The upstream sources are not warning-clean and are not ours to tidy; keep
# the noise down but leave anything that indicates a real problem visible.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
	target_compile_options(rpcemu-headless PRIVATE
		-Wno-deprecated-declarations
		-Wno-pointer-sign
		-Wno-unused-value
	)
elseif(MSVC)
	target_compile_options(rpcemu-headless PRIVATE /wd4996)
endif()
