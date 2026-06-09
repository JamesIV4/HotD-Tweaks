[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $repoRoot

function Import-VcVars32 {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        (Get-Command rc.exe -ErrorAction SilentlyContinue) -and
        (Get-Command link.exe -ErrorAction SilentlyContinue)) {
        return
    }

    $searchRoots = @(
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio")
    ) | Where-Object { Test-Path -LiteralPath $_ }

    $vcvars = Get-ChildItem $searchRoots -Filter vcvars32.bat -Recurse |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $vcvars) {
        throw "Visual Studio C++ x86 build tools were not found."
    }

    $environment = & cmd.exe /d /s /c "`"$($vcvars.FullName)`" >nul && set"
    foreach ($line in $environment) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable(
                $matches[1],
                $matches[2],
                "Process")
        }
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

Import-VcVars32

$managerBuild = Join-Path $repoRoot "HotDModManager\build"
$shimBuild = Join-Path $repoRoot "HotDPostFXShim\build"
New-Item -ItemType Directory -Path $managerBuild, $shimBuild -Force | Out-Null

Invoke-Native rc.exe /nologo /fo "$shimBuild\d3d9_shaders.res" `
    "HotDPostFXShim\d3d9_shaders.rc"
Invoke-Native cl.exe /nologo /c /O2 /MT /EHsc /guard:cf /DUNICODE /D_UNICODE `
    "/Fo$shimBuild\d3d9_postfx_shim.obj" `
    "HotDPostFXShim\d3d9_postfx_shim.cpp"
Invoke-Native link.exe /nologo /DLL /MACHINE:X86 `
    /DEF:HotDPostFXShim\d3d9.def /GUARD:CF /DYNAMICBASE /NXCOMPAT `
    /RELEASE /CETCOMPAT /OPT:REF /OPT:ICF `
    "/OUT:$shimBuild\D3D9.dll" `
    "$shimBuild\d3d9_postfx_shim.obj" `
    "$shimBuild\d3d9_shaders.res" user32.lib uuid.lib

Invoke-Native rc.exe /nologo /fo "$managerBuild\HotDModManager.res" `
    "HotDModManager\HotDModManager.rc"
Invoke-Native cl.exe /nologo /c /O2 /MT /EHsc /guard:cf `
    /DUNICODE /D_UNICODE /DNO_GZIP /D_CRT_SECURE_NO_WARNINGS `
    "/Fo$managerBuild\HotDModManager.obj" `
    "HotDModManager\HotDModManager.cpp"

$zlibObjects = @()
foreach ($name in @(
    "adler32",
    "deflate",
    "inffast",
    "inflate",
    "inftrees",
    "trees",
    "zutil"
)) {
    $object = "$managerBuild\$name.obj"
    Invoke-Native cl.exe /nologo /c /O2 /MT /guard:cf `
        /DNO_GZIP /D_CRT_SECURE_NO_WARNINGS `
        "/Fo$object" "third_party\zlib\$name.c"
    $zlibObjects += $object
}

Invoke-Native link.exe /nologo /SUBSYSTEM:WINDOWS /MACHINE:X86 `
    /GUARD:CF /DYNAMICBASE /NXCOMPAT /RELEASE /CETCOMPAT `
    /OPT:REF /OPT:ICF `
    "/OUT:$managerBuild\HotD-Tweaks.exe" `
    "$managerBuild\HotDModManager.obj" `
    @zlibObjects `
    "$managerBuild\HotDModManager.res" `
    user32.lib gdi32.lib comctl32.lib uxtheme.lib shell32.lib bcrypt.lib

Copy-Item "$managerBuild\HotD-Tweaks.exe" "HotDModManager\HotD-Tweaks.exe" -Force
Copy-Item "$managerBuild\HotDModManager.obj" "HotDModManager\HotDModManager.obj" -Force
Copy-Item "$shimBuild\D3D9.dll" "HotDPostFXShim\D3D9.dll" -Force
Copy-Item "$shimBuild\d3d9_postfx_shim.obj" `
    "HotDPostFXShim\d3d9_postfx_shim.obj" -Force
Copy-Item "$shimBuild\D3D9.lib" "HotDPostFXShim\D3D9.lib" -Force
Copy-Item "$shimBuild\D3D9.exp" "HotDPostFXShim\D3D9.exp" -Force

$distRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "dist"))
$stage = [IO.Path]::GetFullPath((Join-Path $distRoot "HotD-Tweaks"))
if (-not $stage.StartsWith(
    $distRoot + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unexpected release staging path: $stage"
}
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}

$backendStage = Join-Path $stage "dgVoodooBackend"
New-Item -ItemType Directory -Path $backendStage -Force | Out-Null
Copy-Item "$managerBuild\HotD-Tweaks.exe" "$stage\HotD-Tweaks.exe"
Copy-Item "$shimBuild\D3D9.dll" "$stage\D3D9.dll"
Copy-Item "dgVoodooBackend\D3D9.dll" "$backendStage\D3D9.dll"
Copy-Item "dgVoodooBackend\dgVoodoo.conf" "$stage\dgVoodoo.conf"
Copy-Item "dgVoodooBackend\dgVoodoo.conf" "$backendStage\dgVoodoo.conf"
$version = (Get-Item "$stage\HotD-Tweaks.exe").VersionInfo.FileVersion
$archive = Join-Path $distRoot "HotD-Tweaks-$version-portable.zip"
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path "$stage\*" -DestinationPath $archive -CompressionLevel Optimal

$hash = Get-FileHash $archive -Algorithm SHA256
$hashLine = "$($hash.Hash)  $([IO.Path]::GetFileName($archive))`r`n"
[IO.File]::WriteAllText(
    "$archive.sha256.txt",
    $hashLine,
    [Text.Encoding]::ASCII)

Write-Host "Release archive: $archive"
Write-Host "SHA-256: $($hash.Hash)"
