#requires -Version 7.0
function Find-F6VisualStudioTool {
    param([string]$ExplicitPath, [Parameter(Mandatory)][string]$Kind)
    if ($ExplicitPath) {
        if (Test-Path -LiteralPath $ExplicitPath -PathType Leaf) {
            return (Get-Item -LiteralPath $ExplicitPath).FullName
        }
        $command = Get-Command $ExplicitPath -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Source }
        throw "Tool does not exist: $ExplicitPath"
    }
    $toolName = if ($Kind -eq 'MSBuild') { 'MSBuild.exe' } else { 'VsDevCmd.bat' }
    $command = Get-Command $toolName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Cannot locate $toolName. Install Visual Studio 2022 C++ tools or supply its path."
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'No Visual Studio C++ installation found.' }
    $installation = @($installation)[0]
    $relative = if ($Kind -eq 'MSBuild') { 'MSBuild/Current/Bin/amd64/MSBuild.exe' } else { 'Common7/Tools/VsDevCmd.bat' }
    $tool = Join-Path $installation $relative
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing Visual Studio tool: $tool" }
    return $tool
}
