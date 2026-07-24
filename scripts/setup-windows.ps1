# Compatibility entry point. Installation behavior belongs in the canonical
# install.ps1 at the repository root.
$ErrorActionPreference = "Stop"
Write-Warning "scripts/setup-windows.ps1 is deprecated; using canonical install.ps1"

$RootInstaller = $null
if ($PSScriptRoot) {
    $RootInstaller = Join-Path (Split-Path $PSScriptRoot -Parent) "install.ps1"
}
if ($RootInstaller -and (Test-Path $RootInstaller)) {
    & $RootInstaller @args
    exit $LASTEXITCODE
}

$TmpInstaller = Join-Path ([System.IO.Path]::GetTempPath()) "cbm-install-$(Get-Random).ps1"
$Url = "https://raw.githubusercontent.com/0ctacity/codebase-memory-mcp/main/install.ps1"
try {
    Invoke-WebRequest -Uri $Url -OutFile $TmpInstaller -UseBasicParsing
    & $TmpInstaller @args
    exit $LASTEXITCODE
} finally {
    Remove-Item $TmpInstaller -Force -ErrorAction SilentlyContinue
}
