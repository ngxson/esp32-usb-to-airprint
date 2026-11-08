"""PlatformIO targets for a board running MODE=debug (tools/remote.py):
  pio run -t ota     build, then update the board over WiFi
  pio run -t logs    follow the board's log
and, after every firmware.bin, the symbol table appended to it and the ELF
kept as .pio/elf/<elf sha256>.elf (tools/gen_symbols.py).
"""
import os
import sys

Import("env")  # noqa: F821

TOOLS = os.path.join(env.subst("$PROJECT_DIR"), "tools")  # noqa: F821
REMOTE = os.path.join(TOOLS, "remote.py")
sys.path.insert(0, TOOLS)
import gen_symbols  # noqa: E402


def add_symbols(source, target, env):
    gen_symbols.cmd_add(env.subst("$BUILD_DIR/${PROGNAME}.elf"), str(target[0]),
                        os.path.join(env.subst("$PROJECT_DIR"), ".pio", "elf"))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", add_symbols)  # noqa: F821
env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions='"$PYTHONEXE" "%s" upload "$BUILD_DIR/${PROGNAME}.bin"' % REMOTE,
    title="OTA upload",
    description="Update the firmware over WiFi (board in MODE=debug)",
)
env.AddCustomTarget(  # noqa: F821
    name="logs",
    dependencies=None,
    actions='"$PYTHONEXE" "%s" log -f' % REMOTE,
    title="Remote log",
    description="Follow the log of a board in MODE=debug",
    always_build=True,
)
