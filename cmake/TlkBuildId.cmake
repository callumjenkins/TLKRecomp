# Writes OUT, a header defining TLK_BUILD_ID: 12 hex digits of a SHA-256 over the contents of the generated C,
# snesrecomp's runtime sources and our hunks to them, and the runner, then PLATFORM. Savestates are snapshots of the runtime's memory, so a state
# loads only into the build whose id wrote it. TLK_BUILD_VERSION is `git describe --always --dirty` in ROOT, a
# label that leaves the id alone, so a commit that changes no code keeps every save.
#
#   cmake -DROOT=DIR -DGEN=DIR -DRUNTIME=DIR -DOUT=FILE -DPLATFORM=NAME -P TlkBuildId.cmake
#
# The file is rewritten only when its text changes, so a build with no new code or commit recompiles nothing.
foreach(dir ROOT GEN RUNTIME)
    if(NOT IS_DIRECTORY "${${dir}}")   # an empty one globs from /
        message(FATAL_ERROR "TlkBuildId.cmake: ${dir} is not a directory: '${${dir}}'")
    endif()
endforeach()
execute_process(COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE describe OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE git_rc ERROR_QUIET)
if(NOT git_rc EQUAL 0 OR describe STREQUAL "")
    set(describe "nogit")
endif()

file(GLOB gen RELATIVE "${GEN}" "${GEN}/*.c" "${GEN}/*.h")
file(GLOB own RELATIVE "${ROOT}"
    "${ROOT}/cmake/runtime-patches/*.hunks" "${ROOT}/runner/*.c" "${ROOT}/runner/*.h" "${ROOT}/runner/*.cpp")
file(GLOB_RECURSE runtime RELATIVE "${RUNTIME}" "${RUNTIME}/*.c" "${RUNTIME}/*.h" "${RUNTIME}/*.cpp" "${RUNTIME}/*.hpp")
list(SORT gen)
list(SORT own)
list(SORT runtime)
set(sums "")
foreach(f IN LISTS gen)
    file(SHA256 "${GEN}/${f}" h)
    string(APPEND sums "gen/${f} ${h}\n")
endforeach()
foreach(f IN LISTS runtime)
    file(SHA256 "${RUNTIME}/${f}" h)
    string(APPEND sums "runtime/${f} ${h}\n")
endforeach()
foreach(f IN LISTS own)
    file(SHA256 "${ROOT}/${f}" h)
    string(APPEND sums "${f} ${h}\n")
endforeach()
string(SHA256 all "${sums}")
string(SUBSTRING "${all}" 0 12 short)

set(text "#define TLK_BUILD_ID \"${short}-${PLATFORM}\"\n#define TLK_BUILD_VERSION \"${describe}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
