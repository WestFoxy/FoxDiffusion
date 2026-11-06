param([string]$Destination = "$PSScriptRoot/../build/toolchain/cuda")
$ErrorActionPreference = 'Stop'
# Official NVIDIA redistrib_12.8.1.json, pinned independently of the network response.
$packages = @(
    @('cuda_nvcc', '12.8.93', '9fdc70b4271ed9aad4d64cd7076a7d96ec36512d074b9995fe638de669197391'),
    @('cuda_cudart', '12.8.90', '4a39058fd8519444a81cfc7ae055d136f48d1a31ffa41ae255b35b2edd61e13b'),
    @('cuda_cccl', '12.8.90', 'bd8548fa1ae82f92910bebc3079e14bd58c5a92aa64596d46bd610a478cb39d7'),
    @('libcublas', '12.8.4.1', '57a470112cec7e112c95253dde8b3c7184d795dbd92b0bde77a4cb7f8c94c8aa')
)
$Destination = [IO.Path]::GetFullPath($Destination)
$cache = Join-Path (Split-Path $Destination) 'downloads'
New-Item -ItemType Directory -Force $Destination, $cache | Out-Null
foreach ($package in $packages) {
    $name, $version, $hash = $package
    $stem = "$name-windows-x86_64-$version-archive"
    $archive = Join-Path $cache "$stem.zip"
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $hash) {
        & curl.exe -L --fail --show-error "https://developer.download.nvidia.com/compute/cuda/redist/$name/windows-x86_64/$stem.zip" -o $archive
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $name" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $hash) { throw "SHA-256 mismatch: $name" }
    Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
    Get-ChildItem -LiteralPath (Join-Path $cache $stem) | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
    Copy-Item -LiteralPath (Join-Path $cache "$stem/LICENSE") -Destination (Join-Path $Destination "LICENSE-$name") -Force
}
Write-Output "Portable CUDA Toolkit: $Destination"
