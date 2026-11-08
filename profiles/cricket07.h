/* Cricket 07 (EA Sports, 2006): build with PROFILE=profiles/cricket07.h (see the README).
 * The executable is whatever your game folder has (default below; override with RC_EXE natively or ?exe= on
 * the page). It must be one without disc copy protection: the retail executable is wrapped by SafeDisc,
 * whose encrypted code can't be translated. */
#ifndef RC_PROFILE_H
#define RC_PROFILE_H

#define PROFILE_EXE        "Cricket07.exe"
#define PROFILE_GUEST_ROOT "C:\\Cricket07"
#define PROFILE_GUEST_EXE  "Cricket07.exe"
#define PROFILE_TITLE      "Cricket 07"

/* Cricket 07's own patches in host/: widescreen (widescreen.c) and remembered Display Options (settings.c) */
#define PROFILE_CRICKET07  1
/* EA .big archives map lazily (host/core.c); needs the memcpy entry hook in profiles/cricket07.patches.json,
 * which build.sh applies along with this profile */
#define PROFILE_LAZY_VIEWS 1
#define PROFILE_CURSOR     0xa01970u   /* the menu cursor (floats x, y in 640x480): taps steer it (input.c) */

/* The game finds its install folder and language here; profiles and saves go under
 * "My Documents/EA SPORTS(TM) Cricket 07" (userdata/). */
#define PROFILE_REGISTRY \
    { "SOFTWARE\\EA SPORTS\\Cricket 07", "Install Dir", 1, PROFILE_GUEST_ROOT, 0 }, \
    { "SOFTWARE\\EA SPORTS\\Cricket 07", "InstallDir", 1, PROFILE_GUEST_ROOT, 0 }, \
    { "SOFTWARE\\EA SPORTS\\Cricket 07", "Language", 1, "English", 0 }, \
    { "SOFTWARE\\EA SPORTS\\Cricket 07", "Locale", 1, "en_uk", 0 }, \
    { "SOFTWARE\\EA SPORTS\\Cricket 07", "Version", 1, "1.0", 0 },

#endif
