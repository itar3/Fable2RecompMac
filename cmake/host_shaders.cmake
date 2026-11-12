set(REEOT_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/hlsl")

if(NOT REEOT_DXC)
    if(DIRECTX_DXC_TOOL)
        set(_dxc_cmd "")
        foreach(_part IN LISTS DIRECTX_DXC_TOOL)
            if(_part MATCHES "^[A-Z_]+=")
                list(APPEND _dxc_cmd "${CMAKE_COMMAND}" -E env "${_part}")
            else()
                list(APPEND _dxc_cmd "${_part}")
            endif()
        endforeach()
        set(REEOT_DXC "${_dxc_cmd}" CACHE STRING "dxc compiler command" FORCE)
    else()
        find_program(REEOT_DXC NAMES dxc REQUIRED)
    endif()
endif()
message(STATUS "reeot: dxc at ${REEOT_DXC}")

function(reeot_host_shader TARGET STEM PROFILE)
    file(GLOB hlsl_includes "${REEOT_HLSL_DIR}/*.hlsli")

    if(REEOT_D3D12)
        set(ext dxil)
        set(format_args "")
    else()
        set(ext spirv)
        set(format_args -spirv -fvk-use-dx-layout)
        if(PROFILE MATCHES "^vs")
            list(APPEND format_args -fvk-invert-y)
        endif()
    endif()

    set(out "${REEOT_GEN_DIR}/shaders/${STEM}.hlsl.${ext}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REEOT_GEN_DIR}/shaders"
        COMMAND ${REEOT_DXC}
                -T ${PROFILE} -HV 2021 -all-resources-bound
                -Wno-ignored-attributes ${format_args}
                -I "${REEOT_HLSL_DIR}"
                -Fh "${out}" "${REEOT_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_${ext} ${ARGN}
        DEPENDS "${REEOT_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
        COMMENT "Compiling host shader ${STEM}.hlsl (${PROFILE}, ${ext})"
        VERBATIM)
    target_sources(${TARGET} PRIVATE "${out}")
endfunction()
