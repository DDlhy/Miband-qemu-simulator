$nfcPage=ExtraPage 'NFC 数据'
Label $nfcPage 'BES → NFC 为 TX，NFC → BES 为 RX；记录原厂 I2C1 数据，不把下载帧误当作卡片数据。' 18 16 960 | Out-Null
$nfcManual=New-Object Windows.Forms.CheckBox
$nfcManual.Text='启用实验返回总线（手动 HEX，无自动协议响应）'
$nfcManual.Location=New-Object Drawing.Point(18,54);$nfcManual.Size=New-Object Drawing.Size(750,30)
$nfcManual.Add_Click({Send-Control @{operation='nfc';connected=[bool]$nfcManual.Checked}})
$nfcPage.Controls.Add($nfcManual)
Button $nfcPage '导出收发记录…' 800 52 170 {
    if(-not $script:active){$notice.Text='模拟器未连接。';return}
    $source=Join-Path (Join-Path $projectRoot $script:active.session_id) 'nfc-traffic.jsonl'
    if(-not (Test-Path -LiteralPath $source)){$notice.Text='当前会话尚无 NFC 数据。';return}
    $dialog=New-Object Windows.Forms.SaveFileDialog
    $dialog.FileName='nfc-traffic.jsonl';$dialog.Filter='JSON Lines (*.jsonl)|*.jsonl'
    try {if($dialog.ShowDialog() -eq 'OK'){[IO.File]::Copy($source,$dialog.FileName,$true);$notice.Text='NFC 收发记录已导出。'}}finally{$dialog.Dispose()}
}|Out-Null
$nfcInput=Input $nfcPage '' 18 104 780;$nfcInput.Name='nfc_rx_hex'
Button $nfcPage '排入 NFC → BES' 812 102 160 {Send-Control @{operation='nfc';hex=$nfcInput.Text}}|Out-Null
$nfcStatus=Label $nfcPage '等待 NFC 总线…' 18 154 950 66
$nfcLog=New-Object Windows.Forms.TextBox
$nfcLog.Multiline=$true;$nfcLog.ReadOnly=$true;$nfcLog.ScrollBars='Both';$nfcLog.WordWrap=$false
$nfcLog.Location=New-Object Drawing.Point(18,232);$nfcLog.Size=New-Object Drawing.Size(950,280)
$nfcLog.Font=New-Object Drawing.Font('Consolas',10);$nfcLog.Name='nfc_trace';$nfcPage.Controls.Add($nfcLog)
Label $nfcPage '只观察模式保留原有硬件行为。实验模式只提供 I2C ACK / 字节队列，尚无 NFC IRQ 引脚、射频卡片或 eSE 模型。排队不等于固件已读取；不提供虚构卡号、余额或开卡成功。' 18 532 950 66 | Out-Null
$script:nfcLastSeq=0
function Refresh-Nfc($s) {
    if(-not $s -or -not $s.nfc) {$nfcStatus.Text='模拟器未连接或运行时尚不支持 NFC 观察';return}
    $n=$s.nfc
    if(-not $script:pending.Count) {$nfcManual.Checked=[bool]$n.connected}
    $mode=if($n.connected){'实验返回总线'}else{'只观察 / 原有缺失控制器行为'}
    $nfcStatus.Text=('{0} · I2C{1} · 地址 0x{2:X2}`r`nTX {3} 字节 · RX 实际读取 {4} 字节 · 返回排队 {5} · 读请求 {6} · 失败数据命令 {7}' -f $mode,$n.bus,[int]$n.address,$n.tx_bytes,$n.rx_bytes,$n.queued,$n.read_requests,$n.aborts).Replace('`r`n',"`r`n")
    $last=if($n.trace.Count){$n.trace[-1].seq}else{0}
    if($last -eq $script:nfcLastSeq){return}
    $script:nfcLastSeq=$last;$lines=@()
    foreach($e in $n.trace) {
        $hex=[string]$e.hex;$shape='raw / 下载或未识别帧'
        # Identify a complete NCI control packet only when its declared length
        # matches. Unknown / fragmented data is deliberately left as raw HEX.
        if($hex.Length -ge 6) {
            $b0=[Convert]::ToInt32($hex.Substring(0,2),16)
            $len=[Convert]::ToInt32($hex.Substring(4,2),16)
            if($hex.Length -eq (3+$len)*2 -and ($b0 -band 0x10) -eq 0 -and ($b0 -shr 5) -in @(1,2,3)) {
                $kind=@('DATA','CMD','RSP','NTF')[$b0 -shr 5]
                $shape=('NCI {0} GID={1:X} OID={2:X2}' -f $kind,($b0 -band 15),([Convert]::ToInt32($hex.Substring(2,2),16) -band 63))
            }
        }
        $ack=if($e.ack){'ACK'}else{'NACK'}
        $lines+=('#{0} {1} @{2:X2} {3} PC={4:X8} {5}  {6}' -f $e.seq,$e.direction,[int]$e.address,$ack,[uint32]$e.pc,$shape,($hex -replace '(..)(?!$)','$1 '))
    }
    $nfcLog.Text=$lines -join "`r`n";$nfcLog.SelectionStart=$nfcLog.TextLength;$nfcLog.ScrollToCaret()
}
