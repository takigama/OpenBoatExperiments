"""
tobe_pio.py - PlatformIO extra script shared by every TOBE firmware.

    extra_scripts = pre:../../../build/scripts/tobe_pio.py        (path relative to the project's platformio.ini)

It looks this project + environment up in build/projects.json and defines, for every file compiled:

    FW_BUILD                 the build number
    TOBE_PROJECT             "EngineControl"
    TOBE_FW_NAME             "EngineControl-CanSim-C3"
    TOBE_OTA_MANIFEST_URL    where the firmware looks for updates (raw.githubusercontent.com)
    TOBE_OTA_DEVICE          the manifest entry to read, "" for a single-firmware manifest
    TOBE_OTA_VARIANT         ... and the variant within it

so a firmware never hard-codes its own name or version, and `pio run` gives exactly what build.sh gives.
An environment that is not in the registry (an experiment) simply builds without these.
"""
import json
import os

Import("env")  # noqa: F821  (provided by PlatformIO's SCons)


def find_registry(start):
    d = os.path.abspath(start)
    while True:
        cand = os.path.join(d, "build", "projects.json")
        if os.path.isfile(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


project_dir = os.path.realpath(env.subst("$PROJECT_DIR"))  # noqa: F821
pioenv = env["PIOENV"]  # noqa: F821
registry_path = find_registry(project_dir)

if registry_path:
    root = os.path.dirname(os.path.dirname(registry_path))
    with open(registry_path) as f:
        reg = json.load(f)

    for name, fw in reg["firmware"].items():
        if os.path.realpath(os.path.join(root, fw["dir"])) != project_dir or fw["env"] != pioenv:
            continue

        ota = fw.get("ota")
        manifest = fw.get("manifest", fw["dir"] + "/ota/manifest.json")
        defines = [
            ("FW_BUILD", int(fw["version"])),
            ("TOBE_PROJECT", env.StringifyMacro(fw.get("project", name))),  # noqa: F821
            ("TOBE_FW_NAME", env.StringifyMacro(name)),  # noqa: F821
        ]
        if isinstance(ota, dict):
            url = "https://raw.githubusercontent.com/%s/%s/%s" % (reg["repo"], reg["branch"], manifest)
            defines += [
                ("TOBE_OTA_MANIFEST_URL", env.StringifyMacro(url)),  # noqa: F821
                ("TOBE_OTA_DEVICE", env.StringifyMacro(ota.get("device", ""))),  # noqa: F821
                ("TOBE_OTA_VARIANT", env.StringifyMacro(ota.get("variant", ""))),  # noqa: F821
            ]
        env.Append(CPPDEFINES=defines)  # noqa: F821
        print("tobe: %s v%s (%s)" % (name, fw["version"], "OTA " + ("on" if isinstance(ota, dict) else "off")))
        break
    else:
        print("tobe: %s [%s] is not in build/projects.json - building without TOBE_* definitions" % (project_dir, pioenv))


# ---------------------------------------------------------------------------------------------------------------
# A "factory" image next to firmware.bin: bootloader + partition table + boot_app0 + application in one file, to
# flash at 0x0 on a board that has never had firmware. Failure here only warns - firmware.bin is what matters.
# ---------------------------------------------------------------------------------------------------------------
import subprocess
import sys


def make_factory_image(source, target, env):  # noqa: F811
    try:
        board = env.BoardConfig()
        mcu = board.get("build.mcu", "esp32")
        flash_size = board.get("upload.flash_size", "4MB")
        flash_mode = board.get("build.flash_mode", "dio")
        flash_freq = str(board.get("build.f_flash", "80000000L")).replace("000000L", "m")
        app = str(target[0])
        out = os.path.join(os.path.dirname(app), "firmware-factory.bin")
        esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")

        parts = []
        for item in env.get("FLASH_EXTRA_IMAGES", []):
            parts += [env.subst(str(item[0])), env.subst(str(item[1]))]
        parts += [env.subst(str(env.get("ESP32_APP_OFFSET", "0x10000"))), app]

        cmd = [sys.executable, esptool, "--chip", mcu, "merge_bin", "-o", out,
               "--flash_mode", flash_mode, "--flash_freq", flash_freq, "--flash_size", flash_size] + parts
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        print("tobe: factory image %s" % out)
    except Exception as exc:  # noqa: BLE001
        print("tobe: warning - could not make the factory image: %s" % exc)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_factory_image)  # noqa: F821
