if(NOT DEFINED POWERSHELL_EXECUTABLE OR NOT DEFINED ROOT_DIRECTORY)
    message(FATAL_ERROR "POWERSHELL_EXECUTABLE and ROOT_DIRECTORY are required.")
endif()
if(NOT EXISTS "${POWERSHELL_EXECUTABLE}")
    message(FATAL_ERROR "PowerShell not found: ${POWERSHELL_EXECUTABLE}")
endif()

# check_source_style.ps1 자신의 시험이다. 표본 소스를 임시 폴더에 만들고 -root로 검사기를 돌려
# 위반을 잡아내는지, 멀쩡한 소스를 오류로 읽지 않는지 본다. 일부러 틀린 표본을 저장소 안에 두면
# 실제 검사(cyella_source_style)가 그것을 오류로 읽으므로 저장소 밖에서 만든다.
# 어느 PowerShell로든 돌릴 수 있게 실행 파일을 인자로 받는다 (Windows PowerShell 5.1 포함).
if(DEFINED ENV{TEMP})
    set(temp_directory "$ENV{TEMP}")
elseif(DEFINED ENV{TMP})
    set(temp_directory "$ENV{TMP}")
elseif(DEFINED ENV{TMPDIR})
    # macOS·POSIX의 사용자 임시 폴더다.
    set(temp_directory "$ENV{TMPDIR}")
else()
    message(FATAL_ERROR "TEMP is not set.")
endif()
file(TO_CMAKE_PATH "${temp_directory}" temp_directory)
string(RANDOM LENGTH 8 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789" suffix)
set(sample_directory "${temp_directory}/cyella_style_samples_${suffix}")
file(REMOVE_RECURSE "${sample_directory}")

# 1'000'000'000의 '는 숫자 구분자다. 검사기가 이것을 문자 리터럴의 시작으로 읽으면 같은 줄과 이후의
# 중괄호를 모두 놓친다. 표본의 7번째 줄은 멀쩡한 블록의 닫는 중괄호가 코드 뒤에 붙은 위반이다. 위반을
# 구분자와 같은 줄에 두어, 구분자 판정이 없으면 '000); }가 문자 리터럴로 삼켜져 위반을 놓치고 시험이 실패한다.
# valid 표본의 u8'a'는 앞뒤가 16진 글자라 구분자 분기에 들어가고, 낱말이 숫자로 시작하지 않아 문자 리터럴로
# 남아야 한다. 판정이 넓어져 이것을 구분자로 읽으면 따옴표 짝이 뒤집혀 같은 줄의 {가 삼켜지고, 남은 }가 앞
# 줄의 {와 짝지어져 오류가 난다.
# 검사기는 줄 끝이 CRLF가 아니면 오류로 보므로 줄 끝은 file(CONFIGURE)가 CRLF로 맞춘다.
string(JOIN "\n" misplaced_sample
    "namespace sample"
    "{"
    "    constexpr int big { 1'000'000'000 };"
    ""
    "    void run()"
    "    {"
    "        work(1'000); }"
    "}"
    "")
string(JOIN "\n" valid_sample
    "namespace sample"
    "{"
    "    constexpr int big { 1'000'000'000 };"
    "    constexpr int mask { 0xFF'FF };"
    "    constexpr char8_t letter { u8'a' };"
    ""
    "    void run()"
    "    {"
    "        work();"
    "        f(u8'a', { 1'000 });"
    "    }"
    "}"
    "")
file(CONFIGURE OUTPUT "${sample_directory}/misplaced/digit_separator.cpp"
    CONTENT "${misplaced_sample}" @ONLY NEWLINE_STYLE CRLF)
file(CONFIGURE OUTPUT "${sample_directory}/valid/digit_separator.cpp"
    CONTENT "${valid_sample}" @ONLY NEWLINE_STYLE CRLF)

function(run_style_check root_directory result_variable output_variable)
    execute_process(
        COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${ROOT_DIRECTORY}/scripts/check_source_style.ps1"
            -root "${root_directory}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output)
    set("${result_variable}" "${result}" PARENT_SCOPE)
    set("${output_variable}" "${output}${error_output}" PARENT_SCOPE)
endfunction()

run_style_check("${sample_directory}/misplaced" misplaced_result misplaced_output)
run_style_check("${sample_directory}/valid" valid_result valid_output)
# 실패하더라도 임시 폴더는 남기지 않는다.
file(REMOVE_RECURSE "${sample_directory}")

if(NOT misplaced_result EQUAL 1
    OR NOT misplaced_output MATCHES "digit_separator\\.cpp:7: The closing brace"
    OR NOT misplaced_output MATCHES "1 violation\\(s\\)")
    message(FATAL_ERROR
        "The style check must report exactly the misplaced closing brace (exit code ${misplaced_result}):\n${misplaced_output}")
endif()
if(NOT valid_result EQUAL 0
    OR NOT valid_output MATCHES "passed: 1 file\\(s\\)")
    message(FATAL_ERROR
        "The style check must accept digit separators (exit code ${valid_result}):\n${valid_output}")
endif()
