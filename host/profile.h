/* Per-game settings for the Windows layer: where the game believes it is installed and what it finds
 * in the registry. This default suits a game that only needs its own folder. To use your own, copy
 * this file and build with PROFILE=/path/to/your_profile.h (see build.sh).
 */
#ifndef RC_PROFILE_H
#define RC_PROFILE_H

#define PROFILE_EXE        "game.exe"     /* the executable, relative to the game folder */
#define PROFILE_GUEST_ROOT "C:\\Game"     /* the install folder, as the game sees it */
#define PROFILE_GUEST_EXE  "game.exe"     /* the executable's name, as the game sees it */
#define PROFILE_TITLE      "Game"         /* native window title */

/* Extra registry values, appended to the built-in ones in host/misc.c:
 *   { key, value, type (1 = REG_SZ, 4 = REG_DWORD), string data, dword data },
 * e.g. { "SOFTWARE\\Vendor\\Game", "InstallDir", 1, PROFILE_GUEST_ROOT, 0 }, */
#define PROFILE_REGISTRY

#endif
