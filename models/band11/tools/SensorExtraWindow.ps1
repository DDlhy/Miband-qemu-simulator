# Dot-sourced by SensorWindow.ps1, sharing its native window and request queue.
Add-Type -Path (Join-Path $PSScriptRoot '..\..\..\shared\ui\SensorWidgets.cs') -ReferencedAssemblies System.Windows.Forms,System.Drawing
$form.ClientSize=New-Object Drawing.Size(1050, 820)
$form.MinimumSize=New-Object Drawing.Size(1066, 859)
$tabs=New-Object Windows.Forms.TabControl
$tabs.Location=New-Object Drawing.Point(20,108)
$tabs.Size=New-Object Drawing.Size(1010,630)
$tabs.Anchor='Top,Bottom,Left,Right'
$bandPage=New-Object Windows.Forms.TabPage
$bandPage.Text='3D 手环';$bandPage.BackColor=[Drawing.Color]::White
$tabs.TabPages.Add($bandPage)
$basic=New-Object Windows.Forms.TabPage
$basic.Text='电池 / 温度 / 触摸'
foreach($c in @($form.Controls)) {
    if ($c -ne $title -and $c -ne $connection -and $c -ne $notice -and $c -ne $hardwareStatus) {
        $form.Controls.Remove($c);$basic.Controls.Add($c)
        $c.Top-=84
    }
}
$tabs.TabPages.Add($basic)
$form.Controls.Add($tabs)
$hardwareStatus.Location=New-Object Drawing.Point(22,750)
$notice.Location=New-Object Drawing.Point(22,786)
$hardwareStatus.Width=1000;$notice.Width=1000
$hardwareStatus.Anchor='Bottom,Left';$notice.Anchor='Bottom,Left'
$script:extraBoxes=@{}
$script:vectorPads=@{}
function ExtraPage($name) {
    $p=New-Object Windows.Forms.TabPage;$p.Text=$name;$p.BackColor=[Drawing.Color]::White
    $tabs.TabPages.Add($p);return $p
}
function ExtraInput($parent,$caption,$key,$value,$x,$y,$w=150) {
    Label $parent $caption $x $y $w | Out-Null
    $b=Input $parent ([string]$value) $x ($y+28) $w
    $b.Name='sensor_'+$key
    $b.Tag=$key
    $b.Add_TextChanged({ Queue-Input $this.Tag $this })
    $b.Add_Leave({ Flush-Inputs })
    $script:extraBoxes[$key]=$b
    return $b
}
$lightPage=ExtraPage '环境光 / 屏幕'
Label $lightPage '独立 W1150 环境光 · 自动亮度由原厂控制；修改自动发送并保存。' 20 20 940|Out-Null
ExtraInput $lightPage '环境光参考（lux）' 'ambient_lux' 100 20 80 430|Out-Null
ExtraInput $lightPage 'PD 光学偏置（ADC）' 'optical_bias' 100 510 80 430|Out-Null
Label $lightPage '参考：0 暗室，100 室内，10000 强光。输入不限范围，光感 ADC 按硬件位宽饱和。' 20 164 940 45|Out-Null
$lightStatus=Label $lightPage '等待原厂光感采样…' 20 230 940 70
$brightness=New-Object Windows.Forms.ProgressBar;$brightness.Maximum=255;$brightness.SetBounds(20,330,930,36);$lightPage.Controls.Add($brightness)
$screenStatus=Label $lightPage '等待屏幕亮度…' 20 390 940 50
Label $lightPage '在手环“设置 → 显示与亮度”开启自动亮度。原厂按采样周期平滑调节亮度；PD 偏置仅影响心率光学输入。' 20 490 940 65|Out-Null
$motion=ExtraPage '运动 / 指南针'
Label $motion '拖动图形改变方向；三轴数值可任意输入，修改自动发送。' 18 14 950 | Out-Null
$configs=@(@('accel','加速度（m/s²）',9.80665,0,0,9.80665),@('gyro','陀螺仪（°/s）',90,0,0,0),@('mag','指南针磁场（µT）',50,30,0,40))
for($i=0;$i -lt 3;$i++) {
    $cfg=$configs[$i];$key=$cfg[0];$x=18+$i*322
    $l=Label $motion $cfg[1] $x 54 300;$l.Font=New-Object Drawing.Font($form.Font,[Drawing.FontStyle]::Bold)
    $pad=New-Object VelaVectorPad;$pad.Location=New-Object Drawing.Point($x,90)
    $pad.Size=New-Object Drawing.Size(294,255);$pad.Reference=$cfg[2];$pad.X=$cfg[3];$pad.Y=$cfg[4];$pad.Z=$cfg[5]
    $pad.Compass=($key -eq 'mag');$pad.Name='vector_'+$key;$pad.Tag=$key
    $motion.Controls.Add($pad);$script:vectorPads[$key]=$pad
    $pad.Add_VectorChanged({
        $k=$this.Tag
        foreach($axis in @('x','y','z')) {
            $script:extraBoxes[$k+'_'+$axis].Text=$this.($axis.ToUpper()).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)
        }
    })
    for($j=0;$j -lt 3;$j++) {
        ExtraInput $motion (@('X','Y','Z')[$j]) ($key+'_'+@('x','y','z')[$j]) $cfg[3+$j] $x (358+$j*70) 284 | Out-Null
    }
}
$motionStatus=Label $motion '等待固件采样…' 18 572 950 40
$optical=ExtraPage '心率 / 血氧 / 光感 / 震动'
$optical.AutoScroll=$true
Label $optical '外观示意为 4 颗灯珠 / 2 个 PD；下列为芯片逻辑通道，物理映射可调整，尚待实物核对。' 18 14 960 | Out-Null
for($i=0;$i -lt 4;$i++) { ExtraInput $optical ('逻辑 PD'+$i+' 原始值') ('pd_'+$i) 0 (18+$i*240) 54 220 | Out-Null }
Label $optical '参考：0 无光；10000 为测试输入。传感器按有符号 20 位输出，数值框不设上下限。' 18 118 950 | Out-Null
$script:ledIndicators=@();$script:ledMapping=@();$script:pdMapping=@()
$script:logicalLeds=[bool[]]@($false,$false,$false,$false,$false,$false,$false,$false)
for($i=0;$i -lt 2;$i++) {
    Label $optical ('外观 PD'+($i+1)+' 映射') (18+$i*240) 148 220 | Out-Null
    $m=New-Object Windows.Forms.ComboBox;$m.DropDownStyle='DropDownList'
    $m.Location=New-Object Drawing.Point((18+$i*240),174);$m.Size=New-Object Drawing.Size(220,28)
    $m.Items.AddRange(@('逻辑 PD0','逻辑 PD1','逻辑 PD2','逻辑 PD3'));$m.SelectedIndex=$i
    $optical.Controls.Add($m);$script:pdMapping+=$m
}
Label $optical '灯珠颜色只作活动标记，不代表实际发光波长。' 510 162 450 42 | Out-Null
for($i=0;$i -lt 4;$i++) {
    $x=18+$i*240
    Label $optical ('灯珠'+($i+1)+' → 驱动输出') $x 207 220 | Out-Null
    $m=New-Object Windows.Forms.ComboBox;$m.DropDownStyle='DropDownList'
    $m.Location=New-Object Drawing.Point($x,234);$m.Size=New-Object Drawing.Size(220,28)
    $m.Items.AddRange(@('LED0','LED1','LED2','LED3','LED4','LED5','LED6','LED7'));$m.SelectedIndex=@(0,2,4,5)[$i]
    $optical.Controls.Add($m);$script:ledMapping+=$m
    $led=New-Object VelaIndicator;$led.Caption='灯珠'+($i+1);$led.Name='led_'+$i
    $led.ActiveColor=[Drawing.Color]::DarkCyan
    $led.Location=New-Object Drawing.Point($x,264)
    $led.Size=New-Object Drawing.Size(230,50);$optical.Controls.Add($led);$script:ledIndicators+=$led
}
ExtraInput $optical '测试光脉冲频率（次/分）' 'pulse_bpm' 72 18 594 220 | Out-Null
ExtraInput $optical '脉搏光信号幅度（ADC）' 'pulse_amplitude' 0 258 594 220 | Out-Null
Button $optical '参考光信号' 510 620 150 {
    foreach($key in @('pd_0','pd_1','pd_2','pd_3')) {$script:extraBoxes[$key].Text='90000'}
    $script:extraBoxes['pulse_bpm'].Text='120';$script:extraBoxes['pulse_amplitude'].Text='500';Flush-Inputs
}|Out-Null
Button $optical '关闭脉搏波形' 680 620 160 {$script:extraBoxes['pulse_amplitude'].Text='0';Flush-Inputs}|Out-Null
Label $optical '测试波形送入 ADC；频率尚未标定，不等于手环心率。结果由原厂算法决定，幅度 0 关闭波形。' 18 668 960 45 | Out-Null
$opticalStatus=Label $optical '等待光学驱动…' 18 315 950 50
$slotGrid=New-Object Windows.Forms.ListView
$slotGrid.Location=New-Object Drawing.Point(18,370);$slotGrid.Size=New-Object Drawing.Size(650,205)
$slotGrid.View='Details';$slotGrid.FullRowSelect=$true;$slotGrid.GridLines=$true
foreach($c in @(@('时隙',45),@('启用',45),@('LED 掩码',85),@('电流码',70),@('ADC 偏置',75),@('PD 掩码',80),@('固件读到 ADC',130))) {$slotGrid.Columns.Add($c[0],$c[1])|Out-Null}
$optical.Controls.Add($slotGrid)
$vibration=New-Object VelaIndicator;$vibration.Caption='震动输出';$vibration.Name='vibration_output'
$vibration.ActiveColor=[Drawing.Color]::DarkOrange;$vibration.Location=New-Object Drawing.Point(670,380)
$vibration.Size=New-Object Drawing.Size(290,64);$optical.Controls.Add($vibration)
$vibrationStatus=Label $optical '等待震动驱动…' 680 455 280 110
$uartPage=ExtraPage 'BES2700iMP RX / TX'
Label $uartPage '以芯片为视角：RX = 发给 BES；TX = BES 发出。三路均为 UART 字节流。' 18 14 950 | Out-Null
$uartSelect=New-Object Windows.Forms.ComboBox;$uartSelect.DropDownStyle='DropDownList'
$uartSelect.Location=New-Object Drawing.Point(18,56);$uartSelect.Size=New-Object Drawing.Size(130,28)
$uartSelect.Items.AddRange(@('UART0','UART1','UART2'));$uartSelect.SelectedIndex=0;$uartPage.Controls.Add($uartSelect)
$uartFormat=New-Object Windows.Forms.ComboBox;$uartFormat.DropDownStyle='DropDownList'
$uartFormat.Location=New-Object Drawing.Point(164,56);$uartFormat.Size=New-Object Drawing.Size(130,28)
$uartFormat.Items.AddRange(@('HEX','UTF-8 文本'));$uartFormat.SelectedIndex=0;$uartPage.Controls.Add($uartFormat)
$uartInput=Input $uartPage '' 18 106 780;$uartInput.Name='uart_send'
Button $uartPage '发送到 RX' 820 104 140 {
    try {
        $hex=$uartInput.Text
        if($uartFormat.SelectedIndex -eq 1) {$hex=[BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes($hex)).Replace('-',' ')}
        Send-Control @{operation='uart';uart=$uartSelect.SelectedIndex;hex=$hex}
    } catch {$notice.Text=$_.Exception.Message}
}|Out-Null
$uartStatus=Label $uartPage '等待 UART 路由…' 18 154 950 65
$uartLog=New-Object Windows.Forms.TextBox;$uartLog.Multiline=$true;$uartLog.ReadOnly=$true
$uartLog.ScrollBars='Both';$uartLog.WordWrap=$false;$uartLog.Location=New-Object Drawing.Point(18,232)
$uartLog.Size=New-Object Drawing.Size(942,340);$uartLog.Font=New-Object Drawing.Font('Consolas',10)
$uartLog.Name='uart_trace';$uartPage.Controls.Add($uartLog)
$micPage=ExtraPage '电脑录音输入'
Label $micPage '原厂录音机启动采集时，将选中的电脑录音设备送入手环 CODEC。' 18 18 950 | Out-Null
$micEnabled=New-Object Windows.Forms.CheckBox;$micEnabled.Text='路由电脑录音输入'
$micEnabled.Location=New-Object Drawing.Point(18,65);$micEnabled.Size=New-Object Drawing.Size(300,30)
$micEnabled.Checked=$true;$micPage.Controls.Add($micEnabled)
$micDevices=New-Object Windows.Forms.ComboBox;$micDevices.DropDownStyle='DropDownList'
$micDevices.Location=New-Object Drawing.Point(18,120);$micDevices.Size=New-Object Drawing.Size(650,30)
$micPage.Controls.Add($micDevices)
$script:micDeviceList=@();$script:micHydrating=$false
function Send-Microphone {
    if($script:micHydrating) {return}
    $device=-1
    if($micDevices.SelectedIndex -ge 0 -and $script:micDeviceList.Count -gt $micDevices.SelectedIndex) {$device=[int]$script:micDeviceList[$micDevices.SelectedIndex].id}
    Send-Control @{operation='microphone';enabled=[bool]$micEnabled.Checked;device=$device}
}
$micEnabled.Add_Click({Send-Microphone})
$micDevices.Add_SelectedIndexChanged({Send-Microphone})
$micMeter=New-Object Windows.Forms.ProgressBar;$micMeter.Location=New-Object Drawing.Point(18,185)
$micMeter.Size=New-Object Drawing.Size(650,28);$micMeter.Maximum=1000;$micPage.Controls.Add($micMeter)
$micStatus=Label $micPage '等待模拟器连接…' 18 240 950 220
Label $micPage '参考格式：16 kHz、16 bit、单声道 PCM。仅固件开启录音 ADC 时采集，停止后释放电脑输入设备。' 18 500 950 70 | Out-Null

. (Join-Path $PSScriptRoot 'Sensor3DWindow.ps1')
. (Join-Path $PSScriptRoot 'SensorNfcWindow.ps1')

function Hydrate-Extra($hardware) {
    if(-not $hardware) {return}
    foreach($key in $script:extraBoxes.Keys) {
        $property=$hardware.PSObject.Properties[$key]
        if($null -ne $property) {$script:extraBoxes[$key].Text=([double]$property.Value).ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}
    }
}
function Refresh-Extra($s) {
    foreach($k in @('accel','gyro','mag')) {
        $p=$script:vectorPads[$k]
        try {$p.X=Number $script:extraBoxes[$k+'_x'];$p.Y=Number $script:extraBoxes[$k+'_y'];$p.Z=Number $script:extraBoxes[$k+'_z'];$p.Invalidate()}catch{}
    }
    if(-not $s) {
        foreach($led in $script:ledIndicators) {$led.Active=$false;$led.Caption='未连接';$led.Invalidate()}
        $vibration.Active=$false;$vibration.Caption='未连接';$vibration.Invalidate();Refresh-Band3D $null;Refresh-Nfc $null;return
    }
    $lightStatus.Text=('W1150 · I2C{0} / 0x48 · 输入 {1} lux`r`n原厂读取 {2} 字节 · 采样 {3} 次' -f $s.als.bus,$s.als.lux,$s.als.reads,$s.als.samples).Replace('`r`n',"`r`n")
    $brightness.Value=[math]::Max(0,[math]::Min(255,[int]$s.panel.brightness))
    $screenStatus.Text=('屏幕 {0} · 原厂亮度 {1}/255（{2:P0}） · 绘制 {3} 帧' -f $(if($s.panel.power){'开启'}else{'关闭'}),$s.panel.brightness,([double]$s.panel.brightness/255),$s.panel.frames)
    $ar=if($s.imu -and ($s.imu.registers[0x10] -band 0xf0)){'采样中'}else{'固件待机'}
    $gr=if($s.imu -and ($s.imu.registers[0x11] -band 0xf0)){'采样中'}else{'固件待机'}
    $motionStatus.Text=('磁力计实际读取 {0} 次   IMU FIFO 读取 {1} 字节`r`n加速度：{2}   陀螺仪：{3}' -f $s.motion.mag_reads,$s.imu.fifo_reads,$ar,$gr).Replace('`r`n',"`r`n")
    $r=$s.optical.registers
    if($r -and $r.Count -ge 256) {
        $mask=[int]$r[0x34] -bor ([int]$r[0x35] -shl 8)
        $run=($s.optical.writes -gt 0 -and ([int]$r[3] -band 1) -eq 0)
        $ledState=@($false,$false,$false,$false,$false,$false,$false,$false)
        $slotGrid.BeginUpdate();$slotGrid.Items.Clear()
        for($i=0;$i -lt 16;$i++) {
            $enabled=$run -and ($mask -band (1 -shl $i))
            $pdReg=if($i -lt 8){0x34-[Math]::Floor($i/2)}else{0x39-[Math]::Floor(($i-8)/2)}
            $pd=([int]$r[128+$pdReg] -shr (4*($i%2))) -band 15
            # Two ADC slots share each transmitter. 0x3a+slot is the
            # ADC offset DAC; LED current/mask live at 0x10/0x11+2*TX.
            $tx=$i%8
            $current=[int]$r[128+0x10+2*$tx]
            $ledMask=[int]$r[128+0x11+2*$tx]
            $offset=[int]$r[128+0x3a+$i]
            for($output=0;$output -lt 8;$output++) {
                if($enabled -and $current -and ($ledMask -band (1 -shl $output))) {$ledState[$output]=$true}
            }
            $row=New-Object Windows.Forms.ListViewItem([string]$i)
            $last=if([int]$s.optical.last_sample_mask -band (1 -shl $i)){[string]$s.optical.last_samples[$i]}else{'—'}
            foreach($v in @($(if($enabled){'开'}else{'关'}),('0x{0:X2}' -f $ledMask),[string]$current,[string]$offset,('0x{0:X}' -f $pd),$last)) {$row.SubItems.Add($v)|Out-Null}
            $slotGrid.Items.Add($row)|Out-Null
        }
        $slotGrid.EndUpdate()
        $script:logicalLeds=[bool[]]$ledState
        for($i=0;$i -lt 4;$i++) {$led=$script:ledIndicators[$i];$led.Caption='灯珠'+($i+1);$led.Active=$ledState[$script:ledMapping[$i].SelectedIndex];$led.Invalidate()}
        $opticalStatus.Text=('光学采样 {0} 个   固件读取 {1} 字节   FIFO {2} 字节   时隙掩码 0x{3:X4}' -f $s.optical.samples,$s.optical.reads,$s.optical.fifo_bytes,$mask)
    }
    $vibration.Active=[bool]$s.vibration.active;$vibration.Caption=if($s.vibration.active){'正在震动'}else{'震动停止'};$vibration.Invalidate()
    $vibrationStatus.Text=('驱动 {0}`r`n累计启动 {1} 次`r`n强度码 {2}' -f $s.vibration.driver,$s.vibration.events,$s.vibration.gain).Replace('`r`n',"`r`n")
    if($s.uart) {
        $lines=@();foreach($p in $s.uart.ports){$lines+=('UART{0}  TX {1} 字节  RX {2} 字节  排队 {3}' -f $p.uart,$p.tx_bytes,$p.rx_bytes,$p.queued)}
        $uartStatus.Text=$lines -join "`r`n"
        $lines=@();foreach($e in $s.uart.trace){$lines+=('#{0} UART{1} {2}  {3}' -f $e.seq,$e.uart,$e.direction,$e.hex)}
        $text=$lines -join "`r`n";if($uartLog.Text -ne $text){$uartLog.Text=$text;$uartLog.SelectionStart=$uartLog.TextLength;$uartLog.ScrollToCaret()}
    }
    if($s.microphone) {
        $m=$s.microphone;$script:micHydrating=$true
        try {
            if(($script:micDeviceList|ConvertTo-Json -Compress) -ne ($m.devices|ConvertTo-Json -Compress)) {
                $script:micDeviceList=@($m.devices);$micDevices.Items.Clear()
                foreach($d in $m.devices){$micDevices.Items.Add([string]$d.name)|Out-Null}
            }
            if(-not $script:pending.Count) {
                $micEnabled.Checked=[bool]$m.enabled
                for($i=0;$i -lt $script:micDeviceList.Count;$i++) {if($script:micDeviceList[$i].id -eq $m.device){$micDevices.SelectedIndex=$i}}
            }
        } finally {$script:micHydrating=$false}
        $micMeter.Value=[Math]::Min(1000,[Math]::Max(0,[int]($m.level*1000)))
        $mode=if($m.capturing){'正在路由电脑录音'}elseif($s.audio.capture_active){'固件录音 ADC 已开启'}else{'待机：固件未开启录音'}
        $micStatus.Text=('{0}`r`n电脑已采集 {1} 个样本`r`n送入 CODEC {2}，固件已读取 {3}`r`nAUDMA 完成 {4} 块`r`n{5}' -f $mode,$m.samples,$s.audio.injected,$s.audio.consumed,$s.audio.dma_blocks,$m.error).Replace('`r`n',"`r`n")
    }
    Refresh-Band3D $s
    Refresh-Nfc $s
}
