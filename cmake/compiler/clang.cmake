include_guard(GLOBAL)

# Xcode의 clang으로 cyella 자신을 컴파일할 때의 규율이다. MSVC의 `/W4 /WX`와 같은 무게로 둔다.
#  - 컴파일러 ID를 둘 다 적는다. Xcode의 clang은 `Clang`이 아니라 `AppleClang`이라 하나만 적으면 경고 옵션이 오류 없이 빠진다.
#  - 언어도 둘 다 적는다. Objective-C++(.mm)는 `CXX`가 아니라 `OBJCXX`로 컴파일된다.
#  - `-Wno-missing-field-initializers`: 지정 초기화(`{ .a = 1 }`)로 일부 멤버만 주고 나머지는 멤버 기본값을 쓰는 것이 이 코드의
#    관용이다. MSVC는 이것을 경고하지 않는다.
function(cyella_apply_clang_options target)
    cmake_parse_arguments(PARSE_ARGV 1 arguments "" "WARNINGS_AS_ERRORS" "")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_apply_clang_options: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Target not found for clang options: ${target}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY")
        set(option_scope INTERFACE)
    else()
        set(option_scope PRIVATE)
    endif()

    set(is_clang_cxx "$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>")
    set(is_clang_objcxx "$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>")
    set(is_clang "$<OR:${is_clang_cxx},${is_clang_objcxx}>")

    target_compile_options("${target}" ${option_scope}
        $<${is_clang}:-Wall>
        $<${is_clang}:-Wextra>
        $<${is_clang}:-Wno-missing-field-initializers>)
    # .mm은 ARC로 컴파일한다. Objective-C 개체의 수명을 손으로 세지 않는다.
    target_compile_options("${target}" ${option_scope}
        $<${is_clang_objcxx}:-fobjc-arc>)

    if(NOT DEFINED arguments_WARNINGS_AS_ERRORS OR arguments_WARNINGS_AS_ERRORS)
        target_compile_options("${target}" ${option_scope}
            $<${is_clang}:-Werror>)
    endif()
endfunction()

# 빌드한 PC의 경로가 실행 파일에 남지 않게 `__FILE__`의 앞부분을 바꾼다 (MSVC의 `/d1trimfile` 자리).
#  - third_party까지 걸리도록 디렉터리 범위에 건다 (`add_subdirectory`보다 먼저 부른다).
function(cyella_trim_clang_build_paths)
    foreach(_cyella_root "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
        add_compile_options(
            "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-ffile-prefix-map=${_cyella_root}/=>"
            "$<$<COMPILE_LANG_AND_ID:OBJCXX,Clang,AppleClang>:-ffile-prefix-map=${_cyella_root}/=>")
    endforeach()
endfunction()
