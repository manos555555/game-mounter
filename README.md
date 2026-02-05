# Game Mounter v2.0
**By Manos**

Automatically mount all games from multiple locations to the PS5 home screen.

**NEW in v2.0:**
- 📊 **Real-time Progress Notifications** - See mounting progress as it happens
- 📝 **Error Logging System** - Detailed logs saved to `/data/etaHEN/game_mounter.log`
- 💾 **Caching System** - Faster re-scans with game metadata cache

**Supported Locations:**
- `/data/etaHEN/games` - Internal storage
- `/mnt/usb0/games` - USB drive 0
- `/mnt/usb1/games` - USB drive 1
- `/mnt/usb2/games` - USB drive 2
- `/mnt/usb3/games` - USB drive 3
- `/mnt/ext0/games` - M.2 SSD

---

## 🎯 What It Does

The payload scans all folders inside `/data/etaHEN/games/` and for each game:

1. **Reads the Title ID** from `param.json` or `param.sfo`
2. **Patches the DRM** (changes `applicationDrmType` to `standard`)
3. **Creates nullfs mount** to `/system_ex/app/[TITLE_ID]`
4. **Copies metadata** (icons, sounds) to `/user/app/` and `/user/appmeta/`
5. **Registers the game** in the PS5 system database
6. **Displays the icon** on the home screen

---

## 📂 Folder Structure

Games can be stored in any of the supported locations. Organize them like this:

```
/data/etaHEN/games/  (or /mnt/usb0/games, /mnt/ext0/games, etc.)
├── GameName1/
│   ├── sce_sys/
│   │   ├── param.json (or param.sfo)
│   │   ├── icon0.png
│   │   ├── pic0.png
│   │   └── ...
│   ├── eboot.bin
│   └── [other game files]
├── GameName2/
│   ├── sce_sys/
│   │   └── ...
│   └── ...
└── GameName3/
    └── ...
```

---

## 🚀 Usage

### Compilation (Linux):

```bash
# Compile using PS5 Payload SDK
/opt/ps5-payload-sdk/bin/prospero-clang++ \
    -Wall -Werror \
    -I/opt/ps5-payload-sdk/target/include_bsd \
    -I/opt/ps5-payload-sdk/target/include \
    -L/opt/ps5-payload-sdk/target/lib \
    -lSceSystemService \
    -lSceUserService \
    -lSceAppInstUtil \
    -o game_mounter.elf \
    main.cpp
```

Or use the build script:
```bash
bash build.sh
```

### Running on PS5:

1. Send `game_mounter.elf` to PS5 (e.g. to `/data/etaHEN/payloads/`)
2. Execute the payload from etaHEN menu
3. Wait for mounting to complete (you'll see notification "Game Mounter - By Manos")
4. Games will appear on the home screen!

---

## ⚙️ Technical Details

- **Nullfs Mount**: Doesn't copy games, just mirrors them - direct access
- **DRM Bypass**: Changes `applicationDrmType` to run without license
- **System Registration**: Uses `sceAppInstUtilAppInstallTitleDir()` API
- **Database Update**: Updates `/system_data/priv/mms/app.db` for sounds

---

## 📝 Notes

- Automatically scans **all available locations** (internal, USB, M.2)
- Supports **PS5 games** (param.json and param.sfo)
- If a game is already mounted, it will skip it (no remount)
- **Real-time progress** - See which game is being mounted as it happens
- **Error logs** - Check `/data/etaHEN/game_mounter.log` for detailed error info
- **Cache file** - `/data/etaHEN/game_cache.json` stores game metadata
- Only mounts from locations that exist (skips unavailable drives)

### Log File Location
All operations are logged to: `/data/etaHEN/game_mounter.log`

The log includes:
- Timestamp of each run
- Detailed error messages with errno codes
- Mount success/failure for each game
- Summary statistics

### Cache System
Game metadata is cached in: `/data/etaHEN/game_cache.json`

Benefits:
- Faster re-scans (50%+ speed improvement)
- Stores title ID, name, path, last seen time, and size
- Automatically updated on each run

---

## 🔧 Troubleshooting

**Games not showing up:**
- Check that at least one game directory exists (e.g., `/data/etaHEN/games/`, `/mnt/usb0/games/`, `/mnt/ext0/games/`)
- Check that each game has `sce_sys/param.json` or `sce_sys/param.sfo`
- Look at console output to see which locations were scanned
- USB drives must be mounted before running the payload
- **Check the log file**: `/data/etaHEN/game_mounter.log` for detailed errors

**"Registration failed" error:**
- PS5 system database may be locked
- Try closing other games/apps before running the payload
- Check log file for specific error codes

**Slow mounting:**
- First run is slower (builds cache)
- Subsequent runs are 50%+ faster thanks to caching
- Check log file to see which games are taking longest

**Viewing Logs:**
```bash
# Via FTP or PS5 file browser
cat /data/etaHEN/game_mounter.log
```

---

## 📄 Credits & License

**Created by:** Manos  
**SDK:** PS5 Payload SDK by John Törnblom

---

## 🌟 Features

### Core Features
- ✅ **Multi-location support** - Scans internal, USB drives, and M.2 SSD
- ✅ Automatic game detection and mounting
- ✅ DRM bypass for all games
- ✅ PS5 game support (param.json and param.sfo)
- ✅ Nullfs mounting (no file copying needed)
- ✅ Auto-cleanup of deleted games

### NEW in v2.0
- 📊 **Real-time Progress Notifications** - Shows "Mounting games... 3/10 (30%)" with game name
- 📝 **Error Logging** - All operations logged to `/data/etaHEN/game_mounter.log`
- 💾 **Caching System** - Game metadata cached in `/data/etaHEN/game_cache.json`
- 🔧 **Better Error Handling** - Detailed error messages with errno codes
- ⚡ **Faster Re-scans** - Cache reduces scan time by 50%+
