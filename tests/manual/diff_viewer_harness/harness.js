// diff 뷰어 점검 틀의 본체다. 메시지를 만들어 iframe 안의 뷰어에 넣고, 뷰어가 보낸 메시지를 찍는다.
'use strict';

(function () {
    const frame = document.getElementById('frame');
    const bar = document.getElementById('bar');
    const log = document.getElementById('log');
    let seq = 0;
    let theme = 'dark';
    let layout = 'inline';
    let view = 'unstaged';
    let listeners = [];
    // 앱처럼 내용 메시지마다 문구 표(`diff_viewer.*`)와 언어를 싣는다. 문구는 실제 문구 파일에서 읽는다.
    let language = 'ko';
    const catalogs = { ko: {}, en: {} };
    // 지금 보이는 예시다. 뷰어가 `ready`를 (다시) 보내면 이것을 다시 보낸다.
    let current = null;

    function note(text) {
        log.textContent += text + '\n';
        log.scrollTop = log.scrollHeight;
    }

    window.harnessPosted = (message) => {
        note('[webview→C++] ' + JSON.stringify(message).slice(0, 300));
        if (message.type === 'ready')
            (current || sampleCpp)();
    };

    function send(message) {
        message.seq = ++seq;
        note('[C++→webview] ' + message.type + ' seq=' + message.seq);
        for (const listener of listeners)
            listener({ data: message });
    }

    function state() {
        return { layout, image_mode: 'side_by_side', theme: { mode: theme, tokens: {} }, toolbar: true, strings: catalogs[language], lang: language };
    }

    function loadStrings(name) {
        return fetch('../../../assets/locales/' + name + '.json').then((response) => response.json()).then((all) => {
            for (const [key, value] of Object.entries(all))
                if (key.startsWith('diff_viewer.'))
                    catalogs[name][key] = value;
        });
    }

    function file(path, status, hunks, extra) {
        let added = 0;
        let removed = 0;
        for (const hunk of hunks)
            for (const line of hunk.lines) {
                if (line[0] === 'a')
                    ++added;
                else if (line[0] === 'd')
                    ++removed;
            }
        return Object.assign({ old_path: status === 'A' ? '' : path, new_path: status === 'D' ? '' : path, status, similarity: 0, old_mode: '100644', new_mode: '100644', lines_added: added, lines_removed: removed, hunks }, extra);
    }

    function hunk(oldStart, newStart, section, lines) {
        const oldCount = lines.filter((line) => line[0] === 'c' || line[0] === 'd').length;
        const newCount = lines.filter((line) => line[0] === 'c' || line[0] === 'a').length;
        return { header: '@@', old_start: oldStart, old_count: oldCount, new_start: newStart, new_count: newCount, section, lines };
    }

    function ctx(number, text) { return ['c', number, number, text]; }
    function del(number, text) { return ['d', number, 0, text]; }
    function add(number, text) { return ['a', 0, number, text]; }

    function showDiff(path, hunks, options) {
        const total = hunks.reduce((sum, item) => sum + item.lines.length, 0);
        send(Object.assign({
            type: 'show_diff', view, can_stage: view === 'unstaged', can_unstage: view === 'staged', can_discard: view === 'unstaged',
            truncated: false, total_lines: total, shown_lines: total, file: file(path, 'M', hunks),
        }, state(), options));
    }

    function sampleCpp() {
        showDiff('src/graph/lane_layout.cpp', [
            hunk(12, 12, 'namespace cyella::graph {', [
                ctx(12, 'lane_layout::lane_layout(std::size_t max_lanes)'),
                ctx(13, '    : max_lanes_ { max_lanes }'),
                ctx(14, '{'),
                del(15, '    lanes_.resize(max_lanes_);'),
                add(15, '    lanes_.reserve(max_lanes_);'),
                add(16, '    colors_.reserve(max_lanes_);'),
                ctx(17, '}'),
                ctx(18, ''),
                del(19, '    // 첫 부모는 같은 레인을 잇고 나머지는 비어 있는 레인을 찾는다. /* 주석'),
                del(20, '       이어지는 주석 */ std::size_t place(const commit_row& row)'),
                add(19, '    // 첫 부모는 같은 레인을 잇고, 나머지 부모는 빈 레인을 찾는다. /* 주석'),
                add(20, '       이어지는 주석 */ std::size_t place(const commit_row& row, bool merge)'),
                ctx(21, '{'),
                add(22, '    const auto lane { find_or_allocate("abc\\n") };'),
                ctx(23, '    return lane;'),
            ]),
            hunk(80, 82, 'void lane_layout::release', [
                ctx(80, '    }'),
                del(81, '        while(!lanes_.empty() && lanes_.back().empty())'),
                add(83, '        while (!lanes_.empty() && lanes_.back().empty())'),
                ctx(82, '        {'),
                ctx(83, '            lanes_.pop_back();'),
                ctx(84, '        }'),
            ]),
        ]);
    }

    function crlfAndInvalid() {
        const bad = btoa(String.fromCharCode(0x61, 0xff, 0xfe, 0x62));
        showDiff('src/mixed.txt', [
            hunk(1, 1, '', [
                ['c', 1, 1, 'windows line\r'],
                ['d', 2, 0, 'old crlf\r'],
                ['a', 0, 2, 'new crlf\r'],
                ['a', 0, 3, bad, 1],
                ['a', 0, 4, '\t탭과 한글 😀 이모지'],
                ['n', 0, 0, 'No newline at end of file'],
            ]),
        ]);
    }

    function big() {
        const lines = [];
        for (let index = 1; index <= 60000; ++index) {
            if (index % 11 === 0)
                lines.push(add(index, '    const auto value_' + index + ' { compute(' + index + ', "text") }; // added'));
            else if (index % 13 === 0)
                lines.push(del(index, '    int removed_' + index + ' = ' + index + ';'));
            else
                lines.push(ctx(index, '    std::size_t line_' + index + ' = ' + index + '; /* context */'));
        }
        showDiff('src/big.cpp', [hunk(1, 1, 'huge', lines)]);
    }

    function canvasUrl(color, text, width, height) {
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext('2d');
        context.fillStyle = color;
        context.fillRect(0, 0, width, height);
        context.fillStyle = '#fff';
        context.font = '28px sans-serif';
        context.fillText(text, 16, 48);
        context.strokeStyle = '#fff';
        context.strokeRect(8, 8, width - 16, height - 16);
        return canvas.toDataURL('image/png');
    }

    function image() {
        send(Object.assign({
            type: 'show_image', path: 'assets/logo.png', status: 'M', left_url: canvasUrl('#2b6cb0', 'before', 320, 200), right_url: canvasUrl('#c05621', 'after', 360, 220),
            left_size: 2048, right_size: 4096, can_open_external: true,
        }, state()));
    }

    function binary() {
        send(Object.assign({ type: 'show_binary', path: 'art/hero.psd', status: 'M', old_size: 1048576, new_size: 1310720, can_open_external: true }, state()));
    }

    // ------------------------------------------------------------------ blame

    // 지어낸 커밋들이다. time은 초 단위 UTC다.
    const blameOrigins = [
        { sha: '50e889a1c2d3e4f5061728394a5b6c7d8e9f0a1b', author: 'Morgan', mail: 'morgan@example.com', time: 1767574800, summary: 'graph: 레인 배치를 처음 만든다', path: 'src/graph/lanes.cpp', previous: false, uncommitted: false },
        { sha: '9b1c7e4402d3e4f5061728394a5b6c7d8e9f0a1b', author: 'Riley Alexandria Montgomery-Fitzgerald', mail: 'riley@example.com', time: 1771203600, summary: 'graph: reserve로 바꾸고 색 표를 함께 잡는다 — 제목이 길어 정보 띠에서 말줄임으로 잘리는지 보는 커밋이다', path: 'src/graph/lane_layout.cpp', previous: true, uncommitted: false },
        { sha: 'c4d2a90b7e13e4f5061728394a5b6c7d8e9f0a1b', author: '김하늘', mail: 'haneul@example.com', time: 1782867600, summary: 'fix: 빈 레인을 뒤에서부터 거둔다', path: 'src/graph/lane_layout.cpp', previous: true, uncommitted: false },
        { sha: '0000000000000000000000000000000000000000', author: 'Not Committed Yet', mail: 'not.committed.yet', time: 1795395600, summary: '', path: 'src/graph/lane_layout.cpp', previous: true, uncommitted: true },
    ];

    function showBlame(path, revision, lines, options) {
        send(Object.assign({ type: 'show_blame', path, revision, origins: blameOrigins, lines, truncated: false, total_lines: lines.length, focus: '' }, state(), options));
    }

    // [origin 위치, 내용(, 1)] 목록에 줄 번호를 매긴다.
    function numbered(entries) {
        return entries.map((entry, index) => [entry[0], index + 1, ...entry.slice(1)]);
    }

    function blameLines() {
        const bad = btoa(String.fromCharCode(0x2f, 0x2f, 0x20, 0x61, 0xff, 0xfe, 0x62));
        return numbered([
            [0, '#include "graph/lane_layout.h"'],
            [0, ''],
            [0, 'namespace cyella::graph {'],
            [0, '    lane_layout::lane_layout(std::size_t max_lanes)'],
            [0, '        : max_lanes_ { max_lanes }'],
            [0, '    {'],
            [1, '        lanes_.reserve(max_lanes_);'],
            [1, '        colors_.reserve(max_lanes_);'],
            [0, '    }'],
            [0, ''],
            [2, '    /* 첫 부모는 같은 레인을 잇고, 나머지 부모는 빈 레인을 찾는다.'],
            [2, '       여러 줄 주석이 줄을 넘어 이어진다. */'],
            [0, '    std::size_t lane_layout::place(const commit_row& row, bool merge)'],
            [0, '    {'],
            [3, '        const auto lane { find_or_allocate("abc\\n") }; // 아직 커밋하지 않은 줄'],
            [3, '        static_cast<void>(merge);'],
            [0, '        return lane;'],
            [0, '    }'],
            [0, ''],
            [2, '    void lane_layout::release(const std::size_t lane)'],
            [2, '    {'],
            [2, '        lanes_[lane].clear();'],
            [1, '        while (!lanes_.empty() && lanes_.back().empty())\r'],
            [1, bad, 1],
            [2, '            lanes_.pop_back();'],
            [2, '    }'],
            [0, '} // namespace cyella::graph — 가로 스크롤을 보려고 길게 늘인 줄이다: ' + 'lorem ipsum dolor sit amet '.repeat(8)],
        ]);
    }

    function sampleBlame() {
        showBlame('src/graph/lane_layout.cpp', '', blameLines());
    }

    // 커밋 시점의 blame이다. focus의 origin을 고른 채로 열린다.
    function blameAtRevision() {
        const lines = blameLines().filter((line) => line[0] !== 3).map((line, index) => [line[0], index + 1, ...line.slice(2)]);
        showBlame('src/graph/lane_layout.cpp', blameOrigins[2].sha, lines, { focus: blameOrigins[1].sha });
    }

    function bigBlameLines(count) {
        const lines = [];
        let origin = 0;
        let left = 0;
        for (let index = 1; index <= count; ++index) {
            if (left === 0) {
                origin = (origin + 1 + index % 2) % 3;
                left = 1 + (index * 7) % 23;
            }
            --left;
            lines.push([origin, index, '    std::size_t line_' + index + ' = ' + index + '; /* blame */']);
        }
        return lines;
    }

    function bigBlame() {
        showBlame('src/big.cpp', '', bigBlameLines(100000));
    }

    function truncatedBlame() {
        showBlame('src/huge.cpp', '', bigBlameLines(2000), { truncated: true, total_lines: 250000 });
    }

    function emptyBlame() {
        showBlame('src/empty.txt', '', []);
    }

    // 예시를 보내고, `ready`가 다시 오면 같은 것을 보내도록 기억한다.
    function sample(handler) {
        return () => {
            current = handler;
            handler();
        };
    }

    const buttons = [
        ['C++ diff', sample(sampleCpp)],
        ['CRLF·잘못된 UTF-8', sample(crlfAndInvalid)],
        ['60k줄', sample(big)],
        ['이미지', sample(image)],
        ['바이너리', sample(binary)],
        ['blame', sample(sampleBlame)],
        ['blame(커밋·focus)', sample(blameAtRevision)],
        ['blame 10만 줄', sample(bigBlame)],
        ['blame 잘림', sample(truncatedBlame)],
        ['blame 빈 파일', sample(emptyBlame)],
        ['비우기', () => send(Object.assign({ type: 'clear' }, state()))],
        ['staged 보기', () => { view = view === 'staged' ? 'unstaged' : 'staged'; sample(sampleCpp)(); }],
        ['테마 전환', () => { theme = theme === 'dark' ? 'light' : 'dark'; send({ type: 'set_theme', theme: { mode: theme, tokens: {} } }); }],
        ['set_layout 전환', () => { layout = layout === 'inline' ? 'split' : 'inline'; send({ type: 'set_layout', layout }); }],
        ['언어 전환', () => { language = language === 'ko' ? 'en' : 'ko'; (current || sampleCpp)(); }],
    ];
    for (const [label, handler] of buttons) {
        const element = document.createElement('button');
        element.textContent = label;
        element.addEventListener('click', handler);
        bar.appendChild(element);
    }

    // 문구를 읽은 뒤, 뷰어 페이지를 읽어 가짜 chrome.webview를 심고 srcdoc으로 연다.
    Promise.all([loadStrings('ko'), loadStrings('en')]).then(() => fetch('../../../assets/diff-viewer/index.html')).then((response) => response.text()).then((html) => {
        const shim = '<base href="../../../assets/diff-viewer/"><script>window.chrome = { webview: { postMessage: (m) => parent.harnessPosted(JSON.parse(JSON.stringify(m))), addEventListener: (t, f) => parent.harnessListen(f) } };<\/script>';
        window.harnessListen = (listener) => listeners.push(listener);
        frame.srcdoc = html.replace('<head>', '<head>' + shim);
    });
})();
