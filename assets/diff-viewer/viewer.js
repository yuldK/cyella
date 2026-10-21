// diff 뷰어 본체다. C++이 `chrome.webview`로 보내는 메시지(docs/decisions/ADR-002-diff-viewer.md)를 받아
// inline·분할 diff, 이미지 비교, 바이너리 카드를 그리고, 사용자의 동작을 같은 통로로 C++에 돌려보낸다.
//
// 큰 파일은 보이는 행만 DOM에 둔다(가상 스크롤). 행 높이가 고정이라 행 위치를 미리 더해 둔 표로 보이는 구간을
// 이진 탐색한다. 구문 강조와 단어 단위 강조는 그 행이 처음 그려질 때 계산한다.
'use strict';

(function () {
    const { ROW_HUNK, ROW_LINE, ROW_PAIR, ROW_NOTE, LINE_HEIGHT } = window.CyellaDiff;
    const diff = window.CyellaDiff;

    const PROTOCOL_VERSION = 1;
    // 보이는 구간 앞뒤로 더 그려 두는 행 수다. 이 안에서 움직이면 다시 그리지 않는다.
    const OVERSCAN_ROWS = 40;
    // 구문 강조를 한 번에 하는 줄 수다. 한 hunk가 이보다 크면 덩어리마다 따로 강조한다.
    const HIGHLIGHT_CHUNK = 400;
    const GUTTER_INLINE = 46 + 46 + 20;
    const GUTTER_SPLIT = 44 + 20;
    // 코드 칸 오른쪽 여백이다(CSS의 `.dl-code` padding-right와 같다).
    const CODE_PADDING = 16;
    const DISCARD_ARM_MILLISECONDS = 4000;

    const host = window.chrome && window.chrome.webview ? window.chrome.webview : null;

    const state = {
        // 마지막으로 적용한 C++ 메시지의 seq다. 같거나 낮은 것은 늦게 온 것이라 버린다.
        appliedSeq: 0,
        // 지금 보이는 내용을 실어 온 메시지의 seq다. 모든 동작이 이 값을 되돌려 보낸다.
        contentSeq: 0,
        mode: 'empty',
        layout: 'inline',
        imageMode: 'side_by_side',
        toolbar: true,
        view: 'unstaged',
        can: { stage: false, unstage: false, discard: false },
        path: '',
        file: null,
        model: null,
        rows: null,
        language: null,
        truncated: false,
        totalLines: 0,
        shownLines: 0,
        selection: null,
        anchor: null,
        busy: false,
        armed: null,
        image: null,
        binary: null,
    };

    const elements = {
        toolbar: document.getElementById('toolbar'),
        notice: document.getElementById('notice'),
        content: document.getElementById('content'),
        selection: document.getElementById('selection-bar'),
    };

    // 보이는 diff의 DOM 조각이다.
    let scroller = null;
    let sizer = null;
    let windowElement = null;
    let renderedStart = -1;
    let renderedEnd = -1;
    // 지금까지 그려 본 줄 중 가장 긴 코드의 너비(px)다. 가로 스크롤 범위가 이 값을 따라 자란다.
    let measuredCode = 0;
    let armTimer = 0;
    let highlightCache = new Map();
    let wordCache = new Map();
    let dragging = null;
    let changeStarts = [];
    let currentChange = -1;

    // ------------------------------------------------------------------ 통신

    function post(message) {
        if (host)
            host.postMessage(message);
        else
            console.info('[webview→C++]', message);
    }

    function postAction(type, extra) {
        post(Object.assign({ type, seq: state.contentSeq }, extra));
    }

    // ------------------------------------------------------------------ 작은 도구

    const escapeHtml = diff.escapeHtml;

    function button(label, action, className, disabled) {
        return '<button type="button" class="btn ' + (className || '') + '" data-act="' + action + '"' + (disabled ? ' disabled' : '') + '>' + label + '</button>';
    }

    function statusLabel(status) {
        switch (status) {
        case 'A': return '추가됨';
        case 'D': return '삭제됨';
        case 'R': return '이름 변경';
        case 'C': return '복사됨';
        default: return '수정됨';
        }
    }

    // ------------------------------------------------------------------ 테마

    let appliedTokens = [];

    function applyTheme(theme) {
        const root = document.documentElement;
        root.dataset.theme = theme.mode === 'light' ? 'light' : 'dark';
        for (const name of appliedTokens)
            root.style.removeProperty(name);
        appliedTokens = [];
        for (const [name, value] of Object.entries(theme.tokens || {})) {
            if (/^--[a-z0-9-]+$/.test(name)) {
                root.style.setProperty(name, value);
                appliedTokens.push(name);
            }
        }
    }

    // ------------------------------------------------------------------ 머리 줄

    function renderToolbar() {
        if (state.toolbar === false || state.mode === 'empty') {
            elements.toolbar.hidden = true;
            return;
        }
        elements.toolbar.hidden = false;

        let html = '<div class="toolbar-title" title="' + escapeHtml(state.path) + '">';
        const file = state.file;
        if (file && file.old_path && file.new_path && file.old_path !== file.new_path)
            html += '<span class="toolbar-path">' + escapeHtml(file.old_path) + ' → ' + escapeHtml(file.new_path) + '</span>';
        else
            html += '<span class="toolbar-path">' + escapeHtml(state.path) + '</span>';
        if (file)
            html += '<span class="chip">' + statusLabel(file.status) + '</span>';
        html += '</div>';

        if (state.mode === 'diff' && file) {
            html += '<div class="toolbar-stat"><span class="is-add">+' + file.lines_added + '</span><span class="is-del">−' + file.lines_removed + '</span></div>';
        }

        html += '<div class="toolbar-spacer"></div>';

        if (state.mode === 'diff') {
            html += '<div class="segmented" role="group" aria-label="보기">'
                + '<button type="button" class="seg' + (state.layout === 'inline' ? ' is-active' : '') + '" data-layout="inline">inline</button>'
                + '<button type="button" class="seg' + (state.layout === 'split' ? ' is-active' : '') + '" data-layout="split">분할</button>'
                + '</div>';
        } else if (state.mode === 'image') {
            const modes = [['side_by_side', '좌우'], ['onion', '겹치기'], ['swipe', '밀기']];
            html += '<div class="segmented" role="group" aria-label="이미지 비교">';
            for (const [value, label] of modes)
                html += '<button type="button" class="seg' + (state.imageMode === value ? ' is-active' : '') + '" data-image-mode="' + value + '">' + label + '</button>';
            html += '</div>';
        }

        const canOpen = state.mode === 'diff' || (state.mode === 'image' && state.image.canOpenExternal) || (state.mode === 'binary' && state.binary.canOpenExternal);
        if (canOpen)
            html += button('외부 도구로 열기', 'open_external', 'is-quiet');
        elements.toolbar.innerHTML = html;
    }

    function showNotice() {
        if (state.mode === 'diff' && state.truncated) {
            elements.notice.hidden = false;
            elements.notice.innerHTML = '<span>전체 ' + state.totalLines.toLocaleString() + '줄 중 ' + state.shownLines.toLocaleString() + '줄만 표시합니다. 줄 단위 stage는 전체를 볼 때 쓸 수 있습니다.</span>'
                + button('전체 보기', 'show_full', 'is-primary');
        } else {
            elements.notice.hidden = true;
            elements.notice.textContent = '';
        }
    }

    // ------------------------------------------------------------------ 구문·단어 강조

    function sideLines(hunk, side) {
        const wanted = side === 'old' ? ['c', 'd'] : ['c', 'a'];
        const indexes = [];
        const positions = new Int32Array(hunk.lines.length).fill(-1);
        hunk.lines.forEach((line, index) => {
            if (wanted.includes(line.kind)) {
                positions[index] = indexes.length;
                indexes.push(index);
            }
        });
        return { indexes, positions };
    }

    // 줄의 구문 강조 HTML이다. 옛쪽(삭제·context)과 새쪽(추가·context)을 따로 이어 강조해 여러 줄 주석·문자열이
    // 줄을 넘어 이어진다.
    function highlightedHtml(hunkIndex, lineIndex, side) {
        const hunk = state.model.hunks[hunkIndex];
        const key = hunkIndex + side;
        let sides = highlightCache.get(key);
        if (sides === undefined) {
            sides = { ...sideLines(hunk, side), chunks: new Map() };
            highlightCache.set(key, sides);
        }
        const position = sides.positions[lineIndex];
        if (position < 0)
            return escapeHtml(hunk.lines[lineIndex].text);
        const chunk = Math.floor(position / HIGHLIGHT_CHUNK);
        let html = sides.chunks.get(chunk);
        if (html === undefined) {
            const from = chunk * HIGHLIGHT_CHUNK;
            const texts = sides.indexes.slice(from, from + HIGHLIGHT_CHUNK).map((index) => hunk.lines[index].text);
            html = diff.highlightLines(texts, state.language);
            sides.chunks.set(chunk, html);
        }
        return html[position - chunk * HIGHLIGHT_CHUNK];
    }

    function wordRanges(hunkIndex, lineIndex) {
        const hunk = state.model.hunks[hunkIndex];
        const partner = hunk.partner[lineIndex];
        if (partner < 0)
            return null;
        const line = hunk.lines[lineIndex];
        const deleted = line.kind === 'd' ? lineIndex : partner;
        const added = line.kind === 'd' ? partner : lineIndex;
        const key = hunkIndex + ':' + deleted;
        let result = wordCache.get(key);
        if (result === undefined) {
            result = diff.wordDiff(hunk.lines[deleted].text, hunk.lines[added].text);
            wordCache.set(key, result);
        }
        if (result === null)
            return null;
        return line.kind === 'd' ? result.old : result.new;
    }

    function codeHtml(hunkIndex, lineIndex) {
        const line = state.model.hunks[hunkIndex].lines[lineIndex];
        let html = highlightedHtml(hunkIndex, lineIndex, line.kind === 'd' ? 'old' : 'new');
        if (line.kind === 'a' || line.kind === 'd')
            html = diff.applyMarks(html, wordRanges(hunkIndex, lineIndex));
        if (line.cr)
            html += '<span class="eol-cr" title="CRLF">␍</span>';
        return html;
    }

    // ------------------------------------------------------------------ diff 행

    function isSelected(hunkIndex, lineIndex) {
        return state.selection !== null && state.selection.hunk === hunkIndex && state.selection.set.has(lineIndex);
    }

    function hunkRowHtml(hunkIndex) {
        const hunk = state.model.hunks[hunkIndex];
        const locked = state.busy;
        let actions = '';
        if (state.can.stage)
            actions += button('Stage hunk', 'stage_hunk', 'is-primary', locked)
                + button(isArmed('discard_hunk', hunkIndex) ? '정말 버릴까요?' : 'Discard hunk', 'discard_hunk', isArmed('discard_hunk', hunkIndex) ? 'is-danger' : 'is-quiet', locked || state.can.discard === false);
        if (state.can.unstage)
            actions += button('Unstage hunk', 'unstage_hunk', 'is-primary', locked);
        return '<div class="dl-hunk" data-h="' + hunkIndex + '"><div class="dl-hunk-inner">'
            + '<span class="dl-hunk-text">@@ -' + hunk.oldStart + ',' + hunk.oldCount + ' +' + hunk.newStart + ',' + hunk.newCount + ' @@</span>'
            + (hunk.section ? '<span class="dl-hunk-section">' + escapeHtml(hunk.section) + '</span>' : '')
            + '<span class="dl-hunk-spacer"></span>' + actions + '</div></div>';
    }

    function numberCell(value, hunkIndex, lineIndex, selectable) {
        return '<span class="dl-no' + (selectable ? ' is-selectable' : '') + '" data-h="' + hunkIndex + '" data-i="' + lineIndex + '">' + (value > 0 ? value : '') + '</span>';
    }

    function lineRowHtml(row, hunkIndex, lineIndex) {
        const line = state.model.hunks[hunkIndex].lines[lineIndex];
        const change = diff.isChange(line);
        const className = line.kind === 'a' ? ' is-add' : line.kind === 'd' ? ' is-del' : '';
        const mark = line.kind === 'a' ? '+' : line.kind === 'd' ? '−' : ' ';
        return '<div class="dl' + className + (isSelected(hunkIndex, lineIndex) ? ' is-selected' : '') + '" data-row="' + row + '">'
            + numberCell(line.old, hunkIndex, lineIndex, change) + numberCell(line.new, hunkIndex, lineIndex, change)
            + '<span class="dl-mark">' + mark + '</span><span class="dl-code">' + codeHtml(hunkIndex, lineIndex) + '</span></div>';
    }

    function halfHtml(hunkIndex, lineIndex, side) {
        if (lineIndex < 0)
            return '<div class="dl-half is-empty"></div>';
        const line = state.model.hunks[hunkIndex].lines[lineIndex];
        const change = diff.isChange(line);
        const className = line.kind === 'a' ? ' is-add' : line.kind === 'd' ? ' is-del' : '';
        const mark = line.kind === 'a' ? '+' : line.kind === 'd' ? '−' : ' ';
        const number = side === 'old' ? line.old : line.new;
        return '<div class="dl-half' + className + (isSelected(hunkIndex, lineIndex) ? ' is-selected' : '') + '">'
            + numberCell(number, hunkIndex, lineIndex, change) + '<span class="dl-mark">' + mark + '</span><span class="dl-code">' + codeHtml(hunkIndex, lineIndex) + '</span></div>';
    }

    function rowHtml(row) {
        const rows = state.rows;
        const hunkIndex = rows.hunks[row];
        switch (rows.types[row]) {
        case ROW_HUNK:
            return hunkRowHtml(hunkIndex);
        case ROW_LINE:
            return lineRowHtml(row, hunkIndex, rows.first[row]);
        case ROW_PAIR:
            return '<div class="dl is-split" data-row="' + row + '">' + halfHtml(hunkIndex, rows.first[row], 'old') + halfHtml(hunkIndex, rows.second[row], 'new') + '</div>';
        default:
            return '<div class="dl-note"><span>\\ No newline at end of file</span></div>';
        }
    }

    function updateScrollShadows(target) {
        const valid = target !== null && target.clientHeight > 0;
        elements.content.classList.toggle('scroll-top', valid && target.scrollTop > 0.5);
        elements.content.classList.toggle('scroll-bottom', valid && target.scrollTop + target.clientHeight + 0.5 < target.scrollHeight);
        elements.content.classList.toggle('scroll-left', valid && target.scrollLeft > 0.5);
        elements.content.classList.toggle('scroll-right', valid && target.scrollLeft + target.clientWidth + 0.5 < target.scrollWidth);
    }

    function renderWindow(force) {
        if (scroller === null || state.rows === null)
            return;
        updateScrollShadows(scroller);
        const rows = state.rows;
        const top = scroller.scrollTop;
        const bottom = top + scroller.clientHeight;
        const firstVisible = diff.rowAtOffset(rows, top);
        const lastVisible = diff.rowAtOffset(rows, bottom);
        if (force !== true && renderedStart >= 0 && firstVisible >= renderedStart && lastVisible < renderedEnd)
            return;
        const start = Math.max(0, firstVisible - OVERSCAN_ROWS);
        const end = Math.min(rows.count, lastVisible + 1 + OVERSCAN_ROWS);
        let html = '';
        for (let row = start; row < end; ++row)
            html += rowHtml(row);
        windowElement.style.top = Math.round(rows.offsets[start]) + 'px';
        windowElement.innerHTML = html;
        renderedStart = start;
        renderedEnd = end;
        measureRenderedCode();
    }

    // 가로 범위는 그려 본 줄의 실제 너비로 정한다. 줄 길이를 글자 수로 어림하면 한글처럼 칸 너비가 글꼴마다
    // 다른 글자에서 틀리고, 그러면 줄이 짧아도 가로 스크롤 막대가 생긴다. 가상 스크롤이라 아직 그리지 않은
    // 긴 줄은 그 줄이 그려질 때 범위가 늘어난다.
    function layoutWidths() {
        const viewport = scroller.clientWidth;
        const needed = state.layout === 'split'
            ? 2 * (GUTTER_SPLIT + measuredCode + CODE_PADDING)
            : GUTTER_INLINE + measuredCode + CODE_PADDING;
        // 뷰포트에 들어가면 너비를 정하지 않는다. 올림한 값이 뷰포트를 1px 넘어 막대가 생기는 일을 막는다.
        sizer.style.width = needed > viewport ? Math.ceil(needed) + 'px' : '';
        elements.content.style.setProperty('--view-w', viewport + 'px');
        updateScrollShadows(scroller);
    }

    // 그려진 코드 칸의 내용 너비를 잰다. 칸 자체의 너비(scrollWidth)는 가로 범위를 따라 커지므로 되먹임이 생긴다.
    // 그래서 내용의 범위(Range)를 잰다.
    const measuringRange = document.createRange();

    function measureRenderedCode() {
        let widest = measuredCode;
        for (const element of windowElement.getElementsByClassName('dl-code')) {
            measuringRange.selectNodeContents(element);
            widest = Math.max(widest, measuringRange.getBoundingClientRect().width);
        }
        if (widest > measuredCode) {
            measuredCode = widest;
            layoutWidths();
        }
    }

    function buildDiffView(preserveScroll) {
        const previousTop = preserveScroll && scroller !== null ? scroller.scrollTop : 0;
        const previousLeft = preserveScroll && scroller !== null ? scroller.scrollLeft : 0;
        state.rows = diff.buildRows(state.model, state.layout);
        changeStarts = diff.changeStartRows(state.model, state.rows);
        currentChange = -1;

        elements.content.className = 'content';
        elements.content.innerHTML = '<div class="diff-scroll" tabindex="0"><div class="diff-sizer"><div class="diff-window"></div></div></div>';
        scroller = elements.content.firstElementChild;
        sizer = scroller.firstElementChild;
        windowElement = sizer.firstElementChild;
        sizer.style.height = Math.ceil(state.rows.height) + 'px';
        sizer.classList.toggle('is-split', state.layout === 'split');
        measuredCode = 0;
        layoutWidths();
        renderedStart = -1;
        renderedEnd = -1;
        scroller.scrollTop = previousTop;
        scroller.scrollLeft = previousLeft;
        renderWindow(true);
    }

    // ------------------------------------------------------------------ 선택

    function clearSelection() {
        if (state.selection === null)
            return;
        state.selection = null;
        state.anchor = null;
        renderWindow(true);
        renderSelectionBar();
    }

    function setSelection(hunkIndex, indexes) {
        state.selection = indexes.length > 0 ? { hunk: hunkIndex, set: new Set(indexes) } : null;
        renderWindow(true);
        renderSelectionBar();
    }

    function selectedIndexes() {
        return state.selection === null ? [] : Array.from(state.selection.set).sort((a, b) => a - b);
    }

    function selectLine(hunkIndex, lineIndex, event) {
        const hunk = state.model.hunks[hunkIndex];
        const sameHunk = state.selection !== null && state.selection.hunk === hunkIndex;
        if (event.shiftKey && state.anchor !== null && state.anchor.hunk === hunkIndex) {
            setSelection(hunkIndex, diff.changedBetween(hunk, state.anchor.index, lineIndex));
        } else if (event.ctrlKey && sameHunk) {
            const next = new Set(state.selection.set);
            if (next.has(lineIndex))
                next.delete(lineIndex);
            else
                next.add(lineIndex);
            state.anchor = { hunk: hunkIndex, index: lineIndex };
            setSelection(hunkIndex, Array.from(next));
        } else {
            state.anchor = { hunk: hunkIndex, index: lineIndex };
            setSelection(hunkIndex, [lineIndex]);
        }
    }

    function renderSelectionBar() {
        const indexes = selectedIndexes();
        if (indexes.length === 0 || state.mode !== 'diff') {
            elements.selection.hidden = true;
            elements.selection.textContent = '';
            return;
        }
        const locked = state.busy;
        let html = '<span class="selbar-count">' + indexes.length + '줄 선택</span>';
        if (state.can.stage) {
            html += button('선택한 줄 stage', 'stage_lines', 'is-primary', locked);
            if (state.can.discard)
                html += button(isArmed('discard_lines', state.selection.hunk) ? '정말 버릴까요?' : '선택한 줄 discard', 'discard_lines', isArmed('discard_lines', state.selection.hunk) ? 'is-danger' : 'is-quiet', locked);
        }
        if (state.can.unstage)
            html += button('선택한 줄 unstage', 'unstage_lines', 'is-primary', locked);
        html += button('복사', 'copy_selection', 'is-quiet') + button('선택 해제', 'clear_selection', 'is-quiet');
        elements.selection.innerHTML = html;
        elements.selection.hidden = false;
    }

    // ------------------------------------------------------------------ 동작

    function isArmed(action, target) {
        return state.armed !== null && state.armed.action === action && state.armed.target === target;
    }

    // discard는 한 번 더 눌러야 실행한다. 되돌릴 수 없는 동작이라 확인 대화 상자 대신 단추가 바뀐다.
    function confirmDiscard(action, target) {
        if (isArmed(action, target))
            return true;
        state.armed = { action, target };
        clearTimeout(armTimer);
        armTimer = setTimeout(() => {
            state.armed = null;
            renderWindow(true);
            renderSelectionBar();
        }, DISCARD_ARM_MILLISECONDS);
        renderWindow(true);
        renderSelectionBar();
        return false;
    }

    function disarm() {
        clearTimeout(armTimer);
        state.armed = null;
    }

    function sendLines(type) {
        const indexes = selectedIndexes();
        if (indexes.length === 0 || state.busy)
            return;
        postAction(type, { hunk: state.selection.hunk, ranges: diff.toRanges(indexes) });
        state.busy = true;
        state.selection = null;
        disarm();
        renderWindow(true);
        renderSelectionBar();
    }

    function sendHunk(type, hunkIndex) {
        if (state.busy)
            return;
        postAction(type, { hunk: hunkIndex });
        state.busy = true;
        disarm();
        renderWindow(true);
        renderSelectionBar();
    }

    function copySelection() {
        const indexes = selectedIndexes();
        if (indexes.length === 0)
            return;
        postAction('copy', { text: diff.selectionText(state.model.hunks[state.selection.hunk], indexes) });
    }

    function handleAction(action, target) {
        switch (action) {
        case 'stage_hunk':
        case 'unstage_hunk':
            sendHunk(action, Number(target.closest('[data-h]').dataset.h));
            break;
        case 'discard_hunk': {
            const hunkIndex = Number(target.closest('[data-h]').dataset.h);
            if (confirmDiscard('discard_hunk', hunkIndex))
                sendHunk('discard_hunk', hunkIndex);
            break;
        }
        case 'stage_lines':
        case 'unstage_lines':
            sendLines(action);
            break;
        case 'discard_lines':
            if (confirmDiscard('discard_lines', state.selection.hunk))
                sendLines('discard_lines');
            break;
        case 'copy_selection':
            copySelection();
            break;
        case 'clear_selection':
            clearSelection();
            break;
        case 'open_external':
            postAction('open_external', {});
            break;
        case 'show_full':
            postAction('show_full', {});
            break;
        default:
            break;
        }
    }

    function setLayout(layout) {
        if (state.layout === layout)
            return;
        state.layout = layout;
        state.selection = null;
        state.anchor = null;
        renderSelectionBar();
        if (state.mode === 'diff')
            buildDiffView(true);
        renderToolbar();
    }

    function setImageMode(mode) {
        if (state.imageMode === mode)
            return;
        state.imageMode = mode;
        if (state.mode === 'image')
            renderImage();
        renderToolbar();
    }

    // ------------------------------------------------------------------ 이미지 비교

    function pickUrl(url) {
        return url ? escapeHtml(url) : '';
    }

    function imageCaption(label, url, size) {
        return '<figcaption class="image-caption"><span class="chip">' + label + '</span><span class="image-meta" data-meta="' + label + '">'
            + (url ? diff.formatBytes(size) : '없음') + '</span></figcaption>';
    }

    function imageElement(url, label) {
        return url ? '<img class="diff-img" alt="' + label + '" src="' + pickUrl(url) + '" draggable="false">' : '';
    }

    function renderImage() {
        const image = state.image;
        elements.content.className = 'content';
        const left = image.leftUrl;
        const right = image.rightUrl;
        let html = '<div class="image-stage">';
        if (state.imageMode === 'side_by_side') {
            html += '<div class="image-pair">'
                + '<figure class="image-pane">' + imageCaption('이전', left, image.leftSize) + '<div class="checker">' + (left ? imageElement(left, '이전') : '<span class="image-none">(없음)</span>') + '</div></figure>'
                + '<figure class="image-pane">' + imageCaption('이후', right, image.rightSize) + '<div class="checker">' + (right ? imageElement(right, '이후') : '<span class="image-none">(없음)</span>') + '</div></figure>'
                + '</div>';
        } else {
            const slider = state.imageMode === 'onion'
                ? '<label class="image-slider"><span>이전</span><input type="range" min="0" max="100" value="' + image.opacity + '" data-onion><span>이후 <b data-onion-label>' + image.opacity + '%</b></span></label>'
                : '<div class="image-hint">손잡이를 끌거나 ← → 로 옮깁니다 (' + '<b data-swipe-label>' + image.split + '%</b>)</div>';
            html += '<div class="image-pair is-single"><figure class="image-pane">'
                + '<div class="checker is-stack' + (state.imageMode === 'swipe' ? ' is-slide' : '') + '" data-stack>'
                + (left ? '<span class="diff-img-wrap is-left">' + imageElement(left, '이전') + '</span>' : '')
                + (right ? '<span class="diff-img-wrap is-right" data-right>' + imageElement(right, '이후') + '</span>' : '')
                + (state.imageMode === 'swipe' && left && right ? '<span class="slide-handle" tabindex="0" role="slider" aria-label="이전/이후 경계" data-handle><span class="slide-grip">⇔</span></span>' : '')
                + '</div>'
                + '<figcaption class="image-caption">' + '<span class="chip">이전</span><span class="image-meta">' + (left ? diff.formatBytes(image.leftSize) : '없음') + '</span>'
                + '<span class="chip">이후</span><span class="image-meta">' + (right ? diff.formatBytes(image.rightSize) : '없음') + '</span></figcaption>'
                + '</figure>' + slider + '</div>';
        }
        html += '</div>';
        elements.content.innerHTML = html;
        scroller = null;
        updateScrollShadows(null);
        applyStackState();
        for (const element of elements.content.querySelectorAll('img.diff-img')) {
            element.addEventListener('load', () => {
                const meta = element.closest('figure').querySelector('.image-meta');
                if (meta && state.imageMode === 'side_by_side')
                    meta.textContent += ' · ' + element.naturalWidth + '×' + element.naturalHeight;
            });
            element.addEventListener('error', () => {
                element.replaceWith(Object.assign(document.createElement('span'), { className: 'image-none', textContent: '이미지를 읽지 못했습니다' }));
            });
        }
    }

    function applyStackState() {
        const right = elements.content.querySelector('[data-right]');
        const handle = elements.content.querySelector('[data-handle]');
        if (right === null)
            return;
        const image = state.image;
        if (state.imageMode === 'onion') {
            right.style.opacity = String(image.opacity / 100);
            right.style.clipPath = '';
        } else if (state.imageMode === 'swipe') {
            right.style.opacity = '';
            right.style.clipPath = 'inset(0 0 0 ' + image.split + '%)';
            if (handle) {
                handle.style.left = image.split + '%';
                handle.setAttribute('aria-valuenow', String(image.split));
            }
        }
    }

    function moveSwipe(clientX) {
        const stack = elements.content.querySelector('[data-stack]');
        if (stack === null)
            return;
        const box = stack.getBoundingClientRect();
        state.image.split = Math.max(0, Math.min(100, Math.round(((clientX - box.left) / box.width) * 100)));
        applyStackState();
        const label = elements.content.querySelector('[data-swipe-label]');
        if (label)
            label.textContent = state.image.split + '%';
    }

    // ------------------------------------------------------------------ 바이너리 카드

    function renderBinary() {
        const binary = state.binary;
        elements.content.className = 'content';
        let sizes;
        if (binary.oldSize === null && binary.newSize !== null)
            sizes = '새 파일 · ' + diff.formatBytes(binary.newSize);
        else if (binary.newSize === null && binary.oldSize !== null)
            sizes = '삭제됨 · ' + diff.formatBytes(binary.oldSize);
        else {
            const delta = binary.oldSize !== null && binary.newSize !== null ? binary.newSize - binary.oldSize : null;
            sizes = diff.formatBytes(binary.oldSize) + ' → ' + diff.formatBytes(binary.newSize)
                + (delta === null ? '' : ' (' + (delta >= 0 ? '+' : '−') + diff.formatBytes(Math.abs(delta)) + ')');
        }
        elements.content.innerHTML = '<div class="card-wrap"><div class="card">'
            + '<div class="card-icon" aria-hidden="true">▣</div>'
            + '<div class="card-title">바이너리 파일 · 크기 변화</div>'
            + '<div class="card-path">' + escapeHtml(state.path) + '</div>'
            + '<div class="card-sizes">' + escapeHtml(sizes) + '</div>'
            + (binary.canOpenExternal ? button('외부 도구로 열기', 'open_external', 'is-primary') : '')
            + '</div></div>';
        scroller = null;
        updateScrollShadows(null);
    }

    function renderEmpty(text) {
        elements.content.className = 'content';
        elements.content.innerHTML = '<div class="empty">' + escapeHtml(text) + '</div>';
        scroller = null;
        updateScrollShadows(null);
        state.rows = null;
    }

    // ------------------------------------------------------------------ C++ 메시지

    function applyState(message) {
        if (message.theme)
            applyTheme(message.theme);
        if (message.layout)
            state.layout = message.layout;
        if (message.image_mode)
            state.imageMode = message.image_mode;
        if (typeof message.toolbar === 'boolean')
            state.toolbar = message.toolbar;
    }

    function resetTransient() {
        state.selection = null;
        state.anchor = null;
        state.busy = false;
        disarm();
        highlightCache = new Map();
        wordCache = new Map();
        renderSelectionBar();
    }

    function showDiff(message) {
        const sameFile = state.mode === 'diff' && state.file !== null && state.view === message.view
            && state.path === (message.file.new_path || message.file.old_path);
        const keepScroll = sameFile;
        resetTransient();
        state.mode = 'diff';
        state.view = message.view;
        state.can = { stage: message.can_stage, unstage: message.can_unstage, discard: message.can_discard };
        state.file = message.file;
        state.path = message.file.new_path || message.file.old_path;
        state.language = diff.languageFor(state.path);
        state.truncated = message.truncated;
        state.totalLines = message.total_lines;
        state.shownLines = message.shown_lines;
        state.model = diff.buildModel(message.file);
        showNotice();
        if (state.model.hunks.length === 0) {
            renderEmpty('변경 사항이 없습니다');
            state.rows = null;
        } else {
            buildDiffView(keepScroll);
        }
        renderToolbar();
    }

    function showImage(message) {
        resetTransient();
        state.mode = 'image';
        state.path = message.path;
        state.file = { old_path: message.path, new_path: message.path, status: message.status };
        state.rows = null;
        state.image = {
            leftUrl: message.left_url, rightUrl: message.right_url, leftSize: message.left_size, rightSize: message.right_size,
            canOpenExternal: message.can_open_external, opacity: 50, split: 50,
        };
        showNotice();
        renderImage();
        renderToolbar();
    }

    function showBinary(message) {
        resetTransient();
        state.mode = 'binary';
        state.path = message.path;
        state.file = { old_path: message.path, new_path: message.path, status: message.status };
        state.rows = null;
        state.binary = { oldSize: message.old_size, newSize: message.new_size, canOpenExternal: message.can_open_external };
        showNotice();
        renderBinary();
        renderToolbar();
    }

    function clearView() {
        resetTransient();
        state.mode = 'empty';
        state.file = null;
        state.path = '';
        state.rows = null;
        showNotice();
        renderEmpty('보일 변경이 없습니다');
        renderToolbar();
    }

    function receive(message) {
        if (message === null || typeof message !== 'object' || typeof message.seq !== 'number')
            return;
        // 늦게 도착한 옛 메시지는 버린다. 같은 seq의 중복도 버린다.
        if (message.seq <= state.appliedSeq)
            return;
        state.appliedSeq = message.seq;
        switch (message.type) {
        case 'show_diff':
            applyState(message);
            state.contentSeq = message.seq;
            showDiff(message);
            break;
        case 'show_image':
            applyState(message);
            state.contentSeq = message.seq;
            showImage(message);
            break;
        case 'show_binary':
            applyState(message);
            state.contentSeq = message.seq;
            showBinary(message);
            break;
        case 'clear':
            applyState(message);
            state.contentSeq = message.seq;
            clearView();
            break;
        case 'set_layout':
            setLayout(message.layout);
            break;
        case 'set_theme':
            applyTheme(message.theme);
            break;
        case 'set_image_mode':
            setImageMode(message.mode);
            break;
        default:
            break;
        }
    }

    // ------------------------------------------------------------------ 입력

    function navigateChange(direction) {
        if (changeStarts.length === 0 || scroller === null)
            return;
        currentChange = Math.max(0, Math.min(changeStarts.length - 1, currentChange + direction));
        if (currentChange < 0)
            currentChange = 0;
        const target = state.rows.offsets[changeStarts[currentChange]];
        scroller.scrollTop = Math.max(0, target - scroller.clientHeight / 3);
        renderWindow(false);
    }

    function lineElementFromPoint(x, y) {
        const target = document.elementFromPoint(x, y);
        return target === null ? null : target.closest('.dl-half, .dl');
    }

    elements.content.addEventListener('scroll', (event) => {
        updateScrollShadows(event.target);
        renderWindow(false);
    }, true);

    elements.content.addEventListener('pointerdown', (event) => {
        const number = event.target.closest('.dl-no.is-selectable');
        if (number !== null && event.button === 0) {
            const hunkIndex = Number(number.dataset.h);
            const lineIndex = Number(number.dataset.i);
            selectLine(hunkIndex, lineIndex, event);
            if (event.shiftKey === false && event.ctrlKey === false)
                dragging = { hunk: hunkIndex, anchor: lineIndex };
            event.preventDefault();
            return;
        }
        const handle = event.target.closest('[data-stack].is-slide');
        if (handle !== null && event.button === 0) {
            dragging = { swipe: true };
            handle.setPointerCapture(event.pointerId);
            moveSwipe(event.clientX);
        }
    });

    elements.content.addEventListener('pointermove', (event) => {
        if (dragging === null)
            return;
        if (dragging.swipe) {
            moveSwipe(event.clientX);
            return;
        }
        const row = lineElementFromPoint(event.clientX, event.clientY);
        const number = row === null ? null : row.querySelector('.dl-no[data-h]');
        if (number === null || Number(number.dataset.h) !== dragging.hunk)
            return;
        const indexes = diff.changedBetween(state.model.hunks[dragging.hunk], dragging.anchor, Number(number.dataset.i));
        if (indexes.length > 0 && (state.selection === null || indexes.length !== state.selection.set.size || indexes[0] !== selectedIndexes()[0]))
            setSelection(dragging.hunk, indexes);
    });

    window.addEventListener('pointerup', () => {
        dragging = null;
    });

    elements.content.addEventListener('input', (event) => {
        if (event.target.matches('[data-onion]')) {
            state.image.opacity = Number(event.target.value);
            applyStackState();
            const label = elements.content.querySelector('[data-onion-label]');
            if (label)
                label.textContent = state.image.opacity + '%';
        }
    });

    elements.content.addEventListener('keydown', (event) => {
        if (event.target.matches('[data-handle]') && (event.key === 'ArrowLeft' || event.key === 'ArrowRight')) {
            state.image.split = Math.max(0, Math.min(100, state.image.split + (event.key === 'ArrowLeft' ? -5 : 5)));
            applyStackState();
            const label = elements.content.querySelector('[data-swipe-label]');
            if (label)
                label.textContent = state.image.split + '%';
            event.preventDefault();
        }
    });

    function onClick(event) {
        const actionElement = event.target.closest('[data-act]');
        if (actionElement !== null && actionElement.disabled !== true) {
            handleAction(actionElement.dataset.act, actionElement);
            return;
        }
        const layoutElement = event.target.closest('[data-layout]');
        if (layoutElement !== null) {
            setLayout(layoutElement.dataset.layout);
            post({ type: 'layout_changed', layout: state.layout });
            return;
        }
        const modeElement = event.target.closest('[data-image-mode]');
        if (modeElement !== null) {
            setImageMode(modeElement.dataset.imageMode);
            post({ type: 'image_mode_changed', mode: state.imageMode });
        }
    }
    elements.toolbar.addEventListener('click', onClick);
    elements.notice.addEventListener('click', onClick);
    elements.content.addEventListener('click', onClick);
    elements.selection.addEventListener('click', onClick);

    document.addEventListener('keydown', (event) => {
        if (event.key === 'Escape') {
            clearSelection();
            disarm();
        } else if (event.key === 'F7') {
            navigateChange(event.shiftKey ? -1 : 1);
            event.preventDefault();
        } else if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'c' && state.selection !== null) {
            copySelection();
            event.preventDefault();
        }
    });

    new ResizeObserver(() => {
        if (state.mode === 'diff' && scroller !== null && state.model !== null) {
            layoutWidths();
            renderWindow(true);
        }
    }).observe(elements.content);

    // ------------------------------------------------------------------ 시작

    if (host)
        host.addEventListener('message', (event) => receive(event.data));

    window.CyellaDiff.viewer = { receive, state };

    renderEmpty('diff를 기다리는 중입니다');
    renderToolbar();
    post({ type: 'ready', protocol: PROTOCOL_VERSION });
})();
