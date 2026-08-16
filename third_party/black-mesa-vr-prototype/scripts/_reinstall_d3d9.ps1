Copy-Item 'C:\Users\Henning\Documents\cursor\black mesa\BMSVR\build_d3d9\Release\d3d9.dll' 'C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bin\d3d9.dll' -Force
Remove-Item 'C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bmsvr_log.txt' -Force -ErrorAction SilentlyContinue
'installed ' + (Get-Date) | Set-Content 'C:\Users\Henning\Documents\cursor\black mesa\scripts\install_log.txt'
