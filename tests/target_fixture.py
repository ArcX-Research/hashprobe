"""Independent hashlib oracle and deliberately faulty adapter behaviors."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

mode = sys.argv[1]
data = sys.stdin.buffer.read()
digest = hashlib.sha256(data).digest()

if mode == "correct":
    print(digest.hex())
elif mode == "binary":
    sys.stdout.buffer.write(digest)
elif mode == "uppercase":
    print(" \t" + digest.hex().upper() + "\r\n")
elif mode == "zero":
    print("00" * 32)
elif mode == "binary-whitespace":
    sys.stdout.buffer.write(b" " * 32)
elif mode == "malformed":
    print("SHA256=" + digest.hex())
elif mode == "nul-output":
    sys.stdout.buffer.write(b"a" * 63 + b"\0")
elif mode == "exit":
    sys.stderr.buffer.write(b"diagnostic\x00\xff\n")
    print(digest.hex())
    sys.exit(7)
elif mode == "signal":
    os.kill(os.getpid(), signal.SIGTERM)
elif mode == "timeout":
    time.sleep(10)
elif mode == "flood":
    while True:
        os.write(1, b"x" * 4096)
elif mode == "stderr-flood":
    while True:
        os.write(2, b"x" * 4096)
elif mode == "argument":
    assert sys.argv[2] == "literal; $(touch forbidden) `touch forbidden`"
    print(digest.hex())
elif mode == "touch":
    Path(sys.argv[2]).write_text("started")
    print(digest.hex())
elif mode == "wait":
    Path(sys.argv[2]).write_text("started")
    time.sleep(10)
elif mode == "descendant":
    subprocess.Popen([
        sys.executable, "-c",
        "import pathlib,sys,time; time.sleep(0.8); pathlib.Path(sys.argv[1]).write_text('escaped')",
        sys.argv[2],
    ])
else:
    raise ValueError(mode)
