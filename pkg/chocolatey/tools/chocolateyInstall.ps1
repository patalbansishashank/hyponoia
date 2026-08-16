$ErrorActionPreference = 'Stop'

$packageName = 'hyponoia'
$version     = '0.3.1'
# The build publishes only the UI variant, so this is the only archive that exists.
$url64       = "https://github.com/patalbansishashank/hyponoia/releases/download/v${version}/hyponoia-ui-windows-amd64.zip"
# Placeholder until v0.3.1 publishes. The value below WAS the real v0.3.0
# hash, which makes it worse than an obvious blank once the URL points at
# v0.3.1: it would fail at install time with a mismatch nobody could
# attribute to a cause. Zeroed until the real value is copied from the
# release's checksums.txt.
$checksum64  = '0000000000000000000000000000000000000000000000000000000000000000'
$installDir  = Join-Path $env:ChocolateyBinRoot $packageName

Install-ChocolateyZipPackage `
  -PackageName   $packageName `
  -Url64bit      $url64 `
  -Checksum64    $checksum64 `
  -ChecksumType64 'sha256' `
  -UnzipLocation $installDir

# Shim the binary so it is on PATH
$binPath = Join-Path $installDir 'hyponoia.exe'
Install-BinFile -Name 'hyponoia' -Path $binPath
