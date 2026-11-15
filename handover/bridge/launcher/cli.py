"""NX2navi's launcher without its window was launcher/cli.py, and its `update` command finishes an update by running
the NEW version's cli.py (`--root DIR continue-update OLD`). From this version on there is nothing left to finish:
NX2navi is now Triforce, and this version is the handover to it (handover.py). So every command says that and ends
with exit code 0, which lets an `update` started from a console end cleanly.
"""
import sys

TEXT = ('NX2navi is now Triforce: the two projects have merged and go on as Triforce.\n'
        'This version of NX2navi\'s launcher is the handover: start NX2navi (its shortcut or NX2navi.exe) and choose\n'
        'Get Triforce, or get Triforce from https://triforceproject.com\n'
        'Nothing of NX2navi was removed: the built game, its saves and the settings are where they were.')


def main(argv=None):
    print(TEXT, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
