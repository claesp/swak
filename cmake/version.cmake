set(SWAK_REVISION "unknown")
set(SWAK_GIT_TAG "")
set(SWAK_VERSION "${BASE_VERSION}+unknown")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        RESULT_VARIABLE result
    )
    if(result EQUAL 0 AND revision MATCHES "^[0-9a-f]+$")
        set(SWAK_REVISION "${revision}")
        string(SUBSTRING "${revision}" 0 12 short_revision)
        set(SWAK_VERSION "${BASE_VERSION}+g${short_revision}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE dirty
            ERROR_QUIET
            RESULT_VARIABLE status_result
        )
        if(NOT status_result EQUAL 0)
            string(APPEND SWAK_VERSION "-state-unknown")
        elseif(NOT dirty STREQUAL "")
            string(APPEND SWAK_VERSION "-dirty")
        endif()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_VARIABLE tag
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
            RESULT_VARIABLE tag_result
        )
        if(tag_result EQUAL 0)
            string(REGEX REPLACE "[^A-Za-z0-9._+-]" "_" SWAK_GIT_TAG "${tag}")
            string(SUBSTRING "${SWAK_GIT_TAG}" 0 128 SWAK_GIT_TAG)
        endif()
    endif()
endif()
configure_file("${SOURCE_DIR}/src/core/version.h.in" "${OUTPUT_FILE}" @ONLY)
