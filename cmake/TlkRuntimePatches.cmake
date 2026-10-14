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
# originals for the copies in a runtime source list.
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

function(tlk_use_patched_runtime_sources list_name)
    set(sources ${${list_name}})
    foreach(original IN LISTS TLK_RUNTIME_ORIGINAL_SOURCES)
        list(FIND sources "${original}" index)
        if(index EQUAL -1)
            message(FATAL_ERROR
                "TLK runtime adaptation targets ${original}, which is not in ${list_name}")
        endif()
        list(REMOVE_ITEM sources "${original}")
    endforeach()
    list(APPEND sources ${TLK_RUNTIME_PATCHED_SOURCES})
    set(${list_name} ${sources} PARENT_SCOPE)
endfunction()
