# Game Mounter
**By Manos**

Αυτόματο mounting όλων των games από το `/data/etaHEN/games/` στο PS5 home screen.

---

## 🎯 Τι Κάνει

Το payload σκανάρει όλους τους φακέλους μέσα στο `/data/etaHEN/games/` και για κάθε game:

1. **Διαβάζει το Title ID** από το `param.json` ή `param.sfo`
2. **Κάνει patch το DRM** (αλλάζει `applicationDrmType` σε `standard`)
3. **Κάνει nullfs mount** στο `/system_ex/app/[TITLE_ID]`
4. **Αντιγράφει metadata** (icons, sounds) στο `/user/app/` και `/user/appmeta/`
5. **Καταχωρεί το game** στο PS5 system database
6. **Εμφανίζει το icon** στο home screen

---

## 📂 Δομή Φακέλων

Τα games πρέπει να είναι οργανωμένα έτσι:

```
/data/etaHEN/games/
├── GameName1/
│   ├── sce_sys/
│   │   ├── param.json (ή param.sfo)
│   │   ├── icon0.png
│   │   ├── pic0.png
│   │   └── ...
│   ├── eboot.bin
│   └── [άλλα αρχεία game]
├── GameName2/
│   ├── sce_sys/
│   │   └── ...
│   └── ...
└── GameName3/
    └── ...
```

---

## 🚀 Χρήση

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

Ή χρησιμοποίησε το build script:
```bash
bash build.sh
```

### Εκτέλεση στο PS5:

1. Στείλε το `game_mounter.elf` στο PS5 (π.χ. στο `/data/etaHEN/payloads/`)
2. Εκτέλεσε το payload από το etaHEN menu
3. Περίμενε να ολοκληρωθεί το mounting (θα δεις notification "Game Mounter - By Manos")
4. Τα games θα εμφανιστούν στο home screen!

---

## ⚙️ Τεχνικές Λεπτομέρειες

- **Nullfs Mount**: Δεν αντιγράφει τα games, απλά τα "mirror" - άμεση πρόσβαση
- **DRM Bypass**: Αλλάζει το `applicationDrmType` για να τρέξουν χωρίς license
- **System Registration**: Χρησιμοποιεί `sceAppInstUtilAppInstallTitleDir()` API
- **Database Update**: Ενημερώνει το `/system_data/priv/mms/app.db` για sounds

---

## 📝 Σημειώσεις

- Υποστηρίζει **PS5 games** (param.json) και **PS4 games** (param.sfo)
- Αν ένα game είναι ήδη mounted, το unmount και το ξαναμουντάρει
- Εμφανίζει detailed output στο console για debugging
- Στέλνει PS5 notifications για την πρόοδο

---

## 🔧 Troubleshooting

**Δεν εμφανίζονται τα games:**
- Έλεγξε ότι υπάρχει το `/data/etaHEN/games/` directory
- Έλεγξε ότι κάθε game έχει `sce_sys/param.json` ή `sce_sys/param.sfo`
- Κοίταξε το console output για errors

**"Registration failed" error:**
- Το PS5 system database μπορεί να είναι locked
- Δοκίμασε να κλείσεις άλλα games/apps πριν τρέξεις το payload

---

## 📄 Credits & License

**Created by:** Manos  
**Based on:** dump_runner by John Törnblom  
**SDK:** PS5 Payload SDK

---

## 🌟 Features

- ✅ Automatic game detection and mounting
- ✅ DRM bypass for all games
- ✅ PS5 and PS4 game support
- ✅ Nullfs mounting (no file copying needed)
- ✅ PS5 notifications for progress
- ✅ Detailed console output for debugging
- ✅ Auto-cleanup of deleted games
