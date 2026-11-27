/* For the tests on Windows: this test program started again as a second
 * process, to hold a file open the way another BRODALF would, or to be ended
 * the way a crash ends one (TerminateProcess). The child says it is ready
 * by writing a line to a file. Include after brodalf.h and internal.h. */
#ifndef BRODALF_TEST_SPAWN_H
#define BRODALF_TEST_SPAWN_H
#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Starts this program again with args (each quoted as needed); NULL if it
 * could not be started. */
static HANDLE spawn_self(const char *args)
{
    wchar_t exe[MAX_PATH + 1];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH + 1);
    if (n == 0 || n > MAX_PATH) return NULL;
    int alen = MultiByteToWideChar(CP_UTF8, 0, args, -1, NULL, 0);
    if (alen <= 0) return NULL;
    wchar_t *cmd = malloc(sizeof(wchar_t) * ((size_t)n + 4 + (size_t)alen));
    if (!cmd) return NULL;
    cmd[0] = L'"';
    memcpy(cmd + 1, exe, sizeof(wchar_t) * n);
    cmd[n + 1] = L'"';
    cmd[n + 2] = L' ';
    MultiByteToWideChar(CP_UTF8, 0, args, -1, cmd + n + 3, alen);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* In the child: says it is ready, with a number for the parent. */
static void child_ready(const char *ready_path, int code)
{
    char *part = bd_sprintf("%s.part", ready_path);
    FILE *f = part ? bd_fopen(part, "wb") : NULL;
    if (f) {
        fprintf(f, "%d\n", code);
        fclose(f);
        bd_rename_replace(part, ready_path);
    }
    free(part);
}

/* In the parent: waits up to ms for the child's line, -1000 if none came
 * (the child ended first, or is stuck). The line's number otherwise. */
static int wait_ready(HANDLE child, const char *ready_path, int ms)
{
    for (int waited = 0; waited < ms; waited += 50) {
        FILE *f = bd_fopen(ready_path, "rb");
        if (f) {
            int code = -1000;
            if (fscanf(f, "%d", &code) != 1) code = -1000;
            fclose(f);
            return code;
        }
        if (WaitForSingleObject(child, 50) == WAIT_OBJECT_0) break;
    }
    return -1000;
}

/* Ends the child at once, as a crash would, and waits until it is gone. */
static void end_child(HANDLE child)
{
    if (!child) return;
    TerminateProcess(child, 99);
    WaitForSingleObject(child, 30000);
    CloseHandle(child);
}

#endif
#endif
