<p align="center">
  <img src="assets/cyella-symbol.png" alt="cyella" width="128">
</p>

# cyella

cyella는 Windows용 Git 클라이언트다. 여러 저장소를 탭으로 열고, 커밋 그래프를 보며 브랜치·태그·rebase를
끌어 놓기와 우클릭 메뉴로 다룬다. C++20과 [luil](https://github.com/yuldK/luil)(Skia·Win32)로 화면을 그리고,
Git 작업은 설치된 git CLI를 외부 프로세스로 실행한다.

## 기능

- **탭과 시작 페이지**: 여러 저장소를 탭으로 동시에 연다. 최근·고정 저장소, 열기·Clone·Init을 시작 페이지에서 한다.
- **커밋 그래프**: 레인을 증분으로 배치하고 가상 스크롤로 큰 이력도 바로 연다. 브랜치·태그·HEAD 라벨, 서명·CI 표시가 붙는다.
- **변경 검토와 커밋**: 파일·hunk·줄 단위로 stage·unstage·discard한다. diff는 WebView2 뷰어로 inline·분할 보기,
  단어 강조, 구문 강조, 이미지 비교를 지원하고, 확장자별 외부 diff 도구로도 연다.
- **브랜치와 이력 조작**: 라벨을 끌어 놓아 merge·rebase·fast-forward·reset을 고른다. 체크아웃, 태그 만들기와 push,
  cherry-pick, revert, stash, 충돌 해결 화면이 있다.
- **인터랙티브 rebase 편집기**: pick·reword·squash·fixup·drop과 순서를 앱에서 고치고 실행한다.
- **SSH와 인증**: passphrase·호스트 키·HTTPS 자격 증명 질문을 앱 대화 상자로 받는다. 키 생성과 연결 시험을 환경 설정에서 한다.
- **submodule·worktree·Git LFS**: 사이드바 구역에서 상태를 보고 기본 작업을 실행한다.
- **커밋 서명**: GPG·SSH 서명 상태를 그래프와 상세 패널에 보이고, 서명 커밋·태그를 만든다.
- **GitHub·GitLab 연동**: PR·MR 목록과 상세, PR 만들기, 커밋의 CI 상태를 본다. 자체 호스팅 서버도 등록할 수 있다.
- **명령 팔레트와 단축키**: `Ctrl+Shift+P`(또는 `Ctrl+P`)로 연 팔레트에서 명령·저장소·브랜치를 찾는다. 한글 초성 검색을 지원하고
  단축키를 바꿀 수 있다.
- **저장소별 사용자 프로필**: 작성자 이름·메일·서명 키를 프로필로 두고 규칙에 맞는 저장소에 제안한다.
- **화면 설정**: 밝은·어두운·시스템 테마, UI·코드 글꼴, 창 폭에 따라 바뀌는 배치.

## 개인 정보와 자격 증명

- 설정은 `%APPDATA%\cyella\settings.json`에 저장한다.
- 비밀번호·passphrase·호스팅 토큰은 파일에 쓰지 않는다. 토큰은 `git credential`(Git Credential Manager 등)에서 받아
  실행 중에만 메모리에 둔다.
- 작성자 사진(Gravatar)은 기본으로 꺼져 있다. 켜면 작성자 메일의 SHA-256 해시가 gravatar.com으로 간다.

## 실행 환경

- Windows 10·11 x64
- [Git for Windows](https://gitforwindows.org/) 2.43 이상
- [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (diff 뷰어. 없으면 텍스트 목록으로 보인다)
- 선택: Git LFS, OpenSSH, GnuPG, Git Credential Manager

## 빌드

필요한 것:

- Visual Studio 2022 또는 2026, MSVC 19.40 이상
- Windows SDK 10.0.22621.0 이상
- CMake 4.2 이상과 PowerShell

저장소 루트에서 실행한다. Visual Studio 2022를 쓰면 `vs2026` 대신 대응하는 `vs2022` preset을 고른다.

```powershell
git clone https://github.com/yuldK/cyella.git
cd cyella
git submodule update --init
git -C third_party/luil submodule update --init third_party/nlohmann_json
third_party\luil\scripts\fetch_skia.ps1 -Configuration Debug,Release
third_party\luil\scripts\fetch_webview2.ps1
cmake --preset vs2026
cmake --build --preset vs2026-release
cmake --install build\vs2026 --config Release
```

설치하면 `bin\`에 `cyella.exe`, 인증 도우미 `cyella-askpass.exe`, rebase 편집기 도우미 `cyella-rebase-editor.exe`와
`diff-viewer\`가 놓인다. CMake configure는 의존성을 내려받지 않는다. Skia와 WebView2 SDK는 위의 luil 스크립트가 준비한다.

## 테스트

```powershell
cmake --preset vs2026-tests
cmake --build --preset vs2026-tests-debug
ctest --preset vs2026-tests-debug
```

테스트 구성에는 clang-format 검사(`cyella_format_check`)와 소스 스타일 검사(`scripts\check_source_style.ps1`)가 함께 들어 있다.
통합 테스트는 임시 저장소와 실제 git을 쓰고, git-lfs·gpg·ssh-keygen이 없으면 해당 테스트를 건너뛴다.
Node.js가 있으면 diff 뷰어의 JavaScript 테스트도 CTest에 함께 등록된다.

## 구조

| 경로 | 내용 |
| --- | --- |
| `src/domain` | 값 타입과 순수 규칙. 외부 의존이 없다 |
| `src/application` | 저장소 세션, operation 정의와 실행 흐름, 설정 서비스 |
| `src/infrastructure` | 프로세스 실행, git 명령 조립과 출력 해석, 호스팅 API, 설정 파일 |
| `src/presentation` | 화면 상태·snapshot과 luil element tree |
| `src/platform/win32` | 진입점, 창과 웹뷰 연결, askpass·rebase 편집기 도우미 |
| `assets/diff-viewer` | WebView2 diff 뷰어(HTML·JS·CSS) |
| `tests` | Catch2 단위·통합 테스트와 고정 git 출력 |

설계 문서는 [docs/ui-architecture.md](docs/ui-architecture.md), [docs/app-state-design.md](docs/app-state-design.md),
[docs/decisions](docs/decisions)의 ADR에 있다.

## 라이선스

cyella의 소스와 문서는 [MIT 라이선스](LICENSE.md)를 따른다.

포함하거나 링크하는 제3자 구성 요소에는 각 원본의 라이선스가 적용된다. 실행 파일에는 빌드할 때 모은 고지가 함께 들어간다.

| 구성 요소 | 라이선스 |
| --- | --- |
| [luil](https://github.com/yuldK/luil) | MIT |
| [Skia](https://skia.org/) | BSD-3-Clause |
| [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) | BSD-3-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| [highlight.js](https://highlightjs.org/) | BSD-3-Clause |
| [Codicons](https://github.com/microsoft/vscode-codicons) | CC BY 4.0 (글꼴), MIT (코드) |
| [Catch2](https://github.com/catchorg/Catch2) (테스트만) | BSL-1.0 |
