' Silent elevated launch for win2blur (no UAC prompt, no console flash).
' Runs the "win2blur" scheduled task, registered with highest privileges by install.bat.
Set sh = CreateObject("WScript.Shell")
sh.Run "schtasks /run /tn win2blur", 0, False
