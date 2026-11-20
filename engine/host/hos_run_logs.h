/* (heylisten) logs/ in the game folder: this run's log and the last four as .log files, for RecompLauncher, which attaches the newest
   .log of a declared "logs" folder to a crash report the player sends (recomplauncher.json "folders"). hookshot_log.txt and its
   .1.txt to .4.txt stay as they are, for players and the docs. Each .log is a hard link to the same file, so the run's log is there
   from its first line (a copy made at the end would not exist after a crash) and costs no space. A disk with no hard links (exFAT)
   gets no logs folder; the .txt files are still written. Both systems: the game logs to a terminal when it has one, and started
   without one (a desktop or the launcher) it logs to hookshot_log.txt beside itself, whose opening (host/hos_main.c) calls this.
   hos_run_logs_make(dir, log): dir is the game folder with its trailing separator, log this run's open log file. */
#ifndef HOS_RUN_LOGS_H
#define HOS_RUN_LOGS_H
#include <stdio.h>
#include <windows.h>   /* (on Linux host/posix/windows.h, on the include path: the same calls, made in win32_posix.c) */
#define HOS_RUN_LOGS_KEEP 4   /* (the runs before this one) */

static void hos_run_logs_make(const char *dir, const char *log) {
    char folder[MAX_PATH + 32], from[MAX_PATH + 32], to[MAX_PATH + 32];
    if (snprintf(folder, sizeof folder, "%slogs", dir) >= (int)sizeof folder - 20) return;
    CreateDirectoryA(folder, NULL);   /* (already there is fine) */
    for (int k = HOS_RUN_LOGS_KEEP; k >= 1; k--) {   /* (hookshot.3.log -> .4.log ... hookshot.log -> .1.log; the oldest is replaced) */
        if (k == 1) snprintf(from, sizeof from, "%s/hookshot.log", folder);
        else snprintf(from, sizeof from, "%s/hookshot.%d.log", folder, k - 1);
        snprintf(to, sizeof to, "%s/hookshot.%d.log", folder, k);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    snprintf(to, sizeof to, "%s/hookshot.log", folder);
    DeleteFileA(to);
    CreateHardLinkA(to, log, NULL);
}
#endif
