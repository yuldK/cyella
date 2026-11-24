# ADR-013: macOS 판은 같은 코드 위에 OS 어댑터만 더하고, 경로 규칙은 domain이 OS에 맞춰 답한다

## 상태

승인됨 - 2026-11-25

## 배경

luil이 macOS(AppKit host, Metal 렌더러, WKWebView, FSEvents, 키체인 등)를 지원하기 시작했다. cyella의 화면·상태·git 작업은 luil 위의
플랫폼 중립 코드라 그대로 쓸 수 있지만, 두 가지가 Windows에 묶여 있었다.

- **OS 어댑터**: 프로세스 실행(CreateProcess·job object), askpass 서버(named pipe), 파일 감시(ReadDirectoryChangesW), 설정 파일(known folder),
  파일 대화 상자(COM), 도우미 프로그램, 진입점이 `src/platform/win32`에 Win32 API로 있다.
- **경로 규칙**: domain·application·infrastructure·presentation이 Win32 API는 부르지 않지만 Windows 경로의 모양을 전제했다. 절대 경로 판정은 드라이브와 UNC만
  받았고(`is_absolute_windows_path`), 저장소 경로는 `/`를 `\`로 바꿔 저장했으며, git·ssh·gpg는 `git.exe`·`Program Files`·Git for Windows의 `usr\bin`에서 찾았다.

## 결정

### 어댑터: 계약은 함께 쓰고 구현만 OS마다 둔다

- `src/platform/win32`의 헤더(`win32_process_runner.h`, `win32_askpass_server.h` 등)는 OS를 모르는 계약이다. macOS 구현은 `src/platform/macos`에서 **같은 헤더의
  같은 함수**를 정의하고, CMake가 OS마다 한쪽만 컴파일한다. 이름 공간도 `cyella::win32` 그대로다. luil이 macOS에서도 `luil::win32` 이름 공간과
  `luil/win32/*.h` 헤더를 그대로 쓰는 것과 같은 규칙이다. driver·relay·askpass 다리·호스팅 다리처럼 OS를 모르는 조립은 두 OS가 같은 파일을 쓴다.
- Windows 쪽 파일은 옮기거나 이름을 바꾸지 않았다. 바뀐 것은 Windows 전용 대상과 .rc 설정을 `src/platform/win32/win32_targets.cmake`로 떼어 낸 것뿐이다.

| 계약 | macOS 구현 |
| --- | --- |
| 프로세스 실행 | `posix_spawn`. `POSIX_SPAWN_CLOEXEC_DEFAULT`로 stdin·stdout·stderr 말고는 아무 기술자도 넘기지 않는다(상속 목록 자리). 자식은 새 프로세스 그룹의 우두머리이고 취소·시간 초과는 그룹 전체에 SIGKILL을 보낸다(job object 자리). 기다림은 kqueue 하나(`NOTE_EXIT`·사용자 사건·제한 시간)다. I/O thread는 `poll`로 자료와 깨우기 pipe를 함께 기다려, 손자가 쓰기 끝을 쥐고 남아도 유예 뒤에 끝난다 |
| askpass 서버 | 사용자 임시 폴더 아래 `mkdtemp`로 만든 0700 폴더의 Unix 소켓. 받은 연결은 `getpeereid`로 같은 사용자인지 본다. 토큰·프로토콜·연결마다 thread·취소 규칙은 Win32 판과 같다. 소켓 경로는 `CYELLA_ASKPASS_PIPE`로 간다 |
| 파일 감시 | 감시마다 FSEvents stream 하나(파일 단위 사건). 아래 "감시"를 본다 |
| 설정 파일 | 임시 파일 → `F_FULLFSYNC` → `rename`. 백업으로 옮기기는 `renamex_np(RENAME_EXCL)`로 덮어쓰지 않는다 |
| 위치 | 설정·창 배치는 `~/Library/Application Support/cyella`, 지워도 되는 보관 자료는 `~/Library/Caches/cyella` |
| 대화 상자·앞 창 | `NSOpenPanel`(모달), `NSApplicationDidBecomeActiveNotification` |
| diff 뷰어 | 번들의 `Contents/Resources/diff-viewer`를 WKWebView로 연다. luil이 WebView2와 같은 꼴의 `chrome.webview` 다리를 세운다 |
| 자원 | 문구 파일·앱 심볼·타이틀 SVG는 실행 파일에 바이트 배열로 싣는다(RCDATA 자리). test 실행 파일도 같은 문구를 읽는다 |

### 경로 규칙: domain의 `path_syntax`가 이 OS의 규칙을 답한다

- `is_absolute_path`, `native_path_separator`, `is_native_path_separator`, `native_path_separators`, `path_list_separator`, `executable_suffix`, `native_root_length`,
  `join_native_path`, `native_parent_path`를 `domain/path_syntax.h`에 둔다. Windows에서는 지금까지의 규칙과 **같은 값**을 내고(드라이브·UNC, `\`·`/` 둘 다 구분자,
  `;`, `.exe`), POSIX에서는 `/`만 구분자이고 `/`로 시작하는 경로가 절대 경로다(`\`는 파일 이름의 글자다).
- 경로를 받고 짓는 계층은 이 이름들만 쓰고 OS를 직접 묻지 않는다. `#if defined(_WIN32)`는 OS마다 기본값이 다른 몇 곳(git·ssh·gpg의 설치 자리, 다른 앱으로 열기의
  기본 항목, 시간대, `.cmd`·`.bat` 거절, Windows 경로를 `/` 꼴로 바꾸는 곳)에만 남긴다.
- 저장소 열쇠(`repository_path_key`·`normalize_path_key`)의 대소문자 접기는 두 OS가 같다. macOS 기본 볼륨(APFS)도 대소문자를 가리지 않는다.

### 화면 문구: 낱말만 덮어쓴다

Windows의 낱말(탐색기, `cyella-askpass.exe`, Git for Windows, `C:\` 예시, PowerShell 안내)이 든 문구는 `assets/locales/macos/*.json`이 덮어쓴다. 덮어쓰기는
원본에 있는 키만 바꾸고 자리표시자가 같아야 한다(`macos_platform_tests.cpp`가 본다). 원본 문구 파일과 키 검사(ADR-008)는 그대로다.

### 감시: FSEvents의 늦은 배달과 쌓인 플래그

- FSEvents는 시작 직전의 변경을 "지금 이후"의 사건으로 늦게 배달한다. 시작할 때 밀린 사건을 받아 버리고(`FSEventStreamFlushSync`), 시작 뒤 5초 동안은 사건의
  파일이 아직 있고 그 ctime이 시작보다 이르면 버린다. 지워진 항목은 시각을 알 수 없어 늘 알린다.
- 한 경로의 플래그는 쌓여서 온다(예전의 만듦이 남는다). 그래서 상태 영역(`refs/heads` 등) 폴더 자신의 사건은 늘 버리고, 그 안의 파일 사건으로 판단한다.
- FSEvents는 사건을 합칠 뿐 넘치지 않는다. 놓쳤다고 알리면(`MustScanSubDirs`·`UserDropped`·`KernelDropped`) Win32 판과 같이 두 종류를 모두 알린다.

### clang에서 드러난 것

- **nlohmann/json을 json에서 중괄호로 초기화하지 않는다.** `json x { json::parse(...) }`와 `json x { json::array() }`는 MSVC에서 복사지만 clang에서는 원소 하나짜리
  배열이 된다. diff 뷰어의 메시지가 객체가 아니라 배열로 읽혀 뷰어가 비었고, 설정 파일도 `[[]]`로 쓸 뻔했다. 값 초기화는 `json x = ...;`로 쓴다(참조 바인딩
  `const json& x { ... }`와 객체 리터럴 `json x { { "a", 1 } }`은 그대로다).
- macOS 11의 libc++에는 tzdb(`std::chrono::current_zone`)와 실수 `to_chars`가 없어, 시각 글은 `localtime_r`로 짓는다.
- 경고는 MSVC와 같은 무게(`-Wall -Wextra -Werror`)다. 그 김에 쓰지 않는 함수·switch의 빠진 갈래를 정리했다.

### 빌드와 배포

- preset은 `macos-arm64`·`macos-arm64-tests`(Ninja Multi-Config)다. 아키텍처와 배포 하한은 luil의 Skia 패키지와 같은 arm64·11.0이다.
- 앱은 번들 `Cyella.app`(식별자 `io.github.yuldk.cyella`)이다. 도우미는 코드라 `Contents/MacOS`에, 뷰어·고지·아이콘은 `Contents/Resources`에 놓고, 도우미를 먼저
  ad-hoc 서명한 뒤 번들을 서명한다. Developer ID 서명·공증·배포 꾸러미(dmg)는 아직 없다.

## 결과

- 두 OS가 화면·상태·git 작업·인증·감시 규칙을 한 벌로 나눈다. OS 어댑터를 고치면 두 판을 같은 계약의 test(`process_runner_tests`, `askpass_server_tests`,
  `win32_repository_watcher_tests` 등)로 함께 본다.
- 시험은 대부분 두 OS에서 같이 돈다. 시험이 Windows 꼴로 적은 경로는 `tests/native_path_support.h`의 `native_path`가 이 OS의 꼴로 옮긴다. 드라이브·UNC·`.exe`·
  Git for Windows의 배치·`cmd.exe`처럼 Windows 규칙 자체를 보는 시험은 `#if defined(_WIN32)`로 Windows에서만 돌고, macOS의 같은 계약은
  `tests/macos_platform_tests.cpp`가 본다.
- 한계: 번들은 배포용으로 서명·공증되지 않았다. 파일 대화 상자는 모달이다(sheet가 아니다). Windows 전용 기능(작업 표시줄 진행률, WebView2 Runtime 안내,
  ssh-agent 서비스 시작 안내)은 macOS에서 해당 자리가 없거나 macOS의 낱말로 바뀐다.
