# ADR-003: 프로세스 실행기는 gitman의 것을 확장해 쓴다

## 상태

승인됨 - 2026-10-20

## 배경

[ADR-001](ADR-001-git-backend.md)에 따라 모든 Git 작업이 외부 프로세스다. 실행기는 gitman의 process runner를
이식했다. 취소, 시간 초과, job object 정리, 손자 프로세스 처리를 이미 갖추고 있다. 다만 gitman은 상태 조회와
갱신만 해서 cyella에 필요한 몇 가지가 없다. 이 문서는 확장한 부분과 실행 정책에서 달라진 부분만 적는다.

## 결정

### 실행기 확장

1. **표준 입력.** `process_request::standard_input`(`std::string`)이 비어 있지 않으면 pipe를 만들고 쓰기
   스레드가 채운 뒤 닫는다. 비어 있으면 기존처럼 NUL 장치다. 쓰기 스레드는 읽기 스레드와 같은
   목록에서 join하므로 취소·시간 초과 때 `CancelSynchronousIo`가 막힌 `WriteFile`도 풀어 준다.
   자식이 입력을 다 읽지 않고 끝나 pipe가 끊기는 것은 정상으로 본다.
2. **원시 출력 모드.** 스트림마다 `process_output_mode`(`lines`·`raw`)를 정한다.
   - `raw`는 pipe에서 읽은 byte를 `process_output_sink::on_chunk`로 그대로 넘긴다. 줄 나누기, `\r`
     해석, 8 KiB 분할, 인코딩 변환, 마스킹이 없고 NUL이 보존된다. 조각은 한 번에 최대 64 KiB이고
     sink 호출은 직렬화되므로 스트리밍 해석기가 상태를 들고 이어서 소비할 수 있다.
   - `raw`도 캡처 상한을 지킨다. 넘으면 이후 byte를 버리되 pipe는 계속 비운다. 스트리밍 소비자는
     `unlimited_process_capture_bytes`를 쓴다.
   - 스트림별로 둔 이유: `log -z`는 stdout만 `raw`로 받고, 진행 표시가 오는 stderr는 `lines`로 받는다.
   - `raw` 조각은 **마스킹되지 않는다.** 로그에 그대로 남기면 안 된다. 기록용 명령줄
     (`masked_command_line`)과 `lines` 출력만 마스킹한다.
3. 그 밖의 job object(kill-on-close), 취소, 시간 초과, 2초 pipe 유예, 손자 처리는 이식한 그대로다.

### 실행 정책

| 항목 | gitman | cyella |
| --- | --- | --- |
| `GIT_SSH_COMMAND` | `ssh -oBatchMode=yes`로 덮어씀 | 건드리지 않음. 사용자의 `core.sshCommand`와 ssh 설정이 터미널과 같게 적용된다 |
| `GCM_INTERACTIVE` | `Never` | 건드리지 않음. Git Credential Manager가 자기 창을 띄울 수 있다 |
| `GIT_ASKPASS`·`SSH_ASKPASS` | 항상 삭제 | askpass 도우미 경로가 있으면 설정하고 `SSH_ASKPASS_REQUIRE=force`를 더한다. 없으면 삭제한다. |
| `GIT_TERMINAL_PROMPT` | `0` | `0` (같음) |
| `GIT_OPTIONAL_LOCKS` | `0` | 설정하지 않음. `status`가 index를 갱신하려면 잠금이 필요하고, 같은 저장소의 쓰기와 `status`는 한 lane에서 직렬로 실행하므로 충돌하지 않는다 |
| `DISPLAY` | 삭제 | 건드리지 않음 |
| 시간 제한 | 조회 600초, 전환 300초, 갱신 무제한 | 로컬 조회 600초, 로컬 쓰기 300초, **네트워크 무제한**(취소만), 도구 확인 5초 |

| 항목 | 정책 |
| --- | --- |
| `GIT_SSH_COMMAND` | 건드리지 않는다. 사용자의 `core.sshCommand`와 ssh 설정이 터미널과 같게 적용된다 |
| `GCM_INTERACTIVE` | 건드리지 않는다. Git Credential Manager가 자기 창을 띄울 수 있다 |
| `GIT_ASKPASS`·`SSH_ASKPASS` | askpass 도우미 경로가 있으면 설정하고 `SSH_ASKPASS_REQUIRE=force`를 더한다. 없으면 삭제한다([ADR-004](ADR-004-askpass.md)) |
| `GIT_TERMINAL_PROMPT` | `0` |
| `GIT_OPTIONAL_LOCKS` | 환경 변수는 설정하지 않는다. 대신 읽기 조회(`local_query`)에 `--no-optional-locks` 인자를 붙인다. 읽기는 쓰기와 다른 lane에서 병렬로 돌기 때문에, `status`가 index를 고쳐 쓰려고 `index.lock`을 잡으면 같은 저장소의 쓰기가 잠금 오류로 실패한다 |
| `LC_ALL` | `C`. 오류 분류기와 `%(upstream:track)`이 영어 문구를 기대한다. 경로는 `core.quotepath=false`로 UTF-8 원문이 나와 로캘과 무관하다 |
| `GIT_EDITOR` | `true`. `rebase --continue`·`merge`·`revert`가 편집기를 기다리며 worker를 붙잡지 않는다 |
| `DISPLAY` | 건드리지 않는다 |
| 시간 제한 | 로컬 조회 600초, 로컬 쓰기 300초, **네트워크 무제한**(취소만), 도구 확인 5초 |

도우미가 없을 때 인증이 필요하면 `GIT_TERMINAL_PROMPT=0` 때문에 git이 즉시 실패한다.

### 작업 큐

저장소 key를 해시해 worker에 고정하면(gitman의 방식) 서로 다른 저장소가 우연히 같은 worker에
몰려 긴 fetch 뒤에서 기다린다. 그래서 저장소별 FIFO lane을 두고, 일이 있는 lane을 노는 worker가
가져간다.

- 같은 `repository_key`는 접수 순서로 하나씩 실행한다. index·ref를 바꾸는 작업과 `status`가 모두
  같은 key를 쓴다. 서로 다른 key는 병렬이고 동시 실행 상한은 `min(4, hardware_concurrency)`다.
- `interactive` 작업(diff, show, log 한 쪽)은 전용 worker가 접수 순서로 실행한다.
- 작업은 `std::function` job이라 scheduler가 git 작업 종류를 모른다. 접수한 job은 종료 중에도
  정확히 한 번 호출되며, 종료가 시작되면 취소된 token을 받는다.
- 큐 길이 상한은 두지 않는다. job이 작은 closure라서 상한보다 유실을 피하는
  쪽을 택했다.

### 진행률

`git_progress_parser`는 `--progress`의 stderr 줄을 `{단계 이름, 0..1 비율}`로 바꾸는 순수 함수다.
단계 이름이 번역되어도 구조(대문자로 시작하는 이름 + `N%` 또는 숫자)로 판정하고, 오류·안내 줄은
무시한다.

### 그 밖에 바꾼 곳

- `mask_secrets`: `ssh://git@host`처럼 비밀번호 없는 ssh 계정 이름은 가리지 않는다. 가리면 작업 로그에서
  어느 원격에 붙었는지 알 수 없다. `ssh://git:secret@host`와 http(s)의 사용자 정보는 그대로 가린다.
- `process_output_sink`: `on_record`는 순수 가상이 아니다. `on_record`·`on_chunk`가 모두 기본 구현을
  가져, 한 모드만 쓰는 sink는 다른 쪽을 구현하지 않는다.
- `diagnostic`: 프로세스 실행과 git 도구 확인에 쓰는 코드만 남기고 `diagnostic_source`를 뺐다.
  코드는 쓰는 곳이 생길 때 더한다.

## 결과

- 다른 작업도 요청 값(`git_request_options`)만 바꿔 `commit -F -`, `apply --cached`, 스트리밍 `log -z`를
  같은 실행기로 돌린다.
- 인증 도우미([ADR-004](ADR-004-askpass.md))는 정책의 `askpass_helper` 경로만 채우면 된다.
- `TerminateJobObject`로 끝낸 git이 `index.lock`을 남길 수 있는 문제(부드러운 취소)는 다루지 않는다.
