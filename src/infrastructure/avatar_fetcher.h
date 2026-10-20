#pragma once

#include "application/hosting_transport.h"
#include "application/process_cancellation.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace cyella {
    // 커밋 작성자 사진을 Gravatar에서 받아 디스크에 둔다. 환경 설정 `appearance.gravatar`를 켰을 때만 driver가 부른다.
    //  - 주소는 다듬은 메일(앞뒤 공백을 빼고 소문자)의 SHA-256 16진수다. `d=404`라 사진이 없으면 404가 온다.
    //  - 받은 사진은 `<해시>.img`, 사진이 없다는 답은 `<해시>.none`으로 남긴다. 같은 사람은 보관 기한 안에 다시 묻지 않는다.
    //  - 네트워크가 실패하면 기한이 지난 사진이라도 있으면 그것을 쓴다. 실패는 남기지 않아 다음에 다시 묻는다.
    //  - 호출 스레드가 막힌다. 워커에서 부른다.

    // Gravatar가 쓰는 꼴로 메일을 다듬는다 (앞뒤 공백을 빼고 ASCII 소문자). 다듬은 값이 사진 표의 키이기도 하다.
    [[nodiscard]] std::string normalize_avatar_email(std::string_view email);
    [[nodiscard]] std::string gravatar_url(std::string_view hash_hex, int pixel_size);

    struct avatar_fetch_options
    {
        std::filesystem::path cache_directory {};
        // 다듬은 메일의 SHA-256 16진수(소문자 64자)다. 플랫폼이 준다.
        std::function<std::string(std::string_view)> sha256_hex {};
        // 화면은 20~40 논리 픽셀이다. 배율 2에서도 흐리지 않게 받는다.
        int pixel_size { 80 };
        std::chrono::hours keep_image { 24 * 7 };
        std::chrono::hours keep_missing { 24 };
    };

    struct avatar_fetch_result
    {
        // 사진 파일의 내용(PNG·JPEG)이다. 사진이 없거나 실패했으면 비어 있다.
        std::string bytes {};
        // 사진이 없다는 답(404)을 받았거나 보관한 답이 그렇다.
        bool missing { false };
        // 받지 못했고 보관한 것도 없다 (네트워크·취소). 다음에 다시 묻는다.
        bool failed { false };
    };

    class avatar_fetcher
    {
    public:
        avatar_fetcher(hosting_transport& transport, avatar_fetch_options options);

        [[nodiscard]] avatar_fetch_result fetch(std::string_view email, const process_cancellation_token& cancellation) const;

    private:
        hosting_transport* transport_ { nullptr };
        avatar_fetch_options options_ {};
    };
} // namespace cyella
