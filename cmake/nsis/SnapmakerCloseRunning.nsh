; Closes the EdgeSlicer processes that run from the install folder(s) before files are replaced
; (install, upgrade over an old install) or removed (uninstall).
;
; Included through CPACK_NSIS_DEFINES, so it lands at the top level of the generated script and
; defines Function EdgeSlicerCloseRunning for the installer and un.EdgeSlicerCloseRunning for
; the uninstaller. The work is done by close_edgeslicer.ps1 (same folder): it asks the hub to quit
; on its loopback control plane, then ends what is left that runs from the folder, by path. It
; needs nothing from the EdgeSlicer.exe being closed, so it also works on a hub of an old version.
;
;   in : $R0 = install folder(s), '|' separated
;        $R1 = full path of close_edgeslicer.ps1
;   out: $R9 = "0" nothing is running from the folder(s) any more
;              "1" the user cancelled
;   clobbers $R2-$R4. A silent run (/S) ends the installer itself, with exit code 7 when
;   EdgeSlicer is open in a window and 8 when a process could not be stopped.
;
; Script exit codes: 0 clean, 3 an EdgeSlicer window is open, 4 a process could not be stopped,
; anything else (PowerShell blocked by policy, script missing): the old tasklist check.
!macro EdgeSlicerCloseRunning un
Function ${un}EdgeSlicerCloseRunning
  ; A trailing backslash would escape the closing quote of -Dirs "..." on the command line.
  StrCpy $R4 $R0 1 -1
  StrCmp $R4 "\" 0 +2
    StrCpy $R0 $R0 -1
  StrCpy $R2 ""
  StrCpy $R9 "0"

EdgeSlicerClose_run:
  nsExec::ExecToLog /TIMEOUT=180000 '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$R1" -Dirs "$R0" $R2'
  Pop $R3
  StrCmp $R3 "0" EdgeSlicerClose_done
  StrCmp $R3 "3" EdgeSlicerClose_gui
  StrCmp $R3 "4" EdgeSlicerClose_stuck
  Goto EdgeSlicerClose_legacy

EdgeSlicerClose_gui:
  ; An open window is somebody's work: never killed. Ask, and let the app's own close (with its
  ; "save the project?" question) do it.
  IfSilent 0 EdgeSlicerClose_gui_ask
    SetErrorLevel 7
    Quit
EdgeSlicerClose_gui_ask:
  MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION "EdgeSlicer is open.$\n$\nYes: close EdgeSlicer for me (you will be asked to save any open project)$\nNo: I have closed it myself, check again$\nCancel: exit without changing anything" IDYES EdgeSlicerClose_gui_close IDNO EdgeSlicerClose_gui_again
  StrCpy $R9 "1"
  Return
EdgeSlicerClose_gui_close:
  StrCpy $R2 "-CloseGui"
  Goto EdgeSlicerClose_run
EdgeSlicerClose_gui_again:
  StrCpy $R2 ""
  Goto EdgeSlicerClose_run

EdgeSlicerClose_stuck:
  IfSilent 0 EdgeSlicerClose_stuck_ask
    SetErrorLevel 8
    Quit
EdgeSlicerClose_stuck_ask:
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "A background EdgeSlicer process (the phone hub, camera relay or ffmpeg) could not be stopped.$\n$\nEnd it in Task Manager (look for EdgeSlicer.exe, go2rtc.exe and ffmpeg.exe in the EdgeSlicer folder), then click Retry. Cancel exits without changing anything." IDRETRY EdgeSlicerClose_gui_again
  StrCpy $R9 "1"
  Return

EdgeSlicerClose_legacy:
  ; The script did not run (PowerShell missing or blocked): fall back to the old guard, which
  ; only knows the program's names and so also reacts to a copy in another folder.
  nsExec::Exec 'cmd.exe /c tasklist /NH | findstr /i /b /c:EdgeSlicer.exe /c:snapmaker-orca.exe /c:Snapmaker_Orca.exe'
  Pop $R3
  StrCmp $R3 "0" 0 EdgeSlicerClose_done
  IfSilent 0 EdgeSlicerClose_legacy_ask
    SetErrorLevel 7
    Quit
EdgeSlicerClose_legacy_ask:
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "EdgeSlicer is still running.$\nClose the program (and end EdgeSlicer.exe in Task Manager if it keeps running in the background), then click Retry. Cancel exits the installer." IDRETRY EdgeSlicerClose_legacy
  StrCpy $R9 "1"
  Return

EdgeSlicerClose_done:
FunctionEnd
!macroend
!insertmacro EdgeSlicerCloseRunning ""
!insertmacro EdgeSlicerCloseRunning "un."
