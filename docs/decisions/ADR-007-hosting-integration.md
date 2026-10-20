# ADR-007: GitHub·GitLab 연동은 REST API와 자격 증명 도우미로 하고 토큰은 메모리에만 둔다

## 상태

승인됨 - 2026-10-20

## 배경

cyella는 GitHub·GitLab의 PR·MR 목록과 만들기, CI 상태, PR 체크아웃을 앱에 붙인다. 원격 서비스에 닿는 기능이라 두 가지를 먼저 정해야 한다.
토큰을 어디서 어떻게 받고 어디까지 쥘 것인가, 그리고 어떤 API를 어떤 범위로 쓸 것인가다. [ADR-004](ADR-004-askpass.md)에서
"cyella는 비밀을 저장하지 않는다"고 했으므로 같은 원칙을 이어 간다.

## 결정

### 토큰 정책

- **토큰을 받는 길은 `git credential fill` 하나다.** `protocol=<http|https>`와 `host=<호스트[:포트]>`만 주고 사용자가 이미 쓰는 자격 증명
  도우미(Git Credential Manager 등)가 답하게 한다. 도우미가 창을 띄우면 그것은 사용자 자신의 도우미이며 기대한 동작이다. 경로(`path`)는
  주지 않는다.
- **로그인은 명시적이다.** 사이드바 PULL REQUESTS의 "로그인…" 행을 눌렀을 때만 도우미를 부른다. 시작할 때, 탭을 열 때, 커밋을 고를 때,
  PR을 만들거나 CI를 읽을 때는 로그인하지 않는다(로그인하지 않았으면 요청을 보내지 않고 안내한다). 로그인하지 않은 호스트에는 어떤 API 요청도
  보내지 않는다(익명으로도 읽을 수 있는 공개 저장소도 마찬가지다).
- **메모리에만 둔다.** 호스트마다 `hosting_token_store`가 `secret_text`(복사할 수 없고 사라질 때 내용을 지운다)로 쥔다. 설정 파일, 작업 로그,
  `commands[]`, 진단, 요청의 URL·헤더 표·몸에는 토큰이 없다. 전송이 `Authorization: Bearer`를 요청마다 따로 붙인다. 앱을 닫을 때
  (`hosting_bridge::stop`) 모두 지운다.
- **지우기는 최선 노력이다.** 도우미의 출력 버퍼, 요청 문자열, 지운 뒤의 `secret_text`는 지운다. luil HTTP client와 WinHTTP가 내부에 둔
  요청 사본, OS 파이프 버퍼의 잔여분은 앱이 책임질 수 없다(ADR-004와 같은 한계).
- **마스킹을 방어 계층으로 둔다.** 앱은 토큰을 명령줄에 싣지 않지만, `mask_secrets`가 `Authorization`·`PRIVATE-TOKEN` 헤더, `ghp_`·`gho_`·`glpat-` 접두어,
  URL의 사용자 정보에 더해 `git credential`의 `password=`·`oauth_refresh_token=` 줄을 가린다.
- **401은 한 번만 다시 묻는다.** 요청이 401이면 그 토큰을 `git credential reject`로 도우미에서 지우고 메모리에서도 잊은 뒤, 도우미에 한 번 다시
  물어 같은 요청을 한 번 다시 보낸다. 다시 받은 응답도 401이면 토큰을 잊고 "인증이 거절되었습니다"로 끝낸다. 로그인 확인(`/user`)이 실패한
  토큰(연결 실패, 취소 포함)은 로그인으로 치지 않는다.
- **로그아웃은 메모리 토큰만 잊는다.** 도우미의 저장본은 건드리지 않는다. 설정에서 호스트 줄을 지울 때도 그 호스트의 메모리 토큰만 잊는다.
- **터미널·askpass로 새지 않는다.** 도우미를 부르는 git은 `GIT_TERMINAL_PROMPT=0`을 걸고 `GIT_ASKPASS`·`SSH_ASKPASS`를 지운다. 도우미가 없으면
  터미널이나 cyella의 인증 대화 상자로 새지 않고 "토큰을 주지 않았습니다"로 바로 실패한다.

### API와 범위

- **GitHub REST v3**(`/repos/{o}/{r}/pulls`, `/commits/{sha}/status`, `/check-runs`, `/user`)와 **GitLab REST v4**
  (`/projects/{경로%2F부호화}/merge_requests`, `/pipelines?sha=`, `/user`)다. 별도 HTTP 라이브러리를 더하지 않고 luil의
  `luil::net::http_client`(WinHTTP)를 쓴다. 응답 JSON은 nlohmann으로 읽는다.
- **GraphQL을 쓰지 않는다.** 목록·생성·CI 상태에 REST로 충분하고, 두 서비스에 같은 모양의 값(`pull_request_info`, `ci_status_info`)으로 맞추기
  쉽다. 리뷰 상태(승인·변경 요청)는 PR마다 `/reviews`를 한 번씩 더 불러야 해서(N+1) 뺐다.
- **CI는 필요한 커밋만 읽는다.** 목록의 PR마다 읽지 않고 고른 커밋(HEAD 포함)과 열린 PR의 head만 읽는다. 결과는 저장소별 캐시이고 진행 중은
  20초, 확정은 5분, 상태 없음은 1분 뒤에 다시 읽는다. GitHub는 combined status와 check-runs를 합치고 GitLab은 가장 새 파이프라인이다.
- **쪽 매기기와 호출 한도.** GitHub는 `Link: rel="next"`, GitLab은 `X-Next-Page`로 "더 보기"를 판정한다. 호출 한도는 응답 헤더
  (`x-ratelimit-*`, `RateLimit-*`, `Retry-After`)에서 읽는다. GitHub의 403은 남은 횟수가 0이거나 `Retry-After`가 있거나 메시지가 한도를
  말할 때만 한도로 가르고(아니면 권한 오류), 429는 두 서비스 모두 한도다. 한도가 바닥났다고 알려 준 호스트는 초기화 시각까지 요청을 보내지 않고
  남은 횟수와 초기화까지의 시간을 보인다.
- **서비스 종류는 원격 URL로 정한다.** https·http·ssh(`ssh://git@host:port/…`, `git@host:owner/repo`)·git 꼴을 읽어 `github.com`·`gitlab.com`은 내장이고,
  그 밖의 호스트는 설정 `hosting.hosts[{host, provider, api_base}]`에 더해야 한다(GHES·자체 GitLab·시험 서버). 표가 내장 호스트보다 먼저다.
  GitLab 하위 그룹(`g/s/repo`)을 읽는다.
- **PR 체크아웃은 기존 operation이다.** `fetch_request{remote, refspec}`(`pull/<n>/head:pr/<n>`, GitLab은 `merge-requests/<n>/head:mr/<n>`)에
  `checkout_branch_request`를 이어서 낸다. refspec에 `+`를 붙이지 않아 로컬 `pr/<n>`에 사용자의 커밋이 있으면 덮어쓰지 않고 거절된다.
  PR 만들기는 필요하면 `push_execute`(upstream 설정)를 먼저 내고 그 operation이 성공해야 API를 부른다.
- **git operation이 아니다.** 프로세스를 띄우는 `operation_kind`와 달리 HTTP 응답을 기다리고 로그인 상태를 메모리에 두므로,
  `hosting_job`/`hosting_result_intent`(`application/hosting_messages.h`)로 오간다. 일감은 scheduler의 호스트별 lane에서 워커가 돌리고
  전송은 luil의 비동기 client 위에서 응답을 기다려 돌려준다. 공유 enum(`operation_kind` 등)에 종류를 더하지 않는다.

### 테스트 규칙

- 테스트는 실제 github.com·gitlab.com에 닿지 않는다. 가짜 GitHub·GitLab API 서버(`tests/fake_hosting_api.h`, 127.0.0.1)를 설정의 호스트 표로
  가리킨다. 자격 증명은 임시 파일로 갈아 끼운 git 설정(`GIT_CONFIG_GLOBAL`, `GIT_CONFIG_NOSYSTEM=1`)의 `credential.helper=store --file …`뿐이라
  사용자의 Git Credential Manager와 실제 자격 증명을 읽지 않는다.

## 결과

- 토큰이 디스크에 닿는 길이 없다. 사용자가 도우미를 쓰고 있다면 별도 설정 없이 로그인된다. 도우미가 없으면 로그인할 수 없다(토큰을 직접 입력받지 않는다).
- 앱을 다시 켜면 로그인은 다시 눌러야 한다. 사용자의 도우미가 저장해 둔 자격 증명이 있으면 창 없이 곧바로 로그인된다.
- 한 호스트에 계정 하나다(도우미가 호스트당 하나를 준다). 여러 계정, fork로의 PR(다른 저장소 head), 리뷰·코멘트, PR 병합·닫기는 지원하지 않는다.
- PR 본문은 한 줄 입력이다(luil 대화 상자에 여러 줄 칸이 없다. 인터랙티브 rebase 편집기의 여러 줄 칸을 일반화하면 바꿀 수 있다).
