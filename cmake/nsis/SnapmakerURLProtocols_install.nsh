; Machine-wide URL protocol registration (mirrors macOS CFBundleURLSchemes).
; Included by CPack-generated NSIS installer via CPACK_NSIS_INSTALL_SCRIPT.
; On 64-bit Windows, 32-bit NSIS must use the 64-bit registry view so the 64-bit app sees these keys.
SetRegView 64

; Register ONLY our own scheme, so a side-by-side official Snapmaker Orca keeps its
; snapmaker-orca:// / Snapmaker_Orca:// handlers and our uninstall never removes them.
; This is the same scheme the running app registers in HKCU (GUI_App::associate_url);
; the two used to disagree, which was the collision this comment claimed to avoid.
WriteRegStr HKLM "Software\Classes\edgeslicer" "" "URL:EdgeSlicer"
WriteRegStr HKLM "Software\Classes\edgeslicer" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\edgeslicer\shell\open\command" "" '"$INSTDIR\EdgeSlicer.exe" "%1"'

SetRegView 32

; Windows Firewall: the inbound rule keys on the image path, so the rename would
; otherwise leave the old rule dead and pop a consent dialog on the first LAN listen.
; Pre-create it (idempotent: delete any rule of the same name first) so the phone/LAN
; service on TCP 13640 answers straight away. nsExec so nothing flashes a console.
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="EdgeSlicer"'
Pop $0
nsExec::ExecToLog '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="EdgeSlicer" dir=in action=allow program="$INSTDIR\EdgeSlicer.exe" protocol=TCP localport=13640 profile=private,domain enable=yes'
Pop $0
