"""Native nn::fs path/name conversions, without a game or a UTF-8 process manifest.

A long path with a doubled separator opens too: the game asks for its streamed voices as Voice/Resource//Stream/...,
and Windows tidies a doubled separator only in a path without the extended-length prefix, which fs_wide adds from 240
characters. In a game folder deep enough the voices' files were "not found" and the cutscenes silent (Longshot #1298).
The fixture makes a real folder past 240 characters and opens a file in it both ways.

    python -I -B tests/test_fs_path_utf8.py [--cc <Windows clang>]
"""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    if os.name != "nt":
        print("SKIP: native nn::fs UTF-8 conversion fixture runs on Windows")
        return
    cc = args.cc or shutil.which("clang")
    if not cc:
        raise SystemExit("no Windows C compiler (pass --cc)")
    source = (args.source / "host/nn_fs.c").read_text(encoding="utf-8")
    start = source.index("static const wchar_t *fs_wide(")
    end = source.index("static HANDLE fs_CreateFile(", start)
    actual = source[start:end]
    fixture = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define HOS_FS_PATH 1024
/* ACTUAL_CONVERSIONS */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s at line %d\n", #c, __LINE__); return 1; } } while (0)
/* a real file in a folder past 240 characters, opened as the game names it: one separator, and two before "Stream" */
static int long_path_opens(const char *temp) {
    char dir[HOS_FS_PATH], path[HOS_FS_PATH];
    wchar_t wide[HOS_FS_PATH + 8];
    int at = snprintf(dir, sizeof dir, "%s", temp);
    const char *parts[] = {"romfs", "Voice", "Resource", "Stream"};
    for (int i = 0; i < 6; i++) {   /* (two of 100 characters first: every path below is long) */
        if (i < 2) at += snprintf(dir + at, sizeof dir - (size_t)at, "\\%0100d", i);
        else at += snprintf(dir + at, sizeof dir - (size_t)at, "\\%s", parts[i - 2]);
        CHECK(CreateDirectoryW(fs_wide(dir, wide, HOS_FS_PATH + 8), NULL));
    }
    CHECK(strlen(dir) >= 240);
    snprintf(path, sizeof path, "%s\\voice.bwav", dir);
    HANDLE made = CreateFileW(fs_wide(path, wide, HOS_FS_PATH + 8), GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(made != INVALID_HANDLE_VALUE);
    CloseHandle(made);
    /* (as host_path leaves it: the mount's root, a separator, then the game's "Voice/Resource//Stream/..." with \) */
    dir[strlen(dir) - strlen("\\Stream")] = 0;
    snprintf(path, sizeof path, "%s\\\\Stream\\voice.bwav", dir);
    HANDLE h = CreateFileW(fs_wide(path, wide, HOS_FS_PATH + 8), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    const DWORD error = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (error) fprintf(stderr, "a long path with a doubled separator did not open: error %lu, %ls\n", (unsigned long)error, wide);
    /* (the folders go whatever came of it: Python's own removal does not take paths this long) */
    snprintf(path, sizeof path, "%s\\Stream\\voice.bwav", dir);
    DeleteFileW(fs_wide(path, wide, HOS_FS_PATH + 8));
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "\\Stream");
    for (int i = 0; i < 6; i++) {
        RemoveDirectoryW(fs_wide(dir, wide, HOS_FS_PATH + 8));
        *strrchr(dir, '\\') = 0;
    }
    CHECK(!error);
    return 0;
}
int main(int argc, char **argv) {
    wchar_t wide[HOS_FS_PATH + 8], expected[HOS_FS_PATH + 8];
    const char *leaf = "\xe6\xbc\xa2 \xf0\x9f\x8e\xae \xc3\xa9.bin";
    const wchar_t *wleaf = L"\u6f22 \U0001f3ae \u00e9.bin";
    char path[HOS_FS_PATH];
    CHECK(argc == 2);
    if (long_path_opens(argv[1])) return 1;
    CHECK(!wcscmp(fs_wide("C:\\fixture\\ascii.bin", wide, HOS_FS_PATH + 8), L"C:\\fixture\\ascii.bin"));
    snprintf(path, sizeof path, "C:\\fixture\\%s", leaf);
    swprintf(expected, HOS_FS_PATH + 8, L"C:\\fixture\\%ls", wleaf);
    CHECK(!wcscmp(fs_wide(path, wide, HOS_FS_PATH + 8), expected));
    snprintf(path, sizeof path, "C:\\%0240d\\%s", 0, leaf);
    swprintf(expected, HOS_FS_PATH + 8, L"\\\\?\\C:\\%0240d\\%ls", 0, wleaf);
    CHECK(!wcscmp(fs_wide(path, wide, HOS_FS_PATH + 8), expected));
    snprintf(path, sizeof path, "\\\\server\\share\\%0240d\\%s", 0, leaf);
    swprintf(expected, HOS_FS_PATH + 8, L"\\\\?\\UNC\\server\\share\\%0240d\\%ls", 0, wleaf);
    CHECK(!wcscmp(fs_wide(path, wide, HOS_FS_PATH + 8), expected));
    /* a doubled separator, and the game's / among them: one \ in a long path, after the prefix (Windows takes those as
       they are); a short path is left to Windows, as before */
    snprintf(path, sizeof path, "C:\\%0240d\\Voice\\Resource\\\\Stream//a\\/%s", 0, leaf);
    swprintf(expected, HOS_FS_PATH + 8, L"\\\\?\\C:\\%0240d\\Voice\\Resource\\Stream\\a\\%ls", 0, wleaf);
    CHECK(!wcscmp(fs_wide(path, wide, HOS_FS_PATH + 8), expected));
    snprintf(path, sizeof path, "\\\\server\\share\\\\%0240d\\%s", 0, leaf);
    swprintf(expected, HOS_FS_PATH + 8, L"\\\\?\\UNC\\server\\share\\%0240d\\%ls", 0, wleaf);
    CHECK(!wcscmp(fs_wide(path, wide, HOS_FS_PATH + 8), expected));
    CHECK(!wcscmp(fs_wide("C:\\fixture\\\\Stream/a.bin", wide, HOS_FS_PATH + 8), L"C:\\fixture\\\\Stream/a.bin"));
    WIN32_FIND_DATAW input = {0};
    WIN32_FIND_DATAA output = {0};
    wcscpy(input.cFileName, wleaf);
    wcscpy(input.cAlternateFileName, L"\u00e9~1.bin");
    input.dwFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    input.nFileSizeHigh = 1, input.nFileSizeLow = 42;
    fs_find_data(&input, &output);
    CHECK(!strcmp(output.cFileName, leaf));
    CHECK(!strcmp(output.cAlternateFileName, "\xc3\xa9~1.bin"));
    CHECK(output.dwFileAttributes == input.dwFileAttributes);
    CHECK(output.nFileSizeHigh == input.nFileSizeHigh && output.nFileSizeLow == input.nFileSizeLow);
    printf("PASS: actual nn::fs conversions: ASCII, Unicode, long drive/UNC paths, a doubled separator in one (and a real file opened so), both directory names (ACP %u)\n", GetACP());
    return 0;
}
'''.replace("/* ACTUAL_CONVERSIONS */", actual)
    with tempfile.TemporaryDirectory(prefix="hookshot-fs-utf8-") as temporary:
        path = Path(temporary)
        test, exe = path / "conversions.c", path / "conversions.exe"
        test.write_text(fixture, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(path)], check=True)


if __name__ == "__main__":
    main()
