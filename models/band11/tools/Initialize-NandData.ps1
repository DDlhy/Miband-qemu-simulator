param([string]$Path = (Join-Path $PSScriptRoot '..\userdata\nand-data.img'))
$ErrorActionPreference = 'Stop'
$dataPath = [System.IO.Path]::GetFullPath($Path)
$expectedSize = 136MB
if (Test-Path -LiteralPath $dataPath) {
    if ((Get-Item -LiteralPath $dataPath).Length -ne $expectedSize) {
        throw "NAND data image has an unexpected size: $dataPath"
    }
    exit 0
}
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dataPath)) | Out-Null
$block = New-Object byte[] 1MB
$block[0] = 255
for ($filled = 1; $filled -lt $block.Length; $filled *= 2) {
    [System.Array]::Copy($block, 0, $block, $filled, $filled)
}
$stream = [System.IO.File]::Open($dataPath, [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
try {
    for ($offset = 0; $offset -lt $expectedSize; $offset += $block.Length) {
        $stream.Write($block, 0, $block.Length)
    }
    $stream.Flush($true)
} finally {
    $stream.Dispose()
}
Write-Output "Created erased NAND data image: $dataPath"
