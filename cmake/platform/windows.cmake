include_guard(GLOBAL)

function(cyella_validate_windows_platform)
    set(one_value_arguments ARCHITECTURE MINIMUM_SDK_VERSION)
    set(multi_value_arguments SUPPORTED_GENERATORS)
    cmake_parse_arguments(
        PARSE_ARGV 0
        arguments
        ""
        "${one_value_arguments}"
        "${multi_value_arguments}")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_validate_windows_platform: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arguments_ARCHITECTURE OR NOT arguments_MINIMUM_SDK_VERSION)
        message(FATAL_ERROR "ARCHITECTURE and MINIMUM_SDK_VERSION are required.")
    endif()
    if(NOT arguments_SUPPORTED_GENERATORS)
        message(FATAL_ERROR "SUPPORTED_GENERATORS is required.")
    endif()
    if(NOT WIN32)
        message(FATAL_ERROR "${PROJECT_NAME} supports only Windows 11 x64.")
    endif()

    list(FIND arguments_SUPPORTED_GENERATORS "${CMAKE_GENERATOR}" generator_index)
    if(generator_index EQUAL -1)
        string(JOIN ", " supported_generators ${arguments_SUPPORTED_GENERATORS})
        message(FATAL_ERROR
            "A supported generator is required: ${supported_generators}. "
            "Current value: ${CMAKE_GENERATOR}")
    endif()
    if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL arguments_ARCHITECTURE)
        message(FATAL_ERROR
            "The generator platform must be ${arguments_ARCHITECTURE}. "
            "Current value: ${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        message(FATAL_ERROR "Could not determine the Windows SDK selected by Visual Studio.")
    endif()
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS arguments_MINIMUM_SDK_VERSION)
        message(FATAL_ERROR
            "Windows SDK ${arguments_MINIMUM_SDK_VERSION} or newer is required. "
            "Current value: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    endif()
endfunction()

function(cyella_apply_windows_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Target not found for Windows options: ${target}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(option_scope INTERFACE)
    else()
        set(option_scope PRIVATE)
    endif()

    target_compile_definitions("${target}" ${option_scope}
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)
endfunction()

function(cyella_disable_automatic_windows_manifest target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Target not found for the manifest option: ${target}")
    endif()
    target_link_options("${target}" PRIVATE /MANIFEST:NO)
endfunction()

function(cyella_find_visual_studio_clang_format output_variable)
    if(NOT output_variable)
        message(FATAL_ERROR "An output variable for the clang-format path is required.")
    endif()

    # Visual Studio를 여럿 깔면 판마다 clang-format 판이 다르다(2022는 19, 18은 22). 판에 따라 결과가
    # 달라 관계없는 줄까지 바뀌므로, 찾은 것 가운데 가장 높은 판을 쓰고 기준 판(22)보다 낮으면 멈춘다.
    set(minimum_major_version 22)
    file(GLOB clang_format_candidates
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-format.exe"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/bin/clang-format.exe")
    set(best_executable "")
    set(best_major 0)
    foreach(candidate IN LISTS clang_format_candidates)
        execute_process(
            COMMAND "${candidate}" --version
            OUTPUT_VARIABLE version_output
            ERROR_QUIET
            RESULT_VARIABLE version_result)
        if(version_result EQUAL 0 AND version_output MATCHES "clang-format version ([0-9]+)")
            if(CMAKE_MATCH_1 GREATER best_major)
                set(best_major "${CMAKE_MATCH_1}")
                set(best_executable "${candidate}")
            endif()
        endif()
    endforeach()
    if(best_major LESS minimum_major_version)
        message(FATAL_ERROR
            "clang-format ${minimum_major_version} or newer from Visual Studio is required (found: '${best_executable}', major ${best_major}).")
    endif()
    set("${output_variable}" "${best_executable}" PARENT_SCOPE)
endfunction()
