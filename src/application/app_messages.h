#pragma once

#include "application/git_operation.h"
#include "domain/operation_status.h"

#include <string>

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

    // 저장소 탭을 연다. 이미 열린 경로면 그 탭이다. 처음 열면 refresh가 돈다.
    struct open_repository_intent
    {
        std::u8string path {};
    };

    struct close_repository_intent
    {
        std::string repository_id {};
    };

    // UI가 저장소 operation을 낸다. 값은 `git_operation.h`의 요청 타입이다.
    struct submit_operation_intent
    {
        std::string repository_id {};
        git_operation operation {};
    };

    // clone·init·inspect_path를 낸다. 저장소 id 대신 대상 경로로 묶인다.
    struct submit_setup_intent
    {
        git_operation operation {};
    };

    struct cancel_operation_intent
    {
        operation_id id { 0 };
    };
} // namespace cyella
