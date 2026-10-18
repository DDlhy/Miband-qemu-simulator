# Dot-sourced by the sensor window. Tools run separately from the hardware FIFO.
$toolsPage=ExtraPage '工具调用器'
$toolsPage.Name='device_tools_page'
Label $toolsPage '设备身份 · 小米手环 11 NFC' 22 20 930 32 | Out-Null
Label $toolsPage '读取可在运行时使用；写入或撤销前先关闭模拟器，传感器窗口可以保持打开。' 22 59 930 28 | Out-Null
Label $toolsPage '目标存档' 22 106 110 | Out-Null
$toolTarget=New-Object Windows.Forms.ComboBox
$toolTarget.Name='tool_target';$toolTarget.DropDownStyle='DropDownList'
$toolTarget.Location=New-Object Drawing.Point(140,103);$toolTarget.Size=New-Object Drawing.Size(560,30)
$toolTarget.Items.AddRange(@('日常存档 · 启动模拟器 / 原生蓝牙 / 离线模式','独立存档 · 旧测试存档（可选）'))
$toolTarget.SelectedIndex=0;$toolsPage.Controls.Add($toolTarget)
Label $toolsPage '调用工具' 22 151 110 | Out-Null
$toolChoice=New-Object Windows.Forms.ComboBox
$toolChoice.Name='tool_choice';$toolChoice.DropDownStyle='DropDownList'
$toolChoice.Location=New-Object Drawing.Point(140,148);$toolChoice.Size=New-Object Drawing.Size(560,30)
$toolChoice.Items.AddRange(@('读取当前 DID / 序列号','修改 DID / 序列号（自动备份）','撤销上次身份修改'))
$toolChoice.SelectedIndex=0;$toolsPage.Controls.Add($toolChoice)
Label $toolsPage 'DID' 22 204 110 | Out-Null
$toolDid=Input $toolsPage '' 140 201 560;$toolDid.Name='tool_did'
Label $toolsPage '序列号 / SN' 22 250 115 | Out-Null
$toolSerial=Input $toolsPage '' 140 247 560;$toolSerial.Name='tool_serial'
$toolIdentityStatus=Label $toolsPage '尚未读取；切换到本页会自动读取选中存档。' 22 292 930 55
$toolRun=Button $toolsPage '调用工具' 724 146 220 { Start-DeviceTool }
$toolRun.Name='tool_invoke'
Button $toolsPage '打开身份备份目录' 724 201 220 {
    $folder=Join-Path $projectRoot 'userdata\identity-backups'
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    Start-Process -FilePath explorer.exe -ArgumentList ('"'+$folder+'"')
}|Out-Null
Button $toolsPage '打开存档目录' 724 247 220 {
    Start-Process -FilePath explorer.exe -ArgumentList ('"'+(Join-Path $projectRoot 'userdata')+'"')
}|Out-Null
Label $toolsPage '只修改 DID 与序列号，保留设备密钥、蓝牙地址、标定和其他出厂项。不会清空绑定、应用或 OTA。身份改变后可能需要重新绑定；DID 与原有密钥不匹配时，账号验证可能失败。其他存档需分别选择并应用。' 22 356 920 70 | Out-Null
$toolOutput=New-Object Windows.Forms.TextBox
$toolOutput.Name='tool_output';$toolOutput.Multiline=$true;$toolOutput.ReadOnly=$true
$toolOutput.ScrollBars='Vertical';$toolOutput.Location=New-Object Drawing.Point(22,443)
$toolOutput.Size=New-Object Drawing.Size(922,118);$toolOutput.Anchor='Top,Left,Right'
$toolsPage.Controls.Add($toolOutput)
$script:deviceToolJob=$null
$script:deviceToolLoaded=$false

function Start-DeviceTool([string]$Override) {
    if($script:deviceToolJob){return}
    $tool=if($Override){$Override}else{@('identity.read','identity.apply','identity.restore')[$toolChoice.SelectedIndex]}
    $target=if($toolTarget.SelectedIndex -eq 1){'standalone'}else{'bluetooth'}
    $request=@{tool=$tool;target=$target}
    if($tool -eq 'identity.apply') {$request.did=$toolDid.Text;$request.serial=$toolSerial.Text}
    try {
        $id=[guid]::NewGuid().ToString('N')
        $inputPath=Join-Path $work ($id+'.request.json')
        $replyPath=Join-Path $work ($id+'.reply.json')
        [IO.File]::WriteAllText($inputPath,($request|ConvertTo-Json -Depth 4),$utf8)
        $arguments='-X utf8 -B "{0}" --request-file "{1}" --reply-file "{2}"' -f (Join-Path $PSScriptRoot 'device_tools.py'),$inputPath,$replyPath
        $p=Start-Process -FilePath $pythonExe -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $script:deviceToolJob=@{process=$p;request=$inputPath;reply=$replyPath;tool=$tool;started=[DateTime]::UtcNow}
        $toolRun.Enabled=$false;$toolTarget.Enabled=$false;$toolChoice.Enabled=$false
        $toolDid.Enabled=$false;$toolSerial.Enabled=$false
        $toolOutput.Text='正在调用工具…'
    } catch {$toolOutput.Text=$_.Exception.Message}
}

function Complete-DeviceTool {
    $job=$script:deviceToolJob
    if(-not $job){return}
    if(-not (Test-Path -LiteralPath $job.reply)) {
        if(-not $job.process.HasExited){return}
        $toolOutput.Text='工具进程已退出但没有返回结果，请检查 Python 运行环境。'
    } else {
        try {
            $reply=[IO.File]::ReadAllText($job.reply)|ConvertFrom-Json
            if(-not $reply.ok) {$toolOutput.Text=$reply.error}
            elseif($job.tool -eq 'identity.read') {
                $value=if($reply.saved){$reply.saved}else{$reply.source}
                $toolDid.Text=$value.did;$toolSerial.Text=$value.serial
                $where=if($reply.saved){'当前持久化存档'}else{'下次启动配置（尚无 NOR 存档）'}
                $key=if($value.credential_present){'已有，保持原值'}else{'未提供'}
                $toolIdentityStatus.Text=('{0} · 蓝牙 {1}`r`n设备密钥：{2}（不显示）' -f $where,$value.address,$key).Replace('`r`n',"`r`n")
                $sync=if($reply.values_match){'配置与存档身份一致。'}else{'配置与存档身份不同；输入框显示存档中的值，修改工具会同步两处。'}
                $toolOutput.Text=$sync+"`r`n存档："+$reply.nor
                $script:deviceToolLoaded=$true
            } else {
                $toolOutput.Text=$reply.message+"`r`n备份："+$reply.backup
                $toolIdentityStatus.Text='操作完成。可选择“读取当前 DID / 序列号”核对。'
            }
        } catch {$toolOutput.Text='读取工具结果失败：'+$_.Exception.Message}
    }
    foreach($path in @($job.request,$job.reply)) {if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}}
    $job.process.Dispose();$script:deviceToolJob=$null
    $toolRun.Enabled=$true;$toolTarget.Enabled=$true;$toolChoice.Enabled=$true
    $toolDid.Enabled=$true;$toolSerial.Enabled=$true
}
$toolTarget.Add_SelectedIndexChanged({$script:deviceToolLoaded=$false;Start-DeviceTool 'identity.read'})
$tabs.Add_SelectedIndexChanged({
    if($tabs.SelectedTab -eq $toolsPage -and -not $script:deviceToolLoaded){Start-DeviceTool 'identity.read'}
})
