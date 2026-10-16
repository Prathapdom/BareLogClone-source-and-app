param([string]$Compiler = 'F:\Software\TDM-GCC-64\bin\g++.exe', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!(Test-Path -LiteralPath $Compiler)) { $Compiler = (Get-Command g++ -ErrorAction Stop).Source }
    $resourceCompiler = Join-Path (Split-Path -Parent $Compiler) 'windres.exe'
    New-Item -ItemType Directory -Path 'dist' -Force | Out-Null
    & (Join-Path $PSScriptRoot 'packaging\Make-Assets.ps1')
    & $resourceCompiler app.rc -O coff -o dist/app-res.o
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    $warn = '-Wall', '-Wextra', '-Wno-missing-field-initializers'
    & $Compiler '-std=c++17' -O3 -DNDEBUG @warn -municode -mwindows -static '-static-libgcc' '-static-libstdc++' -s main.cpp dist/app-res.o -o dist/BareLog.exe '-lcomctl32' '-lole32' '-luuid' '-lshell32' '-lcomdlg32' '-lgdi32' '-luxtheme' '-ldwmapi'
    if ($LASTEXITCODE) { throw 'Application compilation failed' }
    if (!$SkipTests) {
        & $Compiler '-std=c++17' -O2 @warn -static tests.cpp -o dist/engine-tests.exe '-lole32' '-lshell32' '-luuid'
        if ($LASTEXITCODE) { throw 'Engine test compilation failed' }
        & .\dist\engine-tests.exe
        if ($LASTEXITCODE) { throw 'Engine tests failed' }
        & $Compiler '-std=c++17' -O2 @warn -municode -static ui-tests.cpp -o dist/ui-tests.exe
        if ($LASTEXITCODE) { throw 'UI test compilation failed' }
        & .\dist\ui-tests.exe (Join-Path $PSScriptRoot 'dist\BareLog.exe')
        if ($LASTEXITCODE) { throw 'UI integration tests failed' }
    }
    Write-Host "Built: $PSScriptRoot\dist\BareLog.exe"
} finally { Pop-Location }
