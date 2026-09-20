# Preferences → Extras

## What it is

EdgeSlicer’s own settings live on one **Extras** tab in Preferences, in six sections. Upstream Orca / Snapmaker Orca options stay on their usual tabs; fork-specific behaviour is collected here.

## Where to find it

- **Preferences** (or equivalent app settings) → **Extras**

## How to use it

Work through the sections that match what you need:

### Project

- **Auto-Save project** (with an interval)
- **Keep my printer when opening project files**
- **Skip Settings Mapping Warnings**
- **Drop imported models to the bed**
- **Bottom-referenced Z position**

### Presets

- **Prefer Last Used Print Profile**
- **Seamless System Filament Edits**
- **Import Bambu Studio user presets** (plus **Sync now**)

### Bambu Network

- Enable the network plug-in (UltraNet; bundled on Windows packages)
- **Stealth mode** (no Bambu cloud telemetry)
- Legacy plugin for old firmware

Live view additionally needs Bambu’s own camera component (not redistributed here); the Device tab can offer to download it once. See [Bambu Lab in EdgeSlicer](Bambu-Lab).

### Spool Manager

- Enable [Spoolman](https://github.com/Donkie/Spoolman)
- Server URL
- Deduct filament usage when sending a print (desktop, phone, and Reprints)

See [Spool Manager](Spool-Manager).

### Phone access

- **Start hidden** — serve the phone without a desktop window
- Remember Snapmaker printer certificates so the phone can connect them (**off by default**; keeps a private key at rest)

See [Edge Hub](Edge-Hub) and [Phone access](Phone-access).

### G-Code Archive

- **Store G-Code Files**
- Storage folder (default `<data dir>/gcode_archive`)
- **Maximum Retention** file count (default 100)

See [Store G-Code Files](Store-G-Code-Files).

## Limits / notes

| Situation | What happens |
|---|---|
| Remember Snapmaker certificates | Off by default because it stores a private key at rest. |
| Store G-Code Files | Off by default; past retention, oldest records (and sidecars / previews) are deleted. |
| Bambu Network on Linux / macOS | Windows packages bundle the plug-in; Linux/macOS builds currently do not (standalone plug-in zip may be published beside releases). |

Other notes:

- Prefer putting new fork-only toggles on Extras rather than scattering them across upstream tabs.
- Screenshot of Auto-Save: `docs/images/options-autosave.png`.

## Related

- [Edge Hub](Edge-Hub)  
- [Store G-Code Files](Store-G-Code-Files)  
- [Spool Manager](Spool-Manager)  
- [Bambu Lab in EdgeSlicer](Bambu-Lab)  
- [Compare Slices](Compare-Slices)  
- [Home](Home)  
