param(
    [Parameter(Mandatory)]
    [String]$srcDir,
    [String]$destDir,

    [Parameter(ValueFromRemainingArguments)]
    $AdditionalParams
)

Write-Output $srcDir
Write-Output $destDir
Write-Output "---"
Write-Output $AdditionalParams

$files = Get-ChildItem $srcDir -Recurse -File -Include *.gltf | Select-Object FullName

$gltfNames = $files | Split-Path -LeafBase | Sort | Group-Object -AsHashTable

Write-Output $gltfNames

$additional = $AdditionalParams -join " "

$files | Foreach-Object -ThrottleLimit 5 -Parallel {
    $inFile = $_.FullName
    $gltfName = Split-Path $inFile -LeafBase
    
    if (($using:gltfNames)[$gltfName].Count -gt 1)
    {
        # Need to further qualify the name
        $gltfName = (Split-Path (Split-Path $inFile -Parent) -Leaf) + "_" + $gltfName
    }

    $outFile = Join-Path $using:destDir ($gltfName + ".marc")
    Invoke-Expression "${PSScriptRoot}../Build/x64/Release/Output/MiniArchive/MiniArchive.exe $using:additional `"$inFile`" `"$outFile`""
}