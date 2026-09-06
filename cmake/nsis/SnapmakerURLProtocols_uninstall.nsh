; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
; Remove only our own schemes -- never the official app's snapmaker-orca:// /
; Snapmaker_Orca:// keys. ultraone and snapmaker-ultra were ours under the older
; names, so they go too.
DeleteRegKey HKLM "Software\Classes\edgeslicer"
DeleteRegKey HKLM "Software\Classes\ultraone"
DeleteRegKey HKLM "Software\Classes\snapmaker-ultra"
SetRegView 32
