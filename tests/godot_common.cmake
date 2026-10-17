# Shared head of the tests/godot_*.cmake scripts (docs/testing.md "Godot rows"); include() it after
# cmake_minimum_required. Sets `godot` to the Godot console executable: $ENV{OPENBF3_GODOT} when set, else GODOT_EXE
# (-D, the configure-time OPENBF3_GODOT_EXE that openbf3_godot_row passes), else the script exits 77 (skipped).
# A windowed script sets GODOT_WINDOWED to TRUE before the include: it also needs $ENV{OPENBF3_GODOT_WINDOWED} equal
# to 1 (only the merge pass's window phase sets it) and exits 77 while $ENV{OPENBF3_USER_ACTIVE_FLAG} names a file
# that exists, so no ctest call opens a window unless asked to.
# In a headless script, with -DBUILD_DIR and -DROW (openbf3_godot_row passes both), it also sets godot_cache_root to a per-build cache root
# (${BUILD_DIR}/godot-cache/<row>, kept across runs, never the shared user://bf3cache) and godot_log_args to
# "--log-file ${BUILD_DIR}/godot-logs/<row>.log", so worktrees never share a cache or a log file.
# The godot.project_setup fixture writes .godot/extension_list.cfg before any godot.* row; the fallback write below
# is for a direct `cmake -P` run.
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" AND DEFINED GODOT_EXE AND NOT GODOT_EXE STREQUAL "" AND NOT GODOT_EXE MATCHES "-NOTFOUND$")
	set(godot "${GODOT_EXE}")
endif()
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
	message("SKIPPED: no Godot console executable (OPENBF3_GODOT unset and none found next to the checkout at configure time)")
	cmake_language(EXIT 77)
endif()
if(GODOT_WINDOWED)
	if(NOT "$ENV{OPENBF3_GODOT_WINDOWED}" STREQUAL "1")
		message("SKIPPED: a windowed Godot row; it runs only with OPENBF3_GODOT_WINDOWED=1 (the merge pass's window phase)")
		cmake_language(EXIT 77)
	endif()
	if(NOT "$ENV{OPENBF3_USER_ACTIVE_FLAG}" STREQUAL "" AND EXISTS "$ENV{OPENBF3_USER_ACTIVE_FLAG}")
		message("SKIPPED: the user is active ($ENV{OPENBF3_USER_ACTIVE_FLAG} exists); windowed rows wait for the user-away tenure")
		cmake_language(EXIT 77)
	endif()
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
	file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
endif()
set(godot_cache_root "")
set(godot_log_args "")
if(NOT GODOT_WINDOWED AND DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "" AND DEFINED ROW AND NOT ROW STREQUAL "")
	set(godot_cache_root "${BUILD_DIR}/godot-cache/${ROW}")
	file(MAKE_DIRECTORY "${godot_cache_root}" "${BUILD_DIR}/godot-logs")
	set(godot_log_args --log-file "${BUILD_DIR}/godot-logs/${ROW}.log")
endif()
