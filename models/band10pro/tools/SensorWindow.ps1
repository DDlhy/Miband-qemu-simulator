param([string]$SessionId,[string]$VerifyOutput,[int]$AutoCloseSeconds=0,[switch]$VerifyExercise,[switch]$Tools)
$ErrorActionPreference='Stop'
if($VerifyExercise -and (-not $VerifyOutput -or $SessionId -notlike 'artifacts/verification/*')){throw '窗口验证只能操作独立测试会话。'}
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot '..\..\..\shared\ui\SensorWidgets.cs') -ReferencedAssemblies System.Windows.Forms,System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'SensorBand3D.cs') -ReferencedAssemblies System.Windows.Forms,System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$root=Split-Path $PSScriptRoot -Parent
$python=Join-Path $root '..\..\runtime\python\python.exe'
$utf8=New-Object Text.UTF8Encoding($false)
$script:active=$null;$script:loaded='';$script:hydrating=$false
$script:displayRotation=0;$script:rotationPendingUntil=[DateTime]::MinValue
$script:values=@{};$script:due=[DateTime]::MaxValue;$script:pending=New-Object Collections.ArrayList
$work=Join-Path $root ('artifacts\sensor-window-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work)|Out-Null
$mutexName='Local\MiBandP67NativeSensors';if($VerifyOutput){$mutexName+='-verify'}
$mutex=New-Object Threading.Mutex($false,$mutexName)
$toolsSignal=Join-Path $root 'artifacts\open-device-tools.request'
if(-not $mutex.WaitOne(0)){
    if($Tools){[IO.File]::WriteAllText($toolsSignal,[DateTime]::UtcNow.Ticks.ToString(),$utf8)}
    exit
}
function Read-Shared($path){
    $f=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try{$r=New-Object IO.StreamReader($f,[Text.Encoding]::UTF8);try{$r.ReadToEnd()}finally{$r.Dispose()}}finally{$f.Dispose()}
}
function Label($p,$text,$x,$y,$w=300,$h=28){
    $c=New-Object Windows.Forms.Label;$c.Text=$text;$c.SetBounds($x,$y,$w,$h);$p.Controls.Add($c);return $c
}
function Button($p,$text,$x,$y,$w,$fn){
    $c=New-Object Windows.Forms.Button;$c.Text=$text;$c.SetBounds($x,$y,$w,32);$c.Add_Click($fn);$p.Controls.Add($c);return $c
}
function Page($name){$p=New-Object Windows.Forms.TabPage;$p.Text=$name;$p.BackColor=[Drawing.Color]::White;$tabs.TabPages.Add($p);return $p}
function Number($box){
    $n=[double]::Parse($box.Text,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture)
    if([double]::IsNaN($n)-or[double]::IsInfinity($n)){throw '请输入有限数值。'};return $n
}
function Queue-Input($box){
    if($script:hydrating){return}
    try{
        $script:values[$box.Tag]=Number $box
        if($script:due -eq [DateTime]::MaxValue){$script:due=[DateTime]::UtcNow.AddMilliseconds(120)}
    }catch{$script:values.Remove($box.Tag);$notice.Text='请输入完整数值。'}
}
$script:boxes=@{}
function Input($p,$caption,$key,$value,$x,$y,$w=280){
    Label $p $caption $x $y $w|Out-Null
    $c=New-Object Windows.Forms.TextBox;$c.Text=[string]$value;$c.Tag=$key;$c.Name='sensor_'+$key
    $c.SetBounds($x,($y+29),$w,28);$c.Add_TextChanged({Queue-Input $this});$c.Add_Leave({Flush-Inputs})
    $p.Controls.Add($c);$script:boxes[$key]=$c;return $c
}
function Send-Control($request){
    $id=[guid]::NewGuid().ToString('N');$reply=Join-Path $work ($id+'.reply.json')
    if($script:active){
        $folder=Join-Path $root $script:active.session_id
        $request.expires_at=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()/1000.0+6
        $path=Join-Path $folder ('sensor-requests\'+$id+'.json')
        $reply=Join-Path $folder ('sensor-replies\'+$id+'.json')
        $tmp=$path+'.tmp';[IO.File]::WriteAllText($tmp,($request|ConvertTo-Json -Depth 8),$utf8);[IO.File]::Move($tmp,$path)
        $process=$null
    }else{
        $path=Join-Path $work ($id+'.request.json')
        [IO.File]::WriteAllText($path,($request|ConvertTo-Json -Depth 8),$utf8)
        $args='-X utf8 -B "{0}" --request-file "{1}" --reply-file "{2}"' -f (Join-Path $PSScriptRoot 'sensor_controller.py'),$path,$reply
        if($SessionId){$args+=' --session-id "'+$SessionId+'"'}
        $process=Start-Process -FilePath $python -ArgumentList $args -WindowStyle Hidden -PassThru
    }
    $script:pending.Add(@{request=$path;reply=$reply;process=$process;at=[DateTime]::UtcNow})|Out-Null
}
function Flush-Inputs{
    if(-not $script:values.Count){return}
    $adc=@{operation='inputs'};$hw=@{operation='sensors'};$gnss=@{operation='gnss'}
    foreach($k in $script:values.Keys){if($k -in @('battery_mv','ntc_mv')){$adc[$k]=$script:values[$k]}elseif($k.StartsWith('gnss_')){$gnss[$k]=$script:values[$k]}else{$hw[$k]=$script:values[$k]}}
    $script:values=@{};$script:due=[DateTime]::MaxValue
    if($adc.Count -gt 1){Send-Control $adc};if($hw.Count -gt 1){Send-Control $hw}
    if($gnss.Count -gt 1){Send-Control $gnss}
}
$form=New-Object Windows.Forms.Form;$form.Text='小米手环 10 Pro · 传感器控制器'
$form.ClientSize=New-Object Drawing.Size(1030,744);$form.MinimumSize=New-Object Drawing.Size(1046,783)
$form.StartPosition='CenterScreen';$form.Font=New-Object Drawing.Font('Microsoft YaHei UI',10)
$form.BackColor=[Drawing.Color]::FromArgb(243,246,251)
$title=Label $form '10 Pro 传感器控制器' 18 12 600 35;$title.Font=New-Object Drawing.Font($form.Font.FontFamily,17,[Drawing.FontStyle]::Bold)
$connection=Label $form '正在查找模拟器…' 20 54 800 27
$gestures=@(@('双击唤醒','wake'),@('点击','tap'),@('上滑','up'),@('下滑','down'),@('左滑','left'),@('右滑','right'))
for($i=0;$i -lt $gestures.Count;$i++){
    $g=$gestures[$i][1];$handler={Send-Control @{operation='gesture';gesture=$g}}.GetNewClosure()
    Button $form $gestures[$i][0] (18+$i*94) 87 86 $handler|Out-Null
}
Button $form '恢复传感器默认' 684 87 145 {Send-Control @{operation='defaults'};$script:loaded=''}|Out-Null
$rotationStatus=Label $form '屏幕：0°' 838 65 166 20
Button $form '左转 90°' 838 87 80 {Rotate-Display -Delta -90}|Out-Null
Button $form '右转 90°' 922 87 82 {Rotate-Display -Delta 90}|Out-Null
$tabs=New-Object Windows.Forms.TabControl;$tabs.SetBounds(16,132,998,560);$tabs.Anchor='Top,Bottom,Left,Right';$form.Controls.Add($tabs)
$batteryPage=Page '电池 / 温度 / 充电'
Label $batteryPage '数值仅供参考，可直接输入；超出 ADC 位宽时按低 16 位传给硬件。' 18 16 940|Out-Null
Input $batteryPage '电池电压（mV）' 'battery_mv' 3960 20 62 410|Out-Null
Input $batteryPage 'NTC 探头电压（mV）' 'ntc_mv' 450 500 62 410|Out-Null
Label $batteryPage '参考：3600 低电量，3960 常用，4350 高电量' 20 127 450|Out-Null
Label $batteryPage '参考：450 为默认输入，温度由原厂驱动换算。' 500 127 460|Out-Null
$charging=New-Object Windows.Forms.CheckBox;$charging.Text='接入充电器（立即发送）';$charging.SetBounds(20,175,440,32)
$charging.Add_Click({Flush-Inputs;Send-Control @{operation='charging';connected=[bool]$charging.Checked}});$batteryPage.Controls.Add($charging)
$chargeStatus=Label $batteryPage '等待充电芯片…' 500 178 460
$adcGrid=New-Object Windows.Forms.ListView;$adcGrid.SetBounds(20,232,940,210);$adcGrid.View='Details';$adcGrid.GridLines=$true;$adcGrid.FullRowSelect=$true
foreach($c in @(@('通道',60),@('用途',180),@('输入 mV',170),@('已采样 mV',170),@('固件读取 mV',180),@('读取次数',140))){$adcGrid.Columns.Add($c[0],$c[1])|Out-Null};$batteryPage.Controls.Add($adcGrid)
Label $batteryPage '充电开关不修改电压。电量由原厂采样周期与平滑算法决定，设定值不会直接替换电量百分比。' 20 462 940 55|Out-Null
$motionPage=Page '运动 / 指南针'
$script:pads=@{}
$configs=@(@('accel','加速度（m/s²）',9.80665,0,0,9.80665),@('gyro','陀螺仪（°/s）',90,0,0,0),@('mag','磁场（µT）',50,30,0,40))
for($i=0;$i -lt 3;$i++){
    $cfg=$configs[$i];$key=$cfg[0];$x=18+$i*322
    Label $motionPage $cfg[1] $x 12 290|Out-Null
    $pad=New-Object VelaVectorPad;$pad.SetBounds($x,43,294,230);$pad.Tag=$key;$pad.Name='vector_'+$key
    $pad.Reference=$cfg[2];$pad.X=$cfg[3];$pad.Y=$cfg[4];$pad.Z=$cfg[5];$pad.Compass=($key -eq 'mag');$motionPage.Controls.Add($pad);$script:pads[$key]=$pad
    $pad.Add_VectorChanged({foreach($axis in @('x','y','z')){$script:boxes[$this.Tag+'_'+$axis].Text=$this.($axis.ToUpper()).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}})
    for($j=0;$j -lt 3;$j++){Input $motionPage (@('X','Y','Z')[$j]) ($key+'_'+@('x','y','z')[$j]) $cfg[3+$j] $x (286+$j*65) 285|Out-Null}
}
$motionStatus=Label $motionPage '等待原厂驱动读取…' 18 490 950 36
$opticalPage=Page '光学 / LED / PD'
Label $opticalPage 'HX3695 原始光学输入；灯亮状态来自固件配置，心率与血氧结果由原厂算法产生。' 18 14 940|Out-Null
Input $opticalPage 'PD0 原始 ADC' 'pd_0' 0 20 62 280|Out-Null
Input $opticalPage 'PD1 原始 ADC' 'pd_1' 0 345 62 280|Out-Null
Input $opticalPage 'PD 光学偏置（ADC）' 'optical_bias' 100 670 62 280|Out-Null
Input $opticalPage '测试光脉冲频率（次 / 分）' 'pulse_bpm' 72 20 140 280|Out-Null
Input $opticalPage '光脉冲幅度（ADC）' 'pulse_amplitude' 0 345 140 280|Out-Null
Label $opticalPage '参考：PD 90000、幅度 500；幅度 0 关闭波形。输入频率不保证等于算法心率。' 20 214 940 45|Out-Null
$script:leds=@()
for($i=0;$i -lt 4;$i++){$l=New-Object VelaIndicator;$l.SetBounds((20+$i*236),268,220,58);$l.Caption='LED'+$i;$l.ActiveColor=[Drawing.Color]::DarkCyan;$opticalPage.Controls.Add($l);$script:leds+=$l}
$opticalStatus=Label $opticalPage '等待 HX3695 初始化…' 20 345 940 72
Label $opticalPage 'LED 为逻辑通道活动示意，外壳位置未标定。当前 10 Pro 尚未接入录音、NFC 和震动模型。' 20 448 940 60|Out-Null
$lightPage=Page '环境光 / 屏幕'
Label $lightPage '独立 W1150 环境光传感器 · 修改自动发送；自动亮度由原厂系统控制。' 20 20 940|Out-Null
Input $lightPage '环境光参考（lux）' 'ambient_lux' 100 20 76 430|Out-Null
Label $lightPage '参考：0 暗室，100 室内，10000 强光。数值框不设限制，ADC 按硬件位宽饱和。' 20 150 940 50|Out-Null
$lightStatus=Label $lightPage '等待原厂光感驱动…' 20 220 940 65
$brightness=New-Object Windows.Forms.ProgressBar;$brightness.Maximum=255;$brightness.SetBounds(20,318,930,34);$lightPage.Controls.Add($brightness)
$screenStatus=Label $lightPage '屏幕亮度：等待读取' 20 370 930 50
Label $lightPage '在手环“设置 → 显示与亮度 → 屏幕亮度”开启自动亮度。原厂会平滑亮度变化；数值不会直接替换亮度。' 20 448 940 65|Out-Null
$gnssPage=Page 'GNSS 定位'
Label $gnssPage 'AG3335M 模拟定位源 · 经纬度和轨迹参数自动发送到芯片 UART1。' 20 14 940|Out-Null
$gnssEnabled=New-Object Windows.Forms.CheckBox;$gnssEnabled.Text='开启定位信号';$gnssEnabled.SetBounds(20,50,240,32);$gnssEnabled.Checked=$true;$gnssPage.Controls.Add($gnssEnabled)
$gnssFix=New-Object Windows.Forms.CheckBox;$gnssFix.Text='有效定位';$gnssFix.SetBounds(330,50,240,32);$gnssFix.Checked=$true;$gnssPage.Controls.Add($gnssFix)
$gnssEnabled.Add_Click({Send-Control @{operation='gnss';gnss_enabled=[bool]$gnssEnabled.Checked}})
$gnssFix.Add_Click({Send-Control @{operation='gnss';gnss_fix=[bool]$gnssFix.Checked}})
$gnssFields=@(@('纬度（度）','gnss_latitude',31.2304),@('经度（度）','gnss_longitude',121.4737),@('海拔（m）','gnss_altitude',12),@('速度（m/s）','gnss_speed',0),@('航向（度）','gnss_heading',0),@('卫星数','gnss_satellites',12),@('HDOP','gnss_hdop',0.8))
for($i=0;$i -lt $gnssFields.Count;$i++){$f=$gnssFields[$i];Input $gnssPage $f[0] $f[1] $f[2] (20+($i%3)*320) (106+[math]::Floor($i/3)*90) 285|Out-Null}
$gnssStatus=Label $gnssPage '等待 GNSS 驱动…' 20 398 940 70
Label $gnssPage '原厂在户外运动等功能中开启 GNSS；待机关闭 UART 时保留输入，下一次定位使用新数值。' 20 478 940 50|Out-Null
$uartPage=Page 'BES2700iMP RX / TX'
Label $uartPage '芯片视角：RX 为电脑发送给 BES；TX 为 BES 发出。UART1=0x4000C000，UART2=0x4000D000。' 18 18 950 45|Out-Null
$uartSelect=New-Object Windows.Forms.ComboBox;$uartSelect.DropDownStyle='DropDownList';$uartSelect.SetBounds(20,80,160,30);$uartSelect.Items.AddRange(@('UART1','UART2'));$uartSelect.SelectedIndex=0;$uartPage.Controls.Add($uartSelect)
$uartFormat=New-Object Windows.Forms.ComboBox;$uartFormat.DropDownStyle='DropDownList';$uartFormat.SetBounds(200,80,180,30);$uartFormat.Items.AddRange(@('HEX','UTF-8 文本'));$uartFormat.SelectedIndex=0;$uartPage.Controls.Add($uartFormat)
$uartInput=New-Object Windows.Forms.TextBox;$uartInput.SetBounds(20,132,760,30);$uartPage.Controls.Add($uartInput)
Button $uartPage '发送到 RX' 800 130 150 {
    $hex=$uartInput.Text
    if($uartFormat.SelectedIndex -eq 1){$hex=[BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($hex)).Replace('-',' ')}
    Send-Control @{operation='uart';uart=($uartSelect.SelectedIndex+1);hex=$hex}
}|Out-Null
$uartStatus=Label $uartPage '等待串口…' 20 185 940 36
$uartLog=New-Object Windows.Forms.TextBox;$uartLog.Multiline=$true;$uartLog.ReadOnly=$true;$uartLog.ScrollBars='Both';$uartLog.WordWrap=$false
$uartLog.Font=New-Object Drawing.Font('Consolas',9);$uartLog.SetBounds(20,230,940,270);$uartPage.Controls.Add($uartLog)
$bandPage=Page '3D 姿态'
$band=New-Object VelaBand3D;$band.SetBounds(10,8,620,510);$band.Name='band_3d';$bandPage.Controls.Add($band)
$poseSync=New-Object Windows.Forms.CheckBox;$poseSync.Text='拖动同步加速度 / 角速度 / 磁场';$poseSync.Checked=$true;$poseSync.SetBounds(646,25,325,48);$bandPage.Controls.Add($poseSync)
$band.Add_PoseChanged({
    if(-not $poseSync.Checked){return}
    foreach($kind in @('accel','gyro','mag')){foreach($axis in @('X','Y','Z')){
        $prop=$kind.Substring(0,1).ToUpper()+$kind.Substring(1)+$axis
        $script:boxes[$kind+'_'+$axis.ToLower()].Text=$band.$prop.ToString('G9',[Globalization.CultureInfo]::InvariantCulture)
    }}
})
Button $bandPage '正面' 650 100 140 {$band.Yaw=0;$band.Pitch=0;$band.Roll=0;$band.Invalidate()}|Out-Null
Button $bandPage '背面' 810 100 140 {$band.Yaw=180;$band.Pitch=0;$band.Roll=0;$band.Invalidate()}|Out-Null
Button $bandPage '双击唤醒' 650 155 300 {Send-Control @{operation='gesture';gesture='wake'}}|Out-Null
Label $bandPage '10 Pro 外观示意，传感器位置未标定。`r`n`r`n拖动改变方向，松开角速度归零。`r`n`r`n屏幕显示来自原厂实际渲染。' 650 235 300 220|ForEach-Object {$_.Text=$_.Text.Replace('`r`n',"`r`n")}
. (Join-Path $PSScriptRoot 'SensorToolsWindow.ps1')
if($Tools){$tabs.SelectedTab=$toolsPage}
$notice=Label $form '修改自动发送并保存；充电开关即时发送。' 20 707 990 28;$notice.Anchor='Bottom,Left,Right';$notice.ForeColor=[Drawing.Color]::FromArgb(30,85,145)
$tabs.Add_SelectedIndexChanged({
    $notice.Text=if($tabs.SelectedTab -eq $toolsPage){'身份工具：选择操作后点击“调用工具”；修改或撤销在下次启动生效。'}else{'修改自动发送并保存；充电开关即时发送。'}
})
function Refresh-State{
    $latest=$null;$now=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()/1000.0
    foreach($parent in @('artifacts\sessions','artifacts\verification')){
        $folder=Join-Path $root $parent
        if(-not(Test-Path -LiteralPath $folder)){continue}
        foreach($dir in (Get-ChildItem -LiteralPath $folder -Directory|Sort-Object LastWriteTime -Descending|Select-Object -First 30)){
            try{
                $s=(Read-Shared (Join-Path $dir.FullName 'sensor-status.json'))|ConvertFrom-Json
                $now=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()/1000.0
                if($s.connected -and $now-$s.updated_at -ge 0 -and $now-$s.updated_at -lt 5 -and(-not $SessionId -or $s.session_id -eq $SessionId)){
                    if(-not $latest -or $s.updated_at -gt $latest.updated_at){$latest=$s}
                }
            }catch{}
        }
    }
    $script:active=$latest
    if(-not $latest){
        $connection.Text='模拟器未运行 · 数值可保存，下次启动生效'
        $profile=Join-Path $root 'userdata\sensor-profile.json'
        if($script:loaded -ne 'offline' -and (Test-Path -LiteralPath $profile)){
            try{
                $saved=(Read-Shared $profile)|ConvertFrom-Json
                $script:hydrating=$true
                foreach($item in $saved.hardware.PSObject.Properties){if($script:boxes.ContainsKey($item.Name)){$script:boxes[$item.Name].Text=([double]$item.Value).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}}
                foreach($item in $saved.gnss.PSObject.Properties){if($script:boxes.ContainsKey($item.Name)){$script:boxes[$item.Name].Text=([double]$item.Value).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}}
                if($saved.gnss){$gnssEnabled.Checked=[bool]$saved.gnss.gnss_enabled;$gnssFix.Checked=[bool]$saved.gnss.gnss_fix}
                $raw=[double]$saved.channels.'2';$script:boxes.battery_mv.Text=([math]::Round((800+($raw-15968)*250/4848)*7,2)).ToString([Globalization.CultureInfo]::InvariantCulture)
                $raw=[double]$saved.channels.'5';$script:boxes.ntc_mv.Text=([math]::Round(800+($raw-15968)*250/4848,2)).ToString([Globalization.CultureInfo]::InvariantCulture)
                $charging.Checked=[bool]$saved.charging
                $script:loaded='offline'
            }finally{$script:hydrating=$false}
        }
        return
    }
    $s=$latest.state;$connection.Text='已连接 10 Pro · PID '+$latest.qemu_pid+' · '+$latest.session_id.Split('/')[-1]
    if([DateTime]::UtcNow -ge $script:rotationPendingUntil){
        try{$script:displayRotation=[int]$s.display_rotation}catch{$script:displayRotation=0}
    }
    $rotationStatus.Text=('屏幕：{0}°' -f $script:displayRotation)
    if($script:loaded -ne $latest.session_id){
        $script:hydrating=$true
        try{
            $script:boxes.battery_mv.Text=$s.battery_mv.ToString([Globalization.CultureInfo]::InvariantCulture)
            $script:boxes.ntc_mv.Text=$s.channels[5].input_mv.ToString([Globalization.CultureInfo]::InvariantCulture)
            foreach($kind in @('accel','gyro','mag')){for($i=0;$i -lt 3;$i++){$script:boxes[$kind+'_'+@('x','y','z')[$i]].Text=$s.motion.$kind[$i].ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}}
            for($i=0;$i -lt 2;$i++){$script:boxes['pd_'+$i].Text=[string]$s.optical.pd[$i]}
            foreach($k in @('pulse_bpm','pulse_amplitude')){$script:boxes[$k].Text=[string]$s.optical.$k}
            $script:boxes.optical_bias.Text=[string]$s.optical.optical_bias
            $script:boxes.ambient_lux.Text=[string]$s.als.lux
            foreach($f in $gnssFields){$script:boxes[$f[1]].Text=([double]$s.gnss.($f[1])).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}
            $gnssEnabled.Checked=[bool]$s.gnss.gnss_enabled;$gnssFix.Checked=[bool]$s.gnss.gnss_fix
        }finally{$script:hydrating=$false}
        $script:loaded=$latest.session_id
    }
    foreach($kind in @('accel','gyro','mag')){
        $pad=$script:pads[$kind]
        foreach($axis in @('x','y','z')){try{$pad.($axis.ToUpper())=Number $script:boxes[$kind+'_'+$axis]}catch{}};$pad.Invalidate()
    }
    if(-not $script:pending.Count){$charging.Checked=[bool]$s.charger.connected}
    $chargeStatus.Text=('充电器 {0} · SC7061 读取 {1} 次' -f $(if($s.charger.connected){'接入'}else{'断开'}),$s.charger.reads)
    $adcGrid.BeginUpdate();$adcGrid.Items.Clear()
    foreach($ch in $s.channels){
        $role=if($ch.channel -eq 2){'VBAT / 7'}elseif($ch.channel -eq 4){'板型识别（只读）'}else{'NTC 探头'}
        $row=New-Object Windows.Forms.ListViewItem([string]$ch.channel)
        foreach($v in @($role,[string]$ch.input_mv,$(if($ch.samples){[string]$ch.sampled_mv}else{'—'}),$(if($ch.guest_reads){[string]$ch.last_read_mv}else{'—'}),[string]$ch.guest_reads)){$row.SubItems.Add($v)|Out-Null};$adcGrid.Items.Add($row)|Out-Null
    };$adcGrid.EndUpdate()
    $motionStatus.Text=('ICM42607P：采样 {0}，FIFO 读取 {1} 字节；AK09919C：固件读取 {2} 字节' -f $s.imu.samples,$s.imu.fifo_reads,$s.compass.reads)
    $opticalStatus.Text=('HX3695：采样 {0}，固件读取 {1} 字节，FIFO 已读 {2} 字节，队列 {3} 字节' -f $s.optical.samples,$s.optical.reads,$s.optical.fifo_reads,$s.optical.fifo_bytes)
    $lightStatus.Text=('W1150 · I2C{0} / 0x48 · 输入 {1} lux`r`n原厂读取 {2} 字节 · 采样 {3} 次' -f $s.als.bus,$s.als.lux,$s.als.reads,$s.als.samples).Replace('`r`n',"`r`n")
    $brightness.Value=[math]::Max(0,[math]::Min(255,[int]$s.panel.brightness))
    $screenStatus.Text=('屏幕 {0} · 原厂亮度 {1}/255（{2:P0}） · 绘制 {3} 帧' -f $(if($s.panel.power){'开启'}else{'关闭'}),$s.panel.brightness,([double]$s.panel.brightness/255),$s.panel.frames)
    $gnssStatus.Text=('UART1 · 原厂 DMA 接收 {0} 字节 · GNSS 命令 {1} 条 · {4}`r`n模拟信号已发 {2} 组 · 最后命令 {3}' -f $s.uart_dma.rx_bytes[0],$s.gnss.commands,$s.gnss.emitted_epochs,$s.gnss.last_command,$(if($s.gnss.receiving){'固件接收中'}else{'固件待机'})).Replace('`r`n',"`r`n")
    for($i=0;$i -lt 4;$i++){
        $on=($s.optical.leds -and [bool]$s.optical.leds[$i]);$script:leds[$i].Active=$on;$script:leds[$i].Invalidate();$band.Leds[$i]=$on
    }
    for($i=0;$i -lt 2;$i++){$band.Pd[$i]=[double]$s.optical.pd[$i]};$band.Invalidate()
    $ports=$s.uart.ports;$uartStatus.Text=($ports|ForEach-Object{'UART{0} TX {1} / RX {2} 字节' -f $_.uart,$_.tx_bytes,$_.rx_bytes})-join'    '
    $text=($ports[$uartSelect.SelectedIndex].trace|Select-Object -Last 12|ForEach-Object{
        $value=if($uartFormat.SelectedIndex -eq 0){$_.hex}else{$_.text}
        '{0}  {1}' -f $_.direction,$value
    })-join"`r`n"
    if($uartLog.Text -ne $text){$uartLog.Text=$text;$uartLog.SelectionStart=$uartLog.TextLength;$uartLog.ScrollToCaret()}
}
function Rotate-Display([int]$Delta){
    if(-not $script:active){$notice.Text='先启动模拟器，旋转会即时作用于当前窗口。';return}
    $script:displayRotation=(($script:displayRotation+$Delta)%360+360)%360
    $script:rotationPendingUntil=[DateTime]::UtcNow.AddSeconds(2)
    $rotationStatus.Text=('屏幕：{0}°' -f $script:displayRotation)
    Send-Control @{operation='rotation';degrees=$script:displayRotation}
}
$timer=New-Object Windows.Forms.Timer;$timer.Interval=60
$script:lastRefresh=[DateTime]::MinValue;$script:previewRenew=[DateTime]::MinValue;$script:previewStamp=0;$script:closing=$false
$start=[DateTime]::UtcNow;$script:verified=$false;$script:exercised=$false
$timer.Add_Tick({
    try{
        Complete-DeviceTool
        if(Test-Path -LiteralPath $toolsSignal){
            [IO.File]::Delete($toolsSignal)
            $tabs.SelectedTab=$toolsPage
            if($form.WindowState -eq 'Minimized'){$form.WindowState='Normal'}
            $form.Activate()
        }
        if($script:values.Count -and[DateTime]::UtcNow -ge $script:due){Flush-Inputs}
        foreach($p in @($script:pending.ToArray())){
            if(Test-Path -LiteralPath $p.reply){
                $r=(Read-Shared $p.reply)|ConvertFrom-Json
                $notice.Text=if(-not$r.ok){$r.error}elseif($r.stored_only){'已保存，下次启动生效。'}else{'已发送；请以固件读取计数和手环实际结果为准。'}
                Remove-Item -LiteralPath $p.reply -Force
                if($p.process){$p.process.Dispose();Remove-Item -LiteralPath $p.request -Force -ErrorAction SilentlyContinue}
                $script:pending.Remove($p)
            }elseif(([DateTime]::UtcNow-$p.at).TotalSeconds -gt 10){$script:pending.Remove($p);$notice.Text='请求超时，请检查模拟器状态。'}
        }
        if(([DateTime]::UtcNow-$script:lastRefresh).TotalMilliseconds -ge 300){Refresh-State;$script:lastRefresh=[DateTime]::UtcNow}
        if($tabs.SelectedTab -eq $bandPage -and $script:active){
            if(([DateTime]::UtcNow-$script:previewRenew).TotalSeconds -gt 2){Send-Control @{operation='preview';enabled=$true};$script:previewRenew=[DateTime]::UtcNow}
            $preview=Join-Path (Join-Path $root $script:active.session_id) 'sensor-preview.png'
            if(Test-Path -LiteralPath $preview){$stamp=(Get-Item -LiteralPath $preview).LastWriteTimeUtc.Ticks;if($stamp -ne $script:previewStamp){$band.SetFrame($preview);$script:previewStamp=$stamp}}
        }
        if($VerifyExercise -and -not $script:exercised -and $script:active -and ([DateTime]::UtcNow-$start).TotalSeconds -gt 2){
            $script:boxes.battery_mv.Text='4150';$script:boxes.ntc_mv.Text='510'
            $script:boxes.accel_x.Text='3.25';$script:boxes.gyro_y.Text='12.5';$script:boxes.mag_x.Text='-15'
            $script:boxes.pd_0.Text='91000';$script:boxes.pd_1.Text='93000'
            $charging.Checked=$true
            $click=[Windows.Forms.Control].GetMethod('OnClick',[Reflection.BindingFlags]'Instance,NonPublic')
            $click.Invoke($charging,@([EventArgs]::Empty))|Out-Null
            $tabs.SelectedTab=$bandPage
            $script:exercised=$true
        }
        if($VerifyOutput -and -not $script:verified -and ([DateTime]::UtcNow-$start).TotalSeconds -gt $(if($VerifyExercise){5}else{2})){
            [IO.Directory]::CreateDirectory($VerifyOutput)|Out-Null
            $index=0;foreach($page in $tabs.TabPages){$tabs.SelectedTab=$page;$form.Refresh();$bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height);$form.DrawToBitmap($bitmap,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height)));$bitmap.Save((Join-Path $VerifyOutput ('native-'+$index+'.png')));$bitmap.Dispose();$index++}
            $tabs.SelectedIndex=0;$script:verified=$true
            [IO.File]::WriteAllText((Join-Path $VerifyOutput 'native-window.json'),(@{ok=$true;pages=$index;connected=[bool]$script:active;exercised=$script:exercised;notice=$notice.Text;model='p67cn'}|ConvertTo-Json),$utf8)
        }
        if($AutoCloseSeconds -gt 0 -and ([DateTime]::UtcNow-$start).TotalSeconds -ge $AutoCloseSeconds){$form.Close()}
        if($script:closing -and -not $script:pending.Count -and -not $script:deviceToolJob){$form.Close()}
    }catch{$notice.Text=$_.Exception.Message;if($VerifyOutput){[IO.File]::WriteAllText((Join-Path $VerifyOutput 'native-error.txt'),($_|Out-String),$utf8)}}
})
$form.Add_Shown({Refresh-State;if($Tools -and -not $script:deviceToolLoaded){Start-DeviceTool 'identity.read'};$timer.Start()})
$form.Add_FormClosing({Flush-Inputs;if($script:pending.Count -or $script:deviceToolJob){$_.Cancel=$true;$script:closing=$true}})
try{[Windows.Forms.Application]::Run($form)}finally{$timer.Stop();$timer.Dispose();$form.Dispose();$mutex.ReleaseMutex();$mutex.Dispose()}
