// diff 메시지를 화면 행으로 바꾸는 순수 계산이다. DOM을 만지지 않으므로 node에서도 시험한다.
'use strict';

(function (root) {
    const LINE_HEIGHT = 20;
    const HUNK_HEIGHT = 30;

    // 행 종류다.
    const ROW_HUNK = 0;
    // 한 줄 보기의 줄(a)이다.
    const ROW_LINE = 1;
    // 분할 보기의 줄 한 쌍이다. a는 옛쪽 줄, b는 새쪽 줄이고 없으면 -1이다.
    const ROW_PAIR = 2;
    // `\ No newline at end of file` 표지다. a가 그 표지 줄이다.
    const ROW_NOTE = 3;

    const decoder = new TextDecoder('utf-8');

    function decodeBase64(text) {
        const binary = atob(text);
        const bytes = new Uint8Array(binary.length);
        for (let index = 0; index < binary.length; ++index)
            bytes[index] = binary.charCodeAt(index);
        return bytes;
    }

    // 메시지의 줄 하나 `[종류, 옛 번호, 새 번호, 내용(, 1)]`을 푼다. 다섯째 값이 1이면 내용은 base64 바이트이며
    // (올바른 UTF-8이 아니다) 올바르지 않은 바이트는 U+FFFD로 보인다. 줄 끝 CR은 떼고 cr로 따로 둔다.
    function decodeLine(entry) {
        let text = entry[3];
        let invalid = false;
        if (entry.length > 4 && entry[4] === 1) {
            text = decoder.decode(decodeBase64(text));
            invalid = true;
        }
        let cr = false;
        if (text.endsWith('\r')) {
            cr = true;
            text = text.slice(0, -1);
        }
        return { kind: entry[0], old: entry[1], new: entry[2], text, cr, invalid };
    }

    // 삭제 줄 덩어리와 바로 뒤 추가 줄 덩어리를 차례대로 짝짓는다. partner[i]는 짝 줄의 위치이고 없으면 -1이다.
    function pairChangedLines(lines) {
        const partner = new Int32Array(lines.length).fill(-1);
        let index = 0;
        while (index < lines.length) {
            if (lines[index].kind !== 'd') {
                ++index;
                continue;
            }
            const deleted = [];
            while (index < lines.length && (lines[index].kind === 'd' || lines[index].kind === 'n')) {
                if (lines[index].kind === 'd')
                    deleted.push(index);
                ++index;
            }
            const added = [];
            while (index < lines.length && (lines[index].kind === 'a' || lines[index].kind === 'n')) {
                if (lines[index].kind === 'a')
                    added.push(index);
                ++index;
            }
            const count = Math.min(deleted.length, added.length);
            for (let pair = 0; pair < count; ++pair) {
                partner[deleted[pair]] = added[pair];
                partner[added[pair]] = deleted[pair];
            }
        }
        return partner;
    }

    function buildModel(file) {
        const hunks = file.hunks.map((hunk) => {
            const lines = hunk.lines.map(decodeLine);
            return {
                oldStart: hunk.old_start,
                oldCount: hunk.old_count,
                newStart: hunk.new_start,
                newCount: hunk.new_count,
                section: hunk.section,
                lines,
                partner: pairChangedLines(lines),
            };
        });
        return { hunks };
    }

    // 모델을 행으로 펼친다. layout은 'inline'이나 'split'이다.
    function buildRows(model, layout) {
        const types = [];
        const hunkIndexes = [];
        const first = [];
        const second = [];
        const push = (type, hunk, a, b) => {
            types.push(type);
            hunkIndexes.push(hunk);
            first.push(a);
            second.push(b);
        };

        model.hunks.forEach((hunk, hunkIndex) => {
            push(ROW_HUNK, hunkIndex, -1, -1);
            const lines = hunk.lines;
            if (layout !== 'split') {
                for (let index = 0; index < lines.length; ++index)
                    push(lines[index].kind === 'n' ? ROW_NOTE : ROW_LINE, hunkIndex, index, -1);
                return;
            }

            let index = 0;
            while (index < lines.length) {
                const line = lines[index];
                if (line.kind === 'c') {
                    push(ROW_PAIR, hunkIndex, index, index);
                    ++index;
                    continue;
                }
                if (line.kind === 'n') {
                    push(ROW_NOTE, hunkIndex, index, -1);
                    ++index;
                    continue;
                }
                // 삭제 덩어리와 뒤따르는 추가 덩어리를 한 번에 모아 나란히 놓는다. 그 사이의 표지는 덩어리 뒤로 미룬다.
                const deleted = [];
                const added = [];
                const notes = [];
                while (index < lines.length && (lines[index].kind === 'd' || lines[index].kind === 'n')) {
                    (lines[index].kind === 'd' ? deleted : notes).push(index);
                    ++index;
                }
                while (index < lines.length && (lines[index].kind === 'a' || lines[index].kind === 'n')) {
                    (lines[index].kind === 'a' ? added : notes).push(index);
                    ++index;
                }
                const height = Math.max(deleted.length, added.length);
                for (let pair = 0; pair < height; ++pair)
                    push(ROW_PAIR, hunkIndex, pair < deleted.length ? deleted[pair] : -1, pair < added.length ? added[pair] : -1);
                for (const note of notes)
                    push(ROW_NOTE, hunkIndex, note, -1);
            }
        });

        const count = types.length;
        const offsets = new Float64Array(count + 1);
        for (let index = 0; index < count; ++index)
            offsets[index + 1] = offsets[index] + (types[index] === ROW_HUNK ? HUNK_HEIGHT : LINE_HEIGHT);

        return {
            count,
            types: Uint8Array.from(types),
            hunks: Int32Array.from(hunkIndexes),
            first: Int32Array.from(first),
            second: Int32Array.from(second),
            offsets,
            height: offsets[count],
        };
    }

    // y 위치에 걸친 첫 행의 번호다.
    function rowAtOffset(rows, y) {
        let low = 0;
        let high = rows.count - 1;
        if (high < 0)
            return 0;
        while (low < high) {
            const middle = (low + high + 1) >> 1;
            if (rows.offsets[middle] <= y)
                low = middle;
            else
                high = middle - 1;
        }
        return low;
    }

    function isChange(line) {
        return line.kind === 'a' || line.kind === 'd';
    }

    // 두 줄 위치 사이(양 끝 포함)의 바뀐 줄 위치를 모은다. 선택은 한 hunk 안에서만 한다.
    function changedBetween(hunk, from, to) {
        const low = Math.min(from, to);
        const high = Math.max(from, to);
        const indexes = [];
        for (let index = low; index <= high && index < hunk.lines.length; ++index)
            if (isChange(hunk.lines[index]))
                indexes.push(index);
        return indexes;
    }

    // 정렬된 줄 위치를 이어진 구간 `[처음, 끝]` 목록으로 줄인다. context 줄이 끼면 구간이 나뉜다.
    function toRanges(sortedIndexes) {
        const ranges = [];
        for (const index of sortedIndexes) {
            const last = ranges.length > 0 ? ranges[ranges.length - 1] : null;
            if (last !== null && last[1] === index - 1)
                last[1] = index;
            else
                ranges.push([index, index]);
        }
        return ranges;
    }

    function selectionText(hunk, indexes) {
        return indexes.map((index) => hunk.lines[index].text).join('\n');
    }

    // 바뀐 덩어리가 시작하는 행 번호들이다(이전·다음 변경으로 가는 데 쓴다).
    function changeStartRows(model, rows) {
        const starts = [];
        let previousChange = false;
        for (let row = 0; row < rows.count; ++row) {
            const type = rows.types[row];
            let change = false;
            if (type === ROW_LINE)
                change = isChange(model.hunks[rows.hunks[row]].lines[rows.first[row]]);
            else if (type === ROW_PAIR) {
                const hunk = model.hunks[rows.hunks[row]];
                change = (rows.first[row] >= 0 && isChange(hunk.lines[rows.first[row]])) || (rows.second[row] >= 0 && isChange(hunk.lines[rows.second[row]]));
            } else if (type === ROW_NOTE)
                change = previousChange;
            if (change && previousChange === false)
                starts.push(row);
            previousChange = change;
        }
        return starts;
    }

    function formatBytes(bytes) {
        if (bytes === null || bytes === undefined)
            return '-';
        if (bytes < 1024)
            return bytes + ' B';
        if (bytes < 1024 * 1024)
            return (bytes / 1024).toFixed(1) + ' KB';
        return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }

    const api = {
        LINE_HEIGHT, HUNK_HEIGHT, ROW_HUNK, ROW_LINE, ROW_PAIR, ROW_NOTE,
        decodeLine, pairChangedLines, buildModel, buildRows, rowAtOffset, isChange,
        changedBetween, toRanges, selectionText, changeStartRows, formatBytes,
    };
    if (typeof module !== 'undefined' && module.exports)
        module.exports = api;
    else
        root.CyellaDiff = Object.assign(root.CyellaDiff || {}, api);
})(typeof window !== 'undefined' ? window : globalThis);
