' P4X 地面站 · 后台启动（无窗口）
' 作用：清理 8765/8766 旧占用 → 后台起页面服务与 AI 代理 → 不弹任何窗口
' 用法：双击即可；也放在「启动」文件夹里实现开机自启。

Option Explicit
Dim sh, fso, dir, py, cmd1, cmd2

Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

dir = fso.GetParentFolderName(WScript.ScriptFullName)
py = "C:\py312\pythonw.exe"
If Not fso.FileExists(py) Then py = "C:\py312\python.exe"

' 1) 清掉旧监听（避免端口被占导致 "启动不上"）
sh.Run "cmd /c for /f ""tokens=5"" %p in ('netstat -ano ^| findstr "":8765 :8766"" ^| findstr LISTENING') do taskkill /f /pid %p", 0, True

' 2) 起页面服务（8765）与 AI 代理（8766），全部隐藏
cmd1 = """" & py & """ -m http.server 8765"
cmd2 = """" & py & """ ai_bridge.py 8766"

sh.CurrentDirectory = dir
sh.Run cmd1, 0, False
sh.Run cmd2, 0, False

' 3) 等服务起来再开浏览器
WScript.Sleep 2500
sh.Run "http://localhost:8765/dashboard.html", 1, False