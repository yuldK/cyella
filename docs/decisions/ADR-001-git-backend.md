# ADR-001: Git 작업은 git CLI로 실행한다

## 상태

승인됨 - 2026-10-19

## 배경

Git 클라이언트의 백엔드는 두 가지가 가능하다.

- **git CLI**: 설치된 `git.exe`를 외부 프로세스로 실행하고 출력을 해석한다.
- **libgit2**: 라이브러리를 링크해 저장소를 직접 읽고 쓴다.

cyella는 SSH 원격, 태그 push, rebase를 지원해야 한다.
libgit2로 SSH를 하려면 libssh2를 함께 빌드하고 인증 콜백을 직접 구현해야 한다. 그래도 hook,
일부 설정(`includeIf`, `core.sshCommand`), LFS 필터, credential helper가 CLI와 똑같이 동작하지 않는다.
rebase·merge의 충돌 처리도 CLI보다 기능이 적다.

같은 luil 기반 프로젝트인 gitman에 git CLI를 비대화형 외부 프로세스로 실행하는 구조(process runner,
작업 스케줄러, 취소 등록, 스레드 메시지)가 이미 있어 가져다 쓸 수 있다.

## 결정

1. **모든 Git 작업은 git CLI로 실행한다.** 쓰기·네트워크 작업은 물론 읽기도 CLI로 한다.
2. 출력은 기계가 읽는 형식으로 받는다.
   - 상태: `git status --porcelain=v2 --branch -z`
   - 그래프: `git log --topo-order --parents -z --format=<필드를 0x1F로 구분>`을 스트리밍으로
     읽어 점진적으로 레인을 계산한다.
   - 참조: `git for-each-ref --format=...`
   - diff: `git diff --no-color --no-ext-diff -U<n>`(내장 뷰어용). textconv는 기본값을 따른다.
3. 프로세스는 항상 비대화형으로 실행한다. `GIT_TERMINAL_PROMPT=0`을 설정하고, 인증 프롬프트는
   `GIT_ASKPASS`·`SSH_ASKPASS`(`SSH_ASKPASS_REQUIRE=force`)를 cyella의 askpass 도우미로 돌려
   앱 안의 대화 상자로 받는다([ADR-004](ADR-004-askpass.md)).
4. 진행률은 `--progress`의 stderr 출력을 해석해 작업 진행 메시지로 보낸다.
5. 한 저장소의 쓰기 작업(index·ref를 바꾸는 것)은 저장소마다 하나의 직렬 큐에서 실행한다.
   읽기 작업은 병렬로 돌고, 쓰기가 끝나면 관련 읽기 결과를 무효화한다.
6. 지원하는 git의 최소 버전은 2.43이다. `PATH`에서 찾고, 설정에서 절대 경로를 지정할 수 있다.

## 결과

- SSH, credential helper, hook, LFS, `includeIf`가 사용자의 터미널과 똑같이 동작한다.
- 프로세스를 띄우는 비용이 있다. 그래프는 한 번의 스트리밍 `git log`로 읽고, 상태 갱신은
  파일 감시 이벤트를 모아(debounce) 횟수를 줄인다.
- git 출력 형식이 바뀌는 데 대비해, 해석기마다 실제 git으로 만든 고정 출력으로 단위 테스트를 둔다.
- git 설치가 필수 조건이 된다. 시작할 때 버전을 확인하고, 없으면 안내 화면을 보인다.
