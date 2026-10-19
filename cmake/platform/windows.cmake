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

    file(GLOB visual_studio_llvm_directories
        LIST_DIRECTORIES true
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/bin")
    find_program(clang_format_executable
        NAMES clang-format
        HINTS ${visual_studio_llvm_directories}
        NO_CACHE
        REQUIRED)
    set("${output_variable}" "${clang_format_executable}" PARENT_SCOPE)
endfunction()
