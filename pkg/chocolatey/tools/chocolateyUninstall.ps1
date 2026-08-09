$ErrorActionPreference = 'Stop'

$packageName = 'hyponoia'
$installDir  = Join-Path $env:ChocolateyBinRoot $packageName

Uninstall-BinFile -Name 'hyponoia'

if (Test-Path $installDir) {
  Remove-Item $installDir -Recurse -Force
}
