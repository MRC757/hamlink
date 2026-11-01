# Builds the self-contained Windows app into .\publish  (run on a PC with the .NET 8 SDK).
# The SDK needs NuGet access ONCE to fetch NAudio and System.IO.Ports. Build on a connected PC, then
# copy the whole .\publish folder to the air-gapped machine: it needs no .NET install and no internet.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
dotnet publish HamLink.csproj -c Release -r win-x64 --self-contained true -o publish
Write-Host "Done. Copy the 'publish' folder to the target PC and run HamLink.exe"
