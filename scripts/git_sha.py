# PlatformIO pre-build script: bakes the short git commit into the firmware
# as -DGIT_SHA="<sha>" so the device can tell whether the "latest" web-flash
# build (manifest version "<ver>+<sha>") is the one it is already running.
# Falls back to "unknown" outside a git checkout (e.g. a source tarball);
# the update check then simply always offers "latest".
#
# 7 characters = git's default abbreviation, which is what the Pages workflow
# writes after the "+" in latest/manifest.json; the device compares the two
# by prefix, so a longer abbreviation on either side would still match.
import subprocess

Import("env")  # noqa: F821  (provided by SCons/PlatformIO)

try:
    sha = subprocess.check_output(
        ["git", "rev-parse", "--short=7", "HEAD"],
        cwd=env["PROJECT_DIR"],  # noqa: F821
        stderr=subprocess.DEVNULL,
        text=True,
    ).strip()
except Exception:
    sha = ""

env.Append(CPPDEFINES=[("GIT_SHA", env.StringifyMacro(sha or "unknown"))])  # noqa: F821
