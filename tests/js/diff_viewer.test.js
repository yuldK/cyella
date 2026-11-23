// diff 뷰어(assets/diff-viewer)의 순수 계산을 시험한다. DOM 없이 node로 돌린다.
//   node --test tests/js
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const viewerDirectory = path.join(__dirname, '..', '..', 'assets', 'diff-viewer');
const wordDiff = require(path.join(viewerDirectory, 'word-diff.js'));
const syntax = require(path.join(viewerDirectory, 'syntax.js'));
const model = require(path.join(viewerDirectory, 'model.js'));
const blame = require(path.join(viewerDirectory, 'blame.js'));

// 번들한 highlight.js는 전역 window에 hljs를 단다.
globalThis.window = globalThis;
vm.runInThisContext(fs.readFileSync(path.join(viewerDirectory, 'vendor', 'highlight.min.js'), 'utf8'));

function text(ranges, source) {
    return ranges.map(([start, end]) => source.slice(start, end));
}

test('wordDiff marks only the words that changed', () => {
    const result = wordDiff.wordDiff('lanes_.resize(max_lanes_);', 'lanes_.reserve(max_lanes_);');
    assert.ok(result);
    assert.deepEqual(text(result.old, 'lanes_.resize(max_lanes_);'), ['resize']);
    assert.deepEqual(text(result.new, 'lanes_.reserve(max_lanes_);'), ['reserve']);
});

test('wordDiff finds an insertion and joins ranges split by one space', () => {
    const before = 'if (a == b) return;';
    const after = 'if (a == b && c) return;';
    const result = wordDiff.wordDiff(before, after);
    assert.ok(result);
    assert.deepEqual(result.old, []);
    assert.equal(text(result.new, after).join('').replace(/\s+/g, ' ').trim(), '&& c');
});

test('wordDiff gives up on unrelated lines and blank lines', () => {
    assert.equal(wordDiff.wordDiff('aaaa bbbb cccc', 'xxxx yyyy zzzz'), null);
    assert.equal(wordDiff.wordDiff('', 'x'), null);
    assert.equal(wordDiff.wordDiff('same', 'same'), null);
});

test('wordDiff works with Hangul and surrogate pairs by UTF-16 offsets', () => {
    const before = '한글 설명 😀 끝';
    const after = '한글 해설 😀 끝';
    const result = wordDiff.wordDiff(before, after);
    assert.ok(result);
    assert.deepEqual(text(result.old, before), ['설명']);
    assert.deepEqual(text(result.new, after), ['해설']);
});

test('wordDiff falls back to common ends for very long lines', () => {
    const left = 'x '.repeat(500) + 'LEFT' + ' y'.repeat(500);
    const right = 'x '.repeat(500) + 'RIGHT' + ' y'.repeat(500);
    const result = wordDiff.wordDiff(left, right);
    assert.ok(result);
    assert.deepEqual(text(result.new, right), ['RIGHT']);
});

test('languageFor picks by extension and special names', () => {
    assert.equal(syntax.languageFor('src/a.cpp'), 'cpp');
    assert.equal(syntax.languageFor('src/a.H'), 'cpp');
    assert.equal(syntax.languageFor('CMakeLists.txt'), 'cmake');
    assert.equal(syntax.languageFor('dir\\x.cmake'), 'cmake');
    assert.equal(syntax.languageFor('a/b.ps1'), 'powershell');
    assert.equal(syntax.languageFor('a.cs'), 'csharp');
    assert.equal(syntax.languageFor('a.md'), 'markdown');
    assert.equal(syntax.languageFor('index.html'), 'xml');
    assert.equal(syntax.languageFor('a.sh'), 'bash');
    assert.equal(syntax.languageFor('a.unknownext'), null);
    assert.equal(syntax.languageFor('noextension'), null);
    assert.equal(syntax.languageFor('toString'), null);
});

test('splitHighlighted keeps spans balanced on every line', () => {
    const lines = syntax.splitHighlighted('<span class="a">one\ntwo</span>\nthree');
    assert.deepEqual(lines, ['<span class="a">one</span>', '<span class="a">two</span>', 'three']);
});

test('highlightLines carries a block comment across lines', () => {
    const lines = syntax.highlightLines(['int a; /* start', 'middle', 'end */ int b;'], 'cpp');
    assert.equal(lines.length, 3);
    assert.match(lines[1], /hljs-comment/);
    for (const line of lines) {
        const opens = (line.match(/<span/g) || []).length;
        const closes = (line.match(/<\/span>/g) || []).length;
        assert.equal(opens, closes);
    }
});

test('highlightLines escapes html when the language is unknown', () => {
    assert.deepEqual(syntax.highlightLines(['<b>&</b>'], null), ['&lt;b&gt;&amp;&lt;/b&gt;']);
    assert.deepEqual(syntax.highlightLines(['<b>'], 'no-such-language'), ['&lt;b&gt;']);
});

test('every language in the bundle highlights', () => {
    for (const language of ['c', 'cpp', 'cmake', 'javascript', 'typescript', 'json', 'python', 'markdown', 'xml', 'css', 'powershell', 'bash', 'csharp', 'ini', 'yaml'])
        assert.ok(globalThis.hljs.getLanguage(language), language);
});

test('applyMarks wraps the range in text positions, not html positions', () => {
    assert.equal(syntax.applyMarks('a &amp; b', [[2, 3]]), 'a <span class="is-strong">&amp;</span> b');
    assert.equal(syntax.applyMarks('hello', [[1, 3]]), 'h<span class="is-strong">el</span>lo');
    assert.equal(syntax.applyMarks('plain', []), 'plain');
});

test('applyMarks closes and reopens the mark around highlight tags', () => {
    const html = '<span class="k">int</span> x';
    const marked = syntax.applyMarks(html, [[1, 5]]);
    const opens = (marked.match(/<span/g) || []).length;
    const closes = (marked.match(/<\/span>/g) || []).length;
    assert.equal(opens, closes);
    assert.equal(marked.replace(/<[^>]+>/g, ''), 'int x');
});

test('applyMarks counts a surrogate pair as two positions', () => {
    assert.equal(syntax.applyMarks('😀a', [[2, 3]]), '😀<span class="is-strong">a</span>');
});

test('decodeLine restores raw bytes and splits the carriage return', () => {
    assert.deepEqual(model.decodeLine(['a', 0, 3, 'abc\r']), { kind: 'a', old: 0, new: 3, text: 'abc', cr: true, invalid: false });
    const bad = model.decodeLine(['c', 1, 1, Buffer.from([0x61, 0xff, 0x62]).toString('base64'), 1]);
    assert.equal(bad.text, 'a�b');
    assert.equal(bad.invalid, true);
});

function hunk(lines) {
    return { old_start: 1, old_count: 1, new_start: 1, new_count: 1, section: '', lines };
}

const sample = {
    hunks: [hunk([
        ['c', 1, 1, 'ctx'],
        ['d', 2, 0, 'old one'],
        ['d', 3, 0, 'old two'],
        ['a', 0, 2, 'new one'],
        ['a', 0, 3, 'new two'],
        ['a', 0, 4, 'new three'],
        ['c', 4, 5, 'tail'],
        ['d', 5, 0, 'last'],
        ['n', 0, 0, 'No newline at end of file'],
    ])],
};

test('pairChangedLines pairs deletions with additions in order', () => {
    const built = model.buildModel(sample);
    assert.deepEqual(Array.from(built.hunks[0].partner), [-1, 3, 4, 1, 2, -1, -1, -1, -1]);
});

test('inline rows list every line under its hunk header', () => {
    const rows = model.buildRows(model.buildModel(sample), 'inline');
    assert.equal(rows.count, 1 + 9);
    assert.equal(rows.types[0], model.ROW_HUNK);
    assert.equal(rows.types[9], model.ROW_NOTE);
    assert.equal(rows.height, model.HUNK_HEIGHT + 9 * model.LINE_HEIGHT);
});

test('split rows place deletions beside additions and notes after the block', () => {
    const rows = model.buildRows(model.buildModel(sample), 'split');
    // hunk, ctx, (2 del | 3 add)x3, ctx, del|-, note
    assert.equal(rows.count, 1 + 1 + 3 + 1 + 1 + 1);
    assert.deepEqual([rows.first[2], rows.second[2]], [1, 3]);
    assert.deepEqual([rows.first[3], rows.second[3]], [2, 4]);
    assert.deepEqual([rows.first[4], rows.second[4]], [-1, 5]);
    assert.equal(rows.types[rows.count - 1], model.ROW_NOTE);
});

test('rowAtOffset finds the row that covers a y position', () => {
    const rows = model.buildRows(model.buildModel(sample), 'inline');
    assert.equal(model.rowAtOffset(rows, 0), 0);
    assert.equal(model.rowAtOffset(rows, model.HUNK_HEIGHT - 1), 0);
    assert.equal(model.rowAtOffset(rows, model.HUNK_HEIGHT), 1);
    assert.equal(model.rowAtOffset(rows, rows.height + 1000), rows.count - 1);
});

test('selection helpers keep only changed lines and compress runs', () => {
    const built = model.buildModel(sample);
    const picked = model.changedBetween(built.hunks[0], 6, 1);
    assert.deepEqual(picked, [1, 2, 3, 4, 5]);
    assert.deepEqual(model.toRanges([1, 2, 3, 5, 7]), [[1, 3], [5, 5], [7, 7]]);
    assert.equal(model.selectionText(built.hunks[0], [1, 3]), 'old one\nnew one');
});

test('changeStartRows lists where each block of changes begins', () => {
    const built = model.buildModel(sample);
    assert.deepEqual(model.changeStartRows(built, model.buildRows(built, 'inline')), [2, 8]);
    assert.deepEqual(model.changeStartRows(built, model.buildRows(built, 'split')), [2, 6]);
});

test('rows for a very large file build quickly', () => {
    const lines = [];
    for (let index = 0; index < 60000; ++index)
        lines.push(index % 7 === 0 ? ['a', 0, index, 'added line ' + index] : ['c', index, index, 'context line ' + index]);
    const big = { hunks: [hunk(lines)] };
    const started = Date.now();
    const built = model.buildModel(big);
    const inline = model.buildRows(built, 'inline');
    const split = model.buildRows(built, 'split');
    const elapsed = Date.now() - started;
    assert.equal(inline.count, 60001);
    assert.equal(split.count, 60001);
    assert.ok(elapsed < 3000, 'took ' + elapsed + ' ms');
});

test('a small state message for content the page never got asks for a resync', () => {
    // 내용 메시지(seq 5)가 건너뛰어져 페이지는 아직 seq 3의 내용을 보이고 있다.
    assert.equal(model.needsResync({ type: 'set_theme', seq: 6, content_seq: 5 }, 3), true);
    assert.equal(model.needsResync({ type: 'set_layout', seq: 6, content_seq: 3 }, 3), false);
    // content_seq가 없는 메시지(옛 C++·수동 하네스)는 그대로 받아들인다.
    assert.equal(model.needsResync({ type: 'set_image_mode', seq: 6 }, 3), false);
});

test('formatBytes picks a unit', () => {
    assert.equal(model.formatBytes(512), '512 B');
    assert.equal(model.formatBytes(2048), '2.0 KB');
    assert.equal(model.formatBytes(3 * 1024 * 1024), '3.00 MB');
    assert.equal(model.formatBytes(null), '-');
});

// ------------------------------------------------------------------ blame

const FIRST_SHA = '50e889a1c2d3e4f5061728394a5b6c7d8e9f0a1b';
const SECOND_SHA = '9b1c7e4402d3e4f5061728394a5b6c7d8e9f0a1b';
const ZERO_SHA = '0'.repeat(40);

function blameMessage(lines, extra) {
    return Object.assign({
        type: 'show_blame', seq: 12, path: 'notes.txt', revision: '',
        origins: [
            { sha: FIRST_SHA, author: 'Morgan', mail: 'morgan@example.com', time: 1767574800, summary: 'docs: first note', path: 'notes.txt', previous: false, uncommitted: false },
            { sha: SECOND_SHA, author: 'Riley', mail: 'riley@example.com', time: 1767661200, summary: 'docs: second line', path: 'old-notes.txt', previous: true, uncommitted: false },
            { sha: ZERO_SHA, author: 'Not Committed Yet', mail: 'not.committed.yet', time: 1767747600, summary: '', path: 'notes.txt', previous: true, uncommitted: true },
        ],
        lines, truncated: false, total_lines: lines.length, focus: '',
    }, extra);
}

test('buildBlameModel decodes lines and marks where each run of one origin starts', () => {
    const built = blame.buildBlameModel(blameMessage([
        [0, 1, 'alpha'], [0, 2, 'alpha two'], [1, 3, 'beta'], [0, 4, 'alpha again'], [2, 5, 'draft'], [2, 6, 'draft two'],
    ]));
    assert.equal(built.count, 6);
    assert.deepEqual(Array.from(built.lineOrigins), [0, 0, 1, 0, 2, 2]);
    assert.deepEqual(Array.from(built.numbers), [1, 2, 3, 4, 5, 6]);
    assert.deepEqual(built.texts, ['alpha', 'alpha two', 'beta', 'alpha again', 'draft', 'draft two']);
    // 같은 origin이 떨어져 다시 나오면 새 묶음이다.
    assert.deepEqual(Array.from(built.starts), [1, 0, 1, 1, 1, 0]);
    assert.deepEqual(Array.from(built.first), [0, 2, 4]);
    assert.equal(built.origins[1].previous, true);
    assert.equal(built.origins[2].uncommitted, true);
});

test('buildBlameModel restores base64 lines and splits the carriage return', () => {
    const built = blame.buildBlameModel(blameMessage([
        [0, 1, 'crlf line\r'],
        [0, 2, Buffer.from([0x61, 0xff, 0x62]).toString('base64'), 1],
        [0, 3, Buffer.from([0xfe, 0x0d]).toString('base64'), 1],
        [0, 4, 'YWJj'],
    ]));
    assert.deepEqual(built.texts, ['crlf line', 'a�b', '�', 'YWJj']);
    assert.deepEqual(Array.from(built.cr), [1, 0, 1, 0]);
});

test('buildBlameModel survives origins that are missing or out of range', () => {
    const built = blame.buildBlameModel({ lines: [[0, 1, 'a'], [5, 2, 'b'], [-1, 3, 'c'], [0, 4, 'd']], origins: [{ sha: FIRST_SHA }] });
    assert.deepEqual(Array.from(built.lineOrigins), [0, -1, -1, 0]);
    assert.deepEqual(Array.from(built.starts), [1, 1, 0, 1]);
    assert.deepEqual(built.origins[0], { sha: FIRST_SHA, author: '', mail: '', time: null, summary: '', path: '', previous: false, uncommitted: false });
    const empty = blame.buildBlameModel({});
    assert.equal(empty.count, 0);
    assert.equal(empty.origins.length, 0);
});

test('blame rows are equal lines that the window lookup can search', () => {
    const rows = blame.buildBlameRows(5);
    assert.equal(rows.count, 5);
    assert.equal(rows.height, 5 * model.LINE_HEIGHT);
    assert.equal(model.rowAtOffset(rows, 0), 0);
    assert.equal(model.rowAtOffset(rows, model.LINE_HEIGHT * 3 + 1), 3);
    assert.equal(model.rowAtOffset(rows, 100000), 4);
    assert.equal(blame.buildBlameRows(0).height, 0);
});

test('blame dates are written in the local time zone', () => {
    // 시간대가 어디든 같은 결과가 나오도록 이 컴퓨터의 시각으로 초를 만든다.
    const seconds = (...parts) => new Date(...parts).getTime() / 1000;
    assert.equal(blame.formatBlameDate(seconds(2026, 0, 5, 9, 7)), '2026-01-05');
    assert.equal(blame.formatBlameDateTime(seconds(2026, 0, 5, 9, 7)), '2026-01-05 09:07');
    assert.equal(blame.formatBlameDate(seconds(2025, 11, 31, 23, 59, 59)), '2025-12-31');
    assert.equal(blame.formatBlameDateTime(seconds(2026, 9, 15, 0, 0)), '2026-10-15 00:00');
    assert.equal(blame.formatBlameDate(null), '');
    assert.equal(blame.formatBlameDate(Number.NaN), '');
    assert.equal(blame.formatBlameDateTime(undefined), '');
});

test('shortSha keeps seven letters', () => {
    assert.equal(blame.shortSha(FIRST_SHA), '50e889a');
    assert.equal(blame.shortSha('abc'), 'abc');
    assert.equal(blame.shortSha('f'.repeat(64)), 'fffffff');
});

test('the blame tooltip lists the sha, author, time and summary', () => {
    const origins = blame.buildBlameModel(blameMessage([])).origins;
    assert.deepEqual(blame.blameTooltipLines(origins[0], 'notes.txt'),
        [FIRST_SHA, 'Morgan <morgan@example.com>', blame.formatBlameDateTime(1767574800), 'docs: first note']);
    // 그 커밋에서 파일 이름이 달랐으면 그 경로도 적는다.
    assert.equal(blame.blameTooltipLines(origins[1], 'notes.txt').at(-1), 'old-notes.txt');
    // 꺾쇠가 붙어 온 메일은 한 번만 두르고, 빈 값의 줄은 뺀다.
    const bare = blame.normalizeOrigin({ sha: SECOND_SHA, author: 'Riley', mail: '<riley@example.com>' });
    assert.deepEqual(blame.blameTooltipLines(bare, 'notes.txt'), [SECOND_SHA, 'Riley <riley@example.com>']);
    assert.equal(blame.blameAuthorLine(blame.normalizeOrigin({ author: 'Riley' })), 'Riley');
});

test('findBlameOrigin matches a full sha first and then an abbreviated one', () => {
    const origins = blame.buildBlameModel(blameMessage([])).origins;
    assert.equal(blame.findBlameOrigin(origins, SECOND_SHA), 1);
    assert.equal(blame.findBlameOrigin(origins, '9b1c7e4'), 1);
    assert.equal(blame.findBlameOrigin(origins, '9b1c'), -1);
    assert.equal(blame.findBlameOrigin(origins, ''), -1);
    assert.equal(blame.findBlameOrigin(origins, 'f'.repeat(40)), -1);
    assert.equal(blame.findBlameOrigin(origins, undefined), -1);
});

test('the picked blame origin toggles and follows its sha into a new message', () => {
    assert.equal(blame.toggleBlameOrigin(-1, 2), 2);
    assert.equal(blame.toggleBlameOrigin(2, 2), -1);
    assert.equal(blame.toggleBlameOrigin(2, 0), 0);

    const before = blame.buildBlameModel(blameMessage([])).origins;
    // 다시 온 메시지에서는 origin의 순서가 달라졌다.
    const after = [before[2], before[1], before[0]];
    assert.equal(blame.matchBlameOrigin(after, before[0]), 2);
    assert.equal(blame.matchBlameOrigin(after, before[1]), 1);
    // 커밋하지 않은 origin은 SHA가 아니라 표지로 잇는다.
    assert.equal(blame.matchBlameOrigin(after, before[2]), 0);
    assert.equal(blame.matchBlameOrigin([before[0]], before[1]), -1);
    assert.equal(blame.matchBlameOrigin([before[0]], before[2]), -1);
    assert.equal(blame.matchBlameOrigin(after, undefined), -1);
});

test('focus applies to new content and to a changed focus, not to a repeated message', () => {
    // 새 내용이다(앞에 같은 파일이 없었다).
    assert.equal(blame.shouldApplyBlameFocus(FIRST_SHA, null), true);
    assert.equal(blame.shouldApplyBlameFocus('', null), false);
    // 같은 파일이 같은 focus로 다시 왔다(재동기화). 스크롤을 끌고 가지 않는다.
    assert.equal(blame.shouldApplyBlameFocus(FIRST_SHA, FIRST_SHA), false);
    // 같은 파일인데 focus가 바뀌었다.
    assert.equal(blame.shouldApplyBlameFocus(SECOND_SHA, FIRST_SHA), true);
    assert.equal(blame.shouldApplyBlameFocus(SECOND_SHA, ''), true);
    assert.equal(blame.shouldApplyBlameFocus('', FIRST_SHA), false);
});

test('a blame of a hundred thousand lines builds quickly', () => {
    const lines = [];
    for (let index = 0; index < 100000; ++index)
        lines.push([Math.floor(index / 37) % 3, index + 1, '    const auto value_' + index + ' { compute(' + index + ') };']);
    const started = Date.now();
    const built = blame.buildBlameModel(blameMessage(lines));
    const rows = blame.buildBlameRows(built.count);
    const elapsed = Date.now() - started;
    assert.equal(rows.count, 100000);
    assert.equal(built.starts.reduce((sum, value) => sum + value, 0), Math.ceil(100000 / 37));
    assert.ok(elapsed < 3000, 'took ' + elapsed + ' ms');
});
