' P4X ground station - silent background launcher
' kills stale listeners on 8765/8766, starts serve.py + ai_bridge.py, opens the dashboard.

Option Explicit
Dim sh, fso, dir, py, c1, c2

Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

dir = fso.GetParentFolderName(WScript.ScriptFullName)
py  = "C:\py312\pythonw.exe"
If Not fso.FileExists(py) Then py = "C:\py312\python.exe"

' 1) free the ports
sh.Run "cmd /c for /f ""tokens=5"" %p in ('netstat -ano ^| findstr "":8765 :8766"" ^| findstr LISTENING') do taskkill /f /pid %p", 0, True

' 2) start services (hidden)
sh.CurrentDirectory = dir
c1 = """" & py & """ serve.py 8765"
c2 = """" & py & """ ai_bridge.py 8766"
sh.Run c1, 0, False
sh.Run c2, 0, False

' 3) open the dashboard
WScript.Sleep 2500
sh.Run "http://localhost:8765/dashboard.html", 1, False