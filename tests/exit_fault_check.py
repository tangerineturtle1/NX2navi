"""Run the game executable's crash probes and check what a fault does to the way the process ends (runtime/main.c
crash_handler, mode_done).

  - --probe-crash-call: a fault inside a library while the mode still runs is a crash: a FATAL line that names the
    library, exit code 71.
  - --probe-crash-exit: the same fault after the mode has returned, while the process is ending (an atexit handler here;
    on a player's PC the detach of a library another program put into the process). The work is done by then, so the
    process ends with the mode's own code, 3, and one "exit:" line, with no FATAL.

No game files, no GPU, no window.

Usage: exit_fault_check.py PATH_TO_GAME_EXE
"""
import subprocess
import sys

CRASH_EXIT = 71
PROBE_EXIT = 3


def run(exe, mode):
    p = subprocess.run([exe, mode], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                       timeout=120)
    return p.returncode, p.stdout.splitlines()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    code, lines = run(sys.argv[1], '--probe-crash-call')
    fatal = [ln for ln in lines if ln.startswith('FATAL: ')]
    check(code == CRASH_EXIT, '--probe-crash-call: exit code %d, expected %d' % (code, CRASH_EXIT))
    check(len(fatal) == 1 and 'access violation' in fatal[0], '--probe-crash-call: %d FATAL lines, expected one: %s'
          % (len(fatal), fatal))
    check(not any('exit: ' in ln and 'while the process was ending' in ln for ln in lines),
          '--probe-crash-call: a fault inside the mode was taken for one at the exit')

    code, lines = run(sys.argv[1], '--probe-crash-exit')
    fatal = [ln for ln in lines if ln.startswith('FATAL: ')]
    said = [ln for ln in lines if 'exit: ' in ln and 'while the process was ending' in ln]
    check(any(ln.startswith('probe-crash-exit: returning') for ln in lines), '--probe-crash-exit: the mode did not run')
    check(code == PROBE_EXIT, '--probe-crash-exit: exit code %d, expected the mode\'s own %d' % (code, PROBE_EXIT))
    check(not fatal, '--probe-crash-exit: a FATAL line for a fault after the work was done: %s' % fatal)
    check(len(said) == 1 and 'with its own code %d' % PROBE_EXIT in said[0],
          '--probe-crash-exit: %d "exit:" lines, expected one that names the code: %s' % (len(said), said))

    for f in fails:
        print('exit_fault_check: FAIL:', f)
    if not fails:
        print('exit_fault_check: passed (a fault in the mode: FATAL and %d; a fault at the exit: the mode\'s code %d)'
              % (CRASH_EXIT, PROBE_EXIT))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
