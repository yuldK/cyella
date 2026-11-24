include_guard(GLOBAL)

# macOS 구성을 검사한다. luil의 Skia 패키지(mac-arm64)가 정한 것과 어긋나면 링크에서야 드러나거나(아키텍처) 오래된 OS에서
# 실행할 때에야 드러나므로(최소 판) configure에서 원인과 함께 잡는다. luil의 같은 검사(luil_validate_macos_platform)도 돌지만,
# 그 전에 cyella의 configure가 먼저 멈춰야 고칠 자리가 분명하다.
function(cyella_validate_macos_platform)
    set(one_value_arguments ARCHITECTURE MINIMUM_VERSION)
    cmake_parse_arguments(PARSE_ARGV 0 arguments "" "${one_value_arguments}" "")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_validate_macos_platform: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arguments_ARCHITECTURE OR NOT arguments_MINIMUM_VERSION)
        message(FATAL_ERROR "ARCHITECTURE and MINIMUM_VERSION are required.")
    endif()

    if(CMAKE_OSX_ARCHITECTURES)
        set(architectures ${CMAKE_OSX_ARCHITECTURES})
    else()
        set(architectures "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(NOT architectures STREQUAL arguments_ARCHITECTURE)
        message(FATAL_ERROR
            "The macOS architecture must be ${arguments_ARCHITECTURE}. "
            "Current value: ${architectures}\n"
            "Set CMAKE_OSX_ARCHITECTURES=${arguments_ARCHITECTURE}.")
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "CMAKE_OSX_DEPLOYMENT_TARGET is not set.\n"
            "Set it to ${arguments_MINIMUM_VERSION} or newer before the first configure. "
            "The macos-arm64 presets in CMakePresets.json set it.")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS arguments_MINIMUM_VERSION)
        message(FATAL_ERROR
            "macOS ${arguments_MINIMUM_VERSION} or newer is required as the deployment target. "
            "Current value: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
endfunction()

# Apple 헤더가 식별자를 매크로로 덮지 않게 한다. `AssertMacros.h`의 `check`·`verify`·`require` 매크로가 luil 공개 헤더의
# `check` 같은 이름을 펼친다 (luil_apply_macos_options와 같은 이유다).
function(cyella_apply_macos_options target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Target not found for macOS options: ${target}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(option_scope INTERFACE)
    else()
        set(option_scope PRIVATE)
    endif()

    target_compile_definitions("${target}" ${option_scope}
        __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES=0)
endfunction()

# 대상을 앱 번들(`<OUTPUT_NAME>.app`)로 세운다. Info.plist는 src/platform/macos/resources/Info.plist.in을 채운 것이다.
function(cyella_add_macos_bundle target)
    cmake_parse_arguments(PARSE_ARGV 1 arguments "" "IDENTIFIER;NAME;EXECUTABLE;TEMPLATE;ICON" "")
    if(arguments_UNPARSED_ARGUMENTS OR NOT arguments_IDENTIFIER OR NOT arguments_NAME OR NOT arguments_EXECUTABLE OR NOT arguments_TEMPLATE)
        message(FATAL_ERROR "cyella_add_macos_bundle needs IDENTIFIER, NAME, EXECUTABLE and TEMPLATE.")
    endif()
    set(CYELLA_BUNDLE_IDENTIFIER "${arguments_IDENTIFIER}")
    set(CYELLA_BUNDLE_NAME "${arguments_NAME}")
    set(CYELLA_BUNDLE_EXECUTABLE "${arguments_EXECUTABLE}")
    set(CYELLA_BUNDLE_ICON "${arguments_ICON}")
    set(CYELLA_MINIMUM_MACOS_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
    configure_file("${arguments_TEMPLATE}" "${plist}" @ONLY)
    set_target_properties("${target}" PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${plist}"
        OUTPUT_NAME "${arguments_EXECUTABLE}")
endfunction()
