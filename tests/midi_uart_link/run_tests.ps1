$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
$BuildDir = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Link = Join-Path $RepoRoot "firmware/main-deck-p4/components/midi_uart_link"
$Codec = Join-Path $RepoRoot "firmware/main-deck-p4/components/controller_runtime"

$CommonArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-I$(Join-Path $Link 'include')",
    "-I$(Join-Path $Codec 'include')"
)

$ParserExe = Join-Path $BuildDir "test_midi_stream_parser"
if ($IsWindows -or $env:OS -eq "Windows_NT") { $ParserExe += ".exe" }

gcc @CommonArgs `
    (Join-Path $Link "midi_stream_parser.c") `
    (Join-Path $PSScriptRoot "test_midi_stream_parser.c") `
    -o $ParserExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $ParserExe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

exit 0
