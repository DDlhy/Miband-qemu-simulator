param([string]$VerifyOutput, [int]$AutoCloseSeconds=0)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$root=Split-Path $PSScriptRoot -Parent
$python=Join-Path $root 'runtime\python\python.exe'
$pythonw=Join-Path $root 'runtime\python\pythonw.exe'
$entry=Join-Path $PSScriptRoot 'simulator.py'
$utf8=New-Object Text.UTF8Encoding($false)
if(-not(Test-Path -LiteralPath $pythonw)){
    [Windows.Forms.MessageBox]::Show('这是待编译源码目录。请先编译并生成运行目录，步骤见 docs/BUILD.md；然后打开输出目录中的 Launch.cmd。','Mi Band QEMU Simulator')|Out-Null
    exit 1
}
$catalog=Get-Content -LiteralPath (Join-Path $root 'models\models.json') -Raw -Encoding UTF8|ConvertFrom-Json
$form=New-Object Windows.Forms.Form
$form.Text='Mi Band QEMU Simulator'
$form.ClientSize=New-Object Drawing.Size(780,510)
$form.MinimumSize=New-Object Drawing.Size(796,549)
$form.StartPosition='CenterScreen';$form.AutoScaleMode='Dpi'
$form.Font=New-Object Drawing.Font('Microsoft YaHei UI',10)
$form.BackColor=[Drawing.Color]::FromArgb(245,247,250)
function Label($text,$x,$y,$w,$h=28){
    $c=New-Object Windows.Forms.Label;$c.Text=$text;$c.SetBounds($x,$y,$w,$h);$form.Controls.Add($c);return $c
}
function Button($text,$x,$y,$w,$action){
    $c=New-Object Windows.Forms.Button;$c.Text=$text;$c.SetBounds($x,$y,$w,40);$c.Add_Click($action);$form.Controls.Add($c);return $c
}
$title=Label '小米手环模拟器' 26 22 700 42
$title.Font=New-Object Drawing.Font('Microsoft YaHei UI',19,[Drawing.FontStyle]::Bold)
Label '选择型号后启动；两种型号的存档和传感器设置各自保存。' 28 75 724|Out-Null
Label '型号' 28 123 70|Out-Null
$model=New-Object Windows.Forms.ComboBox;$model.Name='model';$model.DropDownStyle='DropDownList';$model.SetBounds(105,120,300,32)
$modelIds=@($catalog.PSObject.Properties.Name)
foreach($id in $modelIds){[void]$model.Items.Add($catalog.$id.name)}
$form.Controls.Add($model)
Label '运行模式' 430 123 90|Out-Null
$mode=New-Object Windows.Forms.ComboBox;$mode.Name='mode';$mode.DropDownStyle='DropDownList';$mode.SetBounds(525,120,222,32)
$mode.Items.AddRange(@('离线 · 屏幕和触摸','蓝牙 · 原始 HCI 直连'));$mode.SelectedIndex=0;$form.Controls.Add($mode)
$details=Label '' 28 168 720 56
Label '显示方向' 28 232 90|Out-Null
$rotation=New-Object Windows.Forms.ComboBox;$rotation.DropDownStyle='DropDownList';$rotation.SetBounds(125,229,165,32)
$rotation.Items.AddRange(@('竖屏 0°','右转 90°','倒置 180°','左转 270°'));$rotation.SelectedIndex=0;$form.Controls.Add($rotation)
$rotationNote=Label '' 312 233 435
$start=Button '启动所选型号' 28 285 205 {
    $id=Selected-Model;$runMode=if($mode.SelectedIndex -eq 0){'offline'}else{'bluetooth'}
    $angle=if($id -eq 'band10pro'){$rotation.SelectedIndex*90}else{0}
    Start-Process -FilePath $pythonw -WorkingDirectory $root -WindowStyle Hidden -ArgumentList ('-X utf8 -B "{0}" start --model {1} --mode {2} --rotation {3}' -f $entry,$id,$runMode,$angle)|Out-Null
    $notice.Text='正在启动，首次运行请等待原厂语言页出现。'
}
$stop=Button '停止所选型号' 250 285 205 {
    $path=Join-Path $root ('userdata\'+(Selected-Model)+'\stop.request')
    [IO.Directory]::CreateDirectory((Split-Path $path -Parent))|Out-Null
    [IO.File]::WriteAllText($path,'stop',$utf8)
    $notice.Text='已请求正常停止，正在等待原厂固件与存档关闭。'
}
Button '传感器控制器' 28 340 205 {Open-Panel $false}|Out-Null
Button 'DID / 序列号工具' 250 340 205 {Open-Panel $true}|Out-Null
Button '打开型号目录' 475 285 272 {
    Start-Process explorer.exe -ArgumentList ('"'+(Join-Path $root ('models\'+(Selected-Model)))+'"')|Out-Null
}|Out-Null
Button '查看启动日志' 475 340 272 {
    $path=Join-Path $root ('userdata\'+(Selected-Model)+'\startup.log')
    if(Test-Path -LiteralPath $path){Start-Process notepad.exe -ArgumentList ('"'+$path+'"')|Out-Null}
    else{$notice.Text='尚无启动日志。'}
}|Out-Null
$status=Label '' 28 401 720 52
$notice=Label '左键点击，按住拖动滑动；熄屏后快速双击唤醒。' 28 466 720 34
$notice.ForeColor=[Drawing.Color]::FromArgb(80,90,110)
function Selected-Model {return $modelIds[$model.SelectedIndex]}
function Open-Panel($tools){
    $action=if($tools){'identity'}else{'sensors'}
    Start-Process -FilePath $pythonw -WorkingDirectory $root -WindowStyle Hidden -ArgumentList ('-X utf8 -B "{0}" {1} --model {2}' -f $entry,$action,(Selected-Model))|Out-Null
}
function Read-ModelState($id){
    $path=Join-Path $root ('userdata\'+$id+'\state.json')
    if(-not(Test-Path -LiteralPath $path)){return '未启动'}
    try{
        $s=Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json
        if($s.phase -eq 'failed'){return '启动或运行失败 · 请查看启动日志'}
        if($s.phase -eq 'stopped'){return '已停止'}
        if(-not(Get-Process -Id $s.supervisor_pid -ErrorAction SilentlyContinue)){return '已停止'}
        $phase=switch($s.phase){'starting'{'正在启动'} 'running'{'运行中'} 'stopping'{'正在停止'} default{$s.phase}}
        $m=if($s.mode -eq 'offline'){'离线'}else{'蓝牙'}
        return ($phase+' · '+$m)
    }catch{return '正在读取状态'}
}
function Refresh-View {
    $id=Selected-Model;$cfg=$catalog.$id
    $details.Text=('{0} · PID {1} · 初始固件 {2} · {3}×{4}' -f $cfg.board,$cfg.pid,$cfg.firmware,$cfg.display[0],$cfg.display[1])+"`r`n身份配置：models\$id\config\factory\factory.prop"
    $rotation.Enabled=[bool]$cfg.rotation
    $rotationNote.Text=if($cfg.rotation){'运行中也可在传感器窗口实时旋转。'}else{'此型号目前使用原生竖屏。'}
    $status.Text='11 NFC：'+(Read-ModelState 'band11')+"`r`n10 Pro："+(Read-ModelState 'band10pro')
}
$model.Add_SelectedIndexChanged({Refresh-View});$model.SelectedIndex=0
$timer=New-Object Windows.Forms.Timer;$timer.Interval=1000;$timer.Add_Tick({Refresh-View});$timer.Start()
$form.Add_FormClosed({$timer.Stop();$timer.Dispose()})
if($VerifyOutput){
    [IO.Directory]::CreateDirectory($VerifyOutput)|Out-Null
    $form.Add_Shown({
        $bmp=New-Object Drawing.Bitmap($form.Width,$form.Height);$form.DrawToBitmap($bmp,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height)))
        $bmp.Save((Join-Path $VerifyOutput 'launcher.png'));$bmp.Dispose()
        [IO.File]::WriteAllText((Join-Path $VerifyOutput 'launcher.json'),(@{models=@($model.Items);title=$form.Text;status=$status.Text}|ConvertTo-Json),$utf8)
    })
}
if($AutoCloseSeconds -gt 0){
    $closeTimer=New-Object Windows.Forms.Timer;$closeTimer.Interval=$AutoCloseSeconds*1000
    $closeTimer.Add_Tick({$closeTimer.Stop();$form.Close()});$closeTimer.Start()
}
[Windows.Forms.Application]::Run($form)
