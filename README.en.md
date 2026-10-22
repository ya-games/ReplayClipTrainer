# ReplayClipTrainer

[日本語](README.md) | **English**

ReplayClipTrainer is a BakkesMod plugin for Rocket League that turns a replay into a controllable private match. Pick any moment of a replay, take over any player (teammate or opponent), and practice the same scene over and over.

Use it for aerial practice, dribbling, decision-making in specific situations - anything where you want to repeat a play exactly as it happened.

It is a fork of [JumpInReplay](https://github.com/Atomus48/JumpInReplay) (by Atomus48), greatly extended with clip conversion, saved-clip management and a new settings UI.

## Installation

> **Building from source is recommended.** A prebuilt `.dll` is also attached to the Releases, but unsigned DLLs are sometimes flagged or quarantined by antivirus software. If you build it yourself you can read exactly what you are running, and the DLL is built against your own BakkesMod. See [Building](#building).

### Using the prebuilt DLL

1. Download the `.dll` from the repository's Releases.
2. Copy it into BakkesMod's plugins folder:
   - `%APPDATA%\bakkesmod\bakkesmod\plugins`
3. To load it automatically on startup, append this line to `cfg\plugins.cfg`:
   ```
   plugin load replaycliptrainer
   ```
4. Start Rocket League.

## Building

### Requirements

- Windows 10 / 11 (x64)
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) (Community is fine). In the installer, select the "**Desktop development with C++**" workload (it includes MSVC v143 and the Windows SDK).
- [BakkesMod](https://bakkesmod.com/), installed. It ships the plugin SDK (`bakkesmodsdk`) that the build uses.
- Git (to fetch the source; not needed if you download the zip)

### Steps

1. Get the source:
   ```powershell
   git clone https://github.com/ya-games/ReplayClipTrainer.git
   cd ReplayClipTrainer
   ```
2. Build (either way):
   - **From Visual Studio**: open `ReplayClipTrainer\ReplayClipTrainer.sln`, set the configuration to **Release** and the platform to **x64**, then Build -> Build Solution.
   - **From the command line** (PowerShell; Bash can mangle the `/p:` arguments):
     ```powershell
     & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
       "ReplayClipTrainer\ReplayClipTrainer.sln" `
       /p:Configuration=Release /p:Platform=x64 `
       /p:BakkesModPath="$env:APPDATA\bakkesmod\bakkesmod" /nologo /v:minimal
     ```
     If Visual Studio is installed in another edition or location, adjust the path to `MSBuild.exe`.
3. The post-build step runs BakkesMod's `bakkesmod-patch.exe` on the DLL and copies it into BakkesMod's `plugins` folder (if the game is running, it is hot-reloaded). The built DLL also stays at `ReplayClipTrainer\plugins\ReplayClipTrainer.dll`.
4. First time only: add `plugin load replaycliptrainer` to `cfg\plugins.cfg`, or run `plugin load replaycliptrainer` in the BakkesMod console (F6).

### Notes and troubleshooting

- **Where BakkesMod is**: normally found automatically through the registry (`HKCU\Software\BakkesMod\AppPath`). If it is not found or lives elsewhere, pass `/p:BakkesModPath="<your BakkesMod folder>"` (the folder that contains `bakkesmodsdk`).
- **Build fails because a file is in use**: Rocket League / BakkesMod has the DLL open. Close the game and build again.
- **Antivirus quarantined the DLL**: even a DLL you built yourself is unsigned and can occasionally be flagged. The source is public, so after checking it you can add the BakkesMod folder to your antivirus exclusions.
- The version is kept in `ReplayClipTrainer\version.h` (`MAJOR.MINOR.PATCH`).

## Usage

Run `togglemenu ReplayClipTrainer` (or `reclip_window`) in the BakkesMod console, or use F2 -> Plugins -> ReplayClipTrainer, to show / hide the settings window. Almost everything is done from this window.

Overview:

1. Play the replay you want to practice from.
2. Scrub to the moment you want.
3. Press "Save for Later" to save a clip to a file (the main way to use it). Press "Train Now" instead to practice right away without saving.
4. Load a saved clip from the Clips tab; it plays automatically in a private match.
5. Take over the player whenever you like ("Take over").

The window shows Japanese or English; switch it in the Settings tab (the default is Japanese; it falls back to English if the Japanese font cannot be loaded). The button names below are the English ones.

## Features

The settings window has three tabs: Convert / Clips / Settings.

### Convert tab: converting and saving clips

- **Replay position** (only while watching a replay): shows the current position as a bar; click or drag to jump anywhere. Goals (a circle in the scoring team's colour) and highlights (grey diamonds) are marked on the bar; hover one to see what it is and when it happened.
- **Clip range**:
  - "Seconds before / after the scrub position": how much around the current position to cut out. Before + After must add up to at least 5 seconds.
  - "Conversion resolution": the speed at which the original replay is played back while it is recorded (0.1 - 2.0, default 0.5).
    - `1.0` is normal speed.
    - Below `1.0` (e.g. `0.5`): the replay is recorded in slow motion, so more data is captured per second and the converted motion is smoother, but the conversion takes longer.
    - Above `1.0` (e.g. `2.0`): the replay is recorded fast-forwarded - faster, but the motion becomes a bit coarser.
- **Save clip**: type a clip name (auto-named if left blank) and press "Save for Later" to save the clip as a `.reclip` file without opening a private match. You can save several scenes from one replay and practice them later.
  - Turn on "Anonymize player names when saving" to replace names with `Player1, 2, 3...`. The player you were spectating becomes Player1, then their teammates, then the opponents (1v1 to 3v3). Handy when sharing clips.
- **Train without saving**: "Train Now" is a side feature: it converts the range and immediately starts a private match, without saving.
- While converting, a progress bar appears at the top of the screen. If the replay is paused the conversion cannot advance, and the bar tells you so. You can cancel from it.

### Clips tab: managing saved clips

- Saved clips are listed with their save time and player count.
- **Organize with folders**: "New folder" creates one; the "..." on a folder row renames it or deletes it (empty folders only). "Open folder" opens the folder you are viewing in Explorer.
- The "..." on a clip row lets you **rename** it or **move** it to another folder.
- "Load" opens a clip. "Delete" asks for confirmation (it turns into "Confirm delete" / "Cancel").
- "Refresh" updates the list; "Retry last clip" reopens the clip you last converted or loaded.
- "Recent clips only" lists the clips saved within the last 1 - 48 hours (default 12), newest first, regardless of folder.
- Clips saved by an incompatible version are greyed out so you cannot load them by mistake.

### During practice

- The clip plays automatically. The screen shows the current state - "**AUTO-PLAY**" or "**YOU'RE DRIVING**" - with a smaller line below telling you what pressing the key will do (e.g. `Press: "Take over"`).
- "Take over" lets you take control of the player who is being replayed. Right after a clip opens, the player you were actually spectating in the original replay is selected. "Resume playback" hands control back to the recording.
- You can pause, rewind / fast-forward and switch players.
- A small key guide can be shown at the top-left of the screen (toggle it in the Settings tab).

#### Key bindings

Press "Apply standard bindings" in the Settings tab to apply the following bindings (once is enough - BakkesMod stores them):

| Action | Keyboard | Controller |
| --- | --- | --- |
| Skip forward 5 s | Right | D-pad Right |
| Skip back 5 s | Left | D-pad Left |
| Next player | Up | D-pad Up |
| Previous player | Down | D-pad Down |
| Pause / resume | B | Left stick click (L3) |
| Take over / resume playback | V | Back / Select |

To change the keys themselves, use BakkesMod's own "Bindings" tab.

### Settings tab

- **Limited boost in replay**: whether boost is limited or unlimited during practice.
- **Unpause on controller input when jumping in**: when you switch from paused to "Take over", unpause automatically on throttle / boost input.
- **Show HUD**: show the time, player name, state, etc. during practice.
- **Show the key guide on screen**: show the key guide at the top-left during practice.
- **Disable goal replay / explosion**: skip the goal replay and explosion effect (experimental).
- **Let the ball go free on resume (experimental)**: when on, resuming from pause hands the ball to real physics instead of the recorded path - useful for touches later than in the original (the recorded velocity is kept). It is switched off again every time a new practice starts.
- **Key bindings**: apply the standard bindings.
- **Language / window**: language (Japanese / English), window size (also by dragging the grip at the bottom-right) and opacity. The Japanese text uses a Windows system font (BIZ UDGothic); after adding fonts or characters you need to restart the game.

### Safety

- While converting, the buttons that would start another conversion are disabled, and you can cancel safely.
- Clips that are too short are rejected with a message.
- Saved files are checked for version and consistency when loaded, so corrupted or other-version files cannot cause crashes.
- Renaming / moving clips and folders replaces characters Windows does not allow and refuses paths that would leave the clips folder.

## Known limitations

- Japanese on the HUD covers common characters; rare kanji may not be shown.
- Special modes such as Rumble and Dropshot, and timed modes, are not supported.
- Replays where a player left mid-match may not reproduce that player's movement correctly.
- Controlling the native pause / play of the original replay from the GUI is not possible because of BakkesMod limitations (pausing during practice, after conversion, works through the key bindings).

For the development history and detailed changes, see [docs/修正内容.md](docs/修正内容.md) (Japanese).

## License and credits

MIT License - see [LICENSE](LICENSE).

- ReplayClipTrainer by ya-games ([github.com/ya-games](https://github.com/ya-games))
- Based on [JumpInReplay](https://github.com/Atomus48/JumpInReplay) by Atomus48
