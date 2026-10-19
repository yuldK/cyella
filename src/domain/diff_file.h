#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cyella {
    enum class diff_line_kind
    {
        context,
        add,
        del,
        // `\ No newline at end of file` 표지다. 바로 앞 줄에 붙는 주석이며 줄 번호가 없다.
        no_newline,
    };

    struct diff_line
    {
        diff_line_kind kind { diff_line_kind::context };
        // 해당하지 않는 쪽(add의 옛 번호, del의 새 번호)은 0이다. 줄 번호는 1부터 센다.
        std::int32_t old_line { 0 };
        std::int32_t new_line { 0 };
        // 맨 앞 표지(' ', '+', '-')와 줄 끝 '\n'을 뺀 바이트다. CRLF 파일의 '\r'은
        // 내용의 일부로 남긴다.
        std::string content {};
    };

    struct diff_hunk
    {
        // 헤더에서 생략된 개수(`@@ -1 +1 @@`)는 1로 채운다.
        std::int32_t old_start { 0 };
        std::int32_t old_count { 0 };
        std::int32_t new_start { 0 };
        std::int32_t new_count { 0 };
        // `@@ ... @@ section` 줄 전체다(줄 끝 제외). hunk를 patch로 되돌릴 때 쓴다.
        std::string header {};
        // 헤더 뒤의 함수 문맥 문구다. 없으면 비어 있다.
        std::string section {};
        std::vector<diff_line> lines {};
    };

    struct diff_file
    {
        // 추가된 파일의 old_path, 삭제된 파일의 new_path는 비어 있다.
        std::string old_path {};
        std::string new_path {};
        // M 수정, A 추가, D 삭제, R 이름 변경, C 복사.
        char status { 'M' };
        // 이름 변경·복사의 유사도(0~100). 해당하지 않으면 0이다.
        std::int32_t similarity { 0 };
        // `100644` 같은 8진수 mode 문자열이다. 알 수 없으면 비어 있다.
        std::string old_mode {};
        std::string new_mode {};
        // `index` 줄의 축약 blob SHA다.
        std::string old_blob {};
        std::string new_blob {};
        bool is_binary { false };
        std::uint32_t lines_added { 0 };
        std::uint32_t lines_removed { 0 };
        std::vector<diff_hunk> hunks {};

        // 화면에 보일 경로다. 삭제된 파일은 옛 경로다.
        [[nodiscard]] const std::string& display_path() const noexcept
        {
            return new_path.empty() ? old_path : new_path;
        }
    };
} // namespace cyella
