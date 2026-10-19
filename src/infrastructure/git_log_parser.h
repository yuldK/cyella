#pragma once

#include "domain/commit_info.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `git log --topo-order -z --format=<make_git_log_args의 형식>`의 출력을 조각조각
    // 받아 커밋 레코드로 풀어 내는 스트리밍 해석기다.
    //
    // 출력 형식: 커밋마다 `sha 0x1F parents 0x1F author name 0x1F author email 0x1F
    // author time 0x1F committer time 0x1F subject [0x1F body]` 다음에 NUL이 온다.
    // parents는 공백으로 나뉜다.
    //
    // feed()에는 임의 경계로 자른 조각을 넣어도 된다. 레코드 한가운데나 UTF-8 문자
    // 한가운데서 끊겨도 같은 결과가 나온다. NUL은 UTF-8의 어느 바이트에도 나오지
    // 않으므로 NUL만 보고 레코드를 나누고, 바이트는 레코드가 끝난 뒤에야 문자열로
    // 만든다.
    //
    // 복사량은 입력 크기에 비례한다. 조각 안에 완결된 레코드는 조각을 가리키는 view로
    // 바로 읽고, 끝에 남은 미완결 꼬리만 내부 버퍼로 옮긴다. 이어지는 조각에서는 새로
    // 들어온 바이트만 NUL을 찾는다.
    class git_log_parser
    {
    public:
        using commit_handler = std::function<void(commit_info&&)>;

        // has_body는 make_git_log_args에서 본문을 요청했는지와 같아야 한다.
        explicit git_log_parser(commit_handler handler, bool has_body = false);

        void feed(std::string_view chunk);

        // 입력이 끝났음을 알린다. NUL로 끝나지 않은 마지막 조각이 해석 가능하면 레코드로
        // 내고, 아니면 깨진 레코드로 센다. 이후 같은 객체를 새 출력에 다시 쓸 수 있다.
        void finish();

        [[nodiscard]] std::size_t commit_count() const noexcept
        {
            return commit_count_;
        }

        // 필드 수나 숫자가 맞지 않아 버린 레코드 수다. 0이 아니면 형식이 어긋난 것이다.
        [[nodiscard]] std::size_t malformed_record_count() const noexcept
        {
            return malformed_record_count_;
        }

    private:
        void process_record(std::string_view record);

        commit_handler handler_ {};
        bool has_body_ { false };
        // 아직 NUL을 만나지 못한 꼬리다.
        std::string pending_ {};
        // pending_ 중 이미 NUL을 찾아본 앞부분의 길이다.
        std::size_t scanned_ { 0 };
        std::size_t commit_count_ { 0 };
        std::size_t malformed_record_count_ { 0 };
    };

    // 전체 출력이 이미 메모리에 있을 때의 편의 함수다.
    [[nodiscard]] std::vector<commit_info> parse_git_log(std::string_view bytes, bool has_body = false);
} // namespace cyella
