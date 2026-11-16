/* NX2navi.exe: the launcher's own program, in the launcher folder (launcher/paths.py).

   1. A Python staged by an update (python.next) replaces python\ while nothing runs from it.
   2. The release named by app\current.txt starts: python\pythonw.exe app\<version>\launcher\start.pyw with this
      program's arguments, the launcher folder as working folder and in NX2NAVI_LAUNCHER_ROOT.
   3. When that release fails to start (exit code 86 from start.pyw, or any non-zero exit within six seconds) and
      app\previous.txt names another complete release, current.txt goes back to it and it starts with
      --rolled-back <version>.

   --launch TARGET keeps the caller's working folder and returns the game's exit code without updating the current
   release on failure. Its own commands: --desktop-shortcut on|off and --shortcuts on|off (Desktop and Start menu).
   No console window (-mwindows). */
#include "common.h"

#define STARTUP_FAILED 86
#define QUICK_MS 6000

static void message(const wchar_t *text)
{
    MessageBoxW(NULL, text, APP_NAME, MB_OK | MB_ICONERROR);
}

static int release_ok(const wchar_t *root, const wchar_t *ver)
{
    wchar_t p[PATHN];
    swprintf(p, PATHN, L"%ls\\app\\%ls\\launcher\\start.pyw", root, ver);
    return ver[0] && path_exists(p);
}

static void swap_python(const wchar_t *root)
{
    wchar_t next[PATHN], cur[PATHN], old[PATHN];
    swprintf(next, PATHN, L"%ls\\python.next", root);
    if (!is_dir(next)) return;
    swprintf(cur, PATHN, L"%ls\\python", root);
    swprintf(old, PATHN, L"%ls\\python.old-%lu", root, (unsigned long)GetTickCount());
    if (is_dir(cur) && !MoveFileExW(cur, old, 0)) return;   /* in use: try again at the next start */
    if (!MoveFileExW(next, cur, 0)) MoveFileExW(old, cur, 0);
}

/* Start the release. Ordinary starts return -1 if still running after QUICK_MS. */
static long start_release(const wchar_t *root, const wchar_t *ver, const wchar_t *args, const wchar_t *extra, int direct)
{
    static wchar_t cmd[32768];
    wchar_t python[PATHN];
    swprintf(python, PATHN, L"%ls\\python\\pythonw.exe", root);
    if (!path_exists(python)) {
        message(L"The launcher's Python is missing. Run NX2navi-Setup.exe again to repair the installation.");
        return 1;
    }
    swprintf(cmd, 32768, L"\"%ls\" \"%ls\\app\\%ls\\launcher\\start.pyw\" %ls %ls", python, root, ver, args, extra);
    SetEnvironmentVariableW(L"NX2NAVI_LAUNCHER_ROOT", root);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(python, cmd, NULL, NULL, FALSE, 0, NULL, direct ? NULL : root, &si, &pi)) {
        message(L"The launcher could not be started. Run NX2navi-Setup.exe again to repair the installation.");
        return 1;
    }
    CloseHandle(pi.hThread);
    DWORD code = 0;
    long result = -1;
    if (WaitForSingleObject(pi.hProcess, direct ? INFINITE : QUICK_MS) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code))
        result = (long)code;
    CloseHandle(pi.hProcess);
    return result;
}

static int shortcut_command(const wchar_t *root, int argc, wchar_t **argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        int on = !wcscmp(argv[i + 1], L"on");
        if (!wcscmp(argv[i], L"--desktop-shortcut")) return set_shortcut(root, 1, on) ? 0 : 1;
        if (!wcscmp(argv[i], L"--shortcuts")) {
            int a = set_shortcut(root, 1, on), b = set_shortcut(root, 0, on);
            return a && b ? 0 : 1;
        }
    }
    return -1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show)
{
    wchar_t root[PATHN], file[PATHN], ver[128], prev[128], extra[160];
    (void)instance; (void)previous; (void)show;
    exe_dir(root, PATHN);
    if (!root[0]) return 1;
    int argc = 0, direct = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int r = shortcut_command(root, argc, argv);
        for (int i = 1; i < argc && wcscmp(argv[i], L"--"); i++)
            if (!wcscmp(argv[i], L"--launch") || !wcsncmp(argv[i], L"--launch=", 9)) direct = 1;
        LocalFree(argv);
        if (r >= 0) return r;
    }
    swap_python(root);
    swprintf(file, PATHN, L"%ls\\app\\current.txt", root);
    if (!read_line(file, ver, 128) || !release_ok(root, ver)) {
        swprintf(file, PATHN, L"%ls\\app\\previous.txt", root);
        if (!read_line(file, ver, 128) || !release_ok(root, ver)) {
            message(L"NX2navi's files are incomplete. Run NX2navi-Setup.exe again to repair the installation.");
            return 1;
        }
    }
    long code = start_release(root, ver, args ? args : L"", L"", direct);
    if (direct) return (int)code;
    if (code == -1 || code == 0) return 0;
    swprintf(file, PATHN, L"%ls\\app\\previous.txt", root);
    if ((code == STARTUP_FAILED || code > 0) && read_line(file, prev, 128) && wcscmp(prev, ver) &&
        release_ok(root, prev)) {
        swprintf(file, PATHN, L"%ls\\app\\current.txt", root);
        write_line(file, prev);
        swprintf(file, PATHN, L"%ls\\app\\previous.txt", root);
        DeleteFileW(file);
        swprintf(extra, 160, L"--rolled-back %ls", ver);
        start_release(root, prev, args ? args : L"", extra, 0);
        return 0;
    }
    if (code == STARTUP_FAILED) message(L"NX2navi could not start. data\\logs\\crash.log in its folder says why.");
    return (int)code;
}
