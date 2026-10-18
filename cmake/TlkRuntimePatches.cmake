# TLK keeps the pinned snesrecomp submodule intact and applies its runtime
# adaptations to build-directory copies at configure time. The hunk files
# under cmake/runtime-patches name each source and the literal replacements;
# scripts/apply_runtime_patches.py refuses a missing, moved, or
# duplicated anchor so the build fails closed when the pin changes.
#
# Sets:
#   TLK_RUNTIME_PATCH_DIR       the hunk directory
#   TLK_RUNTIME_PATCHED_DIR     where the copies live
#   TLK_RUNTIME_PATCHED_SOURCES the copies (absolute paths)
#   TLK_RUNTIME_ORIGINAL_SOURCES the pinned sources they replace
# and provides tlk_use_patched_runtime_sources(<list-var>) to swap the
# originals for the copies in a runtime source list, and
# tlk_use_patched_target_sources(<target>) for desktop presenter sources.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TLK_RUNTIME_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/runtime-patches" CACHE PATH "Directory of runtime adaptation hunks")
set(TLK_RUNTIME_PATCHED_DIR "${CMAKE_CURRENT_BINARY_DIR}/tlk-runtime")
set(TLK_RUNTIME_PATCH_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/apply_runtime_patches.py")

file(GLOB TLK_RUNTIME_HUNK_FILES CONFIGURE_DEPENDS
    "${TLK_RUNTIME_PATCH_DIR}/*.hunks")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${TLK_RUNTIME_HUNK_FILES} "${TLK_RUNTIME_PATCH_SCRIPT}")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${TLK_RUNTIME_PATCH_SCRIPT}"
        --patches "${TLK_RUNTIME_PATCH_DIR}"
        --snesrecomp "${SNESRECOMP_ROOT}"
        --out "${TLK_RUNTIME_PATCHED_DIR}"
    RESULT_VARIABLE tlk_runtime_patch_result
    OUTPUT_VARIABLE tlk_runtime_patch_output
    ERROR_VARIABLE tlk_runtime_patch_error)
if(NOT tlk_runtime_patch_result EQUAL 0)
    message(FATAL_ERROR
        "TLK runtime adaptation failed; review cmake/runtime-patches against the pinned snesrecomp:\n${tlk_runtime_patch_error}")
endif()

file(STRINGS "${TLK_RUNTIME_PATCHED_DIR}/manifest.txt" tlk_runtime_manifest)
set(TLK_RUNTIME_ORIGINAL_SOURCES)
set(TLK_RUNTIME_PATCHED_SOURCES)
foreach(tlk_manifest_line IN LISTS tlk_runtime_manifest)
    string(REPLACE "\t" ";" tlk_manifest_pair "${tlk_manifest_line}")
    list(GET tlk_manifest_pair 0 tlk_original)
    list(GET tlk_manifest_pair 1 tlk_patched)
    list(APPEND TLK_RUNTIME_ORIGINAL_SOURCES "${tlk_original}")
    list(APPEND TLK_RUNTIME_PATCHED_SOURCES "${tlk_patched}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${tlk_original}")
endforeach()
list(LENGTH TLK_RUNTIME_PATCHED_SOURCES tlk_runtime_patched_count)
message(STATUS "TLK: ${tlk_runtime_patched_count} runtime sources adapted in ${TLK_RUNTIME_PATCHED_DIR}")

# Desktop presenter sources (runner/src/desktop/) are not in the runner source
# list: snesrecomp_target_opengl and friends add them to one target. They are
# swapped on that target by tlk_use_patched_target_sources instead.
set(TLK_RUNTIME_DESKTOP_DIR "${SNESRECOMP_ROOT}/runner/src/desktop/")

function(tlk_use_patched_runtime_sources list_name)
    set(sources ${${list_name}})
    foreach(original patched IN ZIP_LISTS TLK_RUNTIME_ORIGINAL_SOURCES TLK_RUNTIME_PATCHED_SOURCES)
        string(FIND "${original}" "${TLK_RUNTIME_DESKTOP_DIR}" desktop)
        if(desktop EQUAL 0)
            continue()
        endif()
        list(FIND sources "${original}" index)
        if(index EQUAL -1)
            message(FATAL_ERROR
                "TLK runtime adaptation targets ${original}, which is not in ${list_name}")
        endif()
        list(REMOVE_ITEM sources "${original}")
        list(APPEND sources "${patched}")
    endforeach()
    set(${list_name} ${sources} PARENT_SCOPE)
endfunction()

# Swap the patched desktop sources into <target>, after the snesrecomp_target_*
# call that added the originals. Every desktop adaptation must apply here.
function(tlk_use_patched_target_sources target)
    get_target_property(sources ${target} SOURCES)
    foreach(original patched IN ZIP_LISTS TLK_RUNTIME_ORIGINAL_SOURCES TLK_RUNTIME_PATCHED_SOURCES)
        string(FIND "${original}" "${TLK_RUNTIME_DESKTOP_DIR}" desktop)
        if(NOT desktop EQUAL 0)
            continue()
        endif()
        list(FIND sources "${original}" index)
        if(index EQUAL -1)
            message(FATAL_ERROR
                "TLK runtime adaptation targets ${original}, which is not a source of ${target}")
        endif()
        list(REMOVE_ITEM sources "${original}")
        list(APPEND sources "${patched}")
    endforeach()
    set_target_properties(${target} PROPERTIES SOURCES "${sources}")
endfunction()
