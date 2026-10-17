# build.py: compiles fpc_walk.py into fpc_walk.ts4script (a zip with the .pyc), for The Sims 4.
# The game runs Python 3.7, so the bytecode must come from a Python 3.7 interpreter.
# Usage: python build.py C:\path\to\python37\python.exe
import os, subprocess, sys, zipfile

here = os.path.dirname(os.path.abspath(__file__))
py37 = sys.argv[1] if len(sys.argv) > 1 else sys.executable
src = os.path.join(here, "fpc_walk.py")
pyc = os.path.join(here, "build", "fpc_walk.pyc")
os.makedirs(os.path.dirname(pyc), exist_ok=True)
ver = subprocess.check_output([py37, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"], text=True).strip()
if ver != "3.7":
    sys.exit("Python 3.7 is required (got %s)" % ver)
subprocess.check_call([py37, "-c", "import py_compile, sys; py_compile.compile(sys.argv[1], sys.argv[2], doraise=True)", src, pyc])
out = os.path.join(here, "fpc_walk.ts4script")
with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
    z.write(pyc, "fpc_walk.pyc")
print("built", out)
