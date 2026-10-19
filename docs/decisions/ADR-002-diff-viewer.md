# ADR-002: diff 뷰어는 WebView2 안의 자체 렌더러로 만든다

## 상태

승인됨 - 2026-10-19

## 배경

diff 뷰어에는 inline·좌우 분할 보기, 구문 강조, 줄 안의 단어 단위 강조, 큰 파일의 가상 스크롤,
이미지 비교가 필요하다. hunk·줄 단위 stage도 이 화면에서 한다.

luil에는 `webview_element`(WebView2 합성)가 있다.
luil로 직접 그리면 텍스트 배치, 구문 강조, 선택·복사, 단어 단위 강조를 모두 새로 만들어야 한다.

## 결정

1. **diff 본문은 WebView2로 그린다.** 파일 목록, 툴바, stage 버튼처럼 앱의 나머지 부분과 이어지는
   크롬은 luil로 그린다.
2. 렌더러는 **cyella 저장소 안의 HTML·JS·CSS**(`assets/diff-viewer/`)로 두고, 외부 CDN을 쓰지 않는다.
   실행 파일 리소스나 설치 폴더의 파일을 WebView2 가상 호스트(`https://cyella.diff/`)로 제공한다.
3. C++이 `git diff` 출력을 **구조화된 JSON**(파일 → hunk → 줄, 옛·새 줄 번호, 종류)으로 해석해
   웹뷰로 보낸다. 웹뷰는 unified diff 텍스트를 해석하지 않는다. 그래서 줄 선택을 C++가 가진 같은
   모델로 되돌려 받을 수 있다.
4. 구문 강조는 번들한 highlight.js(BSD-3-Clause)를, 줄 안의 단어 단위 강조는 jsdiff(BSD-3-Clause)를
   쓴다. 둘 다 고지를 실행 파일 고지에 넣는다.
5. 메시지는 처음부터 양방향으로 정의한다.
   - C++ → JS: `show_diff`, `set_layout`(inline/split), `set_theme`, `clear`
   - JS → C++: `stage_hunk`, `unstage_hunk`, `discard_hunk`, `stage_lines`, `unstage_lines`,
     `discard_lines`(hunk 번호와 줄 범위), `open_external`, `copy`
   - 줄 단위 stage는 C++가 선택한 줄로 부분 patch를 만들어 `git apply --cached`(또는 `-R`)로 적용한다.
6. 뷰어는 `diff_view` 인터페이스 뒤에 둔다. 나중에 luil 가상 목록으로 직접 그리는 구현으로 바꿀 수 있게
   하기 위해서다.

## 바이너리와 외부 도구

1. 이미지(png, jpg, gif, webp, bmp, svg)는 웹뷰에서 좌우·겹쳐 보기·밀어 보기로 비교한다. 두 판은
   `git show <rev>:<path>`로 임시 파일로 꺼낸다.
2. `.gitattributes`에 `diff=<driver>`가 있고 `diff.<driver>.textconv`가 설정되어 있으면, git이 변환한
   텍스트 diff를 그대로 보인다.
3. 그 밖의 바이너리는 "바이너리 파일"과 크기 변화를 보이고, **외부 도구로 열기**를 제공한다.
   - 설정에 확장자별 외부 도구 표를 둔다. 항목은 확장자, 실행 파일, 인자 템플릿(`{left}` `{right}`
     `{base}` `{path}`)이다.
   - 표에 없으면 `diff.tool`·`difftool.<name>.cmd` 설정이 있을 때 `git difftool --no-prompt`로 넘긴다.
   - 임시 파일은 작업이 끝나거나 앱이 종료될 때 지운다.

## 결과

- 텍스트 배치·선택·복사를 새로 만들지 않고 inline·분할 보기와 구문 강조를 갖출 수 있다.
- WebView2 Runtime이 필요하다. 없으면 diff 영역에 일반 텍스트 대체 보기와 설치 안내를 보인다.
- 웹뷰와 luil 사이의 초점·단축키 이동은 luil `webview_element`의 규칙을 따른다.
