[CmdletBinding()]
param(
    [string]$root
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1은 [CmdletBinding()]이 붙은 스크립트의 param 기본값에서 $PSScriptRoot가 비어 있다.
# 기본값은 본문에서 채운다.
if ([string]::IsNullOrEmpty($root)) {
    $root = Join-Path $PSScriptRoot '..'
}

$resolved_root = (Resolve-Path -LiteralPath $root).Path
$utf8_without_bom = [System.Text.UTF8Encoding]::new($false, $true)
$excluded_directories = @('.git', '.vs', 'bin', 'build', 'third_party', 'vcpkg_installed', 'worktrees')
$extensions = @('.cmake', '.cpp', '.h', '.json', '.md', '.ps1', '.rc', '.version-list', '.xml')
$special_names = @('.clang-format', '.clang-tidy', '.editorconfig', '.gitattributes', '.gitignore', 'CMakeLists.txt')
$changed_count = 0

# [System.IO.Path]::GetRelativePath는 .NET Core 2.0 이상에만 있어 Windows PowerShell 5.1에서는 쓸 수 없다.
# 대상은 모두 root 아래에서 열거한 파일이라 root 길이만큼 앞을 자르면 같은 값이 나온다.
function Get-RelativePathUnderRoot {
    param(
        [Parameter(Mandatory)]
        [string]$root,

        [Parameter(Mandatory)]
        [string]$path
    )

    return $path.Substring($root.TrimEnd([char[]]'\/').Length + 1)
}

$files = Get-ChildItem -LiteralPath $resolved_root -Recurse -File | Where-Object {
    $relative = Get-RelativePathUnderRoot -root $resolved_root -path $_.FullName
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
        (($_.Extension -in $extensions) -or ($_.Name -in $special_names))
}

foreach ($file in $files) {
    $text = $utf8_without_bom.GetString([System.IO.File]::ReadAllBytes($file.FullName))
    $normalized = $text.Replace("`r`n", "`n").Replace("`r", "`n").Replace("`n", "`r`n")
    if ($normalized -cne $text) {
        [System.IO.File]::WriteAllText($file.FullName, $normalized, $utf8_without_bom)
        $changed_count++
    }
}

Write-Host "CRLF and UTF-8 without BOM normalization completed: ${changed_count} changed, $($files.Count) checked"

