# ctest godot.project_setup (FIXTURES_SETUP godot_project; docs/testing.md "Godot rows"): prepares the Godot project
# once before any godot.* row of a ctest call. It writes .godot/extension_list.cfg (one line,
# res://openbf3.gdextension) when that file is missing, write-then-rename so a concurrent reader never sees half a
# file, and checks that the extension DLL exists. It never runs --import: the first --import of a project that was
# never opened registers the extension and then exits 0xC0000005 (README "Run"); the one-line file has the same effect.
#   cmake -DPROJECT_DIR=<bf3-opensource> -DDLL=<extension dll> -P godot_project_setup.cmake
cmake_minimum_required(VERSION 3.29)
set(cfg "${PROJECT_DIR}/.godot/extension_list.cfg")
if(NOT EXISTS "${cfg}")
	string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
	file(MAKE_DIRECTORY "${PROJECT_DIR}/.godot")
	file(WRITE "${cfg}.${tag}.tmp" "res://openbf3.gdextension\n")
	file(RENAME "${cfg}.${tag}.tmp" "${cfg}" RESULT rename_rc)
	if(NOT rename_rc STREQUAL "0" AND NOT EXISTS "${cfg}")
		message(FATAL_ERROR "godot.project_setup: could not write ${cfg} (${rename_rc})")
	endif()
	file(REMOVE "${cfg}.${tag}.tmp")
	message("wrote ${cfg}")
else()
	message("${cfg} present")
endif()
if(NOT DLL STREQUAL "" AND NOT EXISTS "${DLL}")
	message(FATAL_ERROR "godot.project_setup: the extension DLL ${DLL} is missing (build the preset first)")
endif()
message("extension DLL ${DLL} present")
