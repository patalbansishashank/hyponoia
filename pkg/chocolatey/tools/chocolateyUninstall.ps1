$ErrorActionPreference = 'Stop'

$packageName = 'hyponoia'
# Must resolve the same way chocolateyInstall.ps1 does, or the uninstall
# deletes nothing and reports success. See the note there on why
# $env:ChocolateyBinRoot cannot be read directly.
$installDir  = Join-Path (Get-ToolsLocation) $packageName

Uninstall-BinFile -Name 'hyponoia'

if (Test-Path $installDir) {
  Remove-Item $installDir -Recurse -Force
}
