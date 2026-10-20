# ADR-005: 인터랙티브 rebase는 git의 두 편집기 자리에 cyella 도우미를 건다

## 상태

승인됨 - 2026-10-20

## 배경

`git rebase -i`는 대화형 편집기 둘을 부른다.

- **순서 편집기**(`GIT_SEQUENCE_EDITOR`, 없으면 `sequence.editor`·`GIT_EDITOR`): `<git_dir>/rebase-merge/git-rebase-todo`를
  열어 사용자가 할 일 목록을 고치게 한다.
- **메시지 편집기**(`GIT_EDITOR`·`core.editor`): reword 줄과, squash가 든 묶음의 끝에서 `<git_dir>/COMMIT_EDITMSG`를 연다.
  충돌을 풀고 `rebase --continue`를 낼 때도 연다.

cyella는 모든 git을 비대화형으로 돌리고([ADR-003](ADR-003-process-execution.md)) `GIT_EDITOR=true`를 건다. 앱이 화면에서
목록과 메시지를 받으므로 두 편집기 자리를 앱이 채워야 한다. 충돌로 멈추면 operation은 `needs_input{conflict}`로 끝나고
(`needs_input`은 worker를 붙잡지 않는다) 이어지는 `rebase_continue`는 **다른
operation**이다. 그때도 남은 reword·squash 메시지가 적용되어야 한다.

이 결정이 기대는 git 2.52의 동작은 다음과 같다.

- reword 줄은 `pre-commit` → 편집기 → `commit-msg` 순서로 돈다. squash 묶음은 묶음의 **마지막 줄**에서 편집기를 한 번
  연다(`pick A · squash B · fixup C`면 C에서). squash·fixup 커밋에는 commit-msg hook이 돌지 않는다(git의 동작).
- 편집기가 열릴 때 `rebase-merge/done`의 마지막 줄이 지금 처리 중인 할 일이다. 우리가 쓴 줄(SHA 전체)이 그대로 남는다.
- 충돌 뒤 `rebase --continue`는 멈춘 pick에서도 편집기를 연다(`done` 마지막 줄 = `pick <sha>`).
- 순서 편집기가 0이 아닌 값으로 끝나면 git은 rebase를 시작하지 않고 깨끗이 멈춘다("there was a problem with the editor").
  빈 목록이면 "nothing to do"다.
- git이 만드는 목록은 `log --right-only --cherry-pick --max-parents=1 --topo-order upstream...HEAD`와 같다 (이미 위쪽에 같은
  패치가 있는 커밋과 병합 커밋이 빠진다).
- `-i`의 기본 `--empty`는 멈춤이다. 비게 된 커밋(위쪽에 이미 있는 패치)을 pick하면 충돌 파일 없이 진행 중으로 멈춘다.
- rebase가 끝나거나 중단되면 git이 `rebase-merge` 폴더를 통째로 지운다.

## 결정

### 1. 할 일 목록: `GIT_SEQUENCE_EDITOR`에 편집기 도우미를 건다

- 새 실행 파일 `cyella-rebase-editor.exe`(askpass 도우미와 같은 모양: WIN32 subsystem, luil·Skia에 링크하지 않고
  `domain/interactive_rebase.cpp`만 직접 컴파일)를 `cyella.exe` 옆에 둔다.
- 실행기는 operation마다 준비 폴더(`%TEMP%\cyella-rebase\<op>-<시각>-<번호>\`)에 `todo`(우리 형식의 목록), `expected`(편집기에 보인
  범위의 SHA, 한 줄에 하나), `messages/<sha>`를 쓰고, `rebase -i`에 `GIT_SEQUENCE_EDITOR`·`GIT_EDITOR`=도우미,
  `CYELLA_REBASE_STATE`=준비 폴더를 건다. 자료는 명령줄이 아니라 환경 변수로 넘긴다.
- 도우미는 git이 넘긴 파일 이름이 `git-rebase-todo`면 순서 편집기로 일한다. 두 편집기 변수가 한 환경에 함께 있어 환경
  변수로는 역할을 가를 수 없기 때문이다. 하는 일:
  1. git이 만든 목록의 SHA(줄인 것)가 `expected`와 정확히 짝지어지는지 본다. 아니면 `mismatch`를 남기고 1로 끝난다 → git이
     시작하지 않는다 → 실행기가 `refused`("편집기를 연 뒤 rebase할 커밋이 바뀌었습니다")로 알린다.
  2. `messages/`를 `rebase-merge/cyella/messages/`로 옮기고 표지 `rebase-merge/cyella/active`를 쓴다.
  3. `git-rebase-todo`를 우리 목록으로 덮어쓴다.
- 목록 형식은 한 줄에 `<명령> <SHA 전체>`(LF, 주석·제목 없음)이다. 지울 커밋도 `drop <sha>`로 적는다 (빠진 줄을 git이 지우는데,
  사용자 설정 `rebase.missingCommitsCheck=error`면 거절되므로). 줄 이름은 `pick·reword·squash·fixup·fixup -C·drop`이다.
- 명령줄: `rebase -i --empty=drop --no-autosquash --no-update-refs [--autostash] -- <onto> [<branch>]`. `--empty=drop`은 위의
  "충돌 없이 멈춤"을 막고, 나머지 둘은 사용자 설정(`rebase.autoSquash`, `rebase.updateRefs`)이 git의 목록을 바꾸지 않게 한다.
  목록은 앱이 정한다.
- 실행기는 git을 띄우기 전에 같은 범위를 다시 읽어 목록을 검사한다 (비어 있지 않다, 범위의 커밋이 모두 한 번씩 있다, 범위 밖
  SHA가 없다, drop을 뺀 첫 줄이 squash·fixup이 아니다, reword에 메시지가 있다). 진행 중 작업이 이미 있으면 `refused`다.

**셸**: git은 편집기 값에 셸 특수 문자(`|&;<>()$\`\\"' \t\n*?[#~=%`)가 있으면 `sh -c`로, 없으면 바로 실행한다. 값은 도우미 경로
하나뿐이고, 구분자를 `/`로 바꾸면 보통의 설치 경로에는 특수 문자가 없어 셸 없이 돈다. `Program Files`처럼 공백이 있으면
작은따옴표로 감싸(`'\''`) Git for Windows의 `sh`가 돌린다. 공백·작은따옴표·한글이 든 폴더는 통합 test가 다룬다.

Git for Windows의 `cp`를 `GIT_SEQUENCE_EDITOR="cp '<todo>'"`로 쓰는 안은 버렸다. 언제나 셸을 거치고, 범위 검사와 메시지
옮기기를 할 자리가 없다.

### 2. reword·squash 메시지: `GIT_EDITOR`에 같은 도우미, `rebase-merge/done`으로 짝짓기

- 그 밖의 파일(보통 `COMMIT_EDITMSG`)이면 도우미는 메시지 편집기다. 파일의 부모 폴더가 git 디렉터리다(worktree면 그 worktree의
  git 디렉터리이고 `rebase-merge`도 같은 곳이다). `rebase-merge/done`의 마지막 커밋 줄이 reword·squash·fixup이고
  `rebase-merge/cyella/messages/<그 SHA>`가 있으면 그 글로 덮어쓴다. pick이거나 파일이 없으면 손대지 않는다(`true`와 같다).
- 열쇠: reword는 그 줄의 SHA, squash가 든 묶음은 **묶음 마지막 줄**의 SHA다(git이 그 줄에서 편집기를 연다).
- 메시지는 LF로 바꾸고 끝에 줄바꿈 하나를 둔다. git은 편집기에서 돌아온 메시지를 `strip`으로 정리하므로 `#`로 시작하는 줄은
  빠진다(사용자가 터미널에서 편집기를 쓸 때와 같다).

**왜 `pick` + `exec git commit --amend -F`가 아닌가**:

- `exec` 줄도 셸로 돈다. 메시지 파일 경로(사용자 임시 폴더, 한글 사용자 이름)를 셸 인용해야 하고 셸에서 `git`을 찾아야 한다
  (cyella는 설정에서 고른 git을 쓸 수 있다). 셸 문제를 없애 주지 않는다.
- `exec`가 실패하면(hook 거절) 충돌 없이 멈춘 진행 중 상태가 되고, reword와 다른 단계 수(`msgnum/end`)가 화면에 보인다.
- 도우미 방식은 git이 원래 하는 일(같은 hook 순서, 같은 작성자·날짜 규칙, 같은 단계 수)을 바꾸지 않는다. 짝짓기는 줄 순서가
  아니라 `done`의 SHA라 충돌·건너뛰기 뒤에도 맞다.

**hook**: reword는 commit-msg hook이 우리 메시지로 돈다(통합 test가 꼬리말을 더하는 hook으로 다룬다). squash 묶음에는 git이
hook을 돌리지 않는다. 바꾸지 않는다.

### 3. 충돌과 이어 가기: 상태는 `rebase-merge/cyella`에 둔다

- 메시지는 첫 명령에서 도우미가 `rebase-merge/cyella/`로 옮겨 둔다. `rebase_continue`·`rebase_skip`은 `rebase-merge/cyella/active`가
  있으면 `GIT_EDITOR`=도우미를 건다. 상태가 저장소 안에 있으므로 앱을 다시 켜도 이어지고, workspace가 기억할 것이 없다.
- **지우기**: rebase가 끝나거나(`continue`·`skip`의 마지막) 중단되면(`abort`, 터미널의 `--abort`·`--quit` 포함) git이
  `rebase-merge`를 지우므로 메시지도 함께 사라진다. 준비 폴더는 첫 명령이 끝나면 실행기가 바로 지우고, 비정상 종료로 남은
  것은 다음 인터랙티브 rebase가 24시간 넘은 것부터 지운다.
- 진행 중 표시: `in_progress`에 `current_action`·`current_sha`(`done` 마지막 줄), `interactive`(`rebase-merge/interactive`),
  `cyella_messages`(`rebase-merge/cyella/active`)를 더했다. 충돌 화면이 "할 일 2/5 · pick abc1234 · 남은 reword·squash 메시지는
  계속하거나 건너뛰면 그대로 적용됩니다"를 보인다. cyella 밖에서 시작한 rebase면 git의 기본 메시지를 쓴다고 알린다.
- 진행률은 stderr의 `Rebasing (k/n)`을 읽는다 (일반 rebase도 같은 진행률을 얻는다).

### 4. 병합 커밋

`--rebase-merges`는 지원하지 않는다 (label·reset·merge 줄을 다루는 그래프 편집기가 필요하다). 범위에 병합 커밋이 있으면
편집기가 경고하고 "병합 커밋을 빼고 한 줄로 펴도 됩니다"를 확인해야 시작한다(`allow_flatten_merges`). 확인 없이 내면
실행기가 `refused`로 거절한다. 펴면 병합에서 고친 충돌 해결이 사라진다는 것을 경고에 적었다.

### 5. 자동 squash

편집기를 처음 채울 때 `fixup! `·`squash! `·`amend! ` 커밋이 있으면 git `--autosquash`와 같은 규칙(겹친 표지는 모두 벗기고,
앞쪽에서 제목이 같은 커밋 → SHA 앞부분 → 제목 앞부분 순으로 대상을 찾아 그 뒤에 이어 붙인다, `amend!`는 `fixup -C`)으로
정렬하고, "fixup!·squash! 자동 정렬" 칸으로 원래 순서와 오간다. 실행 명령은 `--no-autosquash`다 (정렬은 앱이 이미 했다).

### 6. 원격에 있는 커밋

범위 읽기(`load_rebase_plan`)가 `rev-list <onto>..<branch> --not --remotes`로 어느 원격 브랜치에도 없는 커밋을 가려, 나머지를
"원격에 있음"으로 표시한다. 하나라도 있으면 "강제 push(--force-with-lease)가 필요합니다" 경고가 선다. 막지는 않는다.

## 결과

- `interactive_rebase_request{onto, branch, todo[{action, sha, message?}], autostash, allow_flatten_merges}`와
  `load_rebase_plan_request{onto, branch}`(읽기 lane, 결과는 `rebase_plan_loaded` 사건 → `git_workspace::rebase_plan_of`)가 생겼다.
- 도우미가 없으면(`locate_rebase_editor_helper`가 못 찾으면) 인터랙티브 rebase만 이유를 알리고 실패한다. 다른 작업은 그대로다.
- 요청마다 환경 변수를 더하는 `git_request_options::environment`가 생겼다 (같은 이름은 값을 바꾼다. 실행기는 중복 이름을
  거절한다).
- 사용자가 cyella 밖에서 `git rebase --continue`를 내면 그의 편집기가 열리고 cyella가 맡긴 메시지는 쓰이지 않는다.
- 남은 일: `--rebase-merges`, `edit`·`exec`·`break` 줄, 묶음 메시지에서 `#` 줄을 지키는 cleanup 설정.
