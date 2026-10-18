param(
    [ValidateSet('all','band11','band10pro')][string]$Model='all',
    [string]$Toolchain='C:\msys64',
    [string]$BuildRoot,
    [int]$Jobs=4,
    [switch]$CheckOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildRoot){$BuildRoot=Join-Path $root '.build'}
$Toolchain=[IO.Path]::GetFullPath($Toolchain)
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
foreach($name in @('usr\bin\bash.exe','usr\bin\cygpath.exe','ucrt64\bin\gcc.exe','ucrt64\bin\g++.exe','ucrt64\bin\python.exe','ucrt64\bin\ninja.exe','ucrt64\bin\pkg-config.exe')){
    if(-not(Test-Path -LiteralPath (Join-Path $Toolchain $name))){throw "Missing MSYS2 dependency: $name. See docs/BUILD.md"}
}
if($Jobs -lt 1){throw 'Jobs must be positive'}
Write-Host "Source: $root"
Write-Host "Toolchain: $Toolchain"
Write-Host "Build output: $BuildRoot"
if($CheckOnly){Write-Host 'Build tools found.';exit 0}
$bash=Join-Path $Toolchain 'usr\bin\bash.exe'
$cygpath=Join-Path $Toolchain 'usr\bin\cygpath.exe'
$script=& $cygpath -u (Join-Path $PSScriptRoot 'build-qemu.sh')
$env:TOOLCHAIN_ROOT=& $cygpath -u $Toolchain
$env:BUILD_JOBS=[string]$Jobs
$selection=if($Model -eq 'all'){@('band11','band10pro')}else{@($Model)}
foreach($id in $selection){
    $env:QEMU_BUILD_ROOT=& $cygpath -u (Join-Path $BuildRoot $id)
    & $bash $script $id
    if($LASTEXITCODE -ne 0){throw "Build failed for $id (exit $LASTEXITCODE)"}
}
Write-Host 'Compilation complete. Use tools/assemble_runtime.py to generate fresh images and the runtime.'
