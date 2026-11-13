"""Check the installed optimizer ABI with a public synthetic compute module."""
import ctypes as c
import hashlib
import json
import os
import sys
from pathlib import Path

libpath = Path(sys.argv[1])
lib = c.CDLL(str(libpath))
libc = c.CDLL("msvcrt" if os.name == "nt" else None)
libc.malloc.argtypes = [c.c_size_t]
libc.malloc.restype = c.c_void_p
libc.free.argtypes = [c.c_void_p]
allocate = c.CFUNCTYPE(c.c_void_p, c.c_size_t)(libc.malloc)
opt = lib.nx2navi_spirv_opt_v1
opt.argtypes = [c.POINTER(c.c_uint32), c.c_size_t, type(allocate),
               c.POINTER(c.POINTER(c.c_uint32)), c.POINTER(c.c_size_t)]
opt.restype = c.c_int

def instruction(op, *args):
    return [((1 + len(args)) << 16) | op, *args]

words = [0x07230203, 0x00010300, 0, 5, 0]
words += instruction(17, 1)                       # Shader capability
words += instruction(14, 0, 1)                    # Logical, GLSL450
words += instruction(15, 5, 3, 0x6e69616d, 0)    # GLCompute, main
words += instruction(16, 3, 17, 1, 1, 1)         # LocalSize
words += instruction(19, 1)                       # void
words += instruction(33, 2, 1)                    # function type
words += instruction(54, 1, 3, 0, 2)
words += instruction(248, 4)
words += instruction(0) * 5000
words += instruction(253)
words += instruction(56)
module = (c.c_uint32 * len(words))(*words)
result = c.POINTER(c.c_uint32)()
count = c.c_size_t()
assert opt(module, len(words), allocate, c.byref(result), c.byref(count)) == 1
assert 5 <= count.value < len(words)
assert result[0] == 0x07230203
output_words = count.value
libc.free(result)
module[0] = 0
assert opt(module, len(words), allocate, c.byref(result), c.byref(count)) == 0
assert not result and count.value == 0
status = {'state': 'passed', 'helper_sha256': hashlib.sha256(libpath.read_bytes()).hexdigest(),
          'synthetic_input_words': len(words), 'optimized_words': output_words,
          'invalid_module_rejected': True, 'game_data_used': False}
print(json.dumps(status))
