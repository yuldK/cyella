#pragma once

// cyella 자신의 resource ID다. 101~104는 luil의 .rc 조각이 쓰고, 앱 resource는 105부터 정의한다.
#define IDI_CYELLA 105
// 앱 심볼 SVG다 (assets/cyella.svg). 캡션·시작 페이지·환경 설정에 그리도록 시작할 때 벡터 ui_image로 읽는다.
#define IDR_CYELLA_SYMBOL 106
// 타이틀 캘리그라피 SVG다. 밝은 테마용(짙은 남색 글자)과 어두운 테마용(옅은 바다색 글자) 두 장이다 (assets/cyella-title*.svg).
#define IDR_CYELLA_TITLE 107
#define IDR_CYELLA_TITLE_SEA 108
// 화면 문구 파일이다 (assets/locales/*.json, ADR-008). 한국어가 원문이라 늘 읽고, 고른 언어를 그 위에 얹는다.
#define IDR_LOCALE_KO 109
#define IDR_LOCALE_EN 110
