# UI 구조

cyella 화면은 luil의 불변 tree 위에 선다. 상태는 앱이 들고, frame마다 그 상태에서 tree를 새로 짓는다.
이 문서는 그 흐름과 인증, diff 웹뷰, 대화 상자·메뉴·끌어 놓기 같은 구성 요소가 붙는 자리를 적는다.

## 흐름: 상태 → snapshot → tree

```text
worker 사건 ─ relay ─┐
                     ▼
              cyella_driver::handle ── 백엔드 메시지(app_messages.h) ──▶ git_workspace
                     │
                     └─ 화면 메시지(ui_intents.h) ──▶ shell_controller (탭·ui_state·설정)
                                                          │ after_events: 끝난 operation의 후속
cyella_driver::make_frame
   shell_controller::make_view ──▶ build_view_snapshot (순수 함수) ──▶ view_snapshot
   ui::build_ui_tree(view_snapshot) ──▶ 배치가 끝난 ui_tree + frame_feedback
   shell_controller::apply_feedback(frame_feedback)
```

- `build_view_snapshot`(`presentation/build_view_snapshot.*`)은 workspace·탭·설정·`ui_state`만 읽는 순수 함수다.
  시계도 입력(`view_inputs::now`)이라 같은 입력이면 같은 snapshot이다. test는 세션에 사건을 직접 넣어 만든다.
- `ui::build_ui_tree`(`presentation/ui/*`)는 snapshot만 읽어 tree를 짓고 **한 번만** 배치한다
  (`luil::make_arranged_tree`). 셸 판의 자리는 `compute_shell_layout` 한 함수가 정하고 `absolute_element`가 그대로 놓는다.
- 예외 하나: 그래프 행은 10만 개를 frame마다 복사하지 않으려고 snapshot이 세션의 행 벡터를 포인터로 가리킨다
  (`graph_view::rows`). 포인터는 그 `make_frame` 안에서만 쓰인다 — 행 element는 짓는 순간 자기 값을 복사한다.
- `frame_feedback`은 배치 뒤에야 아는 값(흘릴 수 있는 최대치, 그래프 창 높이)이다. controller가 다음 스크롤을
  다듬고 "끝 근처면 `load_more_log`"를 판정하는 데 쓴다.
- element는 상태를 고치지 않는다. 누르면 intent를 낸다. 저장소 작업은 `submit_operation_intent`를 바로 내고
  (stage·unstage·discard), 화면 상태를 바꾸는 것은 `ui_intents.h`의 메시지다.

## `ui_state` (`presentation/ui_state.h`)

껐다 켜면 잃어도 되는 화면 상태만 담는다. driver가 소유한 `shell_controller` 안에 있다.

| 범위 | 값 |
| --- | --- |
| 탭마다 (`tab_ui_state`, 탭 id가 key) | 고른 커밋 SHA(WIP는 `"WIP"`), 사이드바 고른 항목, 그래프·사이드바·상세·diff 스크롤, 접힌 구역, 사이드바 거르기 초안, 커밋 제목·본문 초안, amend, 연 diff 요청, 낸 커밋 operation, 닫은 배너 |
| 셸 전체 | 사이드바·상세 폭, 작업 로그 열림, 시작 페이지 검색 초안·스크롤, 서랍 열림, 아이콘 막대에서 연 구역, Clone 대화 상자 초안, 안내 한 줄, IME 조합 |

- 탭 목록의 정본은 `repository_tabs`, git 상태의 정본은 workspace·세션, 설정의 정본은 `app_settings_service`다.
- 판 폭은 `app_settings::ui.pane_widths`에 남긴다. 손잡이에 "놓았다"는 사건이 없어서 끄는 동안(`resize_pane_intent`)은 쓸 시각만 500 ms 뒤로 미루고,
  `shell_controller::tick`(driver의 `next_tick`)이 한 번 쓴다. 앱을 닫을 때는 `flush`가 바로 쓴다. 시작할 때 저장된 폭을 범위로 다듬어 되살린다.
- 텍스트 칸은 luil 규약 그대로다. `cyella_policy::text_target_of`가 kind → `text_target`을 답하고, 편집은
  `edit_text_intent`로 와서 `ui_state`의 `text_edit_state`에 `luil::apply_text_edit`로 적용된다. 한 줄 칸이라 붙여넣은 줄바꿈은 공백이 된다.

## 반응형 규칙 (`presentation/shell_layout.*`)

경계는 논리 픽셀(물리 폭 ÷ 배율)로 판정한다.

| 폭 | 배치 |
| --- | --- |
| 1200 이상 (`wide`) | 사이드바 232 · 그래프 · 상세 360의 3단. 판 사이 손잡이로 폭을 바꾼다(사이드바 160~420, 상세 280~560). 그래프는 최소 240을 지키며 상세 → 사이드바 순으로 줄인다. |
| 760~1199 (`medium`) | 상세가 접힌다. 툴바 오른쪽 "상세 패널" 단추나 커밋 고르기가 오른쪽 서랍을 연다. 서랍은 그래프 위에 겹치고 Esc·× 로 닫는다. |
| 760 미만 (`narrow`) | 사이드바가 44px 아이콘 막대가 된다. 아이콘을 누르면 그 구역이 떠 있는 판으로 열린다(같은 행 element라 두 번 누름 체크아웃도 된다). 그래프는 작성자·날짜 열을 감추고 라벨 열을 120으로 줄인다. |
| 960 미만 | 툴바 단추의 글자가 빠진다. |
| 480 미만 | 툴바의 저장소 선택 상자가 숨는다 (창 최소 폭이 480이라 실제로는 경계에서만). |

## 화면 세부

- 환경 설정·테마 단추는 캡션이 아니라 탭 줄 오른쪽에 있다(아래 "알아 둘 것"의 캡션 띠). 판 폭(`ui.pane_widths`)은 설정 파일에만 남고 환경 설정 화면에는 없다.
- 탭은 아이콘 자리 하나에 표시(🔔 ⟳ ⌚ ●)를 그린다(`tab_item`에 칸이 하나뿐이다).
- 커밋 본문은 `multiline_text_element`다. Enter·방향키·Home·End와 여러 줄 붙여넣기를 지원하며, 커밋 입력 영역은 패널 높이에 맞춰 약 280~360 논리 픽셀을 쓴다(낮은 창에서는 파일 목록 자리를 남기며 줄인다).
- 시작 페이지의 git 버전은 제목 아래 한 줄이다.
- 작업 영역의 파일은 목록·트리로 전환하며(`working-files-view`, `ui.working_files_view`), Unstaged·Staged의 폴더 접기는 독립적이다. 행의 단추는 늘 보이고 체크박스 일괄 선택은 없다. discard는 확인 대화 상자를 거친다.
- 원격에 없는 태그 표시는 그 실행에서 fetch(또는 태그 push)가 한 번 돈 뒤에야 맞다. 그 전에는 원격이 있는 저장소의 태그가 모두 점선이다
  (`ref.remotes`가 `ls-remote` 전에는 비어 있다).
- 사이드바의 원격 묶음 머리는 접히지 않는다.

## 구성 요소가 붙는 자리

- **인증**: `askpass_bridge`는 `cyella_driver`가 소유한다(백엔드 멤버들 옆, scheduler보다 앞에 선언). `answer_input_intent`·`cancel_input_intent`는
  `cyella_driver::handle_backend_intent`가 처리한다. 질문은 아래 "대화 상자·메뉴·끌어 놓기"의 입력 상자로 뜬다. 탭의 🔔 표시는 `compute_tab_marker`가 낸다.
- **diff 웹뷰**: `diff_view_coordinator`는 driver가 controller 옆에 두고 `make_frame`에서 `frame->webviews`를 채운다. 아래 "diff 웹뷰" 절.
- **대화 상자**: Clone처럼 `ui_state`에 열림 상태를 두고 `ui::build_ui_tree`의 마지막 자식으로 `luil::modal_host_element`로 단다.
- **메뉴**: `ui_frame::popups`에 `menu_element` tree로 싣는다(`cyella_policy::menu`를 함께 채운다).
- **끌어 놓기**: 그래프 행(`graph_row_element`)의 라벨 칩과 사이드바 행(`sidebar_row_element`)에 `drag`·`drop`을 단다.
- 탭을 닫으면 그 저장소의 끝나지 않은 작업을 취소한다. 취소 전에 묻는 확인 상자는 없다.

## diff 웹뷰

파일을 고르면 `tab_ui_state::diff`가 서고 `diff_file`이 나간다. 그래프 자리를 diff가 덮는 판은 `presentation/ui/diff_webview.cpp`의
`build_diff_area`다. 머리 막대(돌아가기·파일명·Inline/좌우 분할·이미지 비교 방식)는 luil이 그리고, 본문은 `webview_element` 자리표다.

```text
cyella_driver
 ├ diff_view_coordinator ── diff_view_controller (입출력 없음, diff_view_effect를 값으로 돌려준다)
 │    ├ apply_patch_effect   → workspace.submit(apply_patch_request) → 끝나면 같은 diff_file을 다시 낸다
 │    ├ prepare_sides_effect → task_scheduler("diff-view" lane, 취소 token) → diff_git_gateway.extract
 │    ├ open_external_effect → task_scheduler("diff-view" lane, 취소 token) → external_diff_opener.open
 │    └ copy_text_effect     → app_host::post_clipboard_request
 └ make_frame: sync_diff_view() → controller.poll() → frame->webviews (id "diff", 한 개)
```

- **웹뷰는 창에 하나다.** 한 번 diff를 열면 그 뒤로 frame마다 같은 `ui_webview`(id `diff`)를 싣는다(`navigate_revision`은 늘 1이라 다시 읽지
  않는다). 자리표(`webview_element`)가 tree에 없는 동안은 luil이 웹뷰를 숨기고 페이지 상태는 남는다. 탭·파일을 바꾸면 컨트롤러가 `show_diff`만
  새로 보낸다.
- **무엇을 보일지는 `sync`가 맞춘다.** driver가 frame마다 (선택된 탭의 열린 diff 요청, 그 저장소 세션의 `selected_diff`, 그 저장소에서 `file_diff_loaded`가
  온 횟수)를 넘기면, 요청이 같은 diff가 읽혀 있고 횟수가 바뀌었을 때만 `show`한다. 읽는 중이면 낡은 내용을 지워 stage 단추가 엉뚱한 파일에 걸리지
  않게 한다. 동작은 **보이고 있는 diff의 저장소**로 나가므로 탭을 옮긴 사이에 끝나도 맞는 저장소를 고친다.
- **정책**: `allowed_schemes={file}`, 메시지 8 MiB, **개발자 도구 끔**, 기본 컨텍스트 메뉴 끔. 웹뷰가 보낸 메시지는 문서 주소가 우리가 연 뷰어와 같을 때만
  믿는다(`is_viewer_document`). 뷰어 파일은 실행 파일 옆 `diff-viewer\index.html`이다.
- **WebView2 Runtime이 없을 때**: luil은 환경을 만들지 못해도 앱에 사건을 보내지 않고 자리표의 placeholder만 남긴다. 그래서 앱이 시작할 때
  `GetAvailableCoreWebView2BrowserVersionString`으로 직접 확인하고(`webview_runtime_installed`), 뷰어 파일이 없거나 프로필 폴더를 모를 때도 같이 거른다.
  쓸 수 없으면 `diff_surface_state::webview_available`이 거짓이고 본문은 텍스트 hunk 목록(`diff_row_element`)이다. `--no-webview`로 강제할 수 있다.
  웹뷰가 뜬 뒤의 `navigation_failed`·`browser_process_failed`도 같은 대체 보기로 넘긴다.
- **테마**: driver가 `luil::resolve_color_theme` + `color_palette_for`로 팔레트를 얻어 `make_diff_theme`이 뷰어의 CSS 변수(`--window-background` …)로 옮긴다.
  테마가 바뀌면 `set_theme` 메시지 하나가 간다. 시스템 모드는 `AppsUseLightTheme`를 읽는다(고대비 PC는 luil의 고대비 팔레트를 쓰지 않으므로 뷰어는 어두운 기본 색에 가깝다).
- **복사**는 `host->post_clipboard_request(clipboard_copy_request{text})`다.
- **외부 도구**: 결과가 `no_tool`·`launch_failed`·`extraction_failed`·`invalid_template`이면 안내 배너(`show_notice_intent`)로 알린다. 도구를 띄웠으면 짧은 확인이다.
- **discard 확인**: hunk·줄 discard는 웹뷰 안의 확인("정말 버릴까요?")을 쓴다. luil 확인 대화 상자는 **파일 단위** discard에만 걸린다
  (웹뷰 안의 단추를 luil이 가로채지 못한다).
- **접근성**: 아래 "알아 둘 것"의 웹 콘텐츠와 UI Automation 항목을 본다.

## 환경 설정 화면

탭 줄 오른쪽의 톱니바퀴가 `open_settings_intent`를 낸다. 대화 상자(`modal_host_element`, 최대 900×700)이고 왼쪽 목록에서 일반·diff 도구·SSH를 고른다.
`presentation/settings/*`가 상태·메시지·snapshot이고 `presentation/ui/settings/settings_ui.cpp`가 tree다.

```text
settings_controller (shell_controller가 소유) ── handle(intent) ──▶ app_settings_service.update   (테마·git 경로·도구 표)
        │                                       └──────────────▶ git_workspace.submit_setup     (ssh_keygen·ssh_connection_test)
        └ make_view() ──▶ build_settings_view (순수 함수) ──▶ settings_view ──▶ build_settings_dialog
cyella_driver ── request_ssh_info ──▶ task_scheduler("ssh-info") ──▶ collect_ssh_info ──▶ relay ──▶ ssh_info_loaded_intent
```

- **일반**: 테마(system·light·dark, 고르면 바로 저장), git 실행 파일(이번 실행이 찾은 경로·버전·자동 탐색/직접 지정, 경로 초안과 [찾아보기…]·[적용]·[자동 탐색으로 되돌리기]).
  경로는 절대 경로의 존재하는 파일이어야 저장한다. 이번 실행의 git은 이미 정해졌으므로 다음 실행부터 적용된다고 알린다.
- **diff 도구**: `app_settings.external_diff_tools` 표(확장자·실행 파일·인자 템플릿)와 추가·편집·삭제. 편집 상자에서 토큰(`{left}` `{right}` `{base}` `{path}`)을 누르면 인자 끝에 붙고,
  샘플 경로로 푼 **argv 미리보기**를 한 줄에 하나씩 보인다(공백이 든 경로가 한 인자로 남는지 눈으로 확인). 확장자·실행 파일이 비었거나 템플릿의 따옴표가 닫히지 않았으면 저장할 수 없고,
  절대 경로의 실행 파일이 없거나 `{left}`·`{right}`가 없으면 경고만 한다. 저장할 때마다 `app_settings_service`가 파일을 쓴다.
- **SSH**: ssh-agent 서비스 상태(실행 중·중지됨·사용 안 함·설치 안 됨과 그때 할 일), `ssh-add -l` 목록, `%USERPROFILE%\.ssh`의 키 쌍과 [공개 키 복사](클립보드),
  [키 만들기], [연결 시험]. 열 때와 [새로고침], 키 만들기가 끝났을 때 worker가 다시 읽는다. git의 ssh와 `ssh-keygen`은 Git for Windows의 것을 먼저 쓰고,
  agent 조회만 시스템 OpenSSH의 `ssh-add`를 먼저 쓴다(Windows ssh-agent 서비스의 named pipe는 그쪽만 닿는다). 이 둘을 `locate_ssh_programs`가 고른다.
  - **키 만들기·연결 시험은 저장소 없는 setup operation**(`ssh_keygen`, `ssh_connection_test`)이다. passphrase·호스트 키 질문이 askpass로 와서 operation의
    `needs_input`이 되어야 인증 대화 상자가 받을 수 있기 때문이다. passphrase는 명령줄(`-N`)에도 작업 로그에도 남지 않는다.
  - 질문을 답 없이 닫으면 `ssh-keygen`은 빈 passphrase로 키를 **만들어 버린다**(종료 코드 0). 그래서 닫은 것을 성공보다 먼저 가려 `auth_cancelled`로 끝내고 만들어진 파일을 지운다.
  - 연결 시험은 `ssh -T [계정@]호스트[:포트]`의 결과를 `classify_ssh_connection_test`로 분류해 `ssh_connection_tested` 사건으로 올린다. 시험이 돌았으면 operation은 성공이다
    (인증이 되었는지는 분류가 말한다). 입력은 `parse_ssh_target`이 옵션 주입(`-o…`, 공백, 따옴표)을 거른다.
- **읽기 전용**: 설정 파일을 읽지 못했으면(`app_settings_service::is_read_only`) 화면 위에 "이번 실행 동안은 저장하지 않습니다" 배너가 선다. 값은 메모리에서 바뀐다.
  마지막 저장이 실패했을 때(`has_unsaved_changes`)도 배너가 있다.
- 입력 칸의 초안은 `settings_state`가 가지고(대화 상자를 닫으면 버린다), 저장은 [적용]·[저장]처럼 한 번에 한다. 텍스트 대상은 `text_target::settings_*`,
  입력 칸의 kind는 `kind_settings_*_input`(40번부터)이다.

## 화면 세부

- 환경 설정·테마 단추가 캡션이 아니라 탭 줄 오른쪽에 있다 (아래 "알아 둘 것"의 캡션 띠). 환경 설정은 일반·SSH·diff 도구에 더해 `ui.pane_widths`를 따로 설정 파일에만 남긴다
  (화면에는 아직 없다).
- 탭은 아이콘 자리 하나에 표시(🔔 ⟳ ⌚ ●)를 그린다(`tab_item`에 칸이 하나뿐이다).
- 커밋 본문 칸이 한 줄이다. luil `text_input_element`에 여러 줄 칸이 없다.
- 시작 페이지의 오른쪽 안내 카드(Git·자격 증명·알아 두면 좋은 것)가 없고, git 버전은 제목 아래 한 줄이다.
- 작업 영역 행의 단추가 hover 때만이 아니라 늘 보이고, 체크박스 일괄 선택·트리 보기가 없다. discard는 확인 대화 상자를 거친다.
- 원격에 없는 태그 표시는 그 실행에서 fetch(또는 태그 push)가 한 번 돈 뒤에야 맞다. 그 전에는 원격이 있는 저장소의 태그가 모두 점선이다
  (`ref.remotes`가 `ls-remote` 전에는 비어 있다).
- 사이드바의 원격 묶음 머리는 접히지 않는다. 툴바 선택 상자는 표시만 한다.

## 알아 둘 것

- 그래프 목록의 항목 키는 SHA 앞 15자다. 40자 키는 MSVC 짧은 문자열 최적화를 넘어 10만 행이면 frame마다 10만 번
  할당한다. 모델(`virtual_list_item` 벡터)은 세션의 generation·행 수로 cache하고(`shell_controller::graph_items`)
  frame마다 config로 복사만 한다.
- 목록 행 안의 내용 element가 액션을 가지면 hit test가 그 element를 답해 목록 행의 고르기를 가로챈다. 그래서 내용은
  `hit_opaque(false)`이고, 두 번 누름이 필요한 사이드바 브랜치 행만 한 번 누름에도 같은 고르기 메시지를 다시 낸다.
- 캡션 띠는 luil의 창 버튼 밖이 전부 끌기 영역(HTCAPTION)이라 앱 단추를 둘 수 없다. 환경 설정·테마 단추는 탭 줄 오른쪽에 둔다.
- **웹 콘텐츠는 UI Automation에 보이지 않는다.** diff 자리의 `Group 'diff 뷰어'`는 자식이 없고 페이지 글(`Stage hunk`, 줄 내용)은 UIA 트리에 나오지 않는다.
  웹뷰는 composition 호스팅이라 자식 HWND가 없고, luil의 `webview_host.cpp`는 `ICoreWebView2CompositionController::get_UIAProvider`를 부르지 않으며
  `uia_root_provider::GetEmbeddedFragmentRoots`는 빈 배열을 돌려준다. 빠진 연결은 자리표의 UIA provider가 컨트롤러의 `UIAProvider`를 자식 fragment root로 내어
  `Navigate(FirstChild)`와 `GetEmbeddedFragmentRoots`로 잇는 것이다. 그래서 UIA(스크린 리더 포함)로 웹뷰 안을 읽거나 누를 수 없고, diff의 핵심 정보(파일명·hunk 수)는 luil 머리 막대가 말한다.
- **luil의 빈 곳**:
  - 웹 콘텐츠의 UIA 연결이 없다(위).
  - 웹뷰 환경 생성 실패가 앱에 알려지지 않는다. `webview_host::fail()`이 이유를 버리고 `webview_event`도 나가지 않아, 앱이 `GetAvailableCoreWebView2BrowserVersionString`으로 직접 확인한다.
  - `split_handle_config`에 "놓았다" 콜백이 없다(`pointer_drag_target::on_release`는 있다). 그래서 판 폭 저장을 500 ms 조용해질 때까지 미룬다.

## 대화 상자·메뉴·끌어 놓기

### 모달 입력 상자 하나로 모은다

브랜치·태그·push·확인·체크아웃 정책·인증 질문은 모두 `form_view`(`presentation/form_view.h`) 하나로 그려진다. 종류마다 갈리는 것은 어떤
칸·선택지·체크·단추가 서는가뿐이다. `build_view_snapshot`이 `resolve_form_source`로 지금 떠 있는 상자를 정하고(우선순위: 인증 질문 >
사용자가 연 대화 상자 > 체크아웃 정책), `form_builders`가 그 상태에서 `form_view`를 짓는다. 화면 쪽(`ui/form_ui.cpp`)은 `form_view`만 읽는다.

| 상자 | 어디서 | 상태 |
| --- | --- | --- |
| 체크아웃 정책 | `needs_input{dirty_worktree}`로 끝난 체크아웃 | operation(+`tab_ui_state::dirty`) |
| 인증 | 끝나지 않은 `needs_input{passphrase\|host_key\|credentials}` (저장소 operation과 clone·init·ssh-keygen 같은 저장소 없는 operation 모두) | operation |
| 새 브랜치·태그·이름 바꾸기·push·확인 | 툴바, 우클릭 메뉴, 끌어 놓기 메뉴, 배너 조치 | `ui_state::dialog` |

- 입력 칸은 상자마다 최대 셋이고 `ui_state::form`이 든다. 상자를 가르는 열쇠(`form_inputs::identity`: `dialog:N`, `auth:<operation>:<prompt_id>`,
  `dirty:<operation>`)가 바뀌면 글을 지우고 비운다. ssh의 다음 passphrase 질문은 새 `prompt_id`라 앞 시도의 글이 남지 않는다.
- `text_target::form_field_0..2`와 element kind 100~102를 쓴다 (다른 칸의 번호와 겹치지 않게 100부터 쓴다).
- 인증의 답·닫기는 `answer_input_intent`·`cancel_input_intent`로 나간다. controller는 `dependencies::send_backend_intent`(driver가 자기 백엔드 처리기를
  꽂는다)로 보내고, 비어 있으면(test) 같은 일을 workspace에 바로 한다. 답을 쓴 뒤 메시지가 쥔 사본과 입력 칸의 글을 지운다.
- 입력 칸이 비밀이면(`secret`) 점으로 가리고 "입력 보이기" 체크가 있다. 호스트 키 상자의 기본 단추는 **거절**이라 Enter가 모르는 키를 믿지 않는다.

### 호스트 키 "이번만 연결"과 자동 후속 작업

"이번만 연결"은 질문 중인 operation을 취소(알림 없이 `cancelled`)하고, `request_of`로 꺼낸 같은 요청에 `ssh_command_override`(`make_temporary_known_hosts_ssh_command`,
임시 known_hosts 경로는 `%TEMP%\cyella-known-hosts-N-<op>.tmp`)를 넣어 다시 낸다. ssh 실행 파일은 `shell_controller::dependencies::ssh_executable`
(기본 `ssh`, git이 PATH에서 찾는다)이다.

쓰기 뒤 workspace가 자동으로 내는 `refresh_remote_tags`에는 임시 known_hosts가 이미 지워져 같은 호스트 질문이 다시 올 수 있다. 두 가지로 푼다.

1. **자동 operation(`automatic=true`)이 묻는 인증 질문은 대화 상자를 띄우지 않고 조용히 취소한다.** 사용자가 시작하지 않은 작업이 영문 질문을 띄우면
   놀랍고, 취소하면 원격 태그 표시가 이번 갱신만 건너뛴다(다음 fetch가 고친다).
2. **"이번만 연결"로 다시 낸 작업이 성공하면 같은 신뢰를 한 번 더 써서(새 임시 파일) `refresh_remote_tags`를 직접 낸다** (fetch·push_tag·push_tags·
   delete_remote_tag·`push_execute(follow_tags)`일 때). 사용자가 이미 이 연결을 믿기로 했으므로 태그 표시가 오래된 채 남지 않는다.

### 우클릭 메뉴

`ui_state::menu`(`menu_target` + 창 좌표)가 열린 메뉴다. 항목은 snapshot마다 `build_context_menu(target, context)`(`presentation/menu_model.*`, 순수 함수)로
정하고, 고른 키로 controller가 같은 규칙을 다시 불러 실행한다. 메뉴는 `ui_frame::popups`에 싣는다: `build_ui_tree`가 `built_tree::popups`에
`luil::menu_element` tree를 주고 driver가 `ui_popup`으로 옮긴다(닫힘은 `close_menu_intent`). `cyella_policy::menu()`가 ↑/↓/Enter/Esc 탐색을 켠다.
Shift+F10은 고른 사이드바 항목이나 커밋의 메뉴를 연다. SHA·이름·경로 복사는 controller가 글을 들고 있다가 driver가 UI thread 클립보드로 넘긴다.

대상: 커밋(체크아웃(detached 경고)·브랜치/태그 만들기·cherry-pick·revert·reset soft/mixed/hard(hard는 확인)·SHA 복사), 로컬 브랜치(체크아웃·merge/rebase·
push·upstream 설정·이름 바꾸기·삭제(확인, 강제는 체크)), 원격 브랜치, 태그(원격마다 push, 로컬 삭제, 원격에도 삭제), stash(apply·pop·삭제 확인),
TAGS 머리(모든 태그 push), 작업 영역 파일(stage·unstage·버리기 확인·ignore·외부 diff·경로 복사).

### 끌어 놓기

- 그래프 행 안의 라벨은 **`ref_chip_element` 자식**이다(`graph_row_element`가 소유). 칩마다 `set_drag_source`(로컬·원격 브랜치만), `set_drop_target`, 한 번 누름(커밋 고르기),
  두 번 누름(체크아웃), 우클릭(메뉴)을 가진다. 칩의 자리는 배치(`arrange`)에서 정하므로 글꼴 없이 폭을 짐작한다(`estimate_text_width`). 칸이 모자라면 뒤쪽 칩은
  숨고 "+N" 칩이 이름을 툴팁으로 보인다.
- 사이드바 브랜치 행(`sidebar_row_element`)도 같은 payload 키(`l:<이름>`, `rb:<이름>`)로 끌고 받는다. 그래프 행 자체는 커밋 대상이다.
- **`virtual_list` 행 안의 자식에 붙인 `set_drag_source`가 그대로 동작한다.** 눌린 요소는 가장 깊은 상호작용 요소(칩)이고, 6px를 넘기면 끌기가 서며 행의 누름·고르기와 겹치지 않는다.
- 끄는 동안의 문구(`feature/graph를 main 위로 rebase · 커밋 2개`)는 창 전체를 덮는 투명 overlay element(`ui/overlay_ui.cpp`)가 `interaction.drag`로 그린다.
  `drop_hint_data`(읽어 둔 이력의 `graph_index` + 브랜치 표)만 읽으므로 세션 포인터를 tree가 붙잡지 않는다.
- 놓으면 `drop_intent` → `menu_target{drop}` 메뉴가 뜬다. 조상·개수는 먼저 `graph_index`(읽어 둔 이력)로, 모르면 세션이 받아 둔 답으로 채우고, 둘 다 없는 관계만
  `is_ancestor`·`count_commits`로 한 번 묻는다(`menu_state::asked_facts`). 답이 오면 열려 있는 메뉴 항목이 켜진다.

### 충돌 해결 화면

`repository_summary.in_progress`가 있으면(`repository_view::conflict`) 그래프 자리를 닫을 수 없는 충돌 화면이 덮는다(diff를 열면 diff가 먼저). 진행 줄("커밋 2/3 적용 중: …"),
충돌 파일별 현재(ours)·들어오는(theirs)·해결 표시, 아래 막대의 계속·건너뛰기(merge는 없음)·중단(확인)이다. 3-way 편집기는 없다. 외부 merge 도구는 설정에 없어
단추도 없다. 충돌 대기는 취소할 수 없고 abort로만 나간다.

### 알아 둘 것

- 확인이 필요한 조치는 `open_confirm_intent{title, lines, confirm_label, danger, operations}`를 내면 같은 확인 상자로 뜬다.
- 배너 조치(`banner_action_intent`): push 거절(`rejected_non_ff`·`rejected_fetch_first`) → "Pull 후 다시 push"(pull(rebase) 뒤 같은 push, 앞이 성공해야 이어 낸다),
  `stale_info` → fetch, ff-only pull 실패 → "rebase로 Pull", 안 합쳐진 브랜치 삭제 실패 → 강제 삭제 확인, `index_lock` → 잠금 지우기.
- 앞 operation이 성공해야 이어 내는 열(`submit_sequence`)은 원격 태그를 먼저 지우고 로컬을 지울 때(`delete_remote_tag` → `delete_tag`)도 쓴다.
- upstream 설정은 `push --set-upstream`으로 한다 (`branch --set-upstream-to` operation은 없다). 메뉴의 "upstream 설정…"은 그 체크를 켠 push 대화 상자다.
- 사이드바 태그·stash 두 번 누름: 태그는 detached 체크아웃(경고 한 번, "다시 보지 않기"), stash는 apply다.
