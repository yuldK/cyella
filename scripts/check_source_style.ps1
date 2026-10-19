[CmdletBinding()]
param(
    [string]$root = (Join-Path $PSScriptRoot '..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolved_root = (Resolve-Path -LiteralPath $root).Path
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
# worktrees는 git worktree로 만든 임시 작업 트리(체크아웃)라 원본과 같은 파일이 다시 들어 있다.
# 원본을 검사하면 충분하므로 제외한다.
$excluded_directories = @('.git', '.vs', 'bin', 'build', 'third_party', 'vcpkg_installed', 'worktrees')
$checked_extensions = @('.cmake', '.cpp', '.h', '.json', '.md', '.ps1', '.rc', '.version-list', '.xml')
$special_names = @('.clang-format', '.clang-tidy', '.editorconfig', '.gitattributes', '.gitignore', 'CMakeLists.txt')
$errors = [System.Collections.Generic.List[string]]::new()

function Add-MultilineBraceClosureErrors {
    param(
        [Parameter(Mandatory)]
        [string]$text,

        [Parameter(Mandatory)]
        [string]$relative,

        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$errors
    )

    $brace_lines = [System.Collections.Generic.Stack[int]]::new()
    $state = 'code'
    $escaped_state = ''
    $raw_terminator = ''
    $line_number = 1
    $line_start = 0

    for ($index = 0; $index -lt $text.Length; $index++) {
        $character = $text[$index]
        if ($character -eq "`n") {
            $line_number++
            $line_start = $index + 1
            if ($state -eq 'line_comment') {
                $state = 'code'
            }
            elseif ($state -eq 'escape') {
                $state = $escaped_state
            }
            continue
        }

        if ($state -eq 'line_comment') {
            continue
        }
        if ($state -eq 'block_comment') {
            if ($character -eq '*' -and $index + 1 -lt $text.Length -and
                $text[$index + 1] -eq '/') {
                $state = 'code'
                $index++
            }
            continue
        }
        if ($state -eq 'raw_string') {
            if ($index + $raw_terminator.Length -le $text.Length -and
                $text.Substring($index, $raw_terminator.Length) -ceq $raw_terminator) {
                $state = 'code'
                $index += $raw_terminator.Length - 1
            }
            continue
        }
        if ($state -eq 'escape') {
            $state = $escaped_state
            continue
        }
        if ($state -eq 'string') {
            if ($character -eq '\') {
                $escaped_state = 'string'
                $state = 'escape'
            }
            elseif ($character -eq '"') {
                $state = 'code'
            }
            continue
        }
        if ($state -eq 'character') {
            if ($character -eq '\') {
                $escaped_state = 'character'
                $state = 'escape'
            }
            elseif ($character -eq "'") {
                $state = 'code'
            }
            continue
        }

        $next_character = if ($index + 1 -lt $text.Length) { $text[$index + 1] } else { '' }
        if ($character -eq '/' -and $next_character -eq '/') {
            $state = 'line_comment'
            $index++
            continue
        }
        if ($character -eq '/' -and $next_character -eq '*') {
            $state = 'block_comment'
            $index++
            continue
        }
        if ($character -eq 'R' -and $next_character -eq '"') {
            $delimiter_start = $index + 2
            $delimiter_end = $text.IndexOf('(', $delimiter_start)
            if ($delimiter_end -ge $delimiter_start -and $delimiter_end - $delimiter_start -le 16) {
                $delimiter = $text.Substring($delimiter_start, $delimiter_end - $delimiter_start)
                if ($delimiter -notmatch '[\s\\()]') {
                    $raw_terminator = ')' + $delimiter + '"'
                    $state = 'raw_string'
                    $index = $delimiter_end
                    continue
                }
            }
        }
        if ($character -eq '"') {
            $state = 'string'
            continue
        }
        if ($character -eq "'") {
            $state = 'character'
            continue
        }
        if ($character -eq '{') {
            $brace_lines.Push($line_number)
            continue
        }
        if ($character -eq '}' -and $brace_lines.Count -gt 0) {
            $opening_line = $brace_lines.Pop()
            if ($opening_line -ne $line_number) {
                $line_prefix = $text.Substring($line_start, $index - $line_start)
                if ($line_prefix -match '\S') {
                    $errors.Add(
                        "$relative`:$line_number`: The closing brace of a multiline block must be on its own line.")
                }
            }
        }
    }
}

function Get-RelativeKey {
    param(
        [Parameter(Mandatory)]
        [string]$root,

        [Parameter(Mandatory)]
        [string]$path
    )

    return ([System.IO.Path]::GetRelativePath($root, $path)) -replace '\\', '/'
}

# git이 무시하라고 한 파일을 골라낸다.
#
# 저장소가 줄 끝을 정할 수 없는 파일이 검사를 깨뜨리면 깨진 검사가 신호를 잃는다.
# 도구가 만드는 로컬 설정이 그 자리다 — 도구가 다시 쓸 때마다 되살아나므로
# 손으로 고쳐 두는 것으로는 끝나지 않는다.
#  - 무시 목록을 여기에 다시 적지 않고 git에게 묻는다. 목록이 두 벌이 되면 언젠가
#    어긋나고, `.gitignore`만 베끼면 사용자 전역 설정에서 온 것을 놓친다.
#  - 후보 경로만 물어본다. 무시되는 것을 전부 열어 보면 build 트리까지 센다.
#  - 경로는 인자로 넘긴다. `--stdin`은 PowerShell이 줄 끝에 붙이는 CR가 경로의
#    일부가 되어 아무것도 맞히지 못한다. 명령줄 길이 때문에 batch로 나눈다.
#  - git이 없거나 작업 트리가 아니면 아무것도 빼지 않는다.
function Get-IgnoredKeys {
    param(
        [Parameter(Mandatory)]
        [string]$root,

        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [string[]]$keys
    )

    $ignored = [System.Collections.Generic.HashSet[string]]::new()
    if ($keys.Count -eq 0 -or $null -eq (Get-Command git -ErrorAction Ignore)) {
        return ,$ignored
    }

    $inside = & git -C $root rev-parse --is-inside-work-tree 2>$null
    if ($LASTEXITCODE -ne 0 -or $inside -ne 'true') {
        return ,$ignored
    }

    $batch_size = 200
    for ($start = 0; $start -lt $keys.Count; $start += $batch_size) {
        $last = [System.Math]::Min($start + $batch_size, $keys.Count) - 1
        $batch = $keys[$start..$last]
        # quotePath를 끄지 않으면 ASCII 밖 경로가 따옴표에 싸여 돌아와 맞지 않는다.
        foreach ($key in (& git -C $root -c core.quotePath=false check-ignore -- @batch 2>$null)) {
            [void]$ignored.Add($key)
        }
    }
    return ,$ignored
}

$candidates = @(Get-ChildItem -LiteralPath $resolved_root -Recurse -File | Where-Object {
    $relative = [System.IO.Path]::GetRelativePath($resolved_root, $_.FullName)
    $segments = $relative -split '[\\/]'
    $is_excluded = $false
    foreach ($directory in $excluded_directories) {
        if ($segments -contains $directory) {
            $is_excluded = $true
            break
        }
    }
    if ($relative -like 'assets\codicons\*' -or $relative -like 'assets/codicons/*') {
        $is_excluded = $true
    }
    (-not $is_excluded) -and
        (($_.Extension -in $checked_extensions) -or ($_.Name -in $special_names))
})

$ignored_keys = Get-IgnoredKeys -root $resolved_root -keys @($candidates | ForEach-Object { Get-RelativeKey -root $resolved_root -path $_.FullName })
$files = @($candidates | Where-Object { -not $ignored_keys.Contains((Get-RelativeKey -root $resolved_root -path $_.FullName)) })

foreach ($file in $files) {
    $relative = [System.IO.Path]::GetRelativePath($resolved_root, $file.FullName)
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and
        $bytes[2] -eq 0xBF) {
        $errors.Add("$relative`: UTF-8 BOM found.")
    }

    try {
        $text = $utf8.GetString($bytes)
    }
    catch {
        $errors.Add("$relative`: Invalid UTF-8.")
        continue
    }

    if ($text -match '(?<!\r)\n' -or $text -match '\r(?!\n)') {
        $errors.Add("$relative`: Non-CRLF line ending found.")
    }
    if ($text.Contains("`t")) {
        $errors.Add("$relative`: Tab character found.")
    }
    if ($text -match '[ ]+\r\n') {
        $errors.Add("$relative`: Trailing whitespace found.")
    }

    if ($file.Extension -in @('.cpp', '.h')) {
        if ($text -match 'template[ \t]*<[^\r\n]+>[ \t]+\S') {
            $errors.Add("$relative`: A template declaration and its signature must be on separate lines.")
        }
        Add-MultilineBraceClosureErrors -text $text -relative $relative -errors $errors

        $declarations = [regex]::Matches(
            $text,
            '\b(?:namespace|class|struct|enum\s+class)\s+([A-Za-z_][A-Za-z0-9_]*)')
        foreach ($declaration in $declarations) {
            $identifier = $declaration.Groups[1].Value
            if ($identifier -notmatch '^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$') {
                $errors.Add("$relative`: Type or namespace is not snake_case: $identifier")
            }
        }
    }
}

# 종료 코드를 두 갈래 모두에서 **명시로** 정한다.
# 이 script는 네이티브 명령을 부르지 않아 스스로는 $LASTEXITCODE를 세우지 않는다.
# 그래서 -File이 아니라 호출 연산자(`& script.ps1`)로 부르면, 부르는 쪽이 읽는
# $LASTEXITCODE가 그 session에서 **앞서 돈 다른 명령**의 값 그대로 남는다 —
# 검사는 통과했는데 부르는 쪽은 실패로 읽는다. exit가 그 자리를 덮는다.
#  - throw 대신 메시지 한 줄로 끝내는 이유도 같다. throw는 위반 목록 뒤에
#    PowerShell 예외 덩어리를 붙여, 무엇이 틀렸는지가 그 밑에 묻힌다.
if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Write-Host "Source style check failed: $($errors.Count) violation(s)" -ForegroundColor Red
    exit 1
}

Write-Host "Source style check passed: $($files.Count) file(s)"
exit 0

