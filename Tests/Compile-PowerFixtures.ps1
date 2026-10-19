param()
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$temporary=Join-Path $workspace 'Temp/Cpu08530/Asl'
[IO.Directory]::CreateDirectory($temporary)|Out-Null
$compiler=(Get-Command iasl -ErrorAction Stop).Source
foreach($kind in @('pss','cppc')) {
    $prefix=Join-Path $temporary $kind
    $arguments=@('-p',$prefix)
    if($kind -ceq 'cppc'){$arguments+=@('-D','CPPCTEST')}
    $arguments+=(Join-Path $PSScriptRoot 'power_fixture.asl')
    & $compiler @arguments
    if($LASTEXITCODE -ne 0){throw ('ASL fixture compilation failed: '+$kind)}
    $bytes=[IO.File]::ReadAllBytes($prefix+'.aml')
    $lines=[Collections.Generic.List[string]]::new()
    $lines.Add('// Copyright 2026 R4. SPDX-License-Identifier: Apache-2.0')
    $lines.Add('// Generated from power_fixture.asl by Compile-PowerFixtures.ps1.')
    $lines.Add('static const unsigned char power_'+$kind+'_fixture[] = {')
    for($i=0;$i -lt $bytes.Length;$i+=16){
        $part=@($bytes[$i..([Math]::Min($i+15,$bytes.Length-1))]|ForEach-Object {'0x'+$_.ToString('x2')})
        $lines.Add('    '+($part -join ',')+',')
    }
    $lines.Add('};')
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot ('power_'+$kind+'_fixture.h')),($lines -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
}
