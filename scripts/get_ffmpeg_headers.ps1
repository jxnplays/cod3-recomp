<#
    get_ffmpeg_headers.ps1 - fetch the FFmpeg headers XMA decoding needs.

    CoD3Host/xma.cpp decodes XMA through FFmpeg's avcodec, loaded at run time
    with LoadLibrary rather than linked. Only the headers are needed to build.

    Without them the build still succeeds and says so:

        CoD3: no FFmpeg headers, XMA sound will be silent

    so this is a completeness step, not a build blocker.

    WHY THEY ARE NOT IN THE REPOSITORY

    The FFmpeg builds that provide these headers are GPL. Committing GPL headers
    into a repository whose own work is MIT would muddy the licensing of the
    whole project for no benefit, so this fetches them instead. The archive is
    large and the headers are not ours to redistribute.

    VERSION

    Pinned to 8.1, which is the avcodec 62 line. The run-time avcodec-62.dll
    that ships with a build has to match the headers, and FFmpeg 9 moved to
    avcodec 63 - so taking "latest" would compile against an API the DLLs do not
    have. That mismatch is not caught at build time; it shows up as audio that
    decodes to nothing.

    USAGE

        .\scripts\get_ffmpeg_headers.ps1
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
              -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
        cmake --build build
#>

param(
    [string]$Version = '8.1',
    [string]$Destination = "$PSScriptRoot\..\thirdparty\ffmpeg"
)

$ErrorActionPreference = 'Stop'

$url = "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-n$Version-latest-win64-gpl-shared-$Version.zip"

$include = Join-Path $Destination 'include'
$probe   = Join-Path $include 'libavcodec\avcodec.h'

if (Test-Path $probe) {
    Write-Host "FFmpeg headers already present at $include"
    exit 0
}

$scratch = Join-Path $env:TEMP ("ffmpeg-headers-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

try {
    Write-Host "fetching $url"
    $zip = Join-Path $scratch 'ffmpeg.zip'
    Invoke-WebRequest -Uri $url -OutFile $zip -TimeoutSec 900

    Write-Host 'extracting'
    Expand-Archive $zip -DestinationPath $scratch -Force

    # The archive nests the headers under a versioned directory.
    $src = Get-ChildItem $scratch -Recurse -Directory -Filter 'include' |
           Where-Object { Test-Path (Join-Path $_.FullName 'libavcodec\avcodec.h') } |
           Select-Object -First 1

    if (-not $src) { throw 'no include/libavcodec/avcodec.h in the archive' }

    New-Item -ItemType Directory -Force -Path $include | Out-Null
    Copy-Item (Join-Path $src.FullName '*') $include -Recurse -Force

    $count = (Get-ChildItem $include -Recurse -File -Filter '*.h').Count
    Write-Host "installed $count headers to $include"

    # The run-time half. xma.cpp loads avcodec with LoadLibrary instead of
    # linking it, so these have to sit next to CoD3.exe. Without them the
    # decoder silently never opens: the game still runs and still produces
    # audio frames, and the log reads "0 packets decoded", which looks like the
    # title having nothing to play rather than a missing dependency.
    #
    # Copied from the same archive as the headers, so the DLL and the headers
    # are the same build - which is the part that has to agree, since the API
    # moves between majors.
    $bin = Join-Path $scratch '*\bin'
    $dlls = Get-ChildItem $bin -Filter '*.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^(avcodec|avutil|swresample|swscale)-' }

    if ($dlls) {
        Write-Host "found $($dlls.Count) FFmpeg runtime DLLs"
        foreach ($target in @("$PSScriptRoot\..\build\CoD3Host",
                              "$PSScriptRoot\..\build\Release")) {
            if (Test-Path $target) {
                Copy-Item $dlls.FullName $target -Force
                Write-Host "  copied to $target"
            }
        }
    }
    else {
        Write-Warning 'no FFmpeg DLLs in the archive; XMA will not decode at run time'
    }
}
finally {
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $probe)) { throw 'headers did not land where the build looks' }
Write-Host 'done'
