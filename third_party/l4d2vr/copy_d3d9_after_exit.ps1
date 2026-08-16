$src = 'D:\l4d2vr\Release\d3d9.dll'
$dst = 'E:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2\d3d9.dll'
$log = 'D:\l4d2vr\copy_d3d9_after_exit.log'
"waiting for left4dead2 to exit: $(Get-Date -Format o)" | Out-File -FilePath $log -Encoding UTF8
while (Get-Process left4dead2 -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 2 }
try {
    Copy-Item -LiteralPath $src -Destination $dst -Force
    "copied $src to $dst at $(Get-Date -Format o)" | Out-File -FilePath $log -Append -Encoding UTF8
    Get-Item -LiteralPath $dst | Select-Object FullName,Length,LastWriteTime | Out-String | Out-File -FilePath $log -Append -Encoding UTF8
} catch {
    "copy failed at $(Get-Date -Format o): $(.Exception.Message)" | Out-File -FilePath $log -Append -Encoding UTF8
}
