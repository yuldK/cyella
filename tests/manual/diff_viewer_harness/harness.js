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

    function note(text) {
        log.textContent += text + '\n';
        log.scrollTop = log.scrollHeight;
    }

    window.harnessPosted = (message) => {
        note('[webview→C++] ' + JSON.stringify(message).slice(0, 300));
        if (message.type === 'ready')
            sendSample();
    };

    function send(message) {
        message.seq = ++seq;
        note('[C++→webview] ' + message.type + ' seq=' + message.seq);
        for (const listener of listeners)
            listener({ data: message });
    }

    function state() {
        return { layout, image_mode: 'side_by_side', theme: { mode: theme, tokens: {} }, toolbar: true };
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

    function sendSample() {
        sampleCpp();
    }

    const buttons = [
        ['C++ diff', sampleCpp],
        ['CRLF·잘못된 UTF-8', crlfAndInvalid],
        ['60k줄', big],
        ['이미지', image],
        ['바이너리', binary],
        ['비우기', () => send(Object.assign({ type: 'clear' }, state()))],
        ['staged 보기', () => { view = view === 'staged' ? 'unstaged' : 'staged'; sampleCpp(); }],
        ['테마 전환', () => { theme = theme === 'dark' ? 'light' : 'dark'; send({ type: 'set_theme', theme: { mode: theme, tokens: {} } }); }],
        ['set_layout 전환', () => { layout = layout === 'inline' ? 'split' : 'inline'; send({ type: 'set_layout', layout }); }],
    ];
    for (const [label, handler] of buttons) {
        const element = document.createElement('button');
        element.textContent = label;
        element.addEventListener('click', handler);
        bar.appendChild(element);
    }

    // 뷰어 페이지를 읽어 가짜 chrome.webview를 심고 srcdoc으로 연다.
    fetch('../../../assets/diff-viewer/index.html').then((response) => response.text()).then((html) => {
        const shim = '<base href="../../../assets/diff-viewer/"><script>window.chrome = { webview: { postMessage: (m) => parent.harnessPosted(JSON.parse(JSON.stringify(m))), addEventListener: (t, f) => parent.harnessListen(f) } };<\/script>';
        window.harnessListen = (listener) => listeners.push(listener);
        frame.srcdoc = html.replace('<head>', '<head>' + shim);
    });
})();
