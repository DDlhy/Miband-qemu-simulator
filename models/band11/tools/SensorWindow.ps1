param([string]$SessionId, [switch]$Tools, [string]$VerifyOutput, [int]$AutoCloseSeconds=0)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class VelaSensorWindowNative {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int mode);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$utf8 = New-Object Text.UTF8Encoding($false)
$identity = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($projectRoot + $SessionId))).Replace('-', '')
$mutex = New-Object Threading.Mutex($false, ('Local\VelaSensorWindow-' + $identity))
if (-not $mutex.WaitOne(0)) { $mutex.Dispose(); return }
$pythonExe = Join-Path $projectRoot '..\..\runtime\python\python.exe'
if (-not $pythonExe) { throw '未找到 Python 运行环境。' }
$work = Join-Path $projectRoot ('userdata\sensor-window-' + $PID)
[IO.Directory]::CreateDirectory($work) | Out-Null
$script:pending = New-Object Collections.ArrayList
$script:offlineQueue = New-Object Collections.Queue
$script:inputDue = [DateTime]::MaxValue
$script:inputValues = @{}
$script:hydrating = $false
$script:active = $null
$script:loadedSession = ''
$script:busy = $false
$form = New-Object Windows.Forms.Form
$form.Text = '小米手环 11 NFC · 传感器控制器'
$form.ClientSize = New-Object Drawing.Size(820, 685)
$form.MinimumSize = New-Object Drawing.Size(836, 724)
$form.StartPosition = 'CenterScreen'
$form.AutoScaleMode = 'Dpi'
$form.Font = New-Object Drawing.Font('Microsoft YaHei UI', 10)
$form.BackColor = [Drawing.Color]::FromArgb(245, 247, 250)

function Label($parent, $text, $x, $y, $w, $h = 26) {
    $c = New-Object Windows.Forms.Label
    $c.Text = $text; $c.Location = New-Object Drawing.Point($x, $y)
    $c.Size = New-Object Drawing.Size($w, $h)
    $parent.Controls.Add($c); return $c
}
function Button($parent, $text, $x, $y, $w, $action) {
    $c = New-Object Windows.Forms.Button
    $c.Text = $text; $c.Location = New-Object Drawing.Point($x, $y)
    $c.Size = New-Object Drawing.Size($w, 34)
    $c.FlatStyle = 'System'; $c.Add_Click($action)
    $parent.Controls.Add($c); return $c
}
function New-SensorGroup($text, $x, $y, $w, $h) {
    $c = New-Object Windows.Forms.GroupBox
    $c.Text = $text; $c.Location = New-Object Drawing.Point($x, $y)
    $c.Size = New-Object Drawing.Size($w, $h); $form.Controls.Add($c); return $c
}
function Input($parent, $text, $x, $y, $w) {
    $c = New-Object Windows.Forms.TextBox
    $c.Text = $text; $c.Location = New-Object Drawing.Point($x, $y)
    $c.Size = New-Object Drawing.Size($w, 28); $parent.Controls.Add($c); return $c
}
function Number($box) {
    $value = 0.0
    if (-not [double]::TryParse($box.Text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or [double]::IsNaN($value) -or [double]::IsInfinity($value)) {
        throw '请输入数值，可用负数、小数或科学计数法。'
    }
    return $value
}
function Read-SharedText($path) {
    $stream=New-Object IO.FileStream($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
    try {return $reader.ReadToEnd()} finally {$reader.Dispose()}
}
function Send-Control($request) {
    try {
        $id = [guid]::NewGuid().ToString('N')
        if ($script:active) {
            $sessionPath = Join-Path $projectRoot $script:active.session_id
            $inputPath = Join-Path $sessionPath ('sensor-requests\'+$id+'.json')
            $replyPath = Join-Path $sessionPath ('sensor-replies\'+$id+'.json')
            $request.expires_at = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()/1000.0+6
            [IO.File]::WriteAllText(($inputPath+'.tmp'), ($request | ConvertTo-Json -Depth 8), $utf8)
            [IO.File]::Move(($inputPath+'.tmp'), $inputPath)
            $script:pending.Add(@{ process=$null; request=$inputPath; reply=$replyPath; started=[DateTime]::UtcNow; direct=$true; quiet=($request.operation -eq 'preview') }) | Out-Null
            if($request.operation -ne 'preview') {$notice.Text='已即时发送，等待固件处理…'}
            return
        }
        $script:offlineQueue.Enqueue($request)
    } catch { $notice.Text = $_.Exception.Message }
}
function Start-OfflineControl {
    if ($script:busy -or -not $script:offlineQueue.Count) { return }
    $request=$script:offlineQueue.Dequeue()
    try {
        $id = [guid]::NewGuid().ToString('N')
        $inputPath = Join-Path $work ($id + '.request.json')
        $replyPath = Join-Path $work ($id + '.reply.json')
        [IO.File]::WriteAllText($inputPath, ($request | ConvertTo-Json -Depth 8), $utf8)
        $args = '-X utf8 -B "{0}" --request-file "{1}" --reply-file "{2}"' -f (Join-Path $PSScriptRoot 'sensor_controller.py'), $inputPath, $replyPath
        if ($SessionId) { $args += ' --session-id "' + $SessionId + '"' }
        $p = Start-Process -FilePath $pythonExe -ArgumentList $args -PassThru -WindowStyle Hidden
        $script:pending.Add(@{ process = $p; request = $inputPath; reply = $replyPath; started = [DateTime]::UtcNow; direct=$false }) | Out-Null
        $script:busy = $true; $notice.Text = '正在保存…'
    } catch { $notice.Text = $_.Exception.Message }
}
function Queue-Input($key,$box) {
    if ($script:hydrating) { return }
    try {
        $script:inputValues[$key] = Number $box
        $script:inputDue = [DateTime]::UtcNow.AddMilliseconds(120)
    } catch {
        $script:inputValues.Remove($key)
        $notice.Text='输入中… 请填写完整数值。'
    }
}
function Flush-Inputs {
    if (-not $script:inputValues.Count) { return }
    $request=@{ operation='inputs' };$extra=@{operation='sensors'}
    foreach($key in $script:inputValues.Keys) {
        if($key -in @('battery_mv','ntc_mv')) {$request[$key]=$script:inputValues[$key]}
        else {$extra[$key]=$script:inputValues[$key]}
    }
    $script:inputValues=@{}; $script:inputDue=[DateTime]::MaxValue
    if($request.Count -gt 1) {Send-Control $request}
    if($extra.Count -gt 1) {Send-Control $extra}
}

$title = Label $form '传感器与硬件控制' 22 16 480 35
$title.Font = New-Object Drawing.Font('Microsoft YaHei UI', 17, [Drawing.FontStyle]::Bold)
$connection = Label $form '正在查找模拟器…' 22 58 774
$hint = Label $form '数值只给参考，无人为上下限；原厂每 60～120 秒采样，电量缓慢平滑更新。' 22 89 775 28
$batteryGroup = New-SensorGroup '电池' 22 126 378 168
Label $batteryGroup '电池电压（mV）' 16 29 160 | Out-Null
$battery = Input $batteryGroup '3960' 176 27 180
$battery.Add_TextChanged({ Queue-Input 'battery_mv' $battery })
$battery.Add_Leave({ Flush-Inputs })
Label $batteryGroup '参考：3600 低电量，3960 常用，4350 高电量' 16 63 350 26 | Out-Null
$charging = New-Object Windows.Forms.CheckBox
$charging.Text = '接入充电器'; $charging.Location = New-Object Drawing.Point(16, 97)
$charging.Size = New-Object Drawing.Size(140, 28)
$charging.Add_Click({ Flush-Inputs; Send-Control @{ operation='charging'; connected=[bool]$charging.Checked } })
$batteryGroup.Controls.Add($charging)
$chargeStatus = Label $batteryGroup '未连接' 174 99 188
Label $batteryGroup '开关只接入电源，电压由上方独立调整。' 16 132 350 22 | Out-Null
$tempGroup = New-SensorGroup '温度探头' 418 126 378 168
Label $tempGroup 'NTC 电压（mV）' 16 29 160 | Out-Null
$ntc = Input $tempGroup '450' 176 27 180
$ntc.Add_TextChanged({ Queue-Input 'ntc_mv' $ntc })
$ntc.Add_Leave({ Flush-Inputs })
Label $tempGroup '参考：450 为默认探头输入。' 16 63 348 | Out-Null
Label $tempGroup '同时调整 ADC 0、1、3、5、6。
原厂使用第 5 路读取探头，自行换算温度。
可直接输入负数、超参考值或科学计数法。' 16 95 348 65 | Out-Null
$hardware = New-SensorGroup '已支持的硬件' 22 310 774 80
$actions = @(@('双击唤醒','wake'),@('点击','tap'),@('上滑','up'),@('下滑','down'),@('左滑','left'),@('右滑','right'))
for ($i=0;$i -lt $actions.Count;$i++) {
    $gesture=$actions[$i][1]
    $handler={ Send-Control @{ operation='gesture'; gesture=$gesture } }.GetNewClosure()
    Button $hardware $actions[$i][0] (16+$i*88) 30 80 $handler | Out-Null
}
$reboot = Button $hardware '重启固件' 547 30 94 { Send-Control @{ operation='reboot' } }
$defaults = Button $hardware '恢复默认' 648 30 108 { Send-Control @{ operation='defaults' }; $script:loadedSession='' }
$grid = New-Object Windows.Forms.ListView
$grid.Location = New-Object Drawing.Point(22, 408); $grid.Size = New-Object Drawing.Size(774, 190)
$grid.View = 'Details'; $grid.FullRowSelect=$true; $grid.GridLines=$true
foreach ($column in @(@('通道',55),@('用途',154),@('输入 mV',130),@('已采样 mV',130),@('固件读取 mV',139),@('读取次数',125))) {
    $grid.Columns.Add($column[0],$column[1]) | Out-Null
}
$form.Controls.Add($grid)
$hardwareStatus = Label $form '屏幕 —   亮度 —   帧数 —   触摸 —' 22 611 775
$notice = Label $form '修改数值自动发送；充电开关立即发送，支持连续插拔。' 22 645 775 28
$notice.ForeColor = [Drawing.Color]::FromArgb(28, 91, 145)

. (Join-Path $PSScriptRoot 'SensorExtraWindow.ps1')
. (Join-Path $PSScriptRoot 'SensorToolsWindow.ps1')
if($Tools){$tabs.SelectedTab=$toolsPage}

function Refresh-State {
    $latest=$null
    foreach ($mode in @('raw-bluetooth-sessions','offline-sessions')) {
        $folder=Join-Path $projectRoot ('artifacts\'+$mode)
        if (-not (Test-Path -LiteralPath $folder)) { continue }
        $dirs=Get-ChildItem -LiteralPath $folder -Directory | Sort-Object Name -Descending | Select-Object -First 30
        foreach ($dir in $dirs) {
            try {
                $s=(Read-SharedText (Join-Path $dir.FullName 'sensor-status.json')) | ConvertFrom-Json
                $now=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()/1000.0)
                if ($s.connected -and $now-$s.updated_at -ge 0 -and $now-$s.updated_at -lt 5 -and (-not $SessionId -or $s.session_id -eq $SessionId)) {
                    if (-not $latest -or $s.updated_at -gt $latest.updated_at) { $latest=$s }
                }
            } catch { }
        }
    }
    $script:active=$latest
    if (-not $latest) {
        $connection.Text='模拟器未运行 · 输入与充电开关可保存，下次启动生效'
        if (-not $SessionId -and $script:loadedSession -ne 'offline') {
            $profilePath=Join-Path $projectRoot 'userdata\sensor-profile.json'
            if (Test-Path -LiteralPath $profilePath) {
                try {
                    $profile=[IO.File]::ReadAllText($profilePath) | ConvertFrom-Json
                    $script:hydrating=$true
                    $battery.Text=((800+([double]$profile.channels.'2'-15968)*250/4848)*7).ToString('0.##',[Globalization.CultureInfo]::InvariantCulture)
                    $ntc.Text=(800+([double]$profile.channels.'5'-15968)*250/4848).ToString('0.##',[Globalization.CultureInfo]::InvariantCulture)
                    $charging.Checked=[bool]$profile.charging
                    Hydrate-Extra $profile.hardware
                } finally { $script:hydrating=$false }
            }
            $script:loadedSession='offline'
        }
        Refresh-Extra $null
        return
    }
    $s=$latest.state
    $connection.Text=('已连接 · {0} · PID {1}' -f $latest.session_id.Split('/')[-1],$latest.qemu_pid)
    if ($script:loadedSession -ne $latest.session_id) {
        $script:hydrating=$true
        $battery.Text=$s.battery_mv.ToString([Globalization.CultureInfo]::InvariantCulture)
        $ntc.Text=$s.channels[5].input_mv.ToString([Globalization.CultureInfo]::InvariantCulture)
        $h=[pscustomobject]@{}
        foreach($kind in @('accel','gyro','mag')) {for($i=0;$i -lt 3;$i++) {$h|Add-Member NoteProperty ($kind+'_'+@('x','y','z')[$i]) $s.motion.$kind[$i]}}
        for($i=0;$i -lt 4;$i++) {$h|Add-Member NoteProperty ('pd_'+$i) $s.optical.pd[$i]}
          foreach($key in @('optical_bias','pulse_bpm','pulse_amplitude')) {
              if($null -ne $s.optical.PSObject.Properties[$key]) {$h|Add-Member NoteProperty $key $s.optical.$key}
          }
        $h|Add-Member NoteProperty ambient_lux $s.als.lux
        Hydrate-Extra $h
        $script:hydrating=$false
        $script:loadedSession=$latest.session_id
    }
    if (-not $script:pending.Count) { $charging.Checked=[bool]$s.charger.connected }
    $chargeStatus.Text=if($s.charger.connected){'充电器已接入'}else{'充电器已断开'}
    $grid.BeginUpdate(); $grid.Items.Clear()
    foreach ($ch in $s.channels) {
        $role=if($ch.channel -eq 2){'电池 VBAT / 7'}elseif($ch.channel -eq 4){'板型识别（固定）'}else{'温度探头'}
        $row=New-Object Windows.Forms.ListViewItem([string]$ch.channel)
        $sample=if($ch.samples){[string]$ch.sampled_mv}else{'—'}
        $read=if($ch.guest_reads){[string]$ch.last_read_mv}else{'—'}
        foreach($value in @($role,[string]$ch.input_mv,$sample,$read,[string]$ch.guest_reads)) { $row.SubItems.Add($value) | Out-Null }
        $grid.Items.Add($row) | Out-Null
    }
    $grid.EndUpdate()
    Refresh-Extra $s
    $screen=if($s.panel.power -and $s.panel.brightness){'亮屏'}else{'熄屏'}
    $hardwareStatus.Text=('屏幕 {0}   亮度 {1}   绘制帧 {2}   触摸 {3}/{4}   充电芯片读取 {5}' -f $screen,$s.panel.brightness,$s.panel.frames,$s.touch.acknowledged,$s.touch.received,$s.charger.reads)
}
$timer=New-Object Windows.Forms.Timer
$timer.Interval=40
$script:lastRefresh=[DateTime]::MinValue
$timer.Add_Tick({
    try {
        Complete-DeviceTool
        if ($script:inputValues.Count -and [DateTime]::UtcNow -ge $script:inputDue) {
            Flush-Inputs
        }
        foreach ($p in @($script:pending.ToArray())) {
            if (Test-Path -LiteralPath $p.reply) {
                $reply=[IO.File]::ReadAllText($p.reply) | ConvertFrom-Json
                if(-not $p.quiet -or -not $reply.ok) {$notice.Text=if(-not $reply.ok){$reply.error}elseif($reply.stored_only){'已保存，下次启动生效。'}elseif($reply.wrapped_channels.Count){'已发送。超出 ADC 位宽的数值按低 16 位传给固件，没有钳制。'}else{'输入已发送并保存。'}}
                if (Test-Path -LiteralPath $p.request) { Remove-Item -LiteralPath $p.request -Force }
                Remove-Item -LiteralPath $p.reply -Force
                if ($p.process) { $p.process.Dispose(); $script:busy=$false }
                $script:pending.Remove($p)
            } elseif (([DateTime]::UtcNow-$p.started).TotalSeconds -gt 12) {
                $notice.Text='控制请求超时，请检查模拟器状态。'
                if ($p.process) { $p.process.Dispose(); $script:busy=$false }
                $script:pending.Remove($p)
            }
        }
        Start-OfflineControl
        if (([DateTime]::UtcNow-$script:lastRefresh).TotalSeconds -ge .2) {
            Refresh-State; $script:lastRefresh=[DateTime]::UtcNow
        }
        foreach ($c in $hardware.Controls) { if ($c -is [Windows.Forms.Button] -and $c -ne $defaults) { $c.Enabled=($null -ne $script:active) } }
    } catch { $notice.Text=$_.Exception.Message }
})
$script:featureVerifyStart=[DateTime]::UtcNow;$script:featureVerified=$false
$timer.Add_Tick({
    if($VerifyOutput -and -not $script:featureVerified -and ([DateTime]::UtcNow-$script:featureVerifyStart).TotalSeconds -gt 3){
        [IO.Directory]::CreateDirectory($VerifyOutput)|Out-Null
        $index=0
        foreach($page in $tabs.TabPages){
            $tabs.SelectedTab=$page;$form.Refresh()
            $bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height)
            $form.DrawToBitmap($bitmap,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height)))
            $bitmap.Save((Join-Path $VerifyOutput ('native-'+$index+'.png')));$bitmap.Dispose();$index++
        }
        $tabs.SelectedIndex=0;$script:featureVerified=$true
    }
    if($AutoCloseSeconds -gt 0 -and ([DateTime]::UtcNow-$script:featureVerifyStart).TotalSeconds -gt $AutoCloseSeconds){$form.Close()}
})
$form.Add_Shown({
    [VelaSensorWindowNative]::ShowWindow($form.Handle,5) | Out-Null
    [VelaSensorWindowNative]::SetForegroundWindow($form.Handle) | Out-Null
    Refresh-State; $timer.Start()
    if($Tools -and -not $script:deviceToolLoaded){Start-DeviceTool 'identity.read'}
})
$script:closeRequested=$false
$form.Add_FormClosing({
    Flush-Inputs
    # Online requests already belong to the supervisor. Offline saves must finish
    # before temporary request files and the window process can be removed.
    if ($script:busy -or $script:offlineQueue.Count -or $script:deviceToolJob) {
        $_.Cancel=$true
        $script:closeRequested=$true
        $notice.Text='正在保存最后的输入，完成后关闭…'
        Start-OfflineControl
    }
})
$timer.Add_Tick({
    if ($script:closeRequested -and -not $script:busy -and -not $script:offlineQueue.Count -and -not $script:deviceToolJob) { $form.Close() }
})
try { [Windows.Forms.Application]::Run($form) }
finally {
    $timer.Stop(); $timer.Dispose(); $form.Dispose()
    $mutex.ReleaseMutex(); $mutex.Dispose()
    Get-ChildItem -LiteralPath $work -File | Where-Object Name -Match '^[0-9a-f]{32}\.(request|reply)\.json$' | Remove-Item -Force
    if (-not (Get-ChildItem -LiteralPath $work)) { Remove-Item -LiteralPath $work }
}
