// 구문 강조와 줄 단위 HTML 조립이다. highlight.js(vendor/highlight.min.js)가 전역 `hljs`로 있으면 쓰고,
// 없거나 확장자를 모르면 이스케이프만 한다.
'use strict';

(function (root) {
    const LANGUAGE_BY_EXTENSION = {
        c: 'c', h: 'cpp', cpp: 'cpp', cc: 'cpp', cxx: 'cpp', hpp: 'cpp', hh: 'cpp', hxx: 'cpp', inl: 'cpp', ipp: 'cpp', rc: 'cpp', idl: 'cpp',
        cmake: 'cmake',
        js: 'javascript', mjs: 'javascript', cjs: 'javascript', jsx: 'javascript',
        ts: 'typescript', tsx: 'typescript', mts: 'typescript',
        json: 'json', jsonc: 'json', webmanifest: 'json',
        py: 'python', pyw: 'python',
        md: 'markdown', markdown: 'markdown',
        html: 'xml', htm: 'xml', xml: 'xml', xaml: 'xml', svg: 'xml', csproj: 'xml', vcxproj: 'xml', props: 'xml', targets: 'xml', xsd: 'xml', xslt: 'xml', manifest: 'xml', plist: 'xml',
        css: 'css',
        ps1: 'powershell', psm1: 'powershell', psd1: 'powershell',
        sh: 'bash', bash: 'bash', zsh: 'bash',
        cs: 'csharp',
        ini: 'ini', cfg: 'ini', conf: 'ini', toml: 'ini', editorconfig: 'ini',
        yml: 'yaml', yaml: 'yaml',
    };
    const LANGUAGE_BY_NAME = { 'cmakelists.txt': 'cmake', '.clang-format': 'yaml', '.clang-tidy': 'yaml', '.gitattributes': 'ini' };

    function baseName(path) {
        const slash = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
        return (slash >= 0 ? path.slice(slash + 1) : path).toLowerCase();
    }

    // 경로의 확장자로 언어를 고른다. 모르면 null이다.
    function languageFor(path) {
        const name = baseName(path || '');
        if (Object.prototype.hasOwnProperty.call(LANGUAGE_BY_NAME, name))
            return LANGUAGE_BY_NAME[name];
        const dot = name.lastIndexOf('.');
        if (dot < 0)
            return null;
        const extension = name.slice(dot + 1);
        return Object.prototype.hasOwnProperty.call(LANGUAGE_BY_EXTENSION, extension) ? LANGUAGE_BY_EXTENSION[extension] : null;
    }

    function escapeHtml(text) {
        return text.replace(/[&<>"']/g, (letter) => {
            switch (letter) {
            case '&': return '&amp;';
            case '<': return '&lt;';
            case '>': return '&gt;';
            case '"': return '&quot;';
            default: return '&#x27;';
            }
        });
    }

    // highlight.js가 만든 HTML을 줄마다 균형 잡힌 조각으로 자른다. 줄 사이에 걸친 span은 줄 끝에서 닫고 다음
    // 줄 머리에서 다시 연다.
    function splitHighlighted(html) {
        const lines = [];
        const open = [];
        let current = '';
        let position = 0;
        const pattern = /<span[^>]*>|<\/span>|\n/g;
        let match;
        while ((match = pattern.exec(html)) !== null) {
            current += html.slice(position, match.index);
            position = match.index + match[0].length;
            if (match[0] === '\n') {
                current += '</span>'.repeat(open.length);
                lines.push(current);
                current = open.join('');
            } else if (match[0] === '</span>') {
                current += match[0];
                open.pop();
            } else {
                current += match[0];
                open.push(match[0]);
            }
        }
        current += html.slice(position);
        lines.push(current);
        return lines;
    }

    // 여러 줄을 한 번에 강조해 줄마다의 HTML을 돌려준다. 여러 줄 주석·문자열이 줄을 넘어 이어진다.
    function highlightLines(lines, language) {
        if (lines.length === 0)
            return [];
        const hljs = root.hljs;
        if (!language || !hljs || !hljs.getLanguage(language))
            return lines.map(escapeHtml);
        let html;
        try {
            html = hljs.highlight(lines.join('\n'), { language, ignoreIllegals: true }).value;
        } catch (error) {
            return lines.map(escapeHtml);
        }
        const split = splitHighlighted(html);
        // 길이가 어긋나면(드문 경우) 강조를 포기한다.
        return split.length === lines.length ? split : lines.map(escapeHtml);
    }

    // HTML 한 줄 위에 글자 범위(원문 기준, UTF-16 단위)를 `<span class="is-strong">`로 덮는다. 태그를 만나면 강조를
    // 닫았다가 태그 뒤에서 다시 열어 중첩이 어긋나지 않게 한다.
    function applyMarks(html, ranges) {
        if (!ranges || ranges.length === 0)
            return html;
        const OPEN = '<span class="is-strong">';
        let output = '';
        let textPosition = 0;
        let rangeIndex = 0;
        let marking = false;
        let index = 0;
        while (index < html.length) {
            const letter = html[index];
            if (letter === '<') {
                const end = html.indexOf('>', index);
                const tag = html.slice(index, end + 1);
                output += marking ? '</span>' + tag + OPEN : tag;
                index = end + 1;
                continue;
            }
            let unit = letter;
            let width = 1;
            if (letter === '&') {
                const end = html.indexOf(';', index);
                unit = html.slice(index, end + 1);
            } else if (letter >= '\uD800' && letter <= '\uDBFF' && index + 1 < html.length) {
                unit = html.slice(index, index + 2);
                width = 2;
            }
            while (rangeIndex < ranges.length && textPosition >= ranges[rangeIndex][1]) {
                if (marking) {
                    output += '</span>';
                    marking = false;
                }
                ++rangeIndex;
            }
            if (marking === false && rangeIndex < ranges.length && textPosition >= ranges[rangeIndex][0] && textPosition < ranges[rangeIndex][1]) {
                output += OPEN;
                marking = true;
            }
            output += unit;
            textPosition += width;
            index += unit.length;
        }
        if (marking)
            output += '</span>';
        return output;
    }

    const api = { languageFor, escapeHtml, splitHighlighted, highlightLines, applyMarks };
    if (typeof module !== 'undefined' && module.exports)
        module.exports = api;
    else
        root.CyellaDiff = Object.assign(root.CyellaDiff || {}, api);
})(typeof window !== 'undefined' ? window : globalThis);
