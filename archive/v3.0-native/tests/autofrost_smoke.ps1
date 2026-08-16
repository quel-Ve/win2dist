# autofrost_smoke.ps1 - win2blur Auto-frost smoke test (ASCII only)
# Verifies: 1) auto-apply on app open (800ms poll) 2) revert to opaque after list removal
#           3) re-apply after re-add
# Usage: powershell -ExecutionPolicy Bypass -File autofrost_smoke.ps1
$ErrorActionPreference = "Continue"
$src = "D:\Garage\Software\ccproject\12window2clear\native\build5\win2blur.exe"
$dir = "C:\Temp\w2d-test2"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWinProc cb, IntPtr lp);
  public delegate bool EnumWinProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetLayeredWindowAttributes(IntPtr h, out uint cr, out byte a, out uint f);
}
"@

function AlphaOf($hwnd) {
  if ($hwnd -eq [IntPtr]::Zero) { return -1 }
  if (([W]::GetWindowLong($hwnd, -20) -band 0x80000) -eq 0) { return 255 }  # not layered = opaque
  $cr=0; $a=0; $ff=0
  [W]::GetLayeredWindowAttributes($hwnd,[ref]$cr,[ref]$a,[ref]$ff) | Out-Null
  return $a
}

function FindNotepadByPid() {
  $procs = (Get-Process notepad -ErrorAction SilentlyContinue).Id
  if (-not $procs) { return [IntPtr]::Zero }
  $script:hit = [IntPtr]::Zero
  $cb = [W+EnumWinProc]{ param($h, $l)
    $pid2 = 0; [W]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($procs -contains $pid2 -and [W]::IsWindowVisible($h) -and ([W]::GetWindowLong($h, -20) -band 0x80) -eq 0) {
      $script:hit = $h; return $false
    }
    return $true
  }
  [W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  return $script:hit
}

function WriteCfg([string]$appsLine) {
  $lines = @("[AutoFrost]", "Enabled=1", "DefaultAlpha=217", "DefaultBlur=0")
  if ($appsLine) { $lines += $appsLine }
  $lines | Out-File "$dir\config.ini" -Encoding ascii
}

# ---- setup: fresh temp dir with notepad-only autofrost list ----
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dir | Out-Null
Copy-Item $src $dir
WriteCfg "App_0=notepad.exe|"

Get-Process win2blur -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process welcome_demo -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1

$p = Start-Process "$dir\win2blur.exe" -PassThru
Start-Sleep 2
Get-Process welcome_demo -ErrorAction SilentlyContinue | Stop-Process -Force   # unblock main thread
Start-Sleep 1

$notepadPath = "$env:WINDIR\System32\notepad.exe"   # PATH 的 Git notepad shim 会失败，用全路径
Start-Process $notepadPath
Start-Sleep 3   # 800ms poll x ~4

$h = FindNotepadByPid
$r1 = AlphaOf $h
Write-Host ("1) notepad auto-applied: alpha=$r1 (expect ~217)  exstyle=0x{0:X}" -f ([W]::GetWindowLong($h,-20)))

# ---- remove app from list (simulate Settings Remove + Apply) ----
WriteCfg ""
Start-Sleep 2.5   # mtime check + revert round
$r2 = AlphaOf $h
$layered = (([W]::GetWindowLong($h, -20) -band 0x80000) -ne 0)
Write-Host "2) after removal: alpha=$r2 layered=$layered (expect 255/False - reverted to opaque)"

# ---- add back (simulate Add + Apply) ----
WriteCfg "App_0=notepad.exe|"
Start-Sleep 2.5
$r3 = AlphaOf $h
Write-Host "3) re-added: alpha=$r3 (expect ~217 again)"

Get-Process win2blur -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "done"
