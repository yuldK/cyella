# 앱 상태 설계

탭, 설정 파일, 시작 페이지 목록, 파일 감시의 규칙이다. 화면은 presentation 계층이 붙인다. 이 문서의 규칙은 모두 순수
함수나 주입 가능한 경계로 구현되어 있어 UI 없이 테스트한다.

## 탭 규칙 (`application/repository_tabs`)

탭은 `start`(시작 페이지) 또는 `repository{path, display_name}`이다. 탭마다 단조 증가하는 `uint64_t` id가 있고
닫아도 다시 쓰지 않는다. UI는 이 id를 key로 쓴다.

- 탭은 항상 하나 이상이고, 선택된 탭이 정확히 하나다. 처음에는 선택된 시작 탭 하나다.
- `open_repository(path)`
  1. 정규화한 키(구분자 `\`, 끝 구분자 제거, ASCII 소문자)가 같은 저장소 탭이 있으면 그 탭을 선택하고 새 탭을
     만들지 않는다. 이때 선택돼 있던 시작 탭은 그대로 남는다.
  2. 아니고 선택된 탭이 시작 탭이면 그 자리에서 저장소 탭으로 바꾼다. 새 id를 받는다.
  3. 아니면 선택된 탭 바로 오른쪽에 끼우고 선택한다.
- `new_tab()`("+")은 선택된 탭 오른쪽에 시작 탭을 끼우고 선택한다.
- `close(index)`
  - 선택된 탭을 닫으면 오른쪽 이웃을, 오른쪽이 없으면 왼쪽 이웃을 선택한다.
  - 선택되지 않은 탭을 닫으면 선택은 같은 탭에 머문다(인덱스만 당겨진다).
  - 마지막 탭을 닫으면 시작 탭 하나가 남는다. 이미 시작 탭 하나뿐이면 아무것도 바뀌지 않는다(`false`).
- `reorder(from, to)`는 탭을 빼서 `to` 인덱스에 놓고, 선택은 탭을 따라간다.
- 세션 복원: `capture_open_tabs`는 저장소 탭의 경로만 순서대로 `open_tabs`에 담는다. 선택된 탭이 저장소 탭이면
  그 경로가 `active_tab`이고, 시작 탭이면 비운다. `restore_tabs`는 중복과 빈 경로를 거르고, 남는 것이 없으면
  시작 탭 하나로 시작한다. `active_tab`을 경로로 찾아 선택하며(인덱스가 아니라 경로로 둔 이유는 걸러져도
  가리키는 탭이 바뀌지 않게 하려는 것이다) 없으면 첫 탭이다.
- 복원 때 디스크에 없는 경로를 거르지 않는다. 그 판단은 탭을 여는 쪽의 몫이다.

## 설정 파일 (`%APPDATA%\cyella\settings.json`)

위치는 `SHGetKnownFolderPath(FOLDERID_RoamingAppData)` 아래 `cyella` 폴더다. 폴더가 없으면 만든다. UTF-8,
CRLF, 4칸 들여쓰기다. 알려진 키는 저장할 때마다 모두 쓴다.

```json
{
    "schema_version": 1,
    "recent_repositories": [
        { "path": "D:\\work\\repo", "display_name": "repo", "opened_at": "2026-10-20T09:30:12Z", "pinned": true }
    ],
    "open_tabs": ["D:\\work\\repo"],
    "active_tab": "D:\\work\\repo",
    "git_executable": "",
    "external_diff_tools": [
        { "extensions": [".png", ".tar.gz"], "executable": "C:\\tools\\imgdiff.exe", "arguments": "--left \"{left}\" --right \"{right}\"" }
    ],
    "appearance": { "theme": "system" },
    "window": { "x": 100, "y": 100, "width": 1400, "height": 900, "maximized": false },
    "ui": { "pane_widths": { "sidebar": 232, "detail": 360 } }
}
```

| 키 | 규칙 |
| --- | --- |
| `recent_repositories` | 고정(`pinned`)이 앞이고 상한이 없다. 비고정은 20개까지이고 넘으면 오래된 것부터 버린다. `pinned`는 참일 때만 쓴다. 경로 키가 같은 항목은 먼저 나온 것만 남긴다. |
| `open_tabs`, `active_tab` | 저장소 경로. 시작 탭은 저장하지 않는다. |
| `git_executable` | 빈 값이면 자동 탐색. 값이 있으면 절대 경로여야 하고, 아니면 경고하고 자동 탐색. |
| `external_diff_tools` | 확장자(점·`*.` 생략 가능, 대소문자 무시, `.tar.gz` 같은 여러 점 가능), 실행 파일, 인자 템플릿. 확장자나 실행 파일이 없는 줄은 경고하고 버린다. |
| `appearance.theme` | `system`, `light`, `dark`. 그 밖의 값은 경고하고 `system`. |
| `window` | 좌표·크기(정수, 너비·높이 양수)와 `maximized`. 틀리면 경고하고 배치 없이 시작. 값이 없으면 키를 지운다. |
| `ui.pane_widths` | 사이드바·상세 판의 폭(논리 픽셀, 양수, 소수 둘째 자리까지)이다. 한쪽만 있어도 된다. 없거나 틀린 값은 기본 폭이고, 시작할 때 `shell_layout`의 최소·최대로 다듬는다. 판을 끄는 동안에는 쓰지 않고 500 ms 조용해지면(또는 앱을 닫을 때) 한 번 쓴다. 둘 다 없으면 `ui` 키를 쓰지 않는다. |

외부 diff 도구는 `select_external_diff_tool`이 파일 이름의 접미사로 고른다. 여러 도구가 맞으면 가장 긴 확장자가
이기고(`.tar.gz`가 `.gz`보다 구체적), 같으면 목록에서 먼저 나온 도구다. `expand_external_diff_arguments`는
템플릿을 먼저 공백(큰따옴표 묶음 지원)으로 나눈 다음 각 인자 안에서 `{left}` `{right}` `{base}` `{path}`를
바꾸므로 값에 공백·따옴표·셸 문자가 있어도 한 인자로 남고 셸을 거치지 않는다.

### 읽기·쓰기 규칙

- **알 수 없는 키 보존.** 읽은 원본 JSON을 shadow로 들고 있다가 저장할 때 그 위에 알려진 값만 덮어쓴다.
  `appearance`·`window`·`ui`·`ui.pane_widths` 안의 모르는 키도 남는다.
- **원자적 쓰기.** 같은 폴더의 임시 파일에 쓰고 `FlushFileBuffers` 뒤 `MoveFileExW(REPLACE_EXISTING |
  WRITE_THROUGH)`로 교체한다. 폴더가 없으면 만든다.
- **저장 실패.** 메모리 상태(`app_settings_service`)는 그대로 두고 진단 하나(`app_settings_write_failed`)만
  돌려준다. 다음 변경이나 `save()`가 밀린 상태를 한꺼번에 다시 쓴다. `has_unsaved_changes()`로 알 수 있다.
- **깨진 파일.** JSON object로 읽을 수 없으면(문법 오류, 잘못된 UTF-8, object가 아님) 파일을
  `settings.json.bad-<UTC 시각>`(`20261020T093012Z`)으로 옮기고 기본값으로 시작한다. 같은 이름이 있으면
  `-1`, `-2`를 붙인다. 빈 파일은 깨진 것으로 보지 않는다. 필드 하나가 틀린 것은 경고만 남기고 그 필드만
  기본값을 쓴다.
- **읽기 실패.** 파일이 있는데 읽지 못하면(잠김, 접근 거부 등 "없음"이 아닌 실패) 기본값으로 시작하되 이번 실행
  동안 읽기 전용이 된다. `save()`는 파일을 건드리지 않고 진단 하나(`app_settings_write_failed`)만 돌려주며, 메모리
  상태는 평소처럼 바뀐다. 읽지 못한 진짜 파일을 기본값으로 덮어쓰지 않으려는 것이다(깨진 파일은 백업이 있어 해당 없음).
- **스키마 버전.** 없거나 1 미만이면 현재(1)로 읽는다. 현재보다 크면 경고 하나를 남기고 아는 필드만 읽으며,
  저장할 때 버전 번호를 낮추지 않는다(모르는 키는 shadow로 남는다).

## 시작 페이지 목록 (`presentation/start_page_view`)

`build_start_page_view(settings, exists, search)`는 순수 함수다.

- 고정한 항목이 먼저(설정 목록의 순서), 그다음 `opened_at` 내림차순(같거나 없으면 설정 목록의 순서)이다.
- 항목은 표시 이름, 부모 폴더, 전체 경로, `pinned`, `exists`를 가진다. `exists`는 주입한 함수로 정하며
  거짓이면 화면이 "사용할 수 없음"으로 그린다. 목록에서 자동으로 지우지 않는다.
- 검색은 앞뒤 공백을 뗀 검색어를 이름 또는 경로에서 부분 문자열로 찾는다. ASCII 대소문자를 무시하고
  `/`는 `\`로 본다. 한글은 UTF-8 바이트 비교로 찾는다(대소문자가 없고 UTF-8이 글자 경계에서 자기
  동기화되므로 글자 중간에 맞지 않는다). `exists`는 검색을 통과한 항목에만 부른다.

## 파일 감시를 앱에 잇기

감시기는 `cyella_driver`가 소유한다. 열린 저장소(workspace의 세션)마다 하나이고, driver가 메시지를 하나 처리할 때마다 열린 저장소와 맞춘다(탭을 열면 켜고 닫으면 `stop()`).
콜백은 감시 스레드에서 불리므로 상태를 건드리지 않고 relay로 `repository_changed_intent`를 올린다. logic thread의 처리는 `application/watch_refresh_policy`(순수 규칙)가 정한다.

| 알림 | 낼 것 |
| --- | --- |
| `working_tree_changed` | `refresh_status`(status·줄 수만) |
| `repository_state_changed` | `refresh`(전체) |
| `watch_lost` | 감시를 접고 안내 한 번, 전체 `refresh` 한 번. 이후 그 저장소는 창 초점에서 전체를 읽는다 |
| 창 초점 복귀(다른 프로세스 → 이 프로세스) | 선택된 탭에 `refresh_status`(감시가 끊겼으면 `refresh`). 1초 디바운스 |

- 쓰기가 돌거나 줄 서 있으면 건너뛴다. 새로 고침이 도는 중이면 저장소마다 보류 하나만 남기고 끝난 뒤 한 번 낸다.
- `refresh_status`는 `status_loaded`로 세션의 요약·작업 영역 목록만 바꾼다. 브랜치·HEAD가 바뀌었거나 WIP 행이 생기거나 사라져야 하면 전체 `refresh`로 올린다.
- 없는 폴더의 저장소는 감시하지 않는다(그 탭은 이미 열 수 없음으로 보인다). 감시를 시작하지 못하면 안내를 한 번 띄운다.
- 그래서 앱 밖에서 파일을 고치면 WIP 행이 약 0.7초 안에 나타나고, 터미널의 `git commit`·`git branch`·`git branch --set-upstream-to`가 클릭 없이 화면에 따라온다.

## 설정의 diff

`diff` 객체: `layout`(`inline`·`split`, 기본 `inline`)과 `context_lines`(0~25의 정수, 기본 3). 틀린 필드만 기본값으로 두고 경고한다. 환경 설정의 "diff" 쪽에서 고른다
(보기를 바꾸면 열려 있는 diff도 바로 바뀌고, 문맥 줄 수는 다음에 여는 diff부터 `git diff -U<n>`에 쓴다).

## 파일 감시 (`platform/win32/win32_repository_watcher`)

전용 스레드 하나가 `ReadDirectoryChangesW`(겹치기 I/O, 재귀)를 기다리고, 디바운스한 뒤 콜백으로
`repository_state_changed`, `working_tree_changed` 순으로 알린다(둘 다 모였을 때). 분류와 디바운스는 순수
함수·상태 기계(`application/repository_change_classifier`)로 테스트한다.

- 디바운스: 마지막 변경 뒤 300 ms 조용하면 방출한다. 변경이 끊기지 않아도 처음 변경 뒤 2초 안에는 방출한다.
- `.git` 분류 (작업 트리의 `.git/**`는 작업 트리 변경으로 세지 않는다)

| 경로(`.git` 기준) | 분류 |
| --- | --- |
| `*.lock` (모든 규칙보다 먼저) | 무시 |
| `objects/**`, `logs/**` | 무시 |
| `config`(upstream 설정), `HEAD`, `index`, `packed-refs`, `FETCH_HEAD`, `ORIG_HEAD`, `MERGE_HEAD`, `CHERRY_PICK_HEAD`, `REVERT_HEAD` | `repository_state_changed` |
| `refs/**`, `rebase-merge/**`, `rebase-apply/**` | `repository_state_changed` |
| 그 밖(`hooks`, `MERGE_MSG` 등) | 무시 |

  이름은 ASCII 대소문자를 구분하지 않는다. 중첩 저장소(`vendor/lib/.git/**`)의 내부는 무시한다.
- `.git`이 파일(worktree, submodule)이면 `gitdir: <path>`를 읽어 그 gitdir을 따로 감시한다(상대 경로는 `.git`
  파일이 있는 폴더 기준). gitdir에 `commondir`가 있으면 그 공용 디렉터리도 감시하되 `refs/**`와
  `packed-refs`만 본다(HEAD·index는 다른 worktree의 것이다).
- 버퍼가 넘치거나(`ERROR_NOTIFY_ENUM_DIR`, 0바이트 완료) 읽기를 다시 걸지 못하면 놓친 변경이 있을 수 있으므로
  두 종류를 모두 알린다.
- `start`가 돌아오기 전에 첫 읽기를 걸어 두므로 그 뒤의 변경은 놓치지 않는다.
- `stop()`은 어느 스레드에서 몇 번 불러도 안전하고, 걸린 읽기를 `CancelIoEx`로 거둔 뒤 스레드를 join한다.
  돌아온 뒤에는 콜백이 불리지 않는다. 콜백 안에서 부르면 표시만 하고 돌아온다.
- **git을 실행하지 않고 `.gitignore`도 따르지 않는다.** 무시된 경로(`node_modules`, 빌드 산출물)의 변경도
  `working_tree_changed`가 된다. 소비자가 `status`를 돌리면 걸러지고, 디바운스가 호출 횟수를 줄인다.
  산출물이 쏟아지는 저장소는 최대 대기 시간(2초)마다 한 번 `status`가 돈다.
