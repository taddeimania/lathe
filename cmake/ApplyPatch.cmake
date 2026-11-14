# Applies patches to a fetched dependency once. Run as: cmake -DPATCH=<file>[|<file>...] -P ApplyPatch.cmake
# from the dependency's source folder. Each patch already applied (a re-configure): nothing to do.
# One that doesn't apply and isn't applied (the dependency changed): stop with an error.
find_package(Git REQUIRED)

string(REPLACE "|" ";" patches "${PATCH}")
foreach(PATCH IN LISTS patches)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --check "${PATCH}"
                    RESULT_VARIABLE can_apply OUTPUT_QUIET ERROR_QUIET)
    if(can_apply EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH}" RESULT_VARIABLE applied)
        if(NOT applied EQUAL 0)
            message(FATAL_ERROR "Couldn't apply ${PATCH}")
        endif()
        message(STATUS "Applied ${PATCH}")
        continue()
    endif()

    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${PATCH}"
                    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(already EQUAL 0)
        message(STATUS "Already applied: ${PATCH}")
    else()
        message(FATAL_ERROR "${PATCH} neither applies nor is applied: has the JUCE version changed?")
    endif()
endforeach()
