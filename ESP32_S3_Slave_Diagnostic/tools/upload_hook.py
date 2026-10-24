"""PlatformIO Upload uses the native USB recovery path from VS Code and CLI."""
from pathlib import Path
import os
import shlex
import subprocess

Import("env")

project=Path(env.subst("$PROJECT_DIR"))
esptool=env.PioPlatform().get_package_dir("tool-esptoolpy")
command=[env.subst("$PYTHONEXE"),str(project/"tools/usb_upload.py"),"--identity",
         env.GetProjectOption("custom_usb_serial"),"--esptool-dir",esptool,
         "--report",env.subst("$BUILD_DIR/usb_upload.json")]
for address,path in env.get("FLASH_EXTRA_IMAGES",[]):
    command.extend(("--image",str(address),env.subst(path)))
command.extend(("--image",env.subst("$ESP32_APP_OFFSET"),env.subst("$BUILD_DIR/${PROGNAME}.bin")))
env.Replace(UPLOADCMD=subprocess.list2cmdline(command) if os.name=="nt" else shlex.join(command))
