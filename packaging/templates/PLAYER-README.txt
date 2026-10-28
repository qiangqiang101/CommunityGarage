FH2 COMMUNITY GARAGE
Share and download Forza Horizon 2 liveries, tunes and group vinyls in the game's own menus, on the community server.
Unofficial fan project. Not affiliated with Microsoft, Turn 10 Studios or Playground Games.

WHAT YOU NEED
- Forza Horizon 2 on PC, run with Azimuth (tested with Azimuth 0.9.10).
- Windows, 64-bit. An internet connection to reach the community server.

INSTALL (1 minute)
1. Close the game.
2. Extract this zip into your Azimuth folder (the one that contains azimuth.exe), keeping the folders as they are.
   You end up with:
      azimuth.exe
      DWrite.dll                         <- the FH2 Mod Platform (shared by all FH2 mods)
      fh2-mods\CommunityGarage.dll
      communitygarage\server.ini
   If Windows asks to replace DWrite.dll: say yes if yours is older or you are unsure. If you already use other
   FH2 mods, they use the same file, so it is safe. Do not delete files you already have in fh2-mods.
3. Start the game as usual.

USE IT
- Share: open one of your designs or tunes and use the game's normal Share option.
  Tip: the game remembers what it has already uploaded. To share something again, change it a little or save a copy first.
- Find: use the game's Find / Search results and the Suggested / Recommended tabs. Open one and Download it.
- Everything you share is public to all players on the server.

YOUR ACCOUNT
The first time you start the game a random key is created in communitygarage\account.ini. It is your login: keep that
file private, and back it up if you reinstall (without it you would become a new player and lose control of your uploads).
The creator name shown is your gamertag from azimuth.ini (a number is added if someone has the same name).

WHAT IT SENDS
To the community server: your account key, your display name, and what you choose to share. Nothing is sent to
Microsoft or Xbox Live by this mod. Rules: see TERMS.md.
To GitHub: the FH2 Mod Platform looks for a newer release about every 6 hours and says so on the title screen (nothing
is installed for you). Turn it off with check=0 under [updates] in communitygarage\server.ini.

LIMITS
New accounts can share a few items a day at first. Uploads are checked automatically (broken files and offensive text are
refused). The game's Clubs tab is switched off.
Very large liveries (over about 337 KB) can't be shared: the game itself refuses to open them
("Design Unavailable"). Delete some layers and share again.
Don't share designs made with a layer limit patch: they end up far too big for the game to download.

TROUBLESHOOTING
- Look at fh2-mods\platform.log. Lines mentioning CommunityGarage say whether it loaded ("HTTP bridge installed") and
  list any failures.
- "The saved copy of this item is not on this server": change the item slightly (or copy it) and share it again.
- Nothing shows up: check your internet connection and that communitygarage\server.ini still has host=forza.imnotmental.com.
- For a detailed log, set trace=1 under a [debug] section in server.ini and send platform.log to the contact below.

REMOVE
Delete fh2-mods\CommunityGarage.dll and the communitygarage folder. (To remove the platform and every mod built on
it, delete DWrite.dll and the fh2-mods folder.)

CONTACT / TAKEDOWN REQUESTS
<ADD YOUR CONTACT ADDRESS HERE>

CREDITS
FH2 Mod Platform by Trinculo54 (MIT license, see licenses\FH2-Mod-Platform-LICENSE.txt). Azimuth by its authors.
CommunityGarage is MIT licensed (LICENSE).
