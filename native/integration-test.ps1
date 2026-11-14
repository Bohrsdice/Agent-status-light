$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $root ('build\integration-' + [Guid]::NewGuid().ToString('N'))
$taskHome = Join-Path $fixture 'home'
$data = Join-Path $fixture 'state'
$sessions = Join-Path $taskHome 'sessions'
$program = Join-Path $root 'dist\CodexLight.exe'
New-Item -ItemType Directory -Path $sessions -Force | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$logA = Join-Path $sessions 'first.jsonl'
$logB = Join-Path $sessions 'second.jsonl'
function Add-Record($Path, $Value) {
    [IO.File]::AppendAllText($Path, ($Value | ConvertTo-Json -Compress -Depth 20) + "`n", $utf8)
}
function Task-Event($Kind, $Thread, $Turn, $Item = $null) {
    $payload = @{ type=$Kind; thread_id=$Thread; turn_id=$Turn }
    if ($null -ne $Item) { $payload.item=$Item }
    @{ type='event_msg'; timestamp=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ'); payload=$payload }
}
function Wait-State($Expected, $Thread) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        $statusFile = Join-Path $data 'status.json'
        $displayFile = Join-Path $data 'tray-display.json'
        if ((Test-Path -LiteralPath $statusFile) -and (Test-Path -LiteralPath $displayFile)) {
            $s = Get-Content -LiteralPath $statusFile -Raw -Encoding utf8 | ConvertFrom-Json
            $d = Get-Content -LiteralPath $displayFile -Raw -Encoding utf8 | ConvertFrom-Json
            if ($s.state -eq $Expected -and $s.thread_id -eq $Thread -and $d.state -eq $Expected -and $d.icon_registered -and -not $d.is_test) {
                return @{ state=$s.state; thread=$s.thread_id; icon_registered=$d.icon_registered; mode=$d.display_mode }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Expected / $Thread"
}
$threadA = '11111111-1111-4111-8111-111111111111'
$threadB = '22222222-2222-4222-8222-222222222222'
Add-Record $logA @{type='session_meta';payload=@{id=$threadA;cwd='D:\Portable Project A';source='vscode'}}
Add-Record $logA (Task-Event 'task_started' $threadA 'turn-a')
$process = Start-Process -FilePath $program -ArgumentList @('--home',"`"$taskHome`"",'--data-dir',"`"$data`"") -WindowStyle Hidden -PassThru
$checks = @()
try {
    $checks += Wait-State 'running' $threadA
    $card = @{type='AgentMessage';id='integration-question';delivery='async';questions=@(@{title='Integration test confirmation';options=@('Yes','No')})}
    Add-Record $logA (Task-Event 'item_completed' $threadA 'turn-a' $card)
    $checks += Wait-State 'waiting' $threadA
    $questionId = @('request_user_input_async','integration-question',0) | ConvertTo-Json -Compress
    $answer = ConvertTo-Json -InputObject @(@{questionItemId=$questionId;answer='Yes'}) -Compress
    $userItem = @{type='UserMessage';content=@(@{type='text';text="<send_user_message_question_reply>`n$answer`n</send_user_message_question_reply>"})}
    Add-Record $logA (Task-Event 'item_completed' $threadA 'turn-a' $userItem)
    $checks += Wait-State 'running' $threadA
    Add-Record $logA (Task-Event 'task_complete' $threadA 'turn-a')
    $checks += Wait-State 'completed' $threadA
    Add-Record $logB @{type='session_meta';payload=@{id=$threadB;cwd='D:\Portable Project B';source='vscode'}}
    Add-Record $logB (Task-Event 'task_started' $threadB 'turn-b')
    $checks += Wait-State 'running' $threadB
    Add-Record $logA (Task-Event 'task_complete' $threadA 'turn-a')
    Start-Sleep -Milliseconds 700
    $checks += Wait-State 'running' $threadB
    $report = @{ result='passed'; checks=$checks; fixture=$fixture }
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $root 'runtime\native-integration.json') -Encoding utf8
    $report | ConvertTo-Json -Depth 10
} finally {
    Start-Process -FilePath $program -ArgumentList @('--data-dir',"`"$data`"",'--quit') -WindowStyle Hidden -Wait
    if (-not $process.WaitForExit(10000)) { throw 'Test monitor did not exit' }
}
