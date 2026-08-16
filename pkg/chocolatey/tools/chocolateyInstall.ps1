$ErrorActionPreference = 'Stop'

$packageName = 'hyponoia'
$version     = '0.3.1'
# The build publishes only the UI variant, so this is the only archive that exists.
$url64       = "https://github.com/patalbansishashank/hyponoia/releases/download/v${version}/hyponoia-ui-windows-amd64.zip"
# v0.3.1's own checksum, copied from the release's checksums.txt and verified
# independently by re-hashing the downloaded zip. A hash is only ever carried
# across a version bump together with $version: a real hash for the wrong tag
# fails at install time with a mismatch nobody could attribute to a cause.
$checksum64  = 'fee50ab1a80ca00b133311d96d90447b0b991ec66892aaa0a0e0ef2d2753c4fe'
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
