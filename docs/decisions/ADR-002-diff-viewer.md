# ADR-002: diff 뷰어는 WebView2 안의 자체 렌더러로 만든다

## 상태

승인됨 - 2026-10-19

## 배경

diff 뷰어에는 inline·좌우 분할 보기, 구문 강조, 줄 안의 단어 단위 강조, 큰 파일의 가상 스크롤,
이미지 비교가 필요하다. hunk·줄 단위 stage도 이 화면에서 한다.

luil에는 `webview_element`(WebView2 합성)가 있다.
luil로 직접 그리면 텍스트 배치, 구문 강조, 선택·복사, 단어 단위 강조를 모두 새로 만들어야 한다.

## 결정

1. **diff 본문은 WebView2로 그린다.** 파일 목록, 툴바, stage 버튼처럼 앱의 나머지 부분과 이어지는
   크롬은 luil로 그린다.
2. 렌더러는 **cyella 저장소 안의 HTML·JS·CSS**(`assets/diff-viewer/`)로 두고, 외부 CDN을 쓰지 않는다.
   실행 파일 리소스나 설치 폴더의 파일을 WebView2 가상 호스트(`https://cyella.diff/`)로 제공한다.
3. C++이 `git diff` 출력을 **구조화된 JSON**(파일 → hunk → 줄, 옛·새 줄 번호, 종류)으로 해석해
   웹뷰로 보낸다. 웹뷰는 unified diff 텍스트를 해석하지 않는다. 그래서 줄 선택을 C++가 가진 같은
   모델로 되돌려 받을 수 있다.
4. 구문 강조는 번들한 highlight.js(BSD-3-Clause)를, 줄 안의 단어 단위 강조는 jsdiff(BSD-3-Clause)를
   쓴다. 둘 다 고지를 실행 파일 고지에 넣는다.
5. 메시지는 처음부터 양방향으로 정의한다.
   - C++ → JS: `show_diff`, `set_layout`(inline/split), `set_theme`, `clear`
   - JS → C++: `stage_hunk`, `unstage_hunk`, `discard_hunk`, `stage_lines`, `unstage_lines`,
     `discard_lines`(hunk 번호와 줄 범위), `open_external`, `copy`
   - 줄 단위 stage는 C++가 선택한 줄로 부분 patch를 만들어 `git apply --cached`(또는 `-R`)로 적용한다.
6. 뷰어는 `diff_view` 인터페이스 뒤에 둔다. 나중에 luil 가상 목록으로 직접 그리는 구현으로 바꿀 수 있게
   하기 위해서다.

## 바이너리와 외부 도구

1. 이미지(png, jpg, gif, webp, bmp, svg)는 웹뷰에서 좌우·겹쳐 보기·밀어 보기로 비교한다. 두 판은
   `git show <rev>:<path>`로 임시 파일로 꺼낸다.
2. `.gitattributes`에 `diff=<driver>`가 있고 `diff.<driver>.textconv`가 설정되어 있으면, git이 변환한
   텍스트 diff를 그대로 보인다.
3. 그 밖의 바이너리는 "바이너리 파일"과 크기 변화를 보이고, **외부 도구로 열기**를 제공한다.
   - 설정에 확장자별 외부 도구 표를 둔다. 항목은 확장자, 실행 파일, 인자 템플릿(`{left}` `{right}`
     `{base}` `{path}`)이다.
   - 표에 없으면 `diff.tool`·`difftool.<name>.cmd` 설정이 있을 때 `git difftool --no-prompt`로 넘긴다.
   - 임시 파일은 작업이 끝나거나 앱이 종료될 때 지운다.

## 결과

- 텍스트 배치·선택·복사를 새로 만들지 않고 inline·분할 보기와 구문 강조를 갖출 수 있다.
- WebView2 Runtime이 필요하다. 없으면 diff 영역에 일반 텍스트 대체 보기와 설치 안내를 보인다.
- 웹뷰와 luil 사이의 초점·단축키 이동은 luil `webview_element`의 규칙을 따른다.

## 2026-10-20 구현 결정

결정 2·4를 구현하며 바꾼 것과, 구현에서 정한 것이다.

1. **`file://`로 연다.** luil 웹뷰에는 가상 호스트 매핑이 없어(`https://cyella.diff/`는 쓸 수 없다) 뷰어를
   실행 파일 옆 `diff-viewer/`에 두고 `file:///…/index.html`로 연다. `webview_policy::allowed_schemes`에
   `file`만 넣는다.
   - `file://` 문서에서도 항해, 페이지의 `chrome.webview.postMessage`, 공백·한글이 든 경로의 `file://` `<img>`,
     24 MiB `post_message`가 모두 된다. `tests/manual/webview_file_probe`(`--payload-mib`)로 다시 볼 수 있다.
   - file 문서에서는 ES 모듈을 읽지 못하므로 고전 `<script>`만 쓴다.
   - 빌드 후 단계가 `diff-viewer/`를 exe 옆에 복사하고, 설치 규칙(`install(DIRECTORY …)`)이 `bin/diff-viewer/`로 옮긴다.
2. **diff 하나는 메시지 하나다.** luil의 상한(`maximum_message_bytes`, 기본 256 KiB)은 **페이지가 보내는**
   메시지에만 걸리고, 앱이 페이지로 보내는 `post_message`에는 없다.
   - 그래서 쪼개 보내지(`part`·ack) 않는다. frame마다 마지막 메시지만 적용돼 ack 상태 기계가 필요해지기 때문이다.
   - 줄 수 상한(기본 10만 줄)을 넘으면 앞부분만 보내고 `truncated`·`total_lines`를 알린다. 뷰어의 "전체 보기"가
     `show_full`을 보내면 전체를 다시 보낸다. 잘린 동안은 patch 동작을 막는다.
   - `maximum_message_bytes`는 8 MiB로 올린다. 페이지가 보내는 가장 큰 메시지는 `copy`의 텍스트(최대 4 MiB,
     JSON 이스케이프로 두 배까지)이고, 줄 선택은 구간 목록이라 수 KiB다.
   - 모든 C++ → JS 메시지는 단조 증가하는 `seq`를 갖는다. 내용 메시지(`show_diff` 등)가 layout·theme을 함께
     실어 마지막 메시지만 적용돼도 상태가 어긋나지 않는다. JS 동작은 보고 있는 diff의 `seq`를 되돌려 보내고, 다르면
     거절한다. `ready`를 받기 전에는 보내지 않고, `ready`(웹뷰를 다시 읽었을 때 포함)에는 상태 전체를 다시 보낸다.
3. **올바르지 않은 UTF-8은 바이트를 잃지 않는다.** 줄 내용이 올바른 UTF-8이 아니면 base64로 보내고 줄 항목에
   표지(`1`)를 붙인다. 뷰어는 U+FFFD로 보인다. 경로 같은 표시값은 U+FFFD로 바꾼다. 동작은 줄 위치만 되돌려 보내므로
   왕복이 필요 없다.
4. **jsdiff는 쓰지 않는다.** 줄 안의 단어 단위 강조는 토큰 LCS를 직접 구현했다(`word-diff.js`). highlight.js
   11.11.1(BSD-3-Clause)은 c·cpp·cmake·js·ts·json·python·markdown·xml·css·powershell·bash·csharp·ini·yaml만 번들(82 KB)하고,
   라이선스 원문은 실행 파일 고지에 덧붙인다(`cyella_append_third_party_notices`).
5. **임시 파일은 앱이 끝날 때 지운다.** 외부 도구와 웹뷰가 작업이 끝난 뒤에도 파일을 열어 둘 수 있어서
   작업 단위로 지우지 않는다. 비정상 종료로 남은 24시간 넘은 폴더는 다음 실행이 지운다.

## 2026-10-20 구현 결정: 주 창에 끼우기

1. **웹뷰는 창에 하나, 자리표는 diff가 열렸을 때만.** 한 번 diff를 열면 `ui_webview`를 frame마다 싣고(`navigate_revision` 1), `webview_element`는 diff 영역이 열린 동안만 tree에 둔다.
   탭·파일을 바꿔도 같은 웹뷰에 `show_diff`만 보낸다. 무엇을 보일지는 (탭의 diff 요청, 세션의 `selected_diff`, `file_diff_loaded` 횟수)의 `sync`가 맞춘다.
2. **효과 실행은 `diff_view_coordinator`다.** `apply_patch`는 git operation으로, 두 쪽 꺼내기와 외부 도구 열기는 `task_scheduler`의 `diff-view` lane(취소 token, 앱 종료와 이어짐)으로,
   복사는 클립보드 요청으로 보낸다. 취소는 `diff_git_gateway::extract_cancellable`과 `external_open_request::cancellation`으로 닿는다.
3. **Runtime 부재는 앱이 미리 안다.** luil은 환경 생성 실패를 사건으로 알리지 않으므로(placeholder만 남는다) 시작할 때 WebView2 로더에 설치 여부를 묻고, 없으면 텍스트 hunk 목록으로 보인다.
4. **hunk·줄 discard 확인은 웹뷰 안에 둔다.** luil로 그리는 확인 대화 상자는 파일 단위 discard에만 걸린다.
5. **웹 콘텐츠의 접근성은 luil이 연결하지 않는다.** 빠진 연결은 [UI 구조](../ui-architecture.md)의 "알아 둘 것"에 적었다.
