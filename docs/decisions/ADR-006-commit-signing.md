# ADR-006: 커밋 서명은 git에 맡기고 앱은 읽기·설정·실패 안내만 한다

## 상태

승인됨 - 2026-10-20

## 배경

cyella는 GPG와 SSH로 커밋·태그에 서명하고 서명 상태를 보인다. [ADR-001](ADR-001-git-backend.md)에 따라 서명도 git과 그것이 부르는 외부 도구(`gpg`, `ssh-keygen`)가 한다.
passphrase는 [ADR-004](ADR-004-askpass.md)처럼 저장하지 않는다. 이 문서는 정한 것과, 그 근거가 된 도구의 동작을 적는다.

## 결정

### 읽기

- 서명 상태는 `%G?`·`%GS`·`%GK`다. 서명된 커밋마다 gpg·ssh-keygen이 한 번씩 돌아 10만 커밋 그래프 스트림에는 넣지 않는다. 별도 읽기 operation `verify_signatures{shas[]}`가
  `log --no-walk=unsorted --stdin -z`로 읽고, 화면은 **고른 커밋**과 **그래프에 보이는 행 둘레(앞 8행·뒤 24행(한 번에 40행까지))** 만 자동 operation으로 요청한다. 세션이 SHA별로 보관해 한 번만 읽는다.
  SHA는 stdin으로 준다(명령줄 길이, 옵션 주입).
- **ssh 서명의 allowed signers 함정**: `gpg.ssh.allowedSignersFile`이 설정에 없으면 git은 ssh 서명을 **서명 없음(`N`)** 으로 보고한다(stderr에만 안내가 있다). 서명 없는 커밋과 구별할 수 없다.
  설정이 없을 때 검증 명령에 `-c gpg.ssh.allowedSignersFile=`(빈 값)을 붙이면 서명이 읽혀 `U`와 키 지문이 나온다. 이때와 설정한 파일이 없을 때, 서명자가 비고 키가 `SHA256:`인 `U`는
  `cannot_check` + `allowed_signers_missing`으로 바꾼다("확인할 수 없음 (allowed signers 없음)"). 파일이 있는데 목록에 없는 서명자는 그냥 `unknown_key`다.
- 서명 설정(`commit.gpgsign` 등 일곱)은 `git config -z --show-scope --get-regexp` 한 번으로 실효 값과 이 저장소·전역 값을 함께 읽는다. refresh가 실효 값을 snapshot에 담아
  커밋 상자와 태그 상자의 서명 기본값이 된다.

### 쓰기와 설정

- 커밋·태그 요청은 `sign`을 tri-state로 가진다(`-S` / `--no-gpg-sign` / 설정을 따름). 서명 키는 항상 `user.signingkey`다. 화면은 토글이 기본값(설정)과 같으면 값을 싣지 않는다.
  서명하는 요청은 passphrase를 기다리므로 시간 제한 없이 돌고 취소만 받는다.
- 서명 설정을 바꾸는 일은 환경 설정의 worker(`signing_info`)가 `git config --local|--global`로 한다. 저장소 operation으로 만들지 않았다: 저장소가 없어도 전역 설정을 보아야 하고, 설정 몇 줄을
  쓰는 일이라 작업 로그에 남길 일이 아니다. **기본은 이 저장소(`--local`)이고 전역은 사용자가 "전역"을 골라야 하며 쓰기 전에 무엇을 바꾸는지 적은 확인을 받는다.**
- 서명 키 후보: ssh는 SSH 화면이 읽은 `~/.ssh`의 공개 키(`.pub` 경로를 `user.signingkey`에), openpgp는 `gpg --list-secret-keys --with-colons`의 비밀 키(지문). 만료·폐기된 키는 고르지 못하게 한다.

### 실패 분류

- 새 종류 `signing_failed`·`signing_key_missing`·`signing_passphrase_failed`. gpg는 git이 `--status-fd=2`로 불러 `[GNUPG:] FAILURE sign <코드>`가 오고 아래 16비트가 gpg-error 코드(11 BAD_PASSPHRASE,
  17 NO_SECKEY, 85 NO_PINENTRY, 99 CANCELED)다. 번역에 기대지 않는다. ssh는 문구뿐이고 `failed to write commit object`만으로는 디스크 오류와 구별되지 않아 서명 도구가 낸 오류가 함께 있을 때만 분류한다.
- 요약은 gpg 쪽(gpg-agent·pinentry·`gpg.program`)과 ssh 쪽(passphrase·`gpg.ssh.program`)을 가려 한국어로 알린다.

## 도구의 동작

Git for Windows 2.52(gpg 2.4.8)와 OpenSSH 10.2(Git 쪽과 시스템 두 빌드)의 동작이다.

1. **ssh passphrase는 askpass로 온다.** `git commit -S`가 부르는 `ssh-keygen -Y sign`은 `SSH_ASKPASS_REQUIRE=force`면 `Enter passphrase for "<키 경로>": `(큰따옴표)로 askpass를 부른다. 기존 분류기가
   이미 읽는다(`ssh_key_passphrase`). 앱의 `commit`은 askpass 세션을 받으므로 `needs_input{passphrase}`가 되어 인증 대화 상자가 받는다 (통합 test가 다룬다).
2. **gpg의 pinentry는 GUI다.** Git for Windows에는 `pinentry-w32.exe`(= `pinentry.exe`)뿐이다. 콘솔 없는 앱에서 불러도 창이 화면에 뜰 것으로 본다(`pinentry --version`이 `pinentry-w32`다). gpg-agent에 passphrase가 캐시되어 있지 않고 pinentry를 쓸 수
   없는 환경이면 `gpg: signing failed: No pinentry`(`FAILURE sign 67108949`)로 실패한다. 취소는 `Operation cancelled`(…963), 틀린 passphrase는 `Bad passphrase`(…875)다. **앱은 gpg의 passphrase를 받지 않는다.**
   실패 문구에 gpg-agent·pinentry 설정을 확인하라고 안내한다. 테스트는 화면에 창을 띄우지 않도록 pinentry 없는 gpg 래퍼(`--pinentry-mode error|cancel`)로 실패 분류만 다룬다.
3. **`GNUPGHOME`은 msys 경로여야 한다.** Git for Windows의 gpg는 msys 프로그램이라 `C:\...`·`C:/...`를 상대 경로로 읽는다. `/c/...` 꼴로 줘야 하고(bash에서 만든 값은 mingw git을 지나며
   `C:/`로 바뀌므로 `MSYS2_ENV_CONV_EXCL=GNUPGHOME`), gpg-agent의 유닉스 소켓 경로가 108자를 넘으면 `error running '/usr/bin/gpg-agent': exit status 2`로 못 뜬다. 테스트의 임시 홈은 짧게 둔다.
4. **gpg-agent가 앱의 job object에 묶인다 (남은 일).** 앱의 process runner는 git 명령마다 job object(kill-on-close)에 묶는다. gpg가 자동으로 띄우는 `gpg-agent`도 그 안에서 시작되어 명령이 끝나면 함께
   끝난다 (그 뒤 `gpg-connect-agent --no-autostart`는 "no gpg-agent running"을 낸다). 사용자의 agent가 따로 떠 있으면(터미널에서 쓰던 agent, Kleopatra) 그것을 쓰므로 캐시가 유지된다. 떠 있지 않으면
   passphrase가 걸린 키는 서명할 때마다 pinentry가 다시 묻는다. 고치려면 서명 전에 agent를 job 밖에서 시작해 두어야 한다.
5. **`git config -z --show-scope`의 모양**은 `scope\0key\nvalue\0`이고 `=`가 없는 불리언은 `scope\0key\0`다. `--type=path`로 `~/`를 펼친 allowed signers 경로를 받는다.
6. **윈도우 ssh-keygen의 권한 검사**: Git for Windows의 `ssh-keygen`이 만든 키를 시스템 OpenSSH의 `ssh-keygen`이 "UNPROTECTED PRIVATE KEY FILE"로 거절한다. 같은 쪽 도구로 만들고 `gpg.ssh.program`을
   그 도구로 정하면 된다 (테스트는 그렇게 한다).

## 결과

- 서명 검증은 요청한 커밋만 읽고, 이미 읽은 커밋은 다시 읽지 않는다. 실패한 검증은 다시 요청하지 않는다.
- 사용자의 전역 git 설정·`~/.gnupg`·`~/.ssh`·ssh-agent는 테스트가 건드리지 않는다. 통합 테스트는 `GIT_CONFIG_GLOBAL`·`GNUPGHOME`·키를 임시로 만들고 gpg-agent를 끈다 (`tests/signing_test_support.h`).
