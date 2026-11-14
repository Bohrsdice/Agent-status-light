$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$program=Join-Path $root 'dist\CodexLight.exe'
$data=Join-Path $root ('build\cursor-integration-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $data -Force | Out-Null
$utf8=[Text.UTF8Encoding]::new($false)
function Send-Hook($event) {
    $info=[Diagnostics.ProcessStartInfo]::new($program)
    $info.Arguments='--cursor-light-hook-v1 --data-dir "'+$data+'"'
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardInput=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $info.StandardInputEncoding=$utf8;$info.StandardOutputEncoding=$utf8
    $p=[Diagnostics.Process]::Start($info)
    $p.StandardInput.WriteLine(($event|ConvertTo-Json -Depth 10 -Compress));$p.StandardInput.Close()
    $result=$p.StandardOutput.ReadToEnd();$p.WaitForExit()
    if($p.ExitCode -ne 0){throw 'Hook failed'}
    $result|ConvertFrom-Json
}
function Assert-State($expected) {
    $out=Join-Path $data 'snapshot.json'
    Start-Process -FilePath $program -ArgumentList @('--cursor','--data-dir',('"'+$data+'"'),'--snapshot',('"'+$out+'"')) -WindowStyle Hidden -Wait
    $state=Get-Content -LiteralPath $out -Raw -Encoding utf8 | ConvertFrom-Json
    if($state.state -ne $expected){throw "Expected $expected; got $($state.state): $($state.reason). Cursor must be open."}
    Write-Output "PASS executable hook -> snapshot $expected"
}
$e=@{conversation_id='integration';generation_id='one';hook_event_name='beforeSubmitPrompt';workspace_roots=@('C:\test 中文')}
if((Send-Hook $e).continue -ne $true){throw 'Prompt hook response'}
Assert-State 'running'
$e.hook_event_name='preToolUse';$e.tool_name='AskQuestion';$e.tool_use_id='q';$e.tool_input=@{run_async=$false}
if((Send-Hook $e).permission -ne 'allow'){throw 'Question hook response'}
Assert-State 'waiting'
$e.hook_event_name='postToolUse';Send-Hook $e|Out-Null;Assert-State 'running'
$e.hook_event_name='stop';$e.status='completed';Send-Hook $e|Out-Null;Assert-State 'completed'
$e.hook_event_name='beforeSubmitPrompt';$e.generation_id='two';Send-Hook $e|Out-Null
$e.hook_event_name='stop';$e.generation_id='one';Send-Hook $e|Out-Null;Assert-State 'running'
$e.generation_id='two';$e.status='aborted';Send-Hook $e|Out-Null;Assert-State 'interrupted'
# Exercise the same Windows PowerShell pipeline that Cursor uses.
$e.hook_event_name='beforeSubmitPrompt';$e.generation_id='three'
$pipelineResult=($e|ConvertTo-Json -Compress -Depth 10)| & $program --cursor-light-hook-v1 --data-dir $data | Out-String
if(($pipelineResult|ConvertFrom-Json).continue -ne $true){throw 'PowerShell pipe response'}
Assert-State 'running'
# Exercise the exact registered command under Windows PowerShell 5.1.
# This requires neither a model request nor any network access.
$e.generation_id='windows-powershell';$e.prompt='中文测试 — 不执行模型任务'
$payloadPath=Join-Path $data 'payload.json'
[IO.File]::WriteAllText($payloadPath,($e|ConvertTo-Json -Depth 10 -Compress),$utf8)
$registered=((Get-Content -LiteralPath (Join-Path $env:USERPROFILE '.cursor\hooks.json') -Raw -Encoding utf8|ConvertFrom-Json).hooks.beforeSubmitPrompt | Where-Object {$_.command -like '* --cursor-light-hook-v1 --data-dir *'} | Select-Object -First 1).command
$realData=Join-Path $env:LOCALAPPDATA 'CodexLight'
$registered=$registered.Replace("'"+$realData.Replace("'","''")+"'","'"+$data.Replace("'","''")+"'")
$scriptPath=Join-Path $data 'pipeline.ps1'
$script="[IO.File]::ReadAllText('"+$payloadPath.Replace("'","''")+"', [Text.Encoding]::UTF8) | "+$registered
[IO.File]::WriteAllText($scriptPath,$script,[Text.UTF8Encoding]::new($true))
$legacyShell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$result=& $legacyShell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $scriptPath
if(($result|ConvertFrom-Json).continue -ne $true){throw 'Registered Windows PowerShell wrapper failed'}
Assert-State 'running'
$stored=Get-Content -LiteralPath (Join-Path $data 'cursor-state.json') -Raw -Encoding utf8 | ConvertFrom-Json
if($stored.sessions.integration.generation -ne 'windows-powershell' -or $stored.sessions.integration.project -ne 'C:\test 中文'){throw 'UTF-8 input was damaged'}
Write-Output '8 executable integration checks passed, including Windows PowerShell 5.1 UTF-8 pipeline'
