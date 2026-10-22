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
| 탭마다 (`tab_ui_state`, 탭 id가 key) | 고른 커밋 SHA(WIP는 `"WIP"`), 사이드바 고른 항목, 그래프·사이드바·상세·커밋 메타·diff 스크롤, 접힌 구역, 변경 파일 트리에서 접은 폴더, 사이드바 거르기 초안, 커밋 제목·본문 초안, amend, 연 diff 요청, 낸 커밋 operation, 닫은 배너 |
| 셸 전체 | 사이드바·상세 폭, 작업 로그 열림, 시작 페이지 검색 초안·스크롤, 서랍 열림, 아이콘 막대에서 연 구역, Clone 대화 상자 초안, 안내 한 줄, IME 조합 |

- 탭 목록의 정본은 `repository_tabs`, git 상태의 정본은 workspace·세션, 설정의 정본은 `app_settings_service`다.
- 탭 줄은 `repository_tabs_element`가 luil 탭 막대와 끝의 추가 단추를 한 가로 띠에 담는다. 넘칠 때만 가로 스크롤 막대를 보이고,
  선택·추가·창 너비 변경 때 선택 탭을 화면 안으로 들인다. 수동 스크롤은 그대로 유지하며, 배치에서 다듬은 위치를 `frame_feedback`으로 되돌린다.
  탭 경계와 선택 테두리를 표시하고, 끌어 놓으면 이동할 대상 앞·뒤 가장자리를 강조한다.
- 판 폭은 `app_settings::ui.pane_widths`에 남긴다. 손잡이에 "놓았다"는 사건이 없어서 끄는 동안(`resize_pane_intent`)은 쓸 시각만 500 ms 뒤로 미루고,
  `shell_controller::tick`(driver의 `next_tick`)이 한 번 쓴다. 앱을 닫을 때는 `flush`가 바로 쓴다. 시작할 때 저장된 폭을 범위로 다듬어 되살린다.
- 텍스트 칸은 luil 규약 그대로다. `cyella_policy::text_target_of`가 kind → `text_target`을 답하고, 편집은
  `edit_text_intent`로 와서 `ui_state`의 `text_edit_state`에 `luil::apply_text_edit`로 적용된다. 한 줄 칸이라 붙여넣은 줄바꿈은 공백이 된다.

## 반응형 규칙 (`presentation/shell_layout.*`)

경계는 논리 픽셀(물리 폭 ÷ 배율)로 판정한다.

| 폭 | 배치 |
| --- | --- |
| 1200 이상 (`wide`) | 사이드바 232 · 그래프 · 상세 360의 3단. 판 사이 손잡이로 폭을 바꾼다(사이드바 160~420, 상세 280~560). 그래프는 최소 240을 지키며 상세 → 사이드바 순으로 줄인다. |
| 760~1199 (`medium`) | 상세가 접힌다. 툴바 오른쪽 "상세 패널" 단추나 커밋 고르기가 오른쪽 서랍을 연다. 서랍은 그래프 위에 겹치고 Esc·× 로 닫는다. 서랍의 파일을 고르면 서랍이 닫히고 그래프 자리에 diff가 열린다(파일 목록은 "상세 패널" 단추로 다시 연다). |
| 760 미만 (`narrow`) | 사이드바가 44px 아이콘 막대가 된다. 아이콘을 누르면 그 구역이 떠 있는 판으로 열린다(같은 행 element라 두 번 누름 체크아웃도 된다). 그래프는 작성자·날짜 열을 감추고 라벨 열을 110으로 줄인다. |
| 그래프 목록 폭 | 브랜치/태그 열은 가장 넓은 라벨 줄만큼(112~160)만 선다. 목록이 좁으면 메시지 열의 하한(220 또는 폭의 34%)을 먼저 지키고 그래프 → 브랜치/태그 → 작성자 → 날짜 순으로 줄인다 (`make_graph_columns`). |
| 960 미만 | 툴바 단추의 글자가 빠진다. |
| 760 미만 | 툴바의 저장소 선택 상자가 숨는다. 브랜치 선택 상자는 남는 폭까지 줄어 최소 폭 480에서도 Fetch·Pull·Push·Branch·Stash·Pop과 상세 단추가 모두 보인다. 진행 문구는 여유가 있을 때만 툴바에 서며 상태 줄에서는 계속 보인다. |

## 커밋 상세 판 (`build_commit_detail`, `presentation/ui/repository_ui.cpp`)

순서는 머리 → 메시지 카드 → 라벨 칩 → 라벨·값 표 → [이후 커밋 N개 편집…] → 변경 파일이다.

- **머리**: "커밋" · 짧은 SHA(고정폭) · [SHA 복사](`detail-copy-sha`, `copy_commit_sha_intent`) · 서랍이면 ×.
- **메시지 카드**(`commit-message-card`, `card_element`): 커밋 메시지는 제목과 본문으로 나뉜 한 글이라 한 테두리 안에 둔다.
  - 머리 칸은 제목(굵게, 옅은 바탕), 몸 칸은 본문(흐린 글, 없으면 빠진다), 바닥 칸은 작성 정보(옅은 바탕)다.
  - 제목과 본문은 카드 폭에서 줄을 바꾼다(`wrapped_text_element`). 공백에서만 나누고(어절 보존) 한 줄보다 긴 단어만 글자 단위로 자른다
    (`presentation/text_wrap.*`). 측정 단계가 없어 배치 높이는 글자 종류로 넉넉히 짐작하고, 그릴 때 실제 글꼴로 다시 나눈다.
  - 바닥 칸 왼쪽은 작성자(`detail-author`, `author_element`): 사진 동그라미와 굵은 이름이다. 메일은 올리면 툴팁으로 보이고 보조 기술에는
    "이름 <메일>"로 읽힌다. 오른쪽 끝은 시각(`detail-date` 이 컴퓨터 시간대의 절대 시각, `detail-date-relative` 상대 시각)이다.
    폭이 모자라면 상대 시각부터 빠진다(절대 시각의 툴팁에 남는다).
- **작성자 사진**: 환경 설정 "화면"의 "커밋 작성자 사진을 Gravatar에서 가져오기"(`appearance.gravatar`)가 켜졌을 때만 받는다. **기본은 꺼짐**이다
  (켜면 작성자 메일의 SHA-256 해시가 gravatar.com으로 간다). 꺼져 있거나 사진이 없으면(404) 이름 첫 글자 동그라미다.
  - controller가 상세를 지을 때 아직 묻지 않은 메일이면 `request_avatar`를 부르고, driver의 worker가 `avatar_fetcher`로 받아
    `%LOCALAPPDATA%\cyella\avatars`에 둔다(사진 7일, 없음 1일). 결과는 `avatar_loaded_intent`로 돌아오고, 받지 못했으면 5분 뒤 다시 묻는다.
- **라벨·값 표**: 흐린 64px 라벨 칸(SHA·부모·서명·서명자·키·CI) 옆에 값을 둔다. 부모는 누르면 그 커밋을 고르는 링크(`detail-parent-N`)다.
- **[이후 커밋 N개 편집…]**(`detail-interactive-rebase`): 판 폭 전체 단추다. 커밋 우클릭 메뉴의 "여기서부터 인터랙티브 rebase…"와 같은 일이고
  툴팁이 인터랙티브 rebase임을 알린다. N은 `rev-list --count 커밋..HEAD`(`commit_detail_view::commits_above`)다. 다른 갈래의 커밋(main 끝 등)이면
  현재 브랜치를 그 위로 옮겨 쌓는 편집이라 "현재 브랜치 커밋 N개를 이 위로 편집…"이다(`on_current_branch`). N이 0이면(HEAD 커밋 등)
  "이후 커밋 편집…"으로 꺼지고 툴팁이 이유를 말한다.
- 메시지·메타 칸이 판 높이의 55%(최소 160)를 넘으면 그 칸만 흘린다(`scroll_target::commit_meta`, 커밋을 바꾸면 처음으로). 파일 목록 자리는 지킨다.
- **변경 파일 머리**: "변경 파일 N개 +a −r"과 [경로 목록](`detail-files-list`)·[폴더 트리](`detail-files-tree`) 전환 단추.
  고른 보기는 앱 설정 `ui.commit_files_view`(list·tree)에 남는다.
- **폴더 트리**: `build_file_tree`가 경로를 폴더 먼저·이름 순으로 펴고, 파일 없이 하위 폴더 하나뿐인 사슬은 `src/presentation`처럼 한 행으로 합친다.
  폴더 행(`folder-d:<경로>`)은 누르거나 UIA ExpandCollapse로 접고 펴며(`toggle_file_folder_intent`), 키보드 훑기는 파일 행만 지난다.
  접은 폴더는 탭마다 기억하고 커밋을 바꿔도 같은 경로면 접힌 채다.

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

- **구획**: 관련 항목은 제목·여백·테두리를 공유하는 카드로 묶는다. 창이 좁으면 왼쪽 탐색을 아이콘으로 줄인다.
  제목과 필드 이름 아래에는 간격을 두고 한 줄 입력 칸은 24 논리 픽셀이다. 스크롤 영역 바깥의 8 논리 픽셀 여백이 막대를 대화 상자 테두리에서 떼어 둔다.
- **일반**: 밝게·시스템·어둡게 미리보기 카드(시스템은 밝고 어두운 화면을 대각선으로 나눔), 키 컬러, UI·코드 글꼴, 작성자 사진, git 실행 파일을 각각 구획으로 보인다.
  키 컬러는 luil 내장 목록(`assets/accents.json`)의 견본 격자(`settings-accent`, UIA radio group)이고, Windows 강조색을 읽었으면 `system` 견본이 맨 앞에 선다.
  고르면 `appearance.accent`에 바로 저장되고 driver가 `frame->appearance.accent_id`로 넘긴다. 비어 있으면 luil의 기본 키 컬러(민트)다.
  목록에 없는 id는 설정에 남기고 표시만 기본 키 컬러로 되돌린다. 테마 미리보기 카드와 diff 웹뷰도 같은 키 컬러로 칠한다.
  글꼴은 입력 가능한 콤보 상자(`settings-ui-font`, `settings-code-font`)와 검색 목록이며 `appearance.ui_font`·`appearance.code_font`에 저장한다.
  목록 선택은 바로, 직접 입력은 Enter나 [글꼴 적용]으로 반영한다. 빈 이름·설치되지 않은 이름은 기본 글꼴로 표시하며 설정값은 보존한다. diff 웹뷰도 같은 글꼴을 쓴다.
  테마와 보기 전환은 여러 독립 단추 대신 전용 선택 컨트롤(`segmented_control_element`)을 쓰고 UIA Selection으로 현재 값을 공개한다.
  git 실행 파일은 이번 실행이 찾은 경로·버전·자동 탐색/직접 지정, 경로 초안과 [찾아보기…]·[적용]·[자동 탐색으로 되돌리기]를 보인다.
  바꿀 경로와 단추를 먼저 배치하고, 현재 사용 중인 경로·버전은 아래의 읽기 전용 상태 영역으로 구분한다.
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

## 툴바 저장소·브랜치 선택 상자

선택 상자(`selector_element`)를 누르면 `open_selector_intent`가 상자의 왼쪽 아래 좌표와 함께 가고, controller가 `ui_state::selector`(종류·자리·거르는 글)를 세운다. snapshot의 `selector_view`가
목록을 가지며(`presentation/selector_model`의 순수 함수 `build_repository_selector`·`build_branch_selector`), `ui::build_selector_popup`이 popup 하나로 그린다: 위에 거르는 칸(`kind_selector_filter_input`,
`text_target::selector_filter`), 아래에 `luil::menu_element`다. 항목 키는 `tab:<id>`·`recent:<경로>`·`branch:<이름>`·`open_folder`이다.

- 저장소 목록: 열린 탭(현재 탭은 체크·꺼짐) → 구분선 → 아직 열지 않은 최근 저장소(없는 경로는 "사용할 수 없음", 꺼짐) → "폴더에서 저장소 열기…"(UI thread 폴더 대화 상자 명령).
- 브랜치 목록: 로컬 브랜치. 현재 브랜치는 체크 표시와 "(현재)"가 붙고 고를 수 없다. 고르면 사이드바 더블클릭과 같은 `checkout_branch`(정책 `ask`)다 — 작업 트리가 막으면 기존 정책 대화 상자가 뜬다.
- 보이는 항목은 12개까지이고 넘으면 "외 N개 · 좁히세요" 줄이 선다. 거르기는 ASCII 대소문자를 무시하는 부분 문자열(저장소는 이름·경로, `/`와 `\`는 같다)이다.
- popup은 검색 칸이 있어 창 활성이 바뀌어도 남는다(luil의 검색 popup 규약). 바깥 누름·Esc는 닫는다. 바깥 누름이 닫은 직후(400 ms) 같은 상자를 누르면 다시 열지 않는다(눌러서 닫기).
- UIA: 상자는 `ComboBox`(ExpandCollapse)이고 목록은 `Menu`/`MenuItem`, 거르는 칸은 `Edit`이다. **알려진 한계**: popup이 열릴 때 거르는 칸에 초점이 가지 않는다(luil에 popup의 첫 초점 지정이 없다). 눌러서 글을 넣는다.
- 없어진 최근 저장소(시작 페이지): 행을 누르거나 위치 찾기 단추를 누르면 폴더 대화 상자가 열리고(`ui_command{locate_folder, 옛 경로}`), 고른 폴더를 `inspect_path`로 확인해 저장소이면
  최근 항목의 경로를 바꾼다(`relocate_recent_repository`: 고정·시각·자리는 그대로, 새 경로를 이미 가진 다른 항목은 지운다).

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
TAGS 머리(모든 태그 push), 작업 영역 파일(stage·unstage·버리기 확인·ignore·외부 diff·경로 복사), 커밋 상세의 변경 파일(`commit_file`: 변경 내용 보기·경로·파일 이름·`커밋:경로` 복사).

목록 행 안의 내용 element가 툴팁·우클릭을 가지면 포인터의 hit를 그 element가 받는다(`ui_element::interactive`). 그래서 파일 행·그래프 행·사이드바 행은 한 번 누름의
고르기 메시지를 스스로 다시 낸다. 내지 않으면 마우스 누름이 목록까지 가지 못해 키보드로만 고를 수 있다.

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
- upstream 설정은 `set_upstream` operation이다. 메뉴의 "upstream 설정…"은 원격과 원격 브랜치를 고르는 전용 상자이고 "upstream 해제"가 옆에 있다.
- 사이드바 태그·stash 두 번 누름: 태그는 detached 체크아웃(경고 한 번, "다시 보지 않기"), stash는 apply다.

## submodule·worktree·Git LFS

```text
refresh ─ status·refs ─ read_extension_snapshot ─▶ repository_snapshot{submodules, worktrees, lfs}
                                                    │
build_sidebar_rows ◀── append_extension_sidebar_rows ┘  (presentation/sidebar_extension.*, 순수 함수)
build_diff ◀──────────── build_diff_special(diff_file)   (presentation/diff_special.*, 순수 함수)
build_context_menu ◀──── build_extension_menu            (presentation/menu_extension.*)
snapshot_dialogs ◀────── build_add_worktree_form·build_lfs_track_form (presentation/form_extension.*)
shell_controller_extension.cpp: 행 활성화·메뉴 실행·대화 상자 열기
```

- **사이드바**: `sidebar_section`의 끝에 `submodules`·`worktrees`·`lfs`가 있다. 행은 `sidebar_row_view::item`(`ref`·`submodule`·`worktree`·`lfs`)으로
  가르고 키는 `sm:<경로>`·`wt:<폴더>`·`lfs:status`·`lfs:f:<경로>`다. 오른쪽 꼬리 글(`badge`)과 색(`tone`)은 상태를 말한다: 미초기화·수정됨·구버전·충돌, 주·현재·잠김·정리 가능, 받음·포인터.
  LFS 구역은 저장소가 LFS를 쓸 때만 선다(파일 행은 200개까지, 나머지는 "외 N개"). 브랜치 행의 끌어 놓기는 `item == ref`일 때만 붙는다.
- **두 번 누름·Enter(UIA의 Invoke도 같다)**: submodule은 초기화되었으면 새 탭으로 열고 아니면 `submodule_update`(init, recursive)다. worktree는 새 탭으로 연다(지금 연 것은 그대로,
  폴더가 사라진 것은 안내만). 탭과 최근 목록의 열쇠는 경로이므로 연결된 worktree와 submodule은 저마다 별개의 저장소 탭이다(대소문자·구분자가 달라도 같은 탭).
- **다른 worktree의 브랜치**: `ref.worktree_path`가 있으면 사이드바 행과 그래프 라벨(`ref_label::worktree_path`)에 worktree 아이콘이 붙고 이름에 "(다른 worktree에서 사용 중)"이 들어간다. 두 번 누름은
  operation을 내지 않고 이유를 알리며, 메뉴의 "체크아웃"은 꺼지고 "그 worktree 열기"가 선다. 그래도 git이 거절하면(다른 경로로 낸 요청) 백엔드의 `branch_checked_out_elsewhere`가 잡는다.
- **우클릭 메뉴**(`menu_target_kind` 끝에 `submodule`·`submodules_section`·`worktree`·`worktrees_section`·`lfs_section`·`lfs_file`): submodule은 열기·업데이트·경로 복사, 머리는 모두 업데이트·sync.
  worktree는 열기·제거(확인 상자, "강제로 제거" 체크가 `plan.alternate`)·경로 복사, 머리는 추가·prune. LFS 머리는 pull·fetch·fetch --all·추적 패턴 추가. 커밋·브랜치 메뉴에는 worktree 만들기,
  작업 영역 파일 메뉴에는 `*.확장자를 LFS로 추적…`·`LFS 객체 가져오기`가 붙는다. 메뉴는 `menu_context`(snapshot의 submodule·worktree·lfs)로 상태를 다시 찾는다.
- **대화 상자**: `dialog_kind::add_worktree`(폴더 + 방식 셋 [새 브랜치·기존 브랜치·detached] + 브랜치 이름, 시작 위치는 `dialog.subject`)와 `lfs_track`(패턴). 폴더는 저장소 옆
  `<저장소>-<이름>`이 미리 채워지고, 절대 경로·이미 있는 worktree 폴더·브랜치 이름 규칙·"다른 worktree가 쓰는 브랜치"를 `add_worktree_ready`가 가린다.
- **작업 영역·커밋 파일 행**: `file_row_view::lfs`(주황 "LFS" 배지)와 `submodule`(파란 "서브모듈" 배지, 줄 수는 감춘다). UIA 이름에는 "(LFS)"·"(서브모듈)"이 들어간다.
  작업 영역 행은 Invoke로 diff를 열 수 있다(`file_row_element`의 access handler). 커밋의 파일 목록은 목록 항목을 고르면 열린다.
- **diff 카드**: `diff_file::is_special()`(gitlink, 또는 실제 내용이 로컬에 없는 LFS 포인터)이면 `diff_panel_view::special`이 서고 `build_diff_area`가 웹뷰 자리·텍스트 대체 본문 대신
  luil 카드(`ui/diff_special_card.cpp`)를 그린다. 카드라서 UIA에 보인다(`diff-special-title`·`diff-special-row-N-value`·`diff-special-action`·`diff-special-open`).
  `diff_view_controller`도 같은 조건에서 웹뷰에는 비움을 보낸다(`diff_content::special`). 두 쪽의 실제 내용이 로컬에 있는 LFS 포인터는 카드가 아니라 diff 웹뷰의 이미지 비교나 바이너리 카드이며,
  이때 두 쪽 꺼내기가 blob 대신 로컬 LFS 객체를 쓴다(`diff_side_plan::lfs`, `git_diff_gateway`).
- **진행률**: lfs pull·fetch는 상태 줄에 "Downloading LFS objects 33%"로 보인다.

### 알아 둘 것

- 우클릭 메뉴(submodule·worktree·LFS)와 그 메뉴가 여는 worktree 추가·제거 확인·LFS 추적 대화 상자는 UI Automation으로 열 수 없다.
  `tests/extension_view_tests.cpp`가 같은 메뉴 항목과 대화 상자를 controller 수준에서 검증한다(메뉴 조건, 항목 실행이 내는 operation, 대화 상자 규칙).
- LFS 카드의 "현재 내용"은 unstaged 보기에서만 작업 트리 파일로 판정하고 staged·커밋 보기는 객체 저장소로 본다. 커밋 보기에서 객체를 받아 둔 저장소는 이미지 비교가 뜬다.
- 커밋 파일 목록의 LFS 배지는 그 커밋이 아니라 지금 작업 트리의 `.gitattributes`로 판정한다(과거 커밋에서 LFS를 쓰지 않았던 파일도 지금 패턴에 맞으면 배지가 붙는다).
- submodule 상태에서 `out_of_date`와 `modified`의 구분은 `+`인 항목마다 submodule 안에서 `git merge-base --is-ancestor`를 한 번 부른다.

## 인터랙티브 rebase 편집기

결정은 [ADR-005](decisions/ADR-005-interactive-rebase.md)에 있다.

```text
여는 곳: 끌어 놓기 메뉴(interactive_rebase) · 커밋 메뉴(interactive_rebase_here) · 브랜치 메뉴(interactive_rebase_current_onto)
        · 커밋 상세 판의 [이후 커밋 N개 편집…] (open_interactive_rebase_here_intent)
   └▶ shell_controller::open_rebase_editor ─▶ rebase_editor_controller.open ─▶ load_rebase_plan
rebase_editor_controller (shell_controller가 소유, presentation/rebase/*)
   ├ after_events: rebase_plan_of(op)가 오면 start_rebase_editing (fixup!·squash!·amend! 자동 정렬)
   ├ handle: 고르기·동작·옮기기(Alt+↑↓·[위로]·[아래로]·끌기)·켬끔·메시지 되돌리기·시작
   ├ edit_text: 제목(한 줄)·본문(여러 줄) 초안
   └ make_view ─▶ build_rebase_editor_view (순수) ─▶ ui::build_rebase_editor (presentation/ui/rebase/*)
시작: interactive_rebase{todo=rebase_final_todo} → 성공·충돌이면 닫는다(충돌 화면이 이어 받는다), 실행 전 거절(invalid_request·refused)이면 열어 두고 이유를 보인다
```

- **상태**(`rebase_editor_state`)는 열려 있는 동안만 있다. 목록(`todo`)은 메시지를 비워 두고, 메시지 초안은 `drafts`가 든다. 열쇠는 reword 줄이면 `r:<sha>`,
  squash가 든 묶음이면 묶음 첫 줄의 `g:<sha>`다. 고치지 않은 묶음 초안은 묶음이 바뀔 때마다 git과 같은 규칙(`combined_squash_message`)으로 다시 만들고,
  고친 초안은 남긴다. 시작할 때 `rebase_final_todo`가 reword 줄과 묶음 **마지막 줄**에 메시지를 채운다.
- **목록**은 `virtual_list_element`(`rebase-todo`)이고 행 키는 `rebase-row:<sha>`다. 행 내용은 손잡이·번호·동작 알약·SHA·제목·"원격에 있음"이다.
  행 이름(`virtual_list_item::label`)은 번호로 시작한다(`1  squash  abc1234  제목`) — 화면 읽기가 이 글을 읽는다.
  - 커서가 곧 고른 줄이다(`move`도 `rebase_select_intent`). 오른쪽 판이 고른 줄의 동작 선택지·메시지·결과 미리보기를 보인다.
  - **p·r·s·f·d**: Windows는 수식 키 없는 글자를 키(`key_pressed_event`)가 아니라 문자로만 보낸다. 그래서 정책의 `on_key`가 아니라 목록의 글자 탐색 자리
    (`set_key_search_target`)가 받는다. 질의의 마지막 글자가 이번 키다. 글자로 줄을 찾는 기능은 이 목록에 없다.
  - **Alt+↑↓**는 `cyella_policy::on_key`가 편집기 목록이 tree에 있을 때만 `rebase_move_intent`로 바꾼다. 같은 일을 하는 [위로]·[아래로] 단추가 오른쪽 판에 있다
    (마우스 없이, 보조 기술로도 순서를 바꾼다).
  - 끌기는 목록의 `drag`·`accepts_drop`·`drop`이다. 같은 목록의 다른 줄에만 놓이고 `rebase_reorder_intent{moved, target}`가 나간다(아래로 옮기면 대상 뒤, 위로 옮기면 앞).
- **메시지 칸**: 제목은 luil `text_input_element`(한 줄, `text_target::rebase_message_subject`, kind 200), 본문은 앱이 지은 **여러 줄 칸**
  `multiline_text_element`(`text_target::rebase_message_body`, kind 201)다. luil은 여러 줄 붙여넣기·포인터 판정만 지원하고 여러 줄 칸 element는 없다.
  - `text_input()`이 `multiline`을 켜 붙여넣기가 줄바꿈을 지키고, `offset_at_point`가 줄(y)·열(x)로 caret을 정한다. 줄을 감싸지 않는다(긴 줄은 잘린다). caret 줄이 보이게 세로로만 흐른다.
  - Enter는 정책이 줄바꿈 입력(`edit_text_intent{insert "\n"}`)으로, ↑↓는 `rebase_body_line_intent`(같은 열의 윗줄·아랫줄)로 바꾼다. Home·End는 controller가 그 줄의 처음·끝으로 옮긴다.
    그래서 편집기의 [rebase 시작]은 기본 단추가 아니다 (기본 단추가 있으면 본문의 Enter를 먼저 가져간다).
  - 커밋 상자 본문도 같은 element와 줄 단위 caret 이동을 쓴다(`commit_body_line_intent`). Ctrl+Enter는 커밋 명령으로 남는다.
- **경고**: 원격 브랜치에 있는 커밋(`pushed`)이 있으면 강제 push 경고, 병합 커밋이 있으면 경고와 "한 줄로 펴도 됩니다" 확인 칸(켜야 시작)이다. 작업 트리에 추적 파일 변경이 있으면
  autostash 칸이 서고 켜야 시작한다. 시작할 수 없는 이유는 아래 막대의 요약("커밋 7개 → 4개") 밑에 보인다.
- **충돌 화면**: `in_progress.interactive`면 제목이 "인터랙티브 rebase 중"이고, 머리에 "할 일 k/n · 동작 SHA · 남은 reword·squash 메시지는 계속하거나 건너뛰면 그대로 적용됩니다"
  한 줄(`conflict-todo`)이 더 선다. 계속·건너뛰기·중단은 충돌 해결 화면과 같다.
- **진행 표시**: 실행 중에는 편집기가 "실행 중…"이고 상태 줄이 `Rebasing (k/n)` 진행률을 보인다. 편집기를 닫아도 rebase는 계속된다.
- UIA로 닿지 않는 몸짓(라벨 끌어 놓기, 우클릭 메뉴, 할 일 줄 끌기, p·r·s·f·d, Alt+↑↓)은 `tests/rebase_shell_tests.cpp`가 그 몸짓의 intent·element 콜백(`drop()`·`drag()`·
  `key_search()`)·정책(`on_key`)을 직접 불러 확인한다.

## 커밋 서명

결정은 [ADR-006](decisions/ADR-006-commit-signing.md)에 있다.

```text
refresh ─ read_signing_snapshot ─▶ repository_snapshot.signing  (commit.gpgsign 등 실효 값)  ─▶ 커밋 상자·태그 상자의 서명 기본값
commit 선택·그래프 스크롤 ─▶ shell_controller.request_(visible_)signatures ─▶ verify_signatures(자동 operation, 읽기 lane)
        └▶ signatures_verified ─▶ repository_session::signatures() ─▶ commit_detail_view::signature · graph_row_data::signature
settings_controller ─ signing_page_controller ─ request_info/write ─▶ cyella_driver_signing.cpp ─▶ task_scheduler worker ─▶ signing_info_loaded_intent·signing_write_done_intent
```

- **커밋 상자**: amend 토글 옆의 "서명"(`commit-sign`)이다. 기본값은 `snapshot.signing.commit_signing()`이고 사용자가 누른 값은 `tab_ui_state::sign`(값이 없으면 설정을 따른다)이다.
  `commit_request.sign`은 켜면 `true`(`-S`), 설정이 켜져 있는데 껐으면 `false`(`--no-gpg-sign`), 설정을 따르면 값 없음이다. ssh 형식인데 `user.signingkey`가 없으면 커밋 단추 아래
  한 줄이 환경 설정으로 안내한다(`signing_setup_hint`). 서명하는 커밋은 passphrase를 기다리므로 시간 제한 없이 돌고 취소만 받는다.
- **태그 대화 상자**: 종류 선택 아래에 "서명 (tag -s)" 체크가 선다(기본값 `tag.gpgsign`). 서명한 태그는 항상 annotated라 켜면 종류가 annotated로 옮겨가고(메시지 칸이 선다),
  lightweight를 고르면 꺼진다. 설정이 서명인데 끄면 `create_tag_request.sign=false`(`--no-sign`)로 설정을 덮는다.
- **상세 패널 배지**(`detail-signature`, `detail-signature-detail`, `-signer`, `-key`): "서명" 라벨 칸 옆의 값으로 "서명 확인됨"·"알 수 없는 키"·"서명이 올바르지 않음"·"서명 만료"·"키 만료"·"키 폐기됨"·"확인할 수 없음"·"서명 없음".
  읽는 동안은 "확인 중…"이다. ssh 서명이 `allowedSignersFile` 없이 읽히면 "확인할 수 없음 (allowed signers 없음)"과 이유 한 줄이다. 색은 확인됨 초록, 알 수 없음·만료·확인 불가 주황, 올바르지 않음·폐기 빨강이다.
- **그래프 표지**: 서명이 있는 커밋(`none`이 아닌 것)의 제목 앞에 13px 방패(확인됨은 채운 방패, 그 밖은 빈 방패)를 같은 색으로 단다. 서명을 읽기 전과 서명 없는 커밋에는 없다.
  요청은 보이는 행 앞 8행·뒤 24행(한 번에 40행까지)을 한 묶음으로 낸다(`shell_controller_signing.cpp`). 같은 커밋은 세션이 한 번만 읽고(`signature_requested`) 스크롤 때마다 새 묶음이 나가지 않는다.
  표지는 행의 UIA 이름에 넣지 않는다(행 이름은 제목 그대로다).
- **환경 설정 "서명" 쪽**(`settings_page::signing`, `presentation/settings/signing_page_*`, `ui/settings/signing_ui.cpp`): 위에서부터
  1. 바꿀 범위(이 저장소 / 전역)와 새로고침. 전역을 고르면 모든 변경이 확인 상자(`settings-signing-confirm-*`)를 거친다. 저장소 탭이 없으면 전역만 선다.
  2. "지금 적용되는 설정" 표: 일곱 설정의 적용되는 값·출처·이 저장소·전역 값(`settings-signing-effective-<키>`·`-origin-`·`-local-`·`-global-`).
  3. commit.gpgsign·tag.gpgsign(켜기·끄기·설정 지우기), gpg.format(openpgp·ssh·지우기). 선택지의 값은 화면 안에서 유일해야 해서(항목이 값으로 id를 얻는다) `commit:enabled`처럼 그룹 접두어가 붙는다.
  4. 서명 키: ssh면 SSH 화면이 읽은 `~/.ssh`의 공개 키(`<개인 키>.pub`, 슬래시 경로), openpgp면 `gpg --list-secret-keys --with-colons`의 비밀 키(지문). 만료·폐기·서명 불가 키는 [사용]이 꺼진다.
  5. gpg.program(Git for Windows의 gpg를 찾아 [이 gpg로 정하기]), gpg.ssh.program, gpg.ssh.allowedSignersFile 입력과 [적용]·[지우기]. 절대 경로가 아니면 막는다.
  읽기·쓰기는 SSH 화면처럼 worker가 한다(저장소 operation이 아니다: 저장소가 없어도 전역 설정을 보아야 하고 작업 로그에 남길 일이 아니다). gpg 키 목록은 gpg-agent를 띄우느라 수 초 걸릴 수 있어
  설정을 먼저 보이고 키 목록을 따라 보낸다(`gpg_pending`). 입력 칸의 kind는 `kind_settings_signing_*_input`(400번부터), 텍스트 대상은 `text_target::settings_signing_*`(400번부터)이다. 명령·프로필 칸은 300번대를 쓴다.
  쓰기가 끝나면 `shell_controller::on_signing_changed`가 읽어 둔 서명을 버리고 refresh를 낸다(전역이면 열린 모든 저장소).
- 쪽 조각(`page_column`·`make_row`·`help_line`·`tone_color`·`code_line`…)은 `ui/settings/settings_page_parts.h`(`pages` namespace)에 있고, `settings_ui.cpp`의 일반·diff·SSH 쪽도 `using namespace pages`로 같은 조각을 쓴다.

### 알아 둘 것

- 태그 대화 상자는 우클릭 메뉴로만 열려 UI Automation으로 열 수 없다. `tests/signing_ui_tests.cpp`가 controller 수준에서 검증한다.
- passphrase가 걸린 gpg 키는 Git for Windows의 `pinentry-w32`(GUI)가 창을 띄운다. gpg 서명·검증은 통합 test(`tests/signing_integration_tests.cpp`)가
  passphrase 없는 임시 키로, 실패 분류는 실제 stderr 고정 출력으로 덮는다.
- 서명 설정의 실효 값은 refresh가 읽는다. 터미널에서 `git config`로 바꾸면 `.git/config`는 감시가 알려 주지만 전역 파일은 감시하지 않으므로 다음 전체 새로 고침(F5, 쓰기 뒤의 자동 refresh)에 따라온다. 환경 설정 "서명" 쪽은 열 때와 [새로고침]에서 git에서 다시 읽는다.

## GitHub·GitLab 연동

결정은 [ADR-007](decisions/ADR-007-hosting-integration.md)에 있다.

```text
사이드바 행(두 번 누름·Enter·UIA Invoke) ─ activate_sidebar_item_intent{pull_request, path} ─▶ shell_controller::activate_extension_row
   ├ create ─▶ open_create_pull_request_dialog (dialog_kind::create_pull_request, form_hosting.*)
   └ 그 밖 ─▶ hosting_controller::activate_row ─▶ submit_hosting_job(hosting_job) ─▶ cyella_driver ─▶ hosting_bridge
hosting_bridge: scheduler lane "hosting:<host>" 워커 ─ hosting_service(전송 luil http_client + git_credential_source) ─ relay.post(hosting_result_intent)
cyella_driver::drain_relay ─▶ shell_controller::handle ─▶ hosting_controller::apply_result (serial로 낡은 결과 버림)
build_view_snapshot ◀ hosting_controller::view_of / ci_mark   (view_inputs::hosting_view·commit_ci, 순수 값 hosting_repository_view)
   append_hosting_sidebar_rows(hosting_sidebar.*) · repository_view::hosting · commit_detail_view::ci · detail_mode::pull_request
ui::build_pull_request_detail / build_commit_ci_line (presentation/ui/hosting_ui.*)
```

- **사이드바 PULL REQUESTS**(여덟째 구역): 로그인 전에는 "로그인…" 행 하나뿐이다(원격이 없거나 GitHub·GitLab이 아니면 안내 행). 로그인하면 PR·MR 행(`#번호 제목`, 꼬리 글 = 초안·병합됨·닫힘과
  CI 표시 `CI ✓`·`CI ✗`·`CI …`)과 "더 보기…", "닫힌·병합된 PR 포함"(켬/끔), "새로 고침"(계정 이름), "현재 브랜치로 PR 만들기…"가 선다. 행의 UIA 이름은 `#14 제목 (feature → main) 초안`이다.
- **명령 팔레트·단축키**: `app.settings.hosting`(호스팅 설정 열기), `hosting.sign_in`(로그인…), `hosting.refresh`(목록 새로 고침), `hosting.pull_request.new`(PR·MR 만들기…)가 명령 레지스트리에 있다.
  기본 단축키는 없다. 각 명령은 서비스를 아는 원격과 로그인 상태에 따라 팔레트에서 켜지고 꺼진다(`command_availability::hosting_*`).
- **PR 상세**: 사이드바의 PR 행을 열면 오른쪽 상세 판이 PR 상세(제목·브랜치·상태·작성자·갱신·CI 요약과 check 목록·본문 앞부분)로 바뀌고 [브라우저에서 열기]·
  [로컬에 체크아웃 (pr/14)]·[CI 새로고침]·닫기(×)가 있다. 커밋이나 WIP를 고르면 커밋 상세가 돌아온다(`handle_select_commit`이 PR 선택을 푼다). medium·narrow에서는 서랍으로도 열린다.
  브라우저는 `http`·`https` 주소만 연다(`win32_url_opener`, 워커에서 `ShellExecuteW`). `--open-url-log=<경로>`를 주면 브라우저를 띄우지 않고 열려던 주소를 그 파일에 한 줄씩 적는다.
- **커밋 상세의 CI 한 줄**(`detail-ci`): 로그인한 호스트이고 원격이 GitHub·GitLab일 때만 선다. 커밋을 고를 때와 로그인 직후 고른 커밋을 읽고, 진행 중 20초·확정 5분 안에는 다시 읽지 않는다.
- **PR 만들기 대화 상자**(`form_view` 하나, `create-pull-request-dialog`): 대상 브랜치(처음 값은 원격의 main·master·develop·trunk 중 현재가 아닌 것), 제목(처음 값은 HEAD 커밋 제목), 본문(한 줄),
  "초안으로 만들기", 그리고 브랜치가 원격에 없거나 앞서 있으면 "먼저 push하고 만들기"(기본 켬). 먼저 push하면 `push_execute{set_upstream}`을 내고 `after_events`가 그 operation의 성공을 지켜본 뒤에만 API를 부른다.
  만든 뒤에는 그 PR이 상세로 열리고(`pr-created`에 주소) 안내 줄에도 주소가 나온다. 로그인하지 않았거나 detached HEAD면 대화 상자를 열지 않고 이유를 안내한다.
- **환경 설정 "호스팅"**: github.com·gitlab.com(내장)과 설정의 표를 줄마다 서비스 종류·API 주소·이번 실행의 로그인 상태(계정)·호출 한도로 보이고, [로그아웃]은 메모리 토큰만 잊는다.
  표에 호스트·서비스 종류(GitHub/GitLab)·API 주소(선택)를 입력해 더하고 [삭제]로 지운다. 로그인은 이 화면이 아니라 사이드바에서 한다(저장소의 원격이 있어야 도우미에 물을 호스트·프로토콜을 안다).
- 브라우저 열기, 행 더블클릭 몸짓(UIA의 Invoke는 같은 `activate` 동작을 부른다), 한도가 풀린 뒤의 재시도는
  `tests/hosting_shell_tests.cpp`·`tests/hosting_controller_tests.cpp`가 controller 수준에서 확인한다.
- **알려 둔 한계**: PR 본문은 한 줄 입력이다. 같은 호스트에 계정 하나다. fork의 브랜치로 PR을 만들거나 리뷰·병합·닫기는 하지 않는다. GitLab의 초안은 제목의 `Draft: ` 접두어다.

## 명령 팔레트·단축키·사용자 프로필

```text
command_registry (presentation/commands/command_registry.*) ── id · 한글 제목 · 영문 키워드 · 기본 단축키 · 범위, 한 표
  ├ resolve_shortcut_bindings(settings) ──▶ view_snapshot::shortcuts ──▶ shortcut_table_element ("shortcut-table", tree의 보이지 않는 1x1 표식)
  │      cyella_policy::on_key ── chord_of(event) ── 표에서 찾기 ──▶ action_for_command(id) ──▶ run_command_intent{id}  (폴더 대화 상자 둘은 UI 명령)
  ├ palette_model (presentation/palette/palette_model.*) ── rank_palette_items ◀── domain/hangul_search (초성·음절·영문 퍼지)
  └ settings_controller (단축키 쪽) ── check_shortcut_edit · set_command_shortcuts ──▶ app_settings::shortcuts
identity: shell_controller ── request_identity ─▶ driver ─ scheduler("identity:<저장소>") ─ git_identity_gateway ─ relay ─▶ identity_loaded_intent
```

### 명령 레지스트리

한 표(`command_definitions()`)가 앱이 할 수 있는 일을 가진다: `id`(`git.fetch`처럼 점으로 가른 영문, 설정 파일의 키라 바꾸지 않는다), 한글 제목, 영문 키워드, 묶음(앱·탭·저장소·Git 작업·화면·프로필),
범위(`global`, `commit_input`), 기본 단축키. 팔레트와 단축키가 이 표 하나를 읽는다. 지금 할 수 없는 명령은 `command_availability`(툴바 단추가 켜지는 조건과 같다)로 팔레트에서 뺀다.

- 정책이 처리하는 키는 모두 표 항목이다: Ctrl+Enter(`git.commit`, 커밋 상자에 초점이 있을 때만), Ctrl+Tab·Ctrl+Shift+Tab(`tab.next`·`tab.previous`), Ctrl+W(`tab.close`),
  Shift+F10(`view.menu`), F5(`repo.refresh`), Esc(`view.close`), Ctrl+Shift+P·Ctrl+P(`palette.open`), Ctrl+,(`app.settings`), Ctrl+T(`tab.new`), Ctrl+O(`repo.open`).
- 사용자가 바꾼 값은 `app_settings::shortcuts`(명령 id → 정규 표기 목록)에만 남는다. 기본값과 같아지면 기록을 지우고, 빈 목록은 "키 없음"이다.
- 단축키로 쓸 수 있는 키(`check_key_chord`): 글자·숫자·기호는 Ctrl이나 Alt와 함께여야 한다(수정키 없는 글자는 문자 입력으로만 온다). 수정키 없이 쓸 수 있는 것은 F1~F12와 Esc뿐이다.
  텍스트 칸의 편집 키(Ctrl+A·C·V·X·Y·Z)와 Alt+F4는 예약이다. 같은 키를 다른 명령이 쓰면 거절한다(범위와 무관하게 — 전역 명령이 커밋 상자에서도 먼저 잡으므로).

### 키 처리

`cyella_policy`는 input thread에서 불리고 상태를 두지 않는다. 그래서 바뀔 수 있는 단축키 표는 frame마다 tree의 보이지 않는 표식(`shortcut_table_element`)에 실려 오고,
`on_key`는 `tree->find`로 표를 읽는다. 표가 없으면(표식을 싣지 않는 test) 레지스트리의 기본 표를 쓴다. 순서는 rebase 편집기 키 → 팔레트 거르는 칸의 ↑↓·Enter → 단축키 입력 칸 → 레지스트리 표 →
(남은) 칸별 Enter(Clone·git 경로·SSH 시험·입력 상자)다. 자동 반복(`repeat`)은 한 번만 반응한다.

### 명령 팔레트

탭 줄의 "명령 팔레트" 단추(`command-palette`, 툴팁에 지금 단축키)나 `palette.open`으로 연다. 대화 상자(`modal_host_element`)이고 위쪽 가운데에 선다. 거르는 칸(`palette-filter`)이 뜨는 순간 초점을 받고,
목록(`palette-list`)은 `virtual_list_element`다. 보이는 항목은 14개까지이고 넘으면 꺼진 "외 N개 · 입력으로 좁히세요" 줄이 선다(툴바 선택 상자와 같은 규칙).

- **모드**: 명령(`commands`) → "브랜치로 이동…"(`branches`, 로컬+원격 브랜치, 현재 브랜치는 꺼짐, 다른 worktree가 쓰는 브랜치도 꺼짐)·"저장소 열기…"(`repositories`, 열린 탭+최근 저장소+폴더 대화 상자)·
  "이 저장소에 프로필 적용…"(`profiles`)이 모드를 바꿔 이어서 고르게 한다. 고른 항목은 기존 길을 그대로 탄다(`activate_ref_intent`의 체크아웃, `select_from_selector`, `apply_profile`).
- **고른 항목의 메뉴**: 사이드바에서 고른 브랜치·태그·stash나 고른 커밋이 있으면 그 우클릭 메뉴(`build_context_menu`)의 켜진 항목이 "체크아웃 — main"처럼 명령 목록에 함께 선다(`menu:<키>`).
  Shift+F10이 쓰는 대상 계산을 `selection_menu_target()`으로 뽑아 같이 쓴다. 고르면 `run_menu_action`이 우클릭 메뉴와 똑같이 실행한다.
- **검색** (`domain/hangul_search`): 질의를 공백으로 가른 낱말이 모두 제목이나 영문 키워드에서 맞아야 한다. 낱말 하나는 (1) 연속 부분 문자열 — 음절은 같은 음절, **초성 자모(ㅂㄹㅊ)는 그 초성의 음절**,
  ASCII 대소문자 무시 — (2) 연속이 아니면 건너뛰며 맞는 부분 수열(두 글자 이상) 순으로 점수를 매긴다. 글머리 +300, 낱말 머리 +150, 앞쪽일수록·짧을수록 높다. 키워드에서 맞은 점수는 400을 뺀다.
  IME가 조합 중인 마지막 글자(받침 없는 음절 "브래")는 같은 초성·중성의 음절("랜")과 맞는다.
- **순위**: 점수 + 최근 사용 가산점(가장 최근 250에서 한 칸마다 20씩 줄어든다, `app_settings::command_history` 30개). 가산점이 일치 층(+300/+150)보다 작아 훨씬 나은 일치는 뒤집지 않고,
  비슷한 일치끼리는 최근에 쓴 것이 앞선다. 질의가 비면 최근에 쓴 명령이 앞이고 나머지는 레지스트리 순서다. 꺼진 항목은 맞아도 뒤로 간다.
- **키**: 거르는 칸에서 ↑↓는 `palette_move_intent`, Enter는 커서 항목의 액션이다(정책이 tree의 `palette-enter` 표식에서 읽는다). 커서는 목록의 `cursor`로 강조색 행이다. Esc·바깥 누름은 닫는다.
- **UIA**: 거르는 칸은 `Edit`(`palette-filter`, SetValue)이고 항목은 `ListItem`이다. 행을 Select하면 누른 것과 같다(`select`·`activate`가 같은 액션). 선택 표시(`selected`)를 쓰지 않는 이유는
  UIA Select가 "이미 고른 항목"이면 아무것도 하지 않기 때문이다(첫 항목이 늘 커서라 실행이 막힌다).
- 폴더 대화 상자를 여는 두 명령(`repo.open_folder`·`repo.init`)은 UI thread의 일이라 메시지가 아니라 UI 명령 액션을 낸다(`ui::action_for_command`). 폴더를 고르면 팔레트가 닫힌다.

### 단축키 설정 쪽

환경 설정의 "단축키" 쪽(`settings_page::shortcuts`)은 명령을 묶음별로 보이고 줄마다 지금 단축키와 [편집]·[기본값]을 둔다. [편집]을 누르면 그 줄 바로 아래에 입력 칸(`settings-shortcut-input`)이 선다.
칸의 글은 ` / `로 이은 정규 표기(`Ctrl+Shift+K / F7`)이고 (1) 칸에 초점을 두고 **실제 키 조합을 누르면** 정책이 `capture_shortcut_intent`로 바꿔 칸에 적고(Ctrl·Alt 조합과 기능 키, Enter는 적용),
(2) 손으로 쳐도 되며 UIA는 SetValue로 넣는다. 칸 밑에 문제(읽을 수 없는 키, 예약된 키, "이미 '현재 탭 닫기'에 쓰입니다")가 보이고 문제가 있으면 [적용]이 꺼진다.
[모두 기본값으로]와 줄별 [기본값]이 되돌린다. 적용하면 `app_settings_service`가 곧바로 쓰고, 다음 frame의 단축키 표가 새 키를 쓴다(옛 키는 풀린다).

### 저장소별 사용자 프로필

`app_settings`에 `profiles`(이름·`user.name`·`user.email`·서명 키), `profile_rules`(폴더 접두 → 프로필), `profile_assignments`(저장소 경로 → 적용한 프로필)가 있다. 서명 키는
프로필이 해석하지 않고 글 그대로 보관한다. 환경 설정의 "사용자 프로필" 쪽에서 프로필(추가·편집·삭제)과 규칙을 고친다. 프로필을 지우면 그것을 가리키던 규칙·적용 기록도 지운다.

- **신원 읽기**: 선택된 저장소마다 한 번(그리고 15초가 지나면) `git config --show-scope --show-origin --get-regexp '^user\.(name|email|signingkey)$'`를 worker가 읽는다.
  operation 표(workspace)를 거치지 않는다 — 읽기와 쓰기가 같은 lane(`identity:<저장소>`)에서 차례로 돌고, 결과는 relay로 `identity_loaded_intent`가 온다.
  종료 코드 1(맞는 키 없음)은 실패가 아니라 "설정되지 않음"이다. 출력은 `domain/git_identity`가 읽는다: 마지막 값이 git이 쓰는 값이고, `scope`가 `global`인데 파일이 전역 설정 파일(`GIT_CONFIG_GLOBAL`,
  `~/.gitconfig`, `$XDG_CONFIG_HOME/git/config`)이 아니면 `includeIf`로 끌어온 파일이다. 경로에 역슬래시가 있으면 git이 `file:"C:\\Users\\…"`처럼 C 방식 따옴표를 씌워 풀어 읽는다.
- **상태 줄**: `status-identity` 단추가 "작성자: 홍길동 <hong@corp.example> · 로컬"을 보인다. 이름과 이메일이 서로 다른 파일에서 왔으면 각각 말한다(`· 이름 전역 · 이메일 includeIf`).
  이름이나 이메일이 없으면 빨간 단추다. 누르면 프로필 모드 팔레트가 열린다.
- **적용**: `git config --local -- user.name <값>`과 `user.email`만 쓴다(`--local`을 명시하고 값이 `-`로 시작해도 옵션으로 읽히지 않게 `--`를 둔다). 전역 설정은 어디서도 쓰지 않는다.
  성공하면 `profile_assignments`에 선택을 남기고 신원을 다시 읽는다. 적용하는 길은 환경 설정의 "현재 저장소에 적용", 팔레트의 프로필 모드, 상태 줄 단추, 제안 배너의 [적용]이다(모두 `apply_profile_intent`).
- **제안**: 저장소를 열고 신원을 읽으면, 경로가 규칙의 폴더 아래(폴더 경계 기준, 접두가 가장 긴 규칙)이고 신원이 그 프로필과 다르면 안내 배너에 [적용] 조치를 단다. **제안만 한다 — 눌러야 쓴다.**
  사용자가 이 저장소에 다른 프로필을 적용해 두었고 신원이 그대로 그것이면(규칙을 거스르기로 한 선택) 제안하지 않는다. 저장소마다 앱을 켜 두는 동안 한 번이다.
- **첫 커밋 경고**: 작성자가 없거나(이름·이메일 중 하나라도) 규칙의 프로필과 다르면, 커밋 단추(또는 Ctrl+Enter)가 커밋하지 않고 안내 배너에 이유와 [프로필 적용 후 커밋]·[그대로 커밋]을 띄운다.
  저장소마다 한 번이다(다시 커밋하면 경고 없이 커밋한다). "적용 후 커밋"은 worker가 쓰기를 마쳤다고 알리면 커밋한다. 신원을 아직 읽지 못했으면 근거가 없어 막지 않는다.
- 안내 배너의 조치는 `notice_state::actions`다. `banner_action_intent`가 operation이 없는 조치(`apply_profile:<이름>`·`commit_apply:<이름>`·`commit_anyway`)를 `shell_controller`가 먼저 가져간다.

### 알아 둘 것

- 설정 키(`profiles`·`profile_rules`·`profile_assignments`·`shortcuts`·`command_history`)는 `infrastructure/json_command_settings.*`가 읽고 쓴다. 값이 없으면 키를 쓰지 않고, 알 수 없는 키는 shadow로 보존된다.
- 텍스트 대상은 300번대(`palette_filter`·`settings_shortcut_input`·`settings_profile_*`·`settings_rule_prefix`), element kind도 300번대(`kind_palette_*`·`kind_shortcut_input`·`kind_profile_*`·`kind_command_marker`)다.
- 팔레트의 보이는 항목이 14개라 고른 항목의 메뉴 명령(명령 뒤에 선다)은 거르기 전에는 보이지 않을 수 있다.
- 키 입력 몸짓(Ctrl+Shift+P·Ctrl+P, 팔레트의 ↑↓·Enter, 단축키 입력 칸의 실제 키)과 우클릭 메뉴는 UI Automation으로 닿지 않는다. 팔레트는 탭 줄의 단추로, 입력 칸은 SetValue로 닿는다.
  같은 몸짓이 내는 메시지와 정책은 `tests/palette_profile_shell_tests.cpp`가 `cyella_policy::on_key`와 controller를 직접 불러 확인한다(레지스트리 표를 실은 tree에서 키 → `run_command_intent`, 팔레트 표식의 Enter,
  단축키 입력 칸의 `capture_shortcut_intent`, 고른 항목의 메뉴 명령).
