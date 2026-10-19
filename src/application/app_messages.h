#pragma once

namespace cyella {
    // logic thread가 처리하는 앱 메시지(intent)다. 평범한 구조체이며 luil의
    // `app_message`에 담겨 스레드를 건넌다. 이 헤더는 luil UI 헤더를 include하지 않는다.

    // 창 크기와 배율이 정해졌거나 바뀌었다. 길이는 물리 픽셀이다.
    struct window_metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };

    // 창을 닫는다 (caption X, Alt+F4).
    struct close_intent
    {};

    // worker 결과가 `operation_result_relay`에 쌓였으니 비우라는 깨우기다. 내용은 relay 큐에
    // 있고 이 메시지는 아무것도 나르지 않는다.
    struct relay_drain_intent
    {};
} // namespace cyella
