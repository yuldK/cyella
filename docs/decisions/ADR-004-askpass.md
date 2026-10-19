# ADR-004: 인증은 askpass 도우미와 named pipe로 받는다

## 상태

승인됨 - 2026-10-20

## 배경

[ADR-001](ADR-001-git-backend.md)에 따라 git과 OpenSSH가 외부 프로세스로 돈다. 둘은 터미널 없이 돌아야
하므로 `GIT_TERMINAL_PROMPT=0`을 걸고, 비밀(passphrase, 비밀번호)이나 예/아니요 답이 필요할 때는
askpass 프로그램을 실행한다. 프롬프트 문구가 `argv[1]`, 답이 표준 출력, 종료 코드 0이 "답함", 0이 아니면
"취소"다. cyella가 그 프로그램(`cyella-askpass.exe`)을 내고, 질문을 실행 중인 앱에 넘겨 대화 상자로
받는다. **cyella는 비밀을 저장하지 않는다.**

## 결정

### 구성

- `cyella-askpass.exe`: WIN32 subsystem(콘솔 창 없음)의 작은 실행 파일이다. luil·Skia에 링크하지 않고
  프로토콜 코덱과 UTF-8 변환만 컴파일한다. 설치 규칙이 `cyella.exe` 옆(`bin`)에 둔다.
- 앱의 `askpass_server`(`platform/win32`)가 named pipe를 열고, 인터페이스(`askpass_service`)와 핸들러
  타입은 `application`에 있다. 질문은 `credential_prompt`로 분류되어 핸들러에 오고, 핸들러는
  logic thread에 게시한 뒤 `askpass_reply_callback`으로 나중에 답한다.
- git 요청은 `git_request_options`의 `askpass_helper`와 `askpass`(pipe·token·operation)로 환경 변수
  `GIT_ASKPASS`, `SSH_ASKPASS`, `SSH_ASKPASS_REQUIRE=force`, `CYELLA_ASKPASS_PIPE`,
  `CYELLA_ASKPASS_TOKEN`, `CYELLA_ASKPASS_OPERATION`을 받는다. 도우미가 없으면 이 변수들을 모두 지워
  사용자 환경의 값이 새지 않게 하며, 인증이 필요한 작업은 즉시 실패한다.

### pipe 보안 모델

- 이름은 `\\.\pipe\cyella-askpass-<pid>-<난수 64비트>`로 앱 실행마다 다르다.
- `PIPE_REJECT_REMOTE_CLIENTS`로 원격 접속을 막고, DACL은 현재 사용자 SID 한 개에만 접근을 준다
  (`D:P(A;;GA;;;<SID>)`). 첫 인스턴스에 `FILE_FLAG_FIRST_PIPE_INSTANCE`를 걸어 같은 이름을 먼저 차지한
  서버가 있으면 시작이 실패한다. 이어서 만드는 인스턴스에는 걸지 않는다. 다음 인스턴스를 연결 처리 전에
  먼저 만들어 이름이 비는 순간을 없앤다.
- 토큰은 `BCryptGenRandom`의 128비트 난수이고 앱 실행마다 새로 만든다. 요청마다 상수 시간으로 비교하며
  틀리면 핸들러를 부르지 않고 바로 취소로 답한다. 토큰은 자식 환경 변수에만 있고 명령줄과 작업 로그에는
  나오지 않는다. 같은 사용자의 다른 프로세스는 이 보호의 대상이 아니다(그 프로세스는 앱 메모리도 읽을
  수 있다).
- 도우미는 pipe를 `SECURITY_IDENTIFICATION` 수준으로 연다. 서버가 도우미의 토큰으로 가장(impersonate)해
  행동하지 못한다. 환경 변수의 pipe 이름이 `\\.\pipe\`로 시작하지 않으면 토큰을 보내지 않고 끝낸다.
- 프로토콜은 길이 접두어 이진 틀이다(요청 `CYA1`, 답 `CYR1`). 필드마다 상한(질문 64 KiB 등)이 있어 비정상
  길이는 읽기 전에 끊는다. 요청이 10초 안에 오지 않는 연결도 끊는다. 동시 연결은 64개까지다.
- 답에는 `answered` 상태가 따로 있다. 빈 답은 유효한 답이고(`ssh-keygen`의 "passphrase 없음") 취소와
  길이로 구분하지 않는다.

### 비밀을 다루는 방식

- 비밀은 pipe 틀과 도우미의 표준 출력 버퍼에만 거치고, 쓰고 나면 `SecureZeroMemory`로 지운다. JSON
  라이브러리를 쓰지 않는 이유 중 하나다(내부 복사본을 지울 수 없다). 지우기는 최선 노력이다. 핸들러가
  받은 문자열의 복사본, UI 입력 상자, OS 파이프 버퍼의 잔여분은 서버가 책임질 수 없다.
- 답은 로그에 남기지 않는다. 서버는 로그를 쓰지 않는다. 질문 문구는 비밀이 아니다.

### 프롬프트 분류

문구는 로캘에 따라 번역될 수 있어 영어 원문에 대한 최선 노력이다. 알아보지 못하면 `unknown`이고 UI는
원문을 보이고 자유 텍스트를 받는다. 분류 기준 문구는 OpenSSH 10.2(시스템과 Git for Windows 두 빌드)와 git 2.52의
것이다. 두 ssh의 호스트 키 문구는 지문 줄이 다르다(`fingerprint is SHA256:...` 마침표 / `fingerprint is:
SHA256:...`). 분류기는 둘 다 읽는다.

### 호스트 키 흐름

처음 보는 호스트는 `Are you sure you want to continue connecting`이 askpass로 온다. 대화 상자는 호스트,
주소, 키 종류, 지문을 보이고 답은 문자 그대로 `yes` 또는 `no`다. 호스트 키가 **바뀐** 경우
(`REMOTE HOST IDENTIFICATION HAS CHANGED`)는 askpass를 거치지 않고 ssh가 그냥 실패한다. 이는 stderr로
판정한다(`classify_ssh_connection_test`가 `host_key_changed`로 분류하고 git 작업의 실패 분류는 operation 쪽이 맡는다).

"이번만 연결"은 임시 `UserKnownHostsFile`로 구현한다. `make_temporary_known_hosts_ssh_command`가
`GIT_SSH_COMMAND` 값(`<ssh> -o UserKnownHostsFile=<임시> -o StrictHostKeyChecking=accept-new`)을 만든다.
이 값은 사용자의 `core.sshCommand`를 그 한 번의 실행에서 덮어쓰므로 호출자가 사용자가 쓰는 ssh 실행 파일을
넘겨야 한다. 사용자가 지문을 이미 확인했으므로 임시 파일에는 확인 없이 기록해도 되고, 사용자의
`known_hosts`는 바뀌지 않는다. 임시 파일은 호출자가 지운다.

### 키 만들기

passphrase를 `-N`으로 주면 명령줄이 작업 로그와 프로세스 목록에 남는다. OpenSSH 10.2의 `ssh-keygen`은
`-N`이 없으면 `SSH_ASKPASS`와 `SSH_ASKPASS_REQUIRE=force`로 askpass를 부른다(`Enter passphrase (empty for
no passphrase):`와 `Enter same passphrase again:`). 그래서 빌더는 `-N`을 쓰지 않고 앱의
대화 상자가 같은 서버로 답한다. 확인 프롬프트(`ssh_new_key_passphrase_confirm`)를 위해 UI는 첫 답을
그 operation 동안만 메모리에 두고 일치 여부를 확인하거나, 두 번 묻는다. 대상 파일이 이미 있으면
`ssh-keygen`이 덮어쓰기를 askpass가 아닌 곳으로 물어 실패하므로 요청을 만들지 않는다.

### 실패 방식

| 상황 | 도우미 | git·ssh에서 보이는 결과 |
| --- | --- | --- |
| 사용자 취소, 작업 취소, 앱 종료 | 종료 코드 1, 출력 없음 | 인증 실패로 끝남 |
| 토큰 불일치, 형식 오류, 상한 초과 | 종료 코드 1, 출력 없음 | 인증 실패로 끝남 |
| 환경 변수 누락, pipe 없음, 앱 죽음 | 종료 코드 1, 출력 없음 | 인증 실패로 끝남 |
| 핸들러가 예외를 던짐 | 취소 답 | 인증 실패로 끝남 |
| 도우미가 사라짐(자식 종료) | - | 서버가 연결 끊김을 알아채 자기 쪽 대기를 정리하고 이후의 늦은 답은 무시한다. 열려 있는 대화 상자는 서버가 닫지 못하므로 operation 취소가 닫아야 한다 |

- `cancel_operation(operation_id)`는 그 operation의 대기 질문 전부를 취소로 답하고, 같은 operation의 이후
  질문도 즉시 취소한다. git 프로세스를 죽여도 도우미는 별도 프로세스로 남아 대기할 수 있으므로 operation
  취소 시 반드시 부른다.
- `shutdown()`은 대기 질문을 취소로 답하고 listener와 연결 스레드를 멈춘다. 보통 수십 ms에 끝나며
  핸들러가 늦어도 연결 스레드를 2초까지만 기다린다(scheduler의 종료 예산 약 3초 안).
- `ssh-agent` 서비스 상태는 서비스 제어 관리자에서 조회한다. 이는 시스템 OpenSSH의 에이전트이며 Git for
  Windows의 ssh는 `SSH_AUTH_SOCK`으로 다른 에이전트를 쓸 수 있어 둘이 다를 수 있다.

## 결과

- UI는 `askpass_prompt_handler`만 채우면 된다. 분류된 종류가 대화 상자 종류를 정한다.
- 도우미 경로는 앱 실행 파일 옆에서 찾는다(`locate_askpass_helper`). 없으면 askpass 없이 실행하고 인증이
  필요한 작업은 즉시 실패한다.
- 같은 사용자 권한의 악성 프로세스는 막지 못한다는 한계는 의도한 것이다.
