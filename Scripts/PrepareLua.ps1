param(
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 may default to older TLS protocols.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$version = '5.5.1'
$archiveName = "lua-$version.tar.gz"
$expectedSha256 = '1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce'
$src = Join-Path $Destination 'src'
if (Test-Path (Join-Path $src 'lua.h')) {
    Write-Host "Lua $version source already present: $src"
    exit 0
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$archive = Join-Path $env:TEMP $archiveName
$url = "https://www.lua.org/ftp/$archiveName"
Write-Host "Downloading official Lua $version source. This is a build-time dependency only."
try {
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
} catch {
    throw "Could not download Lua 5.5.1 from $url. Check internet/proxy settings, or download the official archive manually and place it at: $archive. Details: $($_.Exception.Message)"
}
$actualSha256 = (Get-FileHash -Algorithm SHA256 -Path $archive).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "Lua source SHA-256 mismatch. Expected $expectedSha256, got $actualSha256"
}
$extract = Join-Path $env:TEMP "rf2-lua-$version"
if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extract | Out-Null
tar.exe -xzf $archive -C $extract
if ($LASTEXITCODE -ne 0) { throw 'Failed to extract Lua source archive.' }
$sourceRoot = Join-Path $extract "lua-$version"
Copy-Item (Join-Path $sourceRoot 'src') $Destination -Recurse -Force
Write-Host "Lua $version source prepared: $src"
