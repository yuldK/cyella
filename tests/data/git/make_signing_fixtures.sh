#!/usr/bin/env bash
# 커밋 서명 해석기의 고정 출력을 실제 git·gpg·ssh-keygen으로 다시 만든다.
#
#   tests/data/git/make_signing_fixtures.sh <scratch_dir> <out_dir>
#
# 사용자의 git 설정·~/.gnupg·~/.ssh·ssh-agent를 건드리지 않는다. 전역 설정은 임시 파일, GNUPGHOME은 임시 폴더, 키는 임시 폴더에
# 새로 만든다. gpg-agent의 유닉스 소켓 경로가 108자를 넘으면 시작하지 못하므로 <scratch_dir>은 짧아야 한다 (예: /c/Users/me/AppData/Local/Temp/sgn).
# 끝나면 이 스크립트가 띄운 gpg-agent를 끈다.
set -euo pipefail

scratch="${1:?scratch dir}"
out="${2:?out dir}"
mkdir -p "$out"
rm -rf "$scratch"
mkdir -p "$scratch/home"
chmod 700 "$scratch/home"

export LC_ALL=C
# bash가 환경 변수의 /c/... 경로를 git(mingw)에 넘기며 C:/...로 바꾸면 msys gpg가 상대 경로로 읽는다.
export MSYS2_ENV_CONV_EXCL=GNUPGHOME
export GNUPGHOME="$scratch/home"
export GIT_CONFIG_GLOBAL="$scratch/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME="Ann Author"
export GIT_AUTHOR_EMAIL="ann@example.com"
export GIT_COMMITTER_NAME="Ann Author"
export GIT_COMMITTER_EMAIL="ann@example.com"
export GIT_AUTHOR_DATE="2026-04-01T10:00:00 +0900"
export GIT_COMMITTER_DATE="2026-04-01T10:00:00 +0900"
: > "$GIT_CONFIG_GLOBAL"

cleanup()
{
    gpgconf --kill gpg-agent > /dev/null 2>&1 || true
}
trap cleanup EXIT

root="$(cd "$scratch" && pwd -W 2>/dev/null || pwd)"
scrub()
{
    # ssh 쪽 문구는 경로 구분자를 역슬래시 하나(`Enter passphrase for "C:\Users\..."`) 또는 둘(`Load key "C:\\Users\\..."`)로 쓴다.
    local one="${root//\//\\\\}"
    local two="${root//\//\\\\\\\\}"
    sed -i -e "s|$scratch|/scratch|g" -e "s|$root|/scratch|g" -e "s|$one|/scratch|g" -e "s|$two|/scratch|g" "$@"
}

# ---------------------------------------------------------------- ssh 키와 저장소
ssh-keygen -q -t ed25519 -N "" -C "ann@example.com" -f "$scratch/key_ann"
ssh-keygen -q -t ed25519 -N "" -C "other@example.com" -f "$scratch/key_other"
echo "ann@example.com $(cut -d' ' -f1,2 "$scratch/key_ann.pub")" > "$scratch/allowed_ann"
echo "other@example.com $(cut -d' ' -f1,2 "$scratch/key_other.pub")" > "$scratch/allowed_other"

git init -q -b main "$scratch/repo"
cd "$scratch/repo"
git config gpg.format ssh
git config user.signingkey "$scratch/key_ann.pub"
git config gpg.ssh.allowedSignersFile "$scratch/allowed_ann"

echo 1 > f && git add f
git -c gpg.format=openpgp commit -q -m "unsigned"
unsigned="$(git rev-parse HEAD)"
echo 2 > f && git add f
git commit -q -S -m "signed by ann"
good="$(git rev-parse HEAD)"
echo 3 > f && git add f
git -c user.signingkey="$scratch/key_other.pub" commit -q -S -m "signed by other"
other="$(git rev-parse HEAD)"

# 서명된 커밋의 내용을 바꾼 것: 같은 서명 머리에 다른 제목을 붙인다.
git cat-file commit "$good" | sed 's/^signed by ann$/tampered/' > "$scratch/tampered.txt"
tampered="$(git hash-object -t commit -w "$scratch/tampered.txt")"

format='%H%x1f%G?%x1f%GS%x1f%GK'
shas="$tampered
$other
$good
$unsigned"

# 서명자 목록이 있는 정상 설정: G(ann), B, U(other는 목록에 없다), N.
printf '%s\n' "$shas" | git log --no-walk=unsorted --stdin -z "--format=$format" > "$out/signing_log_ssh.bin" 2> /dev/null
# 목록이 없는 설정(빈 값으로 덮어 읽는다): 서명된 커밋이 모두 U + 지문이다.
printf '%s\n' "$shas" | git -c gpg.ssh.allowedSignersFile= log --no-walk=unsorted --stdin -z "--format=$format" > "$out/signing_log_ssh_blank.bin" 2> /dev/null
# 같은 설정에서 덮어쓰지 않은 채 목록이 없을 때: 서명된 커밋이 N으로 보이는 함정이다 (이 값이 빈 값 덮어쓰기가 필요한 이유).
git config --unset gpg.ssh.allowedSignersFile
printf '%s\n' "$shas" | git log --no-walk=unsorted --stdin -z "--format=$format" > "$out/signing_log_ssh_unset.bin" 2> "$out/signing_log_ssh_unset_stderr.bin" || true
git config gpg.ssh.allowedSignersFile "$scratch/allowed_ann"

# ---------------------------------------------------------------- 설정 모음
cat > "$GIT_CONFIG_GLOBAL" <<EOF
[user]
	name = Ann Author
[commit]
	gpgsign = true
[tag]
	gpgsign
[gpg]
	program = C:/Program Files/Git/usr/bin/gpg.exe
EOF
git config --local gpg.format ssh
git config --local commit.gpgsign false
git config --local gpg.ssh.program "C:/Windows/System32/OpenSSH/ssh-keygen.exe"
git config -z --show-scope --get-regexp '^(commit\.gpgsign|tag\.gpgsign|gpg\.format|user\.signingkey|gpg\.program|gpg\.ssh\.program|gpg\.ssh\.allowedsignersfile)$' > "$out/signing_config_scopes.bin"
: > "$GIT_CONFIG_GLOBAL"
scrub "$out/signing_config_scopes.bin"

# ---------------------------------------------------------------- 서명 실패의 stderr
cd "$scratch/repo"
git config --unset gpg.ssh.program || true
git config --unset commit.gpgsign || true
echo 4 > f && git add f
git config user.signingkey "$scratch/nowhere/key.pub"
git commit -S -m x > /dev/null 2> "$out/err_sign_ssh_missing_key.bin" || true
git config gpg.format ssh
git config user.signingkey "$scratch/key_pw.pub"
ssh-keygen -q -t ed25519 -N "secret1" -C "pw@example.com" -f "$scratch/key_pw"
GIT_TERMINAL_PROMPT=0 git commit -S -m x < /dev/null > /dev/null 2> "$out/err_sign_ssh_passphrase.bin" || true
git config gpg.ssh.program "$scratch/nowhere/ssh-keygen.exe"
git config user.signingkey "$scratch/key_ann.pub"
git commit -S -m x > /dev/null 2> "$out/err_sign_ssh_spawn.bin" || true
git config --unset gpg.ssh.program

git config gpg.format openpgp
git config user.signingkey nobody@example.com
git commit -S -m x > /dev/null 2> "$out/err_sign_gpg_no_secret_key.bin" || true
git config gpg.program "$scratch/nowhere/gpg.exe"
git commit -S -m x > /dev/null 2> "$out/err_sign_gpg_spawn.bin" || true
git config --unset gpg.program

# ---------------------------------------------------------------- gpg 키와 서명 실패
gpg --batch --passphrase '' --quick-gen-key "Ann Author <ann@example.com>" ed25519 sign never > /dev/null 2>&1
fpr="$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}')"
gpg --batch --passphrase '' --quick-add-uid "$fpr" "Ann Work <ann@work.example>" > /dev/null 2>&1
gpg --batch --passphrase '' --quick-add-key "$fpr" cv25519 encr never > /dev/null 2>&1
gpg --batch --passphrase '' --quick-gen-key "Rsa Keeper <rsa@example.com>" rsa3072 sign never > /dev/null 2>&1
gpg --list-secret-keys --with-colons --batch --fixed-list-mode --with-fingerprint > "$out/gpg_secret_keys.bin"

git config user.signingkey "$fpr"
echo 5 > f && git add f
git commit -q -S -m "gpg signed"
gpg_good="$(git rev-parse HEAD)"
git cat-file commit "$gpg_good" | sed 's/^gpg signed$/gpg tampered/' > "$scratch/gpg_tampered.txt"
gpg_tampered="$(git hash-object -t commit -w "$scratch/gpg_tampered.txt")"
printf '%s\n%s\n' "$gpg_tampered" "$gpg_good" | git log --no-walk=unsorted --stdin -z "--format=$format" > "$out/signing_log_gpg.bin" 2> /dev/null

# passphrase를 받을 수 없는 gpg: 모드별로 한 번씩 실패시킨다 (pinentry가 없다, 취소했다).
printf '#!/bin/sh\nexec gpg --pinentry-mode error "$@"\n' > "$scratch/gpg_nopinentry.sh"
printf '#!/bin/sh\nexec gpg --pinentry-mode cancel "$@"\n' > "$scratch/gpg_cancel.sh"
chmod +x "$scratch/gpg_nopinentry.sh" "$scratch/gpg_cancel.sh"
# 암호가 걸린 키가 있어야 pinentry를 요구한다.
gpg --batch --pinentry-mode loopback --passphrase 'pw123' --quick-gen-key "Locked <locked@example.com>" ed25519 sign never > /dev/null 2>&1
locked="$(gpg --list-secret-keys --with-colons locked@example.com | awk -F: '/^fpr:/ {print $10; exit}')"
# 위 passphrase가 agent에 남아 있으면 pinentry를 부르지 않는다. agent를 껐다 켠다.
gpgconf --kill gpg-agent
sleep 1
git config user.signingkey "$locked"
echo 6 > f && git add f
git config gpg.program "$scratch/gpg_nopinentry.sh"
git commit -S -m x > /dev/null 2> "$out/err_sign_gpg_no_pinentry.bin" || true
git config gpg.program "$scratch/gpg_cancel.sh"
git commit -S -m x > /dev/null 2> "$out/err_sign_gpg_cancelled.bin" || true
git config --unset gpg.program
# 태그도 같은 실패를 낸다. 키가 없는 이름으로 불러 pinentry 창을 띄우지 않는다.
git config user.signingkey nobody@example.com
git tag -s -m x v-nokey > /dev/null 2> "$out/err_sign_tag_gpg_no_secret_key.bin" || true

scrub "$out"/err_sign_*.bin "$out"/signing_log_ssh_unset_stderr.bin
