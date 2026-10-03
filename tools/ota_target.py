"""PlatformIO targets for a board running MODE=debug (tools/remote.py):
  pio run -t ota     build, then update the board over WiFi
  pio run -t logs    follow the board's log
"""
import os

Import("env")  # noqa: F821

REMOTE = os.path.join(env.subst("$PROJECT_DIR"), "tools", "remote.py")  # noqa: F821
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
