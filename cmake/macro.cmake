include_guard(GLOBAL)

# 솔루션 최상위에는 앱 target만 두고 라이브러리와 도구 target은 필터로 접는다.
# `FOLDER`는 IDE generator 전용 속성이며 다른 generator는 무시하므로 분기하지 않는다.
function(cyella_set_ide_folder folder)
    if(NOT folder)
        message(FATAL_ERROR "An IDE folder name is required.")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "At least one target is required for IDE folder ${folder}.")
    endif()

    foreach(target IN LISTS ARGN)
        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Target not found for the IDE folder: ${target}")
        endif()
        set_target_properties("${target}" PROPERTIES FOLDER "${folder}")
    endforeach()
endfunction()

# IDE 필터 하나를 정하고 그 파일을 <variable>에 덧붙인다 (luil의 같은 이름 함수와
# 같은 계약이다).
#
#   cyella_add_source_filter(<variable> <filter> <file>...)
#
# target의 source 목록을 이 <variable>로 만들면 필터와 빌드 목록이 한 벌이 된다.
# 두 벌이면 새 파일이 한쪽에만 들어가고 빠진 쪽은 조용하다 — 빌드는 그대로 되고
# IDE에서만 파일이 엉뚱한 자리에 놓여, 알아채는 것은 한참 뒤다.
#
# 필터 이름의 `/`는 하위 필터를 연다 ("presentation/ui/dialogs"). 상대 경로는
# 부르는 CMakeLists.txt 기준이고 절대 경로(생성 파일)는 그대로 쓴다.
function(cyella_add_source_filter variable filter)
    if(NOT filter)
        message(FATAL_ERROR "A source filter name is required.")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "At least one file is required for the source filter ${filter}.")
    endif()

    set(files)
    foreach(file IN LISTS ARGN)
        if(IS_ABSOLUTE "${file}")
            list(APPEND files "${file}")
        else()
            list(APPEND files "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
        endif()
    endforeach()

    source_group("${filter}" FILES ${files})
    set("${variable}" ${${variable}} ${files} PARENT_SCOPE)
endfunction()

function(cyella_add_clang_format_targets)
    set(one_value_arguments
        CLANG_FORMAT_EXECUTABLE
        FORMAT_TARGET
        CHECK_TARGET)
    set(multi_value_arguments
        SOURCE_DIRECTORIES
        FILE_EXTENSIONS
        CHECK_COMMAND)
    cmake_parse_arguments(
        PARSE_ARGV 0
        arguments
        ""
        "${one_value_arguments}"
        "${multi_value_arguments}")

    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_add_clang_format_targets: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arguments_CLANG_FORMAT_EXECUTABLE)
        message(FATAL_ERROR "CLANG_FORMAT_EXECUTABLE is required.")
    endif()
    if(NOT arguments_FORMAT_TARGET OR NOT arguments_CHECK_TARGET)
        message(FATAL_ERROR "FORMAT_TARGET and CHECK_TARGET are required.")
    endif()
    if(NOT arguments_SOURCE_DIRECTORIES OR NOT arguments_FILE_EXTENSIONS)
        message(FATAL_ERROR "SOURCE_DIRECTORIES and FILE_EXTENSIONS are required.")
    endif()
    if(TARGET "${arguments_FORMAT_TARGET}" OR TARGET "${arguments_CHECK_TARGET}")
        message(FATAL_ERROR "The clang-format target names are already in use.")
    endif()

    set(format_patterns)
    foreach(source_directory IN LISTS arguments_SOURCE_DIRECTORIES)
        foreach(file_extension IN LISTS arguments_FILE_EXTENSIONS)
            list(APPEND format_patterns "${source_directory}/*.${file_extension}")
        endforeach()
    endforeach()

    file(GLOB_RECURSE format_sources CONFIGURE_DEPENDS ${format_patterns})
    if(NOT format_sources)
        message(FATAL_ERROR "No source files were found for clang-format.")
    endif()

    add_custom_target("${arguments_FORMAT_TARGET}"
        COMMAND "${arguments_CLANG_FORMAT_EXECUTABLE}" -i ${format_sources}
        COMMENT "Applying clang-format to project C++ sources"
        VERBATIM)

    if(arguments_CHECK_COMMAND)
        add_custom_target("${arguments_CHECK_TARGET}"
            COMMAND
                "${arguments_CLANG_FORMAT_EXECUTABLE}"
                --dry-run
                --Werror
                ${format_sources}
            COMMAND ${arguments_CHECK_COMMAND}
            COMMENT "Checking clang-format and project source style"
            VERBATIM)
    else()
        add_custom_target("${arguments_CHECK_TARGET}"
            COMMAND
                "${arguments_CLANG_FORMAT_EXECUTABLE}"
                --dry-run
                --Werror
                ${format_sources}
            COMMENT "Checking clang-format"
            VERBATIM)
    endif()
endfunction()
