# Native GDI+ 3D viewport, using the same supervisor request queue.
Add-Type -Path (Join-Path $PSScriptRoot 'SensorBand3D.cs') -ReferencedAssemblies System.Windows.Forms,System.Drawing
$band=New-Object VelaBand3D
$band.Name='band_3d';$band.Location=New-Object Drawing.Point(12,12)
$band.Size=New-Object Drawing.Size(650,570);$bandPage.Controls.Add($band)
$poseSync=New-Object Windows.Forms.CheckBox
$poseSync.Text='旋转同步到运动传感器';$poseSync.Checked=$true
$poseSync.Location=New-Object Drawing.Point(682,16);$poseSync.Size=New-Object Drawing.Size(300,30)
$bandPage.Controls.Add($poseSync)
$script:poseBoxes=@{};$script:poseEditing=$false
$script:lastPoseSend=[DateTime]::MinValue
$script:previewRenew=[DateTime]::MinValue;$script:previewStamp=0
function Update-BandPose {
    $script:poseEditing=$true
    try {
        foreach($key in @('Yaw','Pitch','Roll')) {if(-not $script:poseBoxes[$key].Focused){$script:poseBoxes[$key].Text=$band.$key.ToString('G9',[Globalization.CultureInfo]::InvariantCulture)}}
    } finally {$script:poseEditing=$false}
    if($poseSync.Checked) {
        foreach($kind in @('accel','gyro','mag')) {foreach($axis in @('X','Y','Z')) {
            $value=$band.($kind.Substring(0,1).ToUpper()+$kind.Substring(1)+$axis)
            $script:extraBoxes[$kind+'_'+$axis.ToLower()].Text=$value.ToString('G9',[Globalization.CultureInfo]::InvariantCulture)
        }}
        # Send while dragging, rather than debounce indefinitely until release.
        if(([DateTime]::UtcNow-$script:lastPoseSend).TotalMilliseconds -ge 80 -or
            ($band.GyroX -eq 0 -and $band.GyroY -eq 0 -and $band.GyroZ -eq 0)) {
            Flush-Inputs;$script:lastPoseSend=[DateTime]::UtcNow
        }
    }
}
$band.Add_PoseChanged({Update-BandPose})
for($i=0;$i -lt 3;$i++) {
    $key=@('Yaw','Pitch','Roll')[$i]
    Label $bandPage (@('Yaw 偏航（°）','Pitch 俯仰（°）','Roll 横滚（°）')[$i]) 684 (58+$i*66) 280 | Out-Null
    $b=Input $bandPage '0' 684 (84+$i*66) 280;$b.Tag=$key;$b.Name='pose_'+$key.ToLower()
    $script:poseBoxes[$key]=$b
    $b.Add_TextChanged({
        if($script:poseEditing){return}
        try {$band.($this.Tag)=Number $this;$band.Invalidate();Update-BandPose}catch{$notice.Text=$_.Exception.Message}
    })
}
Button $bandPage '正面' 684 270 130 {$band.Yaw=0;$band.Pitch=0;$band.Roll=0;$band.Invalidate();Update-BandPose}|Out-Null
Button $bandPage '背面' 834 270 130 {$band.Yaw=180;$band.Pitch=0;$band.Roll=0;$band.Invalidate();Update-BandPose}|Out-Null
Button $bandPage '双击唤醒屏幕' 684 314 280 {Send-Control @{operation='gesture';gesture='wake'}}|Out-Null
Button $bandPage '调整 PD 光感 / 灯珠映射' 684 358 280 {$tabs.SelectedTab=$optical}|Out-Null
$bandStatus=Label $bandPage '等待模拟器…' 684 408 290 94
Label $bandPage '旋转生成重力、参考磁场和拖动角速度；松开后角速度归零。独立输入仍可在“运动 / 指南针”中调整。' 684 500 290 83 | Out-Null
$band.Add_PdSelected({
    $index=$script:pdMapping[$band.SelectedPd].SelectedIndex
    $tabs.SelectedTab=$optical;$script:extraBoxes['pd_'+$index].Focus()|Out-Null
})
function Refresh-Band3D($s) {
    if(-not $s) {$band.Vibrating=$false;$band.Leds=[bool[]]@($false,$false,$false,$false);$bandStatus.Text='模拟器未连接';$band.Invalidate();return}
    for($i=0;$i -lt 4;$i++) {$band.Leds[$i]=$script:logicalLeds[$script:ledMapping[$i].SelectedIndex]}
    for($i=0;$i -lt 2;$i++) {$band.Pd[$i]=[double]$s.optical.pd[$script:pdMapping[$i].SelectedIndex]}
    $band.Vibrating=[bool]$s.vibration.active
    $screen=if($s.panel.power -and $s.panel.brightness){'亮屏'}else{'熄屏'}
    $motor=if($band.Vibrating){'正在震动'}else{'震动停止'}
    $bandStatus.Text=('{0} · {1}`r`n震动启动 {2} 次`r`n灯珠通道映射尚待实物核对' -f $screen,$motor,$s.vibration.events).Replace('`r`n',"`r`n")
    if($tabs.SelectedTab -eq $bandPage -and [DateTime]::UtcNow -ge $script:previewRenew) {
        Send-Control @{operation='preview';enabled=$true};$script:previewRenew=[DateTime]::UtcNow.AddSeconds(3)
    }
    if($s.preview.available -and $s.preview.updated_at -ne $script:previewStamp) {
        $path=[IO.Path]::GetFullPath((Join-Path $projectRoot $s.preview.path))
        if($path.StartsWith($projectRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
            $band.SetFrame($path);$script:previewStamp=$s.preview.updated_at
        }
    }
    $band.Invalidate()
}
$tabs.SelectedTab=$bandPage
