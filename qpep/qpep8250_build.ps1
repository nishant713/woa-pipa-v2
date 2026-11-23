# Build the SM8250 port of QualcommPEP (pep/QualcommPEP in this repo) and test-sign it.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out  = Join-Path $root 'out8250'
New-Item -ItemType Directory -Force (Join-Path $root 'logs'), $out | Out-Null
Start-Transcript -Path (Join-Path $root 'logs\qpep8250_build.log') -Force | Out-Null
try {
  Remove-Item (Join-Path $root 'BUILD_FAILED') -ErrorAction SilentlyContinue
  # fresh copy of the repo (main branch) - contains pep\QualcommPEP with the 8250 target
  $zip = Join-Path $root 'woa-pipa.zip'; $repo = Join-Path $root 'woa-pipa-v2-main'
  if (Test-Path $repo) { Remove-Item -Recurse -Force $repo }
  Invoke-WebRequest https://github.com/nishant713/woa-pipa-v2/archive/refs/heads/main.zip -OutFile $zip
  Expand-Archive $zip $root -Force; Remove-Item $zip
  $src = Join-Path $repo 'pep\QualcommPEP'
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
  $msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
  & $msbuild (Join-Path $src 'QcPep8250.sln') /m /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /v:minimal
  Write-Host "MSBuild exit code: $LASTEXITCODE"
  if ($LASTEXITCODE -ne 0) { New-Item -Force (Join-Path $root 'BUILD_FAILED') | Out-Null; throw 'build failed - send logs\qpep8250_build.log' }
  $files = Get-ChildItem -Recurse (Join-Path $src 'ARM64') -Include qcpep.sys,qcpep8250.inf,qcpep.cat,*.inf
  $files | ForEach-Object { Write-Host "OUTPUT: $($_.FullName) $($_.Length)"; Copy-Item $_.FullName $out -Force }
  $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe | Where-Object FullName -match '\\x64\\' | Select-Object -Last 1
  foreach ($f in @('qcpep.sys','qcpep.cat')) {
    $p = Join-Path $out $f
    if (Test-Path $p) { & $signtool.FullName sign /fd sha256 /f (Join-Path $root 'pipa_test_cert.pfx') /p pipa $p; Write-Host "signed $f exit $LASTEXITCODE" }
  }
  # pipa test drivers (drivers\<name>): KTZ8866 backlight, NT36532 touch SPI, pipapwr
  foreach ($dn in @('ktz8866','nt36532spi','pipapwr','pipabatt','pipagpu')) {
    $kz = Join-Path $repo "drivers\$dn"; $kout = Join-Path $out $dn
    if (-not (Test-Path $kz)) { continue }
    New-Item -ItemType Directory -Force $kout | Out-Null
    & $msbuild (Join-Path $kz "$dn.vcxproj") /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /v:minimal
    Write-Host "$dn MSBuild exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { throw "$dn build failed - send logs\qpep8250_build.log" }
    Get-ChildItem -Recurse (Join-Path $kz 'ARM64') -Include "$dn.sys","$dn.inf","$dn.cat" | ForEach-Object { Write-Host "OUTPUT: $($_.FullName) $($_.Length)"; Copy-Item $_.FullName $kout -Force }
    foreach ($f in @("$dn.sys","$dn.cat")) { $p = Join-Path $kout $f; if (Test-Path $p) { & $signtool.FullName sign /fd sha256 /f (Join-Path $root 'pipa_test_cert.pfx') /p pipa $p; Write-Host "signed $f exit $LASTEXITCODE" } }
  }
  Write-Host "DONE -> $out"
  $global:QPEP_BUILD_OK = $true
} catch { Write-Host "ERROR: $_" -ForegroundColor Red }
Stop-Transcript | Out-Null
if (-not $env:PIPA_NOPAUSE) { Read-Host 'Enter' }
