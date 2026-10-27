# Community Garage

Share and download **Forza Horizon 2** liveries, tunes and group vinyls from the game's own menus, through a community
server. The game's Share, Find, Suggested and download screens work again, as they did when the original servers were online.

> Unofficial fan project for the PC version of Forza Horizon 2 running on Azimuth. Not affiliated with, endorsed by or
> connected to Microsoft, Turn 10 Studios or Playground Games. Forza and related names belong to their owners.

<img width="816" height="459" alt="image" src="https://github.com/user-attachments/assets/07bfd071-9139-4313-8827-5e06de09f15c" />
<img width="816" height="459" alt="image" src="https://github.com/user-attachments/assets/d55c46c3-2a69-41f3-8a55-265ffe786a5e" />

## What it does

- **Share** your liveries, tunes and group vinyls with the Share option you already know.
- **Find** other players' designs through the game's search screens, and the **Suggested / Recommended** tabs for the car you're looking at.
- **Download** a livery, tune or group vinyl and use it straight away.
- Works with the game as it is: no extra program to run. It's a mod for the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform).

## Requirements

- Forza Horizon 2 for PC, run with **Azimuth** (tested with Azimuth 0.9.10-1.0.2).
- Windows 10 or 11, 64-bit.
- An internet connection (to reach the community server).
- The **FH2 Mod Platform**: it's `d3dcompiler_47.dll` next to `azimuth.exe`, and the download below already includes it
  (version 2.7.2). If you use other FH2 mods you have it already; it's one shared file.
- Works alongside other platform mods (it has been used with Achievements, FnFNitro, ShowFPS, WheresMyBuddy and WreckingBall)
  and with Extra Options.

## Install

1. Download `CommunityGarage-<version>.zip` from the [latest release](https://github.com/qiangqiang101/CommunityGarage/releases/latest).
2. Close the game.
3. Extract the zip into your **Azimuth folder**, the one that contains `azimuth.exe`. You end up with:

   ```
   azimuth.exe
   d3dcompiler_47.dll                  <- the FH2 Mod Platform (shared by all FH2 mods)
   fh2-mods\CommunityGarage.dll
   communitygarage\server.ini
   ```
   If Windows asks to replace `d3dcompiler_47.dll`, say **yes** unless yours is newer than 2.7.2. Don't delete anything
   else that is already in `fh2-mods`.
4. Start the game. The first time, the mod makes your account key (see below). Nothing else to set up.

To check that it loaded, open `fh2-mods\platform.log`: lines tagged `[CommunityGarage]` should include `HTTP bridge installed`.

## Using it

- **Share:** open one of your designs or tunes and use the game's normal Share option. The game remembers what it has already
  uploaded, so to share something again, change it a little or save a copy first.
- **Find and download:** use the game's Find / Search results and the Suggested / Recommended tabs, open one and download it.
- **Everything you share is public** to all players on the server.
- **Settings:** Pause > Settings > **MODS** > Installed mods > Community Garage has a description, a "Check for updates" switch and
  a troubleshooting log switch. The same settings are in `communitygarage\server.ini`.

### Your account

The first start creates a random **account key** in `communitygarage\account.ini` (older versions kept it in `server.ini`; it's moved over automatically). It's your login:
**keep that file private and back it up** (if you lose it you become a new player and lose control of what you uploaded).
Updates don't touch it. Your creator name is the gamertag from `azimuth.ini` (a number is added if someone already has that
name); put `name=` under `[account]` in `account.ini` to change it. To start a new account, delete `account.ini`.

### Limits

- New accounts can share a few items a day at first; after that there's a daily and a total cap per account.
- Uploads are checked automatically. Broken files and offensive text are refused.
- **Very large liveries can't be shared.** The game refuses to open a downloaded livery bigger than about 337 KB; it just says "Design Unavailable". The server therefore refuses uploads above that size instead of storing
  something nobody can open. Simplify the design (delete some layers) and share it again. This is a limit inside the game, not
  something the mod can change.
- **Don't share designs made with a layer limit patch.** Tools that raise the game's layer limits let you build designs far
  bigger than the game can load from the server, so nobody (including you) will be able to download them.
- Tune previews are only a placeholder picture, AI opponents don't wear community liveries, and the game's **Clubs tab is
  switched off** (there are no club services).

## Privacy

- To the **community server**: your account key (stored there only as a hash), your display name, what you choose to share,
  and the address you connect from.
- To **GitHub**: the FH2 Mod Platform looks for a newer release about every six hours and says so on the title screen. Nothing
  is installed for you. Turn it off with **Check for updates** on the MODS page, or `check=0` under `[updates]` in `server.ini`.
- **Nothing is sent to Microsoft or Xbox Live** by this mod. The game's requests to Xbox Live are answered on your PC or
  refused, never forwarded.

The rules for the server are in [docs/TERMS.md](docs/TERMS.md).

## Uninstall

Close the game and delete:

- `fh2-mods\CommunityGarage.dll`
- the `communitygarage` folder (this holds your account key)

To remove the FH2 Mod Platform and **every** mod built on it, also delete `d3dcompiler_47.dll` and the `fh2-mods` folder.
Your game saves aren't touched. Designs you downloaded stay in your garage.

## Troubleshooting

| Problem | What to try |
|---|---|
| Nothing connects, or "server not available" | Check your internet connection and that `communitygarage\server.ini` still has the server address (`host=`) and `scheme=https`. |
| "The saved copy of this item is not on this server" when sharing | The game remembers it already uploaded this item somewhere else. Change it slightly, or save a copy, and share again. |
| "Design Unavailable" / "Failed to download design" on one livery | That livery is probably over the game's download limit of about 337 KB (see Limits). Liveries uploaded before the server enforced the limit can still be on the server and can't be opened by anyone. Other liveries download normally. |
| A share is refused | The server checks every upload for broken files and offensive text. The game only shows a generic error; the reason is in the server's log, so ask the server's operator. Check the title and keywords first. |
| No `[CommunityGarage]` lines in `platform.log` | The mod didn't load. Check that `d3dcompiler_47.dll` is next to `azimuth.exe` and that `fh2-mods\CommunityGarage.dll` exists. |
| Creator name shows with `#2` | Someone already uses your gamertag on the server; a number keeps names unique. |
| The game crashes | Send the newest files in `fh2-mods\crashes` and `data\logs`. |

For a detailed log, turn on **Detailed log (troubleshooting)** on the MODS page (or add `[debug]` with `trace=1` to
`server.ini`) and restart. It adds extra hooks and makes `platform.log` large, so turn it off afterwards.

## For developers

### Repositories

- **This repository**: the mod (`CommunityGarage.dll`), its packaging and the research notes.
- **CommunityGarageServer**: the community server (Node.js, MySQL optional), kept in its own repository.

### Layout

| Path | What |
|---|---|
| `mod/src/` | The mod: `communitygarage.cpp` (redirects, fixes, Mods page), `xhr_bridge.cpp` (real HTTP for the game's requests) |
| `platform/` | The FH2 Mod Platform, a Git submodule ([Trinculo54/fh2-mod-platform](https://github.com/Trinculo54/fh2-mod-platform)) |
| `packaging/` | Player zip scripts and templates (`package-player.ps1` for Windows, `package-player.sh` for CI) |
| `docs/` | `re-notes.md` (how the game's protocol and share flow were worked out) and `TERMS.md` |
| `.github/workflows/` | CI: builds and packages on a release or by hand |

### Build

Clone with submodules: `git clone --recurse-submodules <url>` (or `git submodule update --init` afterwards).

With llvm-mingw, CMake and Ninja (the code uses MSVC-style SEH, which needs clang):

```
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
cmake --build build
```
This produces `build/CommunityGarage.dll` and `build/platform/d3dcompiler_47.dll`. On Linux, cross-compile with
`-DCMAKE_TOOLCHAIN_FILE=platform/cmake/mingw-w64-x86_64.cmake`.

Then `packaging\package-player.ps1` (Windows) or `packaging/package-player.sh <mod dll> <platform dll> <out.zip>` makes the player zip.

### Release

1. Raise `VERSION` in `CMakeLists.txt` (the mod reports it, and players' update check compares it with the release tag).
2. Create a GitHub release whose tag is that version, such as `v1.0.1`, and publish it.
3. The **Build** workflow builds it, checks that the tag matches `VERSION`, and attaches `CommunityGarage-<tag>.zip` to the release.
   You can also run it by hand from the Actions tab to get the zip as an artifact.

### Updating the platform

`git submodule update --remote platform`, rebuild, and test. The mod is built against the platform's public header.

## Credits and license

- **FH2 Mod Platform** by [Trinculo54](https://github.com/Trinculo54), MIT licensed (its license is included in the download under `licenses\`).
- **Azimuth**, which runs the PC version of the game.
- Community Garage is MIT licensed: see [LICENSE](LICENSE).
