#pragma once

#include "application/repository_watcher.h"

#include <memory>
#include <string_view>

namespace cyella::win32 {
    // `ReadDirectoryChangesW`로 작업 트리를 재귀 감시한다.
    //
    // - 전용 스레드 하나가 겹치기(overlapped) I/O를 기다린다. 작업 트리 하나에 읽기 하나가
    //   걸리고, `.git`이 파일이면(worktree, submodule) gitdir(과 `commondir`)에도 하나씩 걸린다.
    // - `.git` 안의 변경은 `classify_repository_change`가 가른다. 작업 트리의 `.git/**`는
    //   작업 트리 변경으로 세지 않는다.
    // - 버퍼가 넘치거나(`ERROR_NOTIFY_ENUM_DIR`, 0바이트 완료) 감시가 끊기면 이벤트를 놓쳤을 수
    //   있으므로 두 종류를 모두 알린다.
    // - 이 함수는 돌아오기 전에 첫 읽기를 걸어 둔다. 돌아온 뒤의 변경은 놓치지 않는다.
    //
    // 작업 트리를 열 수 없으면 `watcher`가 null이고 진단이 하나 이상 있다. gitdir만 열 수
    // 없으면 작업 트리만 감시하고 경고를 남긴다.
    [[nodiscard]] repository_watch_start_result start_repository_watcher(std::u8string_view working_tree, repository_change_callback callback, const repository_watch_options& options);

    [[nodiscard]] std::unique_ptr<repository_watcher_factory> make_repository_watcher_factory();
} // namespace cyella::win32
