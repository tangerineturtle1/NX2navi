"""The launcher's sound effects: two of the game's own interface sounds, the ones its menus make for a choice and for
going back, read from the player's game (launcher/gamefiles.py extract_sounds decodes them into data/cache/sounds, as
runtime/menu_sound.c plays them in the in-game menu). No sound ships with the launcher and none is made here: until
the player's game has been read, and for a sound it does not hold, the launcher is silent.

They play at Low until the player picks another loudness, or Off, in Customize Launcher (the Sounds row). An effect
is the game's sound as a 16-bit WAV in memory, scaled to the loudness. Windows plays it with PlaySound from that
memory; elsewhere the WAV goes to a file in the cache folder and the first of pw-play, paplay and aplay found plays
it. One effect sounds at a time: a new one cuts the last. Moving the cursor makes no sound, as it would cut a
choice's sound short every time the pointer lands on a row of the next screen.

    select    a row was chosen, a switch or a choice changed, Play, the end of a build    the game's decide sound
    back      back to the screen before, something could not be done                      the game's cancel sound
"""
import ctypes
import os
import shutil
import struct
import subprocess

import numpy as np

LEVELS = {'off': 0.0, 'low': 0.18, 'medium': 0.4, 'high': 0.75}   # the Sounds choice: the loudest sample's share of full scale
DEFAULT_LEVEL = 'low'   # a new launcher.json's (launcher/settings.py DEFAULTS), also for a value it cannot hold
EFFECTS = ('select', 'back')   # the names play takes: the game's sounds of gamefiles.SOUNDS


def wav_bytes(samples, level, rate, channels):
    """A 16-bit WAV of samples (-1..1, interleaved when there are two channels) at level (0..1 of full scale)."""
    pcm = (np.clip(samples * level, -1, 1) * 32767).astype('<i2').tobytes()
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt '
            + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16)
            + b'data' + struct.pack('<I', len(pcm)) + pcm)


def read_wav(data):
    """A 16-bit PCM WAV as written by gamefiles.pcm_to_wav: (rate, channels, samples in -1..1 with its loudest at 1),
    or None when it is another kind of file."""
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:16] != b'WAVEfmt ' or data[36:40] != b'data':
        return None
    kind, channels, rate, _bytes, _align, bits = struct.unpack_from('<HHIIHH', data, 20)
    size = min(struct.unpack_from('<I', data, 40)[0], len(data) - 44)
    if kind != 1 or bits != 16 or channels not in (1, 2) or size < 2 * channels:
        return None
    a = np.frombuffer(data, dtype='<i2', count=size // 2, offset=44).astype(np.float32)
    peak = float(np.abs(a).max())
    return rate, channels, a / peak if peak > 0 else a


class Sounds:
    """Plays the effects at the level of the Sounds choice. Every failure is silent: a PC without a sound device, or
    without a player program, just has a quiet launcher."""

    def __init__(self, cache_dir, level=DEFAULT_LEVEL, log=None):
        self.cache = cache_dir
        self.log = log or (lambda line: None)
        self.level = None     # set_level below: DEFAULT_LEVEL unless the player chose another
        self.game = {}        # the game's sounds decoded from the player's files: name -> (rate, channels, samples)
        self.wavs = {}        # name -> WAV bytes at the current level (Windows plays from this memory: it must stay)
        self.files = {}       # name -> the WAV file elsewhere
        self.player = None    # elsewhere: the program that plays a WAV file
        self.winmm = None
        self.failed = False
        if os.name == 'nt':
            try:
                self.winmm = ctypes.WinDLL('winmm')
                self.winmm.PlaySoundW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
                self.winmm.PlaySoundW.restype = ctypes.c_int
            except (OSError, AttributeError) as e:
                self.log(f'sounds: {e}')
        else:
            self.player = next((p for p in ('pw-play', 'paplay', 'aplay') if shutil.which(p)), None)
        self.set_level(level)
        self.load_game()

    def load_game(self):
        """Takes up the game's sounds decoded into <cache>/sounds (again after gamefiles.extract_sounds wrote them)."""
        game = {}
        for name in EFFECTS:
            try:
                got = read_wav((self.cache / 'sounds' / f'{name}.wav').read_bytes())
            except OSError:
                got = None
            if got:
                game[name] = got
        if set(game) != set(self.game):
            self.stop()
            self.game, self.wavs, self.files = game, {}, {}
        return bool(game)

    def set_level(self, level):
        level = level if level in LEVELS else DEFAULT_LEVEL
        if level == self.level:
            return
        self.stop()
        self.level = level
        self.wavs, self.files = {}, {}

    def wav(self, name):
        if name not in self.wavs:
            rate, channels, samples = self.game[name]
            self.wavs[name] = wav_bytes(samples, LEVELS[self.level], rate, channels)
        return self.wavs[name]

    def stop(self):
        """Ends what is sounding, before the memory it plays from goes."""
        if self.winmm:
            try:
                self.winmm.PlaySoundW(None, None, 0)
            except OSError:
                pass

    def play(self, name):
        if LEVELS.get(self.level, 0) <= 0 or self.failed or name not in self.game:
            return
        try:
            if self.winmm:
                # SND_ASYNC | SND_NODEFAULT | SND_MEMORY: PlaySound reads the bytes while it plays, and self.wavs
                # keeps them until the level changes (set_level stops the sound first)
                self.winmm.PlaySoundW(self.wav(name), None, 0x0001 | 0x0002 | 0x0004)
            elif self.player:
                if name not in self.files:
                    path = self.cache / f'sound-{name}-{self.level}.wav'
                    path.write_bytes(self.wav(name))
                    self.files[name] = path
                subprocess.Popen([self.player, str(self.files[name])], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        except (OSError, ValueError) as e:   # no device, no player: stay quiet from here on
            self.failed = True
            self.log(f'sounds: {e}')
