param(
    [switch]$Check
)

$clangFormatPath = Get-Command clang-format-21 -ErrorAction SilentlyContinue
if (-not $clangFormatPath) {
    $clangFormatPath = Get-Command clang-format -ErrorAction SilentlyContinue
}

if (-not $clangFormatPath) {
    Write-Host "Error: clang-format not found in PATH" -ForegroundColor Red
    exit 1
}

& $clangFormatPath --version

$failed = $false

Get-ChildItem -Path . -Include "*.cpp", "*.h", "*.hlsl" -Recurse |
    Where-Object {
        $_.FullName -notmatch '[\\/]external[\\/]' -and
        $_.FullName -notmatch '[\\/]build[\\/]'
    } |
    ForEach-Object {
        if ($Check) {
            & $clangFormatPath --dry-run --Werror $_.FullName
        }
        else {
            & $clangFormatPath -i $_.FullName
        }
        if ($LASTEXITCODE -ne 0) {
            $failed = $true
        }
    }

if ($failed) {
    exit 1
}
