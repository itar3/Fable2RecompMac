# Patch series for the submodules (see cmake/native_renderer.cmake).

# Applies the patch series in PATCH_DIR (*.patch, in name order) to the
# working tree of the submodule thirdparty/NAME, unless it is applied already.
# A series cannot be checked patch by patch (later patches change the context
# of earlier ones), so the check applies it to a scratch copy of the index and
# compares that with the working tree. A tree with something else applied
# (an older patch set) is reset to the submodule commit first.
function(fable2_apply_patch_series NAME PATCH_DIR)
    set(_dir "${FABLE2_THIRDPARTY}/${NAME}")
    file(GLOB _patches "${PATCH_DIR}/*.patch")
    list(SORT _patches)
    if(NOT _patches)
        return()
    endif()

    # Not a git checkout of its own (the release app's source comes from
    # git archive): apply the series with plain git apply and record it in a
    # marker file, so a later configure knows it is applied. Git must not
    # pick up a repository in a parent directory, hence the ceiling.
    get_filename_component(_parent "${_dir}" DIRECTORY)
    execute_process(COMMAND git -C "${_dir}" rev-parse --show-toplevel
        OUTPUT_VARIABLE _top OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rev ERROR_QUIET)
    set(_own_repo FALSE)
    if(_rev EQUAL 0)
        file(REAL_PATH "${_top}" _top_real)
        file(REAL_PATH "${_dir}" _dir_real)
        if(_top_real STREQUAL _dir_real)
            set(_own_repo TRUE)
        endif()
    endif()
    if(NOT _own_repo)
        set(_hash "")
        foreach(_patch IN LISTS _patches)
            file(SHA256 "${_patch}" _one)
            string(APPEND _hash "${_one}")
        endforeach()
        string(SHA256 _hash "${_hash}")
        set(_marker "${_dir}/.fable2-patches-applied")
        if(EXISTS "${_marker}")
            file(READ "${_marker}" _old)
            string(STRIP "${_old}" _old)
            if(_old STREQUAL _hash)
                return()
            endif()
            message(FATAL_ERROR "thirdparty/${NAME} has another patch set applied (${_marker}): "
                                "restore its original files")
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${_parent}"
                                git apply ${_patches}
                        WORKING_DIRECTORY "${_dir}" RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Could not apply the patches in ${PATCH_DIR} to thirdparty/${NAME}")
        endif()
        file(WRITE "${_marker}" "${_hash}\n")
        list(LENGTH _patches _count)
        message(STATUS "fable2: applied ${_count} patches to thirdparty/${NAME}")
        return()
    endif()

    set(_index "${CMAKE_BINARY_DIR}/fable2-${NAME}-patches.index")
    file(REMOVE "${_index}")
    set(_git ${CMAKE_COMMAND} -E env "GIT_INDEX_FILE=${_index}" git -C "${_dir}")
    execute_process(COMMAND ${_git} read-tree HEAD RESULT_VARIABLE _r1 OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${_git} apply --cached ${_patches} RESULT_VARIABLE _r2 OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${_git} diff --quiet RESULT_VARIABLE _r3 OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE "${_index}")
    if(_r1 EQUAL 0 AND _r2 EQUAL 0 AND _r3 EQUAL 0)
        return()
    endif()
    execute_process(COMMAND git -C "${_dir}" diff --quiet HEAD RESULT_VARIABLE _clean OUTPUT_QUIET ERROR_QUIET)
    if(NOT _clean EQUAL 0)
        message(STATUS "fable2: thirdparty/${NAME} has other changes applied: resetting it to the submodule commit")
        execute_process(COMMAND git -C "${_dir}" checkout -- . RESULT_VARIABLE _reset)
        if(NOT _reset EQUAL 0)
            message(FATAL_ERROR "Could not reset thirdparty/${NAME}")
        endif()
    endif()
    execute_process(COMMAND git -C "${_dir}" apply ${_patches} RESULT_VARIABLE _applied)
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "Could not apply the patches in ${PATCH_DIR} to thirdparty/${NAME}")
    endif()
    list(LENGTH _patches _count)
    message(STATUS "fable2: applied ${_count} patches to thirdparty/${NAME}")
endfunction()
