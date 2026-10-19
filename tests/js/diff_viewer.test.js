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

test('formatBytes picks a unit', () => {
    assert.equal(model.formatBytes(512), '512 B');
    assert.equal(model.formatBytes(2048), '2.0 KB');
    assert.equal(model.formatBytes(3 * 1024 * 1024), '3.00 MB');
    assert.equal(model.formatBytes(null), '-');
});
