# add_current_repro.ps1 - "Add current" regression test (ASCII only)
# Scenarios:
#   1) fg=notepad at Settings open -> Add -> notepad.exe appears (count 8)
#   2) Add again -> "Already in list" feedback, count unchanged
#   3) user scenario: Remove notepad, simulate switching to notepad (WM_ACTIVATE),
#      Add -> notepad.exe re-added (count 8) - the "deleted then add back" flow
$ErrorActionPreference = "Continue"
$src = "D:\Garage\Software\ccproject\12window2clear\native\build5\win2blur.exe"
$dir = "C:\Temp\w2d-debug1"
$log = "C:\Temp\win2blur_debug.log"

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W3 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWinProc cb, IntPtr l);
  public delegate bool EnumWinProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc cb, IntPtr l);
  public delegate bool EnumChildProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessage")] public static extern IntPtr SendMessageSB(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@

$WM_COMMAND = 0x0111; $BM_CLICK = 0x00F5; $WM_CLOSE = 0x0010; $WM_ACTIVATE = 0x0006; $WA_ACTIVE = 1
$ID_SETTINGS = 1001; $LB_SETCURSEL = 0x0186; $LB_GETCURSEL = 0x0188

function FindByClass([string]$cls) {
  $script:found = [IntPtr]::Zero
  $cb = [W3+EnumWinProc]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 64
    [W3]::GetClassName($h, $sb, 64) | Out-Null
    if ($sb.ToString() -eq $cls) { $script:found = $h; return $false }
    return $true
  }
  [W3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  return $script:found
}

function FindByPid([int]$targetPid) {
  $script:found = [IntPtr]::Zero
  $cb = [W3+EnumWinProc]{ param($h, $l)
    $pid2 = 0; [W3]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $targetPid -and [W3]::IsWindowVisible($h)) { $script:found = $h; return $false }
    return $true
  }
  [W3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  return $script:found
}

function SetFg([IntPtr]$h) {
  $tpid = 0; [W3]::GetWindowThreadProcessId($h, [ref]$tpid) | Out-Null
  [W3]::AttachThreadInput([W3]::GetCurrentThreadId(), $tpid, $true) | Out-Null
  [W3]::SetForegroundWindow($h) | Out-Null
  [W3]::AttachThreadInput([W3]::GetCurrentThreadId(), $tpid, $false) | Out-Null
  Start-Sleep -Milliseconds 500
}

function CtrlById($dlg, [int]$id) {
  $script:ctrl = [IntPtr]::Zero
  $cb = [W3+EnumChildProc]{ param($h, $l)
    if ([W3]::GetDlgCtrlID($h) -eq $id) { $script:ctrl = $h; return $false }
    return $true
  }
  [W3]::EnumChildWindows($dlg, $cb, [IntPtr]::Zero) | Out-Null
  return $script:ctrl
}

function FindList($dlg) {
  $script:list = [IntPtr]::Zero
  $cb = [W3+EnumChildProc]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 64
    [W3]::GetClassName($h, $sb, 64) | Out-Null
    if ($sb.ToString() -eq "LISTBOX") { $script:list = $h; return $false }
    return $true
  }
  [W3]::EnumChildWindows($dlg, $cb, [IntPtr]::Zero) | Out-Null
  return $script:list
}

function FindStatusText($dlg, [string]$prefix) {
  # find a child STATIC whose text starts with the prefix
  $script:res = ""
  $cb = [W3+EnumChildProc]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 64
    [W3]::GetClassName($h, $sb, 64) | Out-Null
    if ($sb.ToString() -eq "STATIC") {
      $t = New-Object System.Text.StringBuilder 256
      [W3]::GetWindowText($h, $t, 256) | Out-Null
      if ($t.ToString().StartsWith($prefix)) { $script:res = $t.ToString(); return $false }
    }
    return $true
  }
  [W3]::EnumChildWindows($dlg, $cb, [IntPtr]::Zero) | Out-Null
  return $script:res
}

function DumpList($dlg, [string]$tag) {
  $list = FindList $dlg
  $count = [W3]::SendMessage($list, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
  Write-Host "$tag : count=$count"
  for ($i = 0; $i -lt $count; $i++) {
    $buf = New-Object System.Text.StringBuilder 256
    [W3]::SendMessageSB($list, 0x0189, [IntPtr]$i, $buf) | Out-Null
    Write-Host "$tag :   [$i] $($buf.ToString())"
  }
  return $count
}

function ClickButton($dlg, [int]$id) {
  $btn = CtrlById $dlg $id
  [W3]::SendMessage($btn, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 400
}

# ---- setup ----
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dir | Out-Null
Copy-Item $src $dir
Remove-Item $log -Force -ErrorAction SilentlyContinue
$lines = @("[AutoFrost]", "Enabled=1", "DefaultAlpha=217", "DefaultBlur=0",
  "App_0=WindowsTerminal.exe|", "App_1=msedge.exe|", "App_2=explorer.exe|CabinetWClass",
  "App_3=cloudmusic.exe|", "App_4=WeChat.exe|", "App_5=CherryStudio.exe|", "App_6=Code.exe|")
$lines | Out-File "$dir\config.ini" -Encoding ascii

Get-Process win2blur -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process welcome_demo -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1

Start-Process "$dir\win2blur.exe" | Out-Null
Start-Sleep 2
Get-Process welcome_demo -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1

$np = Start-Process "$env:WINDIR\System32\notepad.exe" -PassThru
Start-Sleep 2
$nph = FindByPid $np.Id
Write-Host ("notepad hwnd=0x{0:X8}" -f $nph.ToInt64())

# open Settings (foreground is whatever it is - the WM_ACTIVATE simulation below makes
# the scenarios deterministic regardless of which window owns the real foreground)
$tray = [W3]::FindWindow("win2blurTray", $null)
[W3]::PostMessage($tray, $WM_COMMAND, $ID_SETTINGS, [IntPtr]::Zero) | Out-Null
Start-Sleep 1
$dlg = FindByClass "win2blurSettings"
Write-Host ("settings hwnd=0x{0:X8}" -f $dlg.ToInt64())
Write-Host ("status at open: '$((FindStatusText $dlg 'Add current:') )'")

# ---- scenario 1: user activates notepad -> Add -> notepad.exe appears ----
[W3]::SendMessage($dlg, $WM_ACTIVATE, [IntPtr]$WA_ACTIVE, $nph) | Out-Null
Start-Sleep -Milliseconds 300
ClickButton $dlg 106
$c1 = DumpList $dlg "[1]"
Write-Host "[1] status: '$((FindStatusText $dlg 'Added:') )'  expected 'Added: notepad.exe'"
if ($c1 -eq 8 -and (FindStatusText $dlg "Added:") -match "notepad.exe") { Write-Host "[1] PASS" } else { Write-Host "[1] FAIL" }

# ---- scenario 2: Add again -> Already in list feedback ----
ClickButton $dlg 106
$c2 = DumpList $dlg "[2]"
Write-Host "[2] status: '$((FindStatusText $dlg 'Already') )'  expected 'Already in list: notepad.exe'"
if ($c2 -eq 8 -and (FindStatusText $dlg "Already") -match "notepad.exe") { Write-Host "[2] PASS" } else { Write-Host "[2] FAIL" }

# ---- scenario 3: user scenario - remove notepad, switch to it, add back ----
$list = FindList $dlg
[W3]::SendMessage($list, $LB_SETCURSEL, [IntPtr]7, [IntPtr]::Zero) | Out-Null
ClickButton $dlg 107   # Remove selected
$c3 = DumpList $dlg "[3a]"
Write-Host "[3a] status: '$((FindStatusText $dlg 'Removed:') )'  (expect count 7)"
# simulate: user activates notepad again, then clicks back into Settings
[W3]::SendMessage($dlg, $WM_ACTIVATE, [IntPtr]$WA_ACTIVE, $nph) | Out-Null
Start-Sleep -Milliseconds 300
Write-Host "[3b] status after switching: '$((FindStatusText $dlg 'Add current:') )'  (expect notepad.exe)"
ClickButton $dlg 106
$c4 = DumpList $dlg "[3c]"
Write-Host "[3c] status: '$((FindStatusText $dlg 'Added:') )'  (re-added - expect count 8)"
if ($c4 -eq 8 -and (FindStatusText $dlg "Added:") -match "notepad.exe") { Write-Host "[3] PASS" } else { Write-Host "[3] FAIL" }

[W3]::PostMessage($dlg, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep 1

Write-Host "---- debug log ----"
if (Test-Path $log) { Get-Content $log } else { Write-Host "(no log file)" }

Get-Process win2blur -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "done"
