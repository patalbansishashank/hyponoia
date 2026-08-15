$ErrorActionPreference = 'Stop'

$packageName = 'hyponoia'
$version     = '0.3.0'
$url64       = "https://github.com/patalbansishashank/hyponoia/releases/download/v${version}/hyponoia-windows-amd64.zip"
# Checksums are placeholders until v0.3.0 publishes. They were upstream's,
# for a v0.8.1 release this fork never cut; a plausible-but-wrong hash fails
# at install time with a mismatch nobody can attribute, so they are zeroed
# until the real values are copied from the release's checksums.txt.
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
