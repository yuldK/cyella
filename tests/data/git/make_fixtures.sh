#!/usr/bin/env bash
# 이 폴더의 *.bin / *.patch 고정 출력을 실제 git으로 다시 만든다.
#
#   tests/data/git/make_fixtures.sh <scratch_dir> <out_dir>
#
# 해석기 test는 git 버전에 흔들리지 않도록 이 출력을 그대로 읽는다. git이 출력 형식을
# 바꾸면 이 스크립트로 다시 만들고 diff를 눈으로 확인한 뒤 커밋한다. 날짜와 작성자를
# 고정해 같은 git에서는 SHA까지 같은 출력이 나온다.
set -euo pipefail

scratch="${1:?scratch dir}"
out="${2:?out dir}"
mkdir -p "$out"
rm -rf "$scratch"
mkdir -p "$scratch"

export LC_ALL=C
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME="Ann Author"
export GIT_AUTHOR_EMAIL="ann@example.com"
export GIT_COMMITTER_NAME="Cy Committer"
export GIT_COMMITTER_EMAIL="cy@example.com"

# 오류 문구에 들어간 임시 경로를 지워 고정 출력이 사용자 환경을 드러내지 않게 한다.
scrub_paths()
{
    local root
    root="$(cd "$scratch" && pwd -W 2>/dev/null || pwd)"
    sed -i "s|$root|/scratch|g" "$@"
}

set_date()
{
    export GIT_AUTHOR_DATE="$1 +0900"
    export GIT_COMMITTER_DATE="$1 +0900"
}

new_repo()
{
    git init -q -b main "$1"
    git -C "$1" config core.autocrlf false
}

# git_command_builder의 형식과 같아야 한다.
ref_format='%(refname)%1f%(objectname)%1f%(*objectname)%1f%(HEAD)%1f%(upstream)%1f%(upstream:short)%1f%(upstream:track)%1f%(symref)'
log_format='%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s'
stash_format='%gd%x1f%H%x1f%P%x1f%ct%x1f%gs'
status_args=(status --porcelain=v2 --branch -z --untracked-files=all)
diff_args=(--no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/)

# ---------------------------------------------------------------- 초기 저장소
new_repo "$scratch/initial"
git -C "$scratch/initial" "${status_args[@]}" > "$out/status_initial.bin"

# ---------------------------------------------------------------- 원격과 작업 저장소
git init -q --bare -b main "$scratch/origin.git"
new_repo "$scratch/work"
cd "$scratch/work"
git remote add origin "$scratch/origin.git"

set_date 2026-01-01T10:00:00
printf 'hello\nworld\n' > a.txt
mkdir -p dir
printf 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n' > dir/b.txt
printf 'to be renamed\nline 2\nline 3\nline 4\nline 5\n' > old_name.txt
printf 'to be deleted\n' > gone.txt
printf 'ignore_me.log\n' > .gitignore
git add .
git commit -q -m "initial commit"
git push -q origin main
git remote set-head origin main

set_date 2026-01-02T10:00:00
git switch -q -c feature
printf 'feat\n' > f.txt
git add f.txt
git commit -q -m "feat: add f" -m $'Body line 1\nBody line 2\n\n- bullet'
git push -q -u origin feature

set_date 2026-01-03T10:00:00
git switch -q main
printf 'hello\nmain\n' > a.txt
git commit -q -am "main change 한글 제목 ✓"
git push -q origin main

set_date 2026-01-04T10:00:00
git merge -q --no-ff feature -m "Merge branch 'feature'"
git tag v1.0
set_date 2026-01-05T10:00:00
git tag -a v2.0 -m "release 2"
git push -q origin v1.0

# 다른 clone에서 origin/main을 전진시켜 main이 앞서면서 뒤처지게(diverged) 만든다.
git clone -q "$scratch/origin.git" "$scratch/other"
(
    cd "$scratch/other"
    git config core.autocrlf false
    set_date 2026-01-06T10:00:00
    printf 'other\n' > other.txt
    git add other.txt
    git commit -q -m "other side commit"
    git push -q origin main
)
git fetch -q origin
git branch -q --set-upstream-to=origin/main main
git push origin main > /dev/null 2> "$out/err_push_rejected.bin" || true

# 원격에서 지워진 upstream을 가진 브랜치(gone)
git push -q origin main:stale
git branch -q --track gone-branch origin/stale
git push -q origin --delete stale

git for-each-ref --format="$ref_format" refs/heads refs/remotes refs/tags > "$out/for_each_ref.bin"
git log --topo-order -z "--format=$log_format" --branches --remotes --tags HEAD -- > "$out/log.bin"
git log --topo-order -z "--format=$log_format%x1f%b" --branches --remotes --tags HEAD -- > "$out/log_body.bin"
git log --topo-order -z --max-count=2 --skip=1 "--format=$log_format" --branches --remotes --tags HEAD -- > "$out/log_paged.bin"
git show -m --first-parent --format= "${diff_args[@]}" -U3 HEAD -- > "$out/diff_merge_first_parent.patch"
git ls-remote --tags origin > "$out/ls_remote_tags.bin"

# ---------------------------------------------------------------- 작업 트리 상태
set_date 2026-01-07T10:00:00
printf 'hello\nmain\nstaged\n' > a.txt
git add a.txt
printf 'one\ntwo\nTHREE\nfour\nfive\nsix\nseven\neight\nnine\nten\n' > dir/b.txt
printf 'new staged\n' > added.txt
git add added.txt
printf 'changed after add\n' >> added.txt
git rm -q gone.txt
git mv old_name.txt "new name.txt"
printf 'to be renamed\nline 2\nline 3\nline 4\nline 5\nextra\n' > "new name.txt"
printf 'untracked\n' > untracked.txt
mkdir -p untracked_dir
printf 'x\n' > untracked_dir/inner.txt
printf 'ignored\n' > ignore_me.log
git "${status_args[@]}" --ignored > "$out/status_basic.bin"
git diff --numstat -z > "$out/numstat_unstaged.bin"
git diff --cached --numstat -z -M > "$out/numstat_staged.bin"
git diff "${diff_args[@]}" -U3 -- dir/b.txt > "$out/diff_unstaged_modify.patch"
git diff --cached "${diff_args[@]}" -U3 -M > "$out/diff_staged_all.patch"
git diff --no-index "${diff_args[@]}" -U3 -- /dev/null untracked.txt > "$out/diff_untracked.patch" || true

git stash push -q -u -m "work in progress ✓"
git "${status_args[@]}" > "$out/status_clean.bin"
git stash list -z "--format=$stash_format" > "$out/stash_list.bin"
printf 'second\n' > second.txt
git add second.txt
git stash push -q -m "second stash"
printf 'third\n' >> a.txt
git stash push -q
git stash list -z "--format=$stash_format" > "$out/stash_list_two.bin"
git switch -q --detach v1.0
git "${status_args[@]}" > "$out/status_detached.bin"
git switch -q main

# ---------------------------------------------------------------- diff 모양 모음
new_repo "$scratch/shapes"
cd "$scratch/shapes"
set_date 2026-02-01T10:00:00
{
    echo '#include <stdio.h>'
    echo
    echo 'int add(int a, int b)'
    echo '{'
    for i in $(seq 1 12); do echo "    /* pad $i */"; done
    echo '    return a + b;'
    echo '}'
    echo
    echo 'int sub(int a, int b)'
    echo '{'
    for i in $(seq 1 12); do echo "    /* pad2 $i */"; done
    echo '    return a - b;'
    echo '}'
} > code.c
printf 'single\n' > one_line.txt
printf 'keep\r\nme\r\nCRLF\r\n' > crlf.txt
printf 'no newline at end' > nonl.txt
printf '#!/bin/sh\necho hi\n' > script.sh
printf '\x00\x01\x02binary\x00' > blob.bin
printf 'del me\n' > deleted.txt
printf 'rename me\nwith\nsome\ncontent\nlines\nhere\nand there\n' > before_rename.txt
printf 'pure rename\nbody\nbody2\n' > pure_before.txt
printf 'korean\n' > "한글.txt"
printf 'space\n' > "with space.txt"
git add .
git commit -q -m "base"

set_date 2026-02-02T10:00:00
sed -i 's/return a + b;/return a + b + 0;/; s/return a - b;/return a - b - 0;/' code.c
printf 'single changed\n' > one_line.txt
printf 'keep\r\nme CHANGED\r\nCRLF\r\n' > crlf.txt
printf 'no newline at end, changed' > nonl.txt
git update-index --chmod=+x script.sh
printf '\x00\x01\x02binary\x00changed' > blob.bin
git rm -q deleted.txt
git mv before_rename.txt after_rename.txt
printf 'rename me\nwith\nsome\ncontent\nlines\nhere\nand there\nappended\n' > after_rename.txt
git mv pure_before.txt pure_after.txt
printf 'korean\nline2\n' > "한글.txt"
printf 'space\nmore\n' > "with space.txt"
printf 'brand new\nfile\n' > created.txt
printf 'no newline new' > created_nonl.txt
git add -A
git commit -q -m "shapes"
git show --format= "${diff_args[@]}" -U3 --find-renames HEAD -- > "$out/diff_shapes.patch"
git diff --numstat -z --find-renames HEAD~1 HEAD > "$out/numstat_shapes.bin"
git show --format= "${diff_args[@]}" -U3 "$(git rev-list --max-parents=0 HEAD)" -- > "$out/diff_root_commit.patch"
git diff "${diff_args[@]}" > "$out/diff_empty.patch"
git diff "${diff_args[@]}" -U0 HEAD~1 HEAD -- code.c > "$out/diff_u0.patch"
git diff "${diff_args[@]}" --binary HEAD~1 HEAD -- blob.bin > "$out/diff_binary_patch.patch"
git -c core.quotepath=false diff "${diff_args[@]}" HEAD~1 HEAD -- "한글.txt" > "$out/diff_unquoted_path.patch"

# ---------------------------------------------------------------- 충돌 상태와 오류 문구
new_repo "$scratch/conflict"
cd "$scratch/conflict"
set_date 2026-03-01T10:00:00
printf 'base\n' > both.txt
printf 'base\n' > del_ours.txt
printf 'base\n' > del_theirs.txt
git add .
git commit -q -m "base"
git switch -q -c topic
set_date 2026-03-02T10:00:00
printf 'topic\n' > both.txt
git rm -q del_ours.txt
printf 'topic change\n' > del_theirs.txt
printf 'topic add\n' > both_added.txt
git add -A
git commit -q -m "topic"
git switch -q main
set_date 2026-03-03T10:00:00
printf 'main\n' > both.txt
printf 'main change\n' > del_ours.txt
git rm -q del_theirs.txt
printf 'main add\n' > both_added.txt
git add -A
git commit -q -m "main"
git merge topic > "$out/merge_conflict_stdout.bin" 2> "$out/merge_conflict_stderr.bin" || true
git "${status_args[@]}" > "$out/status_conflict.bin"
git merge --abort

printf 'dirty\n' > both.txt
git switch topic > /dev/null 2> "$out/err_checkout_overwrite.bin" || true
git checkout -q -- both.txt
: > .git/index.lock
git add both.txt 2> "$out/err_index_lock.bin" || true
rm -f .git/index.lock
git fetch "$scratch/does-not-exist" > /dev/null 2> "$out/err_not_a_repository.bin" || true
scrub_paths "$out"/err_*.bin
