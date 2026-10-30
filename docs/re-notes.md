# Reverse-engineering notes

Build: `forza_durango_release_final.exe` (24 Nov 2015). Addresses are VAs at the
preferred base `0x140000000`; the mod works from the base the platform reports.

## How the game talks to its server

- Own HTTP client (`HttpProtocolFactory`), binary protocol `bin/xtsw`, POSTs to
  `https://<host>:<port><url>`. Not standard SOAP; `XLS_E_SOAP_EXCEPTION` is just an error name.
- Retail endpoint: `gameservices.FH2.forzamotorsport.net`, `/Services.FH2Release/o.xtsw`
  (and `/Services.FH2Release.Trial/o.xtsw`), port `443`.
- The endpoints are a static table in `.data` at `0x142F65F50`, `0x50` bytes per
  environment (TURN, CERT, MSFT Dogfood/PreRelease, ... FH2Release, FH2Release.Trial), as
  `{section, key, value}` pointer triples:
  `TestLspProvider/unsecureAddress/<host>` at `+0x00`, `HttpProtocolFactory/port/<"443">` at
  `+0x18`, `HttpProtocolFactory/url/<path>` at `+0x30`.
  FH2Release is at `0x142F66270`, the Trial twin at `0x142F662C0`.
- The reader is the function at `0x141B7C0E0` (`port`, `url`, `UseHTTPS`, `unsecureAddress`
  looked up in a config object). Nothing in the shipped data files defines these keys.
- `UseHTTPS` has no entry in the table; its default is whatever `[provider+0x2A]` starts as.
  Not yet determined, test with a real TLS endpoint.

## Service surface (from exe strings)

`Forza.WebServices.Storefront`, `StorefrontLivery`, `StorefrontTuning`, `StorefrontLayerGroup`,
`StorefrontPhoto`, `StorefrontReplay`, plus `OnlineProfile` (car setups and liveries),
`GiftLivery`, `GiftTuning`, `User`, `LiveServices`, `ContentVersioning`.
Storefront calls: `LiveryCreate`, `TuningCreate`, `SearchLiveryFiles`, `SearchTuningFiles`,
`GetFileDataIfShared`, `GetFileHeavyWeightData`, `GetMyFileList`, `RemoveFile`,
`GetCuratedContent`, `GetFeaturedLiveriesForRandomCars`, `IncrementLikes/Views/Usage`,
`ReportContent`, `ChangeFileTitle/Description`, `ToggleFollowedCreator`.

## Local save layout

Azimuth saves: `data\titles\1A96545B\saves\player-1\<Type>_<carId>_<timestamp>\revisions\<n>\`
with `data\{C_livery,header}` and a `commit` manifest (SHA-256 + size per file) that must
match. Types seen: `Livery`, `BaseLivery`, `GarageThumbnail`, `Photo`.
Tunes: not a save entry; likely in `PlayerDatabase` (`TuningUGCSaveState` exists).

## Open questions

1. `xtsw` wire format and the login/session handshake (the game needs `ConnectedToLSP`).
2. Whether the game's HTTPS accepts a public CA certificate or pins one.
3. Where tunes live and what a `StorefrontTuning` payload contains.

## Sharing and browsing (working as of 2026-10-24)

Share fails with "error communicating with server" unless three Xbox Live dependencies are satisfied:
- `Azimuth.OfflineTokenRequest` (azimuth+0xD3E20) builds every `GetTokenAndSignatureAsync` result as an op already failed with
  0x800704CF. The mod hooks the constructor call at azimuth+0xD3EAD and swaps in hr=0 plus a fake result object whose
  property getters return a null HSTRING. Do not use combase HSTRINGs: Azimuth has its own string table and aborts
  ("invalid local string handle").
- Xbox Services `privacy.xboxlive.com` permission checks and `client-strings.xboxlive.com/system/strings/validate`
  are answered locally by the XHR bridge (JSON, "allowed" / one `{"resultCode":0}` per string).
- Only then do the game's own calls reach the server.

Wire layouts come from runtime reflection tables (rows of 12 qwords: kind, name, ..., size@q8, typeinfo@q9, element row@q10;
struct typeinfo = {rows*, count}). Kinds: 0 u8, 1/2 u16, 3/4/7 u32 (7 float), 5/6 u64, 9 str, 0xA/0x10 date string,
0xB/0x11 bool, 0xC struct, 0xE array (u32 count), 0x14 blob (u32 count), 0x12 guid (16 bytes), 0x13 f64. The "typedump" command
(platform command pipe, while the game runs) writes `communitygarage/typedump.txt` (`dump_types`: BFS from the descriptor slots);
decode with the row layout above. A method's two slots are the `lea rax,[rip+...]` operands just before its registration call
(`lea rdx,"<Method>"; lea rcx,"Forza.WebServices.<Service>, Forza.WebServices"; call game+0xEDA580`). The rows themselves are
static (.rdata); nested struct type info is filled at runtime by `lea rdx,rows; lea rcx,slot; mov r8d,count; jmp game+0xEE4A00`.

Scoreboard (2026-10-30): slots game+0x354D310..0x354D4A0 (DownloadFile, GetBestScore, GetLadderRows, GetRival, GetRows,
GetRowsAnthem, GetTimeTrials, NotifyRival, SubmitScore, SubmitXpScore, UploadFile). Layouts are written out in the server's
scoreboard.js. Speed traps are scoreType 5, boards 1-30 (SpeedCameras.str IDS_SpeedTrapNN); speed zones scoreType 6, boards 36-50
(IDS_SpeedZone01-15). Score = f64 m/s. The game submits only a new personal best. With `[debug] trace=1` the bridge saves every
Scoreboard request/reply to `communitygarage/captures/`.

Decoded: Storefront Search*/GetMyFileList/GetCuratedContent (FileSummary), GetFileHeavyWeightData and
GetFeaturedLiveriesForRandomCars (FileData), GetLiveriesForDrivatar, RetrieveVersionedTuneById (CarId, BinaryVersion u16,
CreatorName, CreatorXuid, then the CreateVersionedTuneFromStructs body minus its CarId) and RetrieveVersionedLiveryById
(profileId, carId, numberOfLayers u16, xuid, data blob echoed from UpdateLivery, two empty thumbnails).
Create bodies start with a FileHeader (Title, Description, Quantity, DownloadCount, date, rating u8, Type u32 [0 tune, 1 layer group,
2 livery], PayloadSize, KeywordOne u8, KeywordTwo u8) followed by the u64 versioned id.
The game caches the versioned id of a tune it already uploaded and skips re-uploading, so deleting server data
orphans those ids; a modified/new tune is needed.

## Lessons about server replies (2026-10-24)
- Unknown calls get an all-zero success reply by default. That is NOT harmless: all-zero `GetEconomySettings` made every race pay
  0 credits/0 XP and the game kept it. Serve real values (`economy.json` in the server repository) or reply with an error.
- An empty *successful* `GetDrivatarsForCache` makes the game write `tmp/Turn10Temp.scratch/DrivatarGlobalCache/DownloadCache.bin`
  ({u32 4, u32 0}) and stop spawning free-roam AI. Reply with an error instead; delete that file once to recover.
- Made-up drivatar records (skills zeroed) are ignored/harmful; `SERVE_DRIVATARS=1` enables them for experiments only.
- WheresMyBuddy only rewrites local DefaultCache.bin / AIPlayers.str names; it does not conflict with this mod.
- A tune's InstalledParts are upgrade row ids of its own car (`carId*1000 + n`, e.g. 2042000 = car 2042's stock drivetrain).
  The car builder at game+0x1056210 looks each one up for the car it is building; the engine/motor rows are null-checked, the
  drivetrain row (`[rbp+0x518]`) is not, so a tune applied to another car crashes at game+0x1056BAE reading "DrivetrainID"
  (crash 2026-10-25 22:49). `FileSummary.Ordinal` is the file's car: the server must send the car id there (it sent the list
  index), and never list a tune without a known car.
- The builder's r8 is a car setup record: u32 carId, u32 x2 (swaps), 43 u32 part ids (copied from +4, 0xB4 bytes), byte +0x168 =
  use stock parts. The crash (2026-10-25 23:07, detailed log) followed GetMyFileList -> GetFileHeavyWeightData ->
  RetrieveVersionedTuneById: a My Files tune for another car applied to the current one. The mod's "car builder: foreign tune
  parts" hook (game+0x1056263) passes a copy with the stock flag set when car-level slots (0-11, 34-41) name another car.
  That avoids the crash but fits stock parts (the rims changed). The caller (game+0xAF1559) builds the record from the current
  car id plus all 45 values of the tune, so the car's own parts are not at hand there. The XHR bridge now refuses such a tune
  earlier: it remembers the car from GetCuratedContent / SearchTuningFiles (body offset 4) and answers a RetrieveVersionedTuneById
  whose CarId (reply offset 12) differs with an error reply; the builder guard stays as a backstop.

## Clubs tab (2026-10-25)
The Clubs tab came on once the game was logged in to the community server (it was locked offline). Failing the CarClub calls
(HTTP 503) did not lock it again. The tab follows the menu state value `IsClubsEnabled`, which the game sets with
`mov byte ptr [rax], 1` at game+0x6F0897 (bytes `C6 00 01`, in the function at game+0x6F05B0 that fills the menu's state
map). The mod patches the immediate byte (game+0x6F0899) to 0, always, expecting the original 1 there; the tab disappears.
`economy_probe` / `economy_scan` in server.ini are diagnostics only: the economy object's default values were not found.

