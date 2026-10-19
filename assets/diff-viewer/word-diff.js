// 줄 안의 단어 단위 차이다. 옛 줄과 새 줄을 단어·공백·기호 토큰으로 나누고, 토큰 열의 최장 공통 부분
// 수열(LCS)에 들지 못한 토큰을 바뀐 곳으로 본다. 외부 라이브러리(jsdiff)를 쓰지 않는다.
'use strict';

(function (root) {
    // 문자·숫자·밑줄의 연속, 공백의 연속, 그 밖의 기호 한 글자를 한 토큰으로 본다.
    const TOKEN_PATTERN = /[\p{L}\p{N}_]+|\s+|[^\s\p{L}\p{N}_]/gu;
    // 두 쪽 토큰 수의 곱이 이 값을 넘으면 LCS 표를 만들지 않고 앞뒤 공통부분만 걷어 낸다.
    const MAXIMUM_CELLS = 40000;
    // 같은 토큰이 차지하는 글자 비율이 이보다 낮으면 전혀 다른 줄로 보고 강조하지 않는다.
    const MINIMUM_SIMILARITY = 0.3;

    function tokenize(text) {
        const tokens = [];
        for (const match of text.matchAll(TOKEN_PATTERN))
            tokens.push({ text: match[0], start: match.index, end: match.index + match[0].length });
        return tokens;
    }

    // 두 토큰 열에서 짝이 있는 토큰의 위치 쌍을 돌려준다.
    function matchTokens(a, b) {
        let head = 0;
        while (head < a.length && head < b.length && a[head].text === b[head].text)
            ++head;
        let tail = 0;
        while (tail < a.length - head && tail < b.length - head && a[a.length - 1 - tail].text === b[b.length - 1 - tail].text)
            ++tail;

        const pairs = [];
        for (let index = 0; index < head; ++index)
            pairs.push([index, index]);

        const rows = a.length - head - tail;
        const columns = b.length - head - tail;
        if (rows > 0 && columns > 0 && rows * columns <= MAXIMUM_CELLS) {
            // table[i][j]는 a[head+i..]와 b[head+j..]의 LCS 길이다.
            const width = columns + 1;
            const table = new Uint16Array((rows + 1) * width);
            for (let i = rows - 1; i >= 0; --i) {
                for (let j = columns - 1; j >= 0; --j) {
                    table[i * width + j] = a[head + i].text === b[head + j].text
                        ? table[(i + 1) * width + j + 1] + 1
                        : Math.max(table[(i + 1) * width + j], table[i * width + j + 1]);
                }
            }
            let i = 0;
            let j = 0;
            while (i < rows && j < columns) {
                if (a[head + i].text === b[head + j].text) {
                    pairs.push([head + i, head + j]);
                    ++i;
                    ++j;
                } else if (table[(i + 1) * width + j] >= table[i * width + j + 1]) {
                    ++i;
                } else {
                    ++j;
                }
            }
        }

        for (let index = 0; index < tail; ++index)
            pairs.push([a.length - tail + index, b.length - tail + index]);
        return pairs;
    }

    // 짝이 없는 토큰의 글자 범위를 모은다. 공백만 사이에 둔 범위는 하나로 잇는다.
    function changedRanges(tokens, matchedFlags) {
        const ranges = [];
        for (let index = 0; index < tokens.length; ++index) {
            if (matchedFlags[index])
                continue;
            const token = tokens[index];
            const last = ranges.length > 0 ? ranges[ranges.length - 1] : null;
            if (last !== null && last[1] === token.start) {
                last[1] = token.end;
            } else if (last !== null && /^\s+$/u.test(tokens[index - 1].text) && matchedFlags[index - 1] && ranges.length > 0 && last[1] === tokens[index - 1].start) {
                // 바뀐 토큰 사이의 공백 하나는 같은 덩어리로 칠한다.
                last[1] = token.end;
            } else {
                ranges.push([token.start, token.end]);
            }
        }
        return ranges;
    }

    // 두 줄의 바뀐 곳을 `{ old: [[시작, 끝], ...], new: [...] }`로 돌려준다. 위치는 UTF-16 글자 단위다.
    // 너무 다른 줄이거나 한쪽이 비었으면 null이다(줄 전체가 바뀐 것이라 단어 강조가 소음이다).
    function wordDiff(oldText, newText) {
        if (oldText.trim() === '' || newText.trim() === '' || oldText === newText)
            return null;
        const oldTokens = tokenize(oldText);
        const newTokens = tokenize(newText);
        const pairs = matchTokens(oldTokens, newTokens);

        const oldMatched = new Array(oldTokens.length).fill(false);
        const newMatched = new Array(newTokens.length).fill(false);
        let sharedLetters = 0;
        for (const [i, j] of pairs) {
            oldMatched[i] = true;
            newMatched[j] = true;
            if (/\S/u.test(oldTokens[i].text))
                sharedLetters += oldTokens[i].text.length;
        }
        const longest = Math.max(oldText.trim().length, newText.trim().length);
        if (sharedLetters / longest < MINIMUM_SIMILARITY)
            return null;

        return { old: changedRanges(oldTokens, oldMatched), new: changedRanges(newTokens, newMatched) };
    }

    const api = { wordDiff, tokenize };
    if (typeof module !== 'undefined' && module.exports)
        module.exports = api;
    else
        root.CyellaDiff = Object.assign(root.CyellaDiff || {}, api);
})(typeof window !== 'undefined' ? window : globalThis);
