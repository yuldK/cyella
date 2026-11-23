#pragma once

#include "domain/commit_info.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 파일 이력의 한 커밋이다.
    struct git_file_log_entry
    {
        commit_info commit {};
        // 이 커밋에서의 경로다.
        std::string path {};
        // 이 커밋이 이름을 바꿨거나 복사했으면 옛 경로다. 아니면 비어 있다.
        std::string previous_path {};
        // name-status의 상태 글자다 (A·M·D·R·C·T). 유사도 숫자(`R088`)는 버린다.
        char status { 'M' };
    };

    // `git log --follow -z --name-status --format=<make_git_file_log_args의 형식>`의 출력을 조각조각 받아 읽는 스트리밍 해석기다.
    //
    // 출력 형식 (git 2.56에서 확인): 레코드는 `0x1E <log 필드를 0x1F로 구분> NUL`로 시작하고, 변경이 있으면
    // `\n <상태> NUL <경로> NUL`이 뒤따른다. 이름 바꾸기·복사는 `\n R088 NUL <옛 경로> NUL <새 경로> NUL`이다. 다음 레코드는
    // 줄바꿈 없이 바로 0x1E로 시작한다.
    //
    // NUL로 토큰을 나눈 뒤 순서로 읽는다 (머리 → 상태 → 경로 하나나 둘). 경로 토큰은 개수로 세어 가져가므로 경로나 제목에
    // 0x1E가 들어 있어도 레코드 경계로 오인하지 않는다.
    //
    // name-status가 없는 레코드는 직전(더 새) 항목에서 이어지는 경로를 물려받는다. 직전 항목이 이름을 바꿨으면 그 옛 경로이고,
    // 첫 레코드면 요청한 경로다. `--follow`는 diff가 비는 커밋을 내지 않으므로 실제로는 드물다.
    //
    // feed()에는 임의 경계로 자른 조각을 넣어도 같은 결과가 나온다.
    class git_file_log_parser
    {
    public:
        using entry_handler = std::function<void(git_file_log_entry&&)>;

        // path는 이력을 읽기 시작한 리비전에서의 경로다.
        git_file_log_parser(entry_handler handler, std::string path);

        void feed(std::string_view chunk);

        // 입력이 끝났음을 알린다. 마지막 레코드는 여기서 나간다. NUL로 끝나지 않은 꼬리는 잘린 출력이라 깨진 레코드로 세고,
        // 경로가 잘린 레코드는 내지 않는다.
        void finish();

        [[nodiscard]] std::size_t entry_count() const noexcept
        {
            return entry_count_;
        }

        // 머리의 필드나 상태가 맞지 않아 버린 레코드 수다. 0이 아니면 형식이 어긋난 것이다.
        [[nodiscard]] std::size_t malformed_record_count() const noexcept
        {
            return malformed_record_count_;
        }

    private:
        void process_token(std::string_view token);
        void begin_record(std::string_view header);
        void begin_status(std::string_view status);
        void emit_current();

        entry_handler handler_ {};
        // 아직 NUL을 만나지 못한 꼬리와, 그중 이미 NUL을 찾아본 앞부분의 길이다.
        std::string pending_ {};
        std::size_t scanned_ { 0 };
        // 머리를 읽고 name-status를 기다리는 레코드다.
        std::optional<git_file_log_entry> current_ {};
        // 지금 레코드가 상태를 이미 받았다. 한 레코드에 항목이 여럿이면 첫 항목만 쓴다.
        bool current_has_status_ { false };
        // 읽고 있는 상태의 남은 경로 토큰 수와, 그 경로를 지금 레코드에 담을지다.
        std::size_t paths_wanted_ { 0 };
        bool keep_paths_ { false };
        // 깨진 머리를 만났다. 다음 머리까지의 토큰을 버린다.
        bool skipping_ { false };
        // 다음(더 오래된) 커밋에서 파일이 가진 경로다.
        std::string followed_path_ {};
        std::string initial_path_ {};
        std::size_t entry_count_ { 0 };
        std::size_t malformed_record_count_ { 0 };
    };

    // 전체 출력이 이미 메모리에 있을 때의 편의 함수다.
    [[nodiscard]] std::vector<git_file_log_entry> parse_git_file_log(std::string_view bytes, std::string_view path);
} // namespace cyella
