// blame 메시지(`show_blame`)를 화면 줄로 바꾸는 순수 계산이다. DOM을 만지지 않으므로 node에서도 시험한다.
'use strict';

(function (root) {
    // 줄 내용 풀기와 행 높이는 diff와 같은 것을 쓴다. 브라우저에서는 model.js가 먼저 실린다.
    const base = typeof module !== 'undefined' && module.exports ? require('./model.js') : root.CyellaDiff;

    const SHORT_SHA_LENGTH = 7;

    function textOf(value) {
        return typeof value === 'string' ? value : '';
    }

    // 메시지의 origin 하나를 화면이 쓰는 꼴로 고른다. 빠진 값은 빈 값이 된다. 메일은 꺾쇠가 붙어 와도 뗀다.
    function normalizeOrigin(origin) {
        const source = origin !== null && typeof origin === 'object' ? origin : {};
        return {
            sha: textOf(source.sha),
            author: textOf(source.author),
            mail: textOf(source.mail).replace(/^<|>$/g, ''),
            time: typeof source.time === 'number' && Number.isFinite(source.time) ? source.time : null,
            summary: textOf(source.summary),
            path: textOf(source.path),
            previous: source.previous === true,
            uncommitted: source.uncommitted === true,
        };
    }

    // `show_blame` 메시지를 줄마다의 배열로 푼다. 줄 하나는 `[origin 위치, 줄 번호, 내용(, 1)]`이고 넷째 값이 1이면
    // 내용이 base64다. 큰 파일(10만 줄)을 견디도록 줄마다 객체를 만들지 않는다.
    //  - lineOrigins[i]: 그 줄의 origin 위치. 메시지의 값이 origins 범위를 벗어나면 -1이다.
    //  - starts[i]: 같은 origin이 이어지는 묶음의 첫 줄이면 1이다.
    //  - first[o]: origin o가 처음 나오는 줄 위치. 한 번도 안 나오면 -1이다.
    function buildBlameModel(message) {
        const origins = (Array.isArray(message.origins) ? message.origins : []).map(normalizeOrigin);
        const entries = Array.isArray(message.lines) ? message.lines : [];
        const count = entries.length;
        const lineOrigins = new Int32Array(count);
        const numbers = new Int32Array(count);
        const texts = new Array(count);
        const cr = new Uint8Array(count);
        const starts = new Uint8Array(count);
        const first = new Int32Array(origins.length).fill(-1);
        let previous = -2;
        for (let index = 0; index < count; ++index) {
            const entry = entries[index];
            const origin = Number.isInteger(entry[0]) && entry[0] >= 0 && entry[0] < origins.length ? entry[0] : -1;
            const content = base.decodeContent(textOf(entry[2]), entry.length > 3 && entry[3] === 1);
            lineOrigins[index] = origin;
            numbers[index] = entry[1];
            texts[index] = content.text;
            cr[index] = content.cr ? 1 : 0;
            if (origin !== previous)
                starts[index] = 1;
            if (origin >= 0 && first[origin] < 0)
                first[origin] = index;
            previous = origin;
        }
        return { origins, count, lineOrigins, numbers, texts, cr, starts, first };
    }

    // blame의 행은 모두 같은 높이의 줄이다. diff의 창 그리기(rowAtOffset)가 그대로 쓰는 꼴로 만든다.
    function buildBlameRows(count) {
        const offsets = new Float64Array(count + 1);
        for (let index = 0; index < count; ++index)
            offsets[index + 1] = offsets[index] + base.LINE_HEIGHT;
        return { count, offsets, height: offsets[count] };
    }

    function shortSha(sha) {
        return sha.slice(0, SHORT_SHA_LENGTH);
    }

    function twoDigits(value) {
        return value < 10 ? '0' + value : String(value);
    }

    // 초 단위 시각을 이 컴퓨터 시간대의 `YYYY-MM-DD`로 적는다. 시각이 없으면 빈 문자열이다.
    function formatBlameDate(time) {
        if (typeof time !== 'number' || Number.isFinite(time) === false)
            return '';
        const date = new Date(time * 1000);
        if (Number.isNaN(date.getTime()))
            return '';
        return date.getFullYear() + '-' + twoDigits(date.getMonth() + 1) + '-' + twoDigits(date.getDate());
    }

    // 초 단위 시각을 이 컴퓨터 시간대의 `YYYY-MM-DD HH:MM`으로 적는다.
    function formatBlameDateTime(time) {
        const day = formatBlameDate(time);
        if (day === '')
            return '';
        const date = new Date(time * 1000);
        return day + ' ' + twoDigits(date.getHours()) + ':' + twoDigits(date.getMinutes());
    }

    // `작성자 <메일>`이다. 메일이 없으면 작성자만이다.
    function blameAuthorLine(origin) {
        if (origin.mail === '')
            return origin.author;
        return origin.author === '' ? '<' + origin.mail + '>' : origin.author + ' <' + origin.mail + '>';
    }

    // 여백 칸 툴팁의 줄들이다: 전체 SHA, 작성자 <메일>, 날짜 시각, summary. 그 커밋에서 파일 이름이 달랐으면 그 경로도 적는다.
    // 빈 값의 줄은 뺀다. 커밋하지 않은 origin의 문구는 뷰어가 문구 표에서 따로 고른다.
    function blameTooltipLines(origin, currentPath) {
        const lines = [origin.sha, blameAuthorLine(origin), formatBlameDateTime(origin.time), origin.summary];
        if (origin.path !== '' && origin.path !== currentPath)
            lines.push(origin.path);
        return lines.filter((line) => line !== '');
    }

    // SHA로 origin을 찾는다. 온전한 SHA가 같은 것이 먼저고, 없으면 줄인 SHA(7자 이상)로 시작하는 첫 origin이다. 없으면 -1이다.
    function findBlameOrigin(origins, sha) {
        if (typeof sha !== 'string' || sha === '')
            return -1;
        const exact = origins.findIndex((origin) => origin.sha === sha);
        if (exact >= 0 || sha.length < SHORT_SHA_LENGTH)
            return exact;
        return origins.findIndex((origin) => origin.sha.startsWith(sha));
    }

    // 같은 파일의 blame이 다시 왔을 때, 앞에서 고른 origin이 새 목록의 어디인지 찾는다. 위치는 바뀔 수 있어 SHA로 찾고,
    // 커밋하지 않은 origin은 커밋하지 않은 origin끼리 잇는다. 없으면 -1이다.
    function matchBlameOrigin(origins, wanted) {
        if (wanted === null || wanted === undefined)
            return -1;
        if (wanted.uncommitted)
            return origins.findIndex((origin) => origin.uncommitted);
        if (wanted.sha === '')
            return -1;
        return origins.findIndex((origin) => origin.uncommitted === false && origin.sha === wanted.sha);
    }

    // 여백 칸을 눌렀을 때의 다음 고른 origin이다. 고른 것을 다시 누르면 풀린다(-1).
    function toggleBlameOrigin(selected, origin) {
        return selected === origin ? -1 : origin;
    }

    // 내용 메시지의 `focus`를 따를 것인가. 새 내용이면 따르고, 같은 파일이 다시 온 것이면(previousFocus가 문자열)
    // focus가 바뀌었을 때만 따른다. 재동기화로 같은 메시지가 다시 올 때마다 스크롤이 끌려가지 않게 하기 위해서다.
    function shouldApplyBlameFocus(focus, previousFocus) {
        return focus !== '' && focus !== previousFocus;
    }

    const api = {
        normalizeOrigin, buildBlameModel, buildBlameRows, shortSha, formatBlameDate, formatBlameDateTime, blameAuthorLine,
        blameTooltipLines, findBlameOrigin, matchBlameOrigin, toggleBlameOrigin, shouldApplyBlameFocus,
    };
    if (typeof module !== 'undefined' && module.exports)
        module.exports = api;
    else
        root.CyellaDiff = Object.assign(root.CyellaDiff || {}, api);
})(typeof window !== 'undefined' ? window : globalThis);
