include_guard(GLOBAL)

function(cyella_configure_msvc_link_directories)
    if(NOT MSVC OR NOT CMAKE_CXX_COMPILER)
        return()
    endif()

    # CMake 4.2에서는 Visual Studio 생성기 사용 시 MSVC 암시적 링크
    # 디렉터리가 비어 있을 수 있다. Skia 패키지 설정은 이 목록을 사용해
    # d3d12.lib, dxgi.lib과 같은 Windows 시스템 라이브러리를 찾는다.
    get_filename_component(_cyella_msvc_arch_dir
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_cyella_msvc_host_dir
        "${_cyella_msvc_arch_dir}" DIRECTORY)
    get_filename_component(_cyella_msvc_bin_dir
        "${_cyella_msvc_host_dir}" DIRECTORY)
    get_filename_component(_cyella_msvc_version_dir
        "${_cyella_msvc_bin_dir}" DIRECTORY)

    set(_cyella_msvc_lib_dir
        "${_cyella_msvc_version_dir}/lib/${CMAKE_GENERATOR_PLATFORM}")

    cmake_host_system_information(
        RESULT _cyella_program_files_x86
        QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows/CurrentVersion"
        VALUE "ProgramFilesDir (x86)"
        VIEW 64)
    set(_cyella_windows_sdk_lib_dir
        "${_cyella_program_files_x86}/Windows Kits/10/Lib/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    foreach(_cyella_link_dir
        "${_cyella_msvc_lib_dir}"
        "${_cyella_windows_sdk_lib_dir}/um/${CMAKE_GENERATOR_PLATFORM}"
        "${_cyella_windows_sdk_lib_dir}/ucrt/${CMAKE_GENERATOR_PLATFORM}")
        if(EXISTS "${_cyella_link_dir}")
            list(APPEND CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES
                "${_cyella_link_dir}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
    set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES
        "${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}" PARENT_SCOPE)
endfunction()

function(cyella_validate_msvc)
    cmake_parse_arguments(PARSE_ARGV 0 arguments "" "MINIMUM_VERSION" "")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_validate_msvc: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arguments_MINIMUM_VERSION)
        message(FATAL_ERROR "MINIMUM_VERSION is required.")
    endif()
    if(NOT MSVC OR MSVC_VERSION LESS arguments_MINIMUM_VERSION)
        message(FATAL_ERROR
            "MSVC ${arguments_MINIMUM_VERSION} or newer is required. "
            "Current value: ${MSVC_VERSION}")
    endif()
endfunction()

function(cyella_apply_msvc_options target)
    cmake_parse_arguments(PARSE_ARGV 1 arguments "" "ENABLE_ANALYZE" "")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_apply_msvc_options: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Target not found for MSVC options: ${target}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(option_scope INTERFACE)
    else()
        set(option_scope PRIVATE)
    endif()

    target_compile_options("${target}" ${option_scope}
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/sdl>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:__cplusplus>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:preprocessor>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/external:anglebrackets>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/external:W0>)

    if(arguments_ENABLE_ANALYZE)
        target_compile_options("${target}" ${option_scope}
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/analyze>
            $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/analyze:external->)
    endif()
endfunction()
