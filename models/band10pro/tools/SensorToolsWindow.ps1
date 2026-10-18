# Dot-sourced by SensorWindow.ps1. Identity tools are separate from sensor inputs.
$toolsPage=Page '工具调用器'
$toolsPage.Name='device_tools_page'
Label $toolsPage '设备身份 · 小米手环 10 Pro' 20 16 940 32 | Out-Null
Label $toolsPage '读取可在运行时使用；修改、撤销前先关闭模拟器，传感器窗口可以保留。' 20 57 940 28 | Out-Null
Label $toolsPage '操作' 20 106 110 | Out-Null
$toolChoice=New-Object Windows.Forms.ComboBox
$toolChoice.Name='tool_choice';$toolChoice.DropDownStyle='DropDownList'
$toolChoice.SetBounds(142,103,550,30)
$toolChoice.Items.AddRange(@('读取当前 DID / 序列号','修改 DID / 序列号（自动备份）','撤销上次身份修改'))
$toolChoice.SelectedIndex=0;$toolsPage.Controls.Add($toolChoice)
Label $toolsPage 'DID' 20 161 110 | Out-Null
$toolDid=New-Object Windows.Forms.TextBox;$toolDid.Name='tool_did'
$toolDid.SetBounds(142,158,550,30);$toolsPage.Controls.Add($toolDid)
Label $toolsPage '序列号 / SN' 20 209 115 | Out-Null
$toolSerial=New-Object Windows.Forms.TextBox;$toolSerial.Name='tool_serial'
$toolSerial.SetBounds(142,206,550,30);$toolsPage.Controls.Add($toolSerial)
$toolRun=Button $toolsPage '调用工具' 718 102 235 {Start-DeviceTool}
$toolRun.Name='tool_invoke'
Button $toolsPage '打开身份备份目录' 718 157 235 {
    $folder=Join-Path $root 'userdata\identity-backups'
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    Start-Process -FilePath explorer.exe -ArgumentList ('"'+$folder+'"')
}|Out-Null
Button $toolsPage '打开出厂配置目录' 718 205 235 {
    Start-Process -FilePath explorer.exe -ArgumentList ('"'+(Join-Path $root 'config\factory')+'"')
}|Out-Null
$toolIdentityStatus=Label $toolsPage '切换到本页会自动读取当前持久化身份。' 20 263 940 60
Label $toolsPage '修改会同步当前存档和下次启动配置。只改 DID 与序列号，保留密钥、MAC、GNSS 和用户数据。身份改变后可能需要重新绑定；DID 与原有设备密钥不匹配时，账号验证可能失败。' 20 335 930 65 | Out-Null
$toolOutput=New-Object Windows.Forms.TextBox
$toolOutput.Name='tool_output';$toolOutput.Multiline=$true;$toolOutput.ReadOnly=$true
$toolOutput.ScrollBars='Vertical';$toolOutput.SetBounds(20,416,940,92)
$toolsPage.Controls.Add($toolOutput);$toolOutput.Anchor='Top,Left'
$script:deviceToolJob=$null;$script:deviceToolLoaded=$false

function Start-DeviceTool([string]$Override) {
    if($script:deviceToolJob){return}
    $tool=if($Override){$Override}else{@('identity.read','identity.apply','identity.restore')[$toolChoice.SelectedIndex]}
    $request=@{tool=$tool}
    if($tool -eq 'identity.apply'){$request.did=$toolDid.Text;$request.serial=$toolSerial.Text}
    try {
        $id=[guid]::NewGuid().ToString('N')
        $inputPath=Join-Path $work ($id+'.request.json')
        $replyPath=Join-Path $work ($id+'.reply.json')
        [IO.File]::WriteAllText($inputPath,($request|ConvertTo-Json -Depth 4),$utf8)
        $arguments='-X utf8 -B "{0}" --request-file "{1}" --reply-file "{2}"' -f (Join-Path $PSScriptRoot 'device_tools.py'),$inputPath,$replyPath
        $p=Start-Process -FilePath $python -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $script:deviceToolJob=@{process=$p;request=$inputPath;reply=$replyPath;tool=$tool}
        $toolRun.Enabled=$false;$toolChoice.Enabled=$false;$toolDid.Enabled=$false;$toolSerial.Enabled=$false
        $toolOutput.Text='正在调用工具…'
    }catch{$toolOutput.Text=$_.Exception.Message}
}

function Complete-DeviceTool {
    $job=$script:deviceToolJob
    if(-not $job){return}
    if(-not(Test-Path -LiteralPath $job.reply)) {
        if(-not $job.process.HasExited){return}
        $toolOutput.Text='工具进程已退出但没有返回结果，请检查 Python 运行环境。'
    }else{
        try{
            $reply=(Read-Shared $job.reply)|ConvertFrom-Json
            if(-not $reply.ok){$toolOutput.Text=$reply.error}
            elseif($job.tool -eq 'identity.read'){
                $value=if($reply.saved){$reply.saved}else{$reply.source}
                $toolDid.Text=$value.did;$toolSerial.Text=$value.serial
                $where=if($reply.saved){'当前持久化存档'}else{'下次启动配置（尚无存档）'}
                $key=if($value.credential_present){'已有，保持原值'}else{'未提供'}
                $toolIdentityStatus.Text=$where+' · 蓝牙 '+$value.address+"`r`n设备密钥："+$key+'（不显示）'
                $sync=if($reply.values_match){'配置与存档身份一致。'}else{'配置与存档身份不同；输入框显示存档中的值，修改会同步两处。'}
                $toolOutput.Text=$sync+"`r`n存档："+$reply.journal
                $script:deviceToolLoaded=$true
            }else{
                $toolOutput.Text=$reply.message+"`r`n备份："+$reply.backup
                $toolIdentityStatus.Text='操作完成。可选择“读取当前 DID / 序列号”核对。'
            }
        }catch{$toolOutput.Text='读取工具结果失败：'+$_.Exception.Message}
    }
    foreach($path in @($job.request,$job.reply)){if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}}
    $job.process.Dispose();$script:deviceToolJob=$null
    $toolRun.Enabled=$true;$toolChoice.Enabled=$true;$toolDid.Enabled=$true;$toolSerial.Enabled=$true
}
$tabs.Add_SelectedIndexChanged({
    if($tabs.SelectedTab -eq $toolsPage -and -not $script:deviceToolLoaded){Start-DeviceTool 'identity.read'}
})
