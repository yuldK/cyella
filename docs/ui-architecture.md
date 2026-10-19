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
- 판 폭은 아직 설정에 남기지 않는다(`app_settings`에 칸이 없다). 칸을 더하면 `shell_controller::update_settings`로 저장한다.
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

- **인증 대화 상자**: `askpass_bridge`는 `cyella_driver`가 소유한다(백엔드 멤버들 옆, scheduler보다 앞에 선언).
  `answer_input_intent`·`cancel_input_intent`는 `cyella_driver::handle_backend_intent`에 분기를 더한다. 대화 상자는
  `ui::build_ui_tree`의 마지막 자식으로 Clone 대화 상자와 같은 모양(`luil::modal_host_element`)으로 단다 — 지금은
  `needs_input{passphrase|host_key|credentials}`가 배너로 "인증 대화 상자는 아직 없습니다"를 보인다
  (`build_view_snapshot.cpp`의 `operation_banner`). 탭의 🔔 표시는 이미 `compute_tab_marker`가 낸다.
- **diff 웹뷰**: 파일을 고르면 `tab_ui_state::diff`가 서고 `diff_file`이 나간다. 그래프 자리를 diff가 덮는 판은
  `repository_ui.cpp`의 `build_graph_area` 첫 갈래다. 머리 막대(돌아가기·제목)는 luil에 남기고, 그 아래의 임시 줄 목록
  (`diff_row_element`의 `virtual_list_element`)을 `luil::webview_element` 자리표로 바꾼 뒤 `cyella_driver::make_frame`에서
  `diff_view_controller`가 `frame->webviews`를 채운다. 자리는 snapshot의 `repository_view::diff`(요청·제목)를 그대로 쓴다.
- **대화 상자·끌어 놓기·메뉴**: 지금은 `show_notice_intent`로 "…는 아직 없습니다"를 띄우는 자리들이다 —
  툴바 Branch, upstream 없는 Push, 저장소·브랜치 선택 상자, 태그·stash 체크아웃, 없는 경로의 위치 찾기, 환경 설정,
  체크아웃 정책(`needs_input{dirty_worktree}` 배너), 충돌 해결(`needs_input{conflict}` 배너), discard 확인.
  대화 상자는 Clone처럼 `ui_state`에 열림 상태를 두고 `build_ui_tree` 끝에 `modal_host_element`로 단다.
  메뉴는 `ui_frame::popups`에 `menu_element` tree로 싣는다(`cyella_policy::menu`를 함께 채운다).
  끌어 놓기는 그래프 행 내용(`graph_row_element`)과 사이드바 행(`sidebar_row_element`)에 `drag`·`drop`을 단다 — 둘 다
  지금은 누름을 목록 행에 넘기도록 지어져 있으니, 라벨 칩 단위로 끌려면 라벨마다 자식 element가 필요하다.

## 화면 세부

- 환경 설정·테마 단추가 캡션이 아니라 탭 줄 오른쪽에 있다 (아래 "알아 둘 것"의 캡션 띠).
- 탭은 아이콘 자리 하나에 표시(🔔 ⟳ ⌚ ●)를 그린다(`tab_item`에 칸이 하나뿐이다).
- 커밋 본문 칸이 한 줄이다. luil `text_input_element`에 여러 줄 칸이 없다.
- 시작 페이지의 오른쪽 안내 카드(Git·자격 증명·알아 두면 좋은 것)가 없고, git 버전은 제목 아래 한 줄이다.
- 작업 영역 행의 단추가 hover 때만이 아니라 늘 보이고, 체크박스 일괄 선택·트리 보기가 없다. discard는 확인 대화 상자 대신 두 번 눌러야 한다.
- 원격에 없는 태그 표시는 그 실행에서 fetch(또는 태그 push)가 한 번 돈 뒤에야 맞다. 그 전에는 원격이 있는 저장소의 태그가 모두 점선이다
  (`ref.remotes`가 `ls-remote` 전에는 비어 있다).
- 사이드바의 원격 묶음 머리는 접히지 않는다. 툴바 선택 상자와 우클릭 메뉴는 표시만 한다.

## 알아 둘 것

- 그래프 목록의 항목 키는 SHA 앞 15자다. 40자 키는 MSVC 짧은 문자열 최적화를 넘어 10만 행이면 frame마다 10만 번
  할당한다. 모델(`virtual_list_item` 벡터)은 세션의 generation·행 수로 cache하고(`shell_controller::graph_items`)
  frame마다 config로 복사만 한다.
- 목록 행 안의 내용 element가 액션을 가지면 hit test가 그 element를 답해 목록 행의 고르기를 가로챈다. 그래서 내용은
  `hit_opaque(false)`이고, 두 번 누름이 필요한 사이드바 브랜치 행만 한 번 누름에도 같은 고르기 메시지를 다시 낸다.
- 캡션 띠는 luil의 창 버튼 밖이 전부 끌기 영역(HTCAPTION)이라 앱 단추를 둘 수 없다. 환경 설정·테마 단추는 탭 줄 오른쪽에 둔다.
