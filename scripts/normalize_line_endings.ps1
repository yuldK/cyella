[CmdletBinding()]
param(
    [string]$root = (Join-Path $PSScriptRoot '..')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolved_root = (Resolve-Path -LiteralPath $root).Path
$utf8_without_bom = [System.Text.UTF8Encoding]::new($false, $true)
$excluded_directories = @('.git', '.vs', 'bin', 'build', 'third_party', 'vcpkg_installed', 'worktrees')
$extensions = @('.cmake', '.cpp', '.h', '.json', '.md', '.ps1', '.rc', '.version-list', '.xml')
$special_names = @('.clang-format', '.clang-tidy', '.editorconfig', '.gitattributes', '.gitignore', 'CMakeLists.txt')
$changed_count = 0

$files = Get-ChildItem -LiteralPath $resolved_root -Recurse -File | Where-Object {
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

