Import("env")

from pathlib import Path
import subprocess

# esptool 5 uses Unicode progress bars. Force UTF-8 for child processes so
# Windows code pages such as cp1252 do not abort long flash operations.
env["ENV"]["PYTHONIOENCODING"] = "utf-8"

# pioarduino installs its Python uploader as an editable package. When an
# overridden tool version is installed for the first time, the old editable
# link can remain in PlatformIO's Python environment. Repair it before any
# bootloader-image or upload action invokes esptool.
python_exe = env.subst("$PYTHONEXE")
platform = env.PioPlatform()
esptool_dir = platform.get_package_dir("tool-esptoolpy")
if not esptool_dir or not (Path(esptool_dir) / "esptool").is_dir():
    core_dir = Path(env.GetProjectConfig().get("platformio", "core_dir"))
    esptool_dir = str(core_dir / "tools" / "tool-esptoolpy")

version_probe = subprocess.run(
    [
        python_exe,
        "-c",
        "import esptool; print(esptool.__version__)",
    ],
    capture_output=True,
    text=True,
)
if version_probe.returncode != 0 or version_probe.stdout.strip() != "5.4.0":
    subprocess.check_call(
        [
            python_exe,
            "-m",
            "pip",
            "install",
            "--quiet",
            "--disable-pip-version-check",
            "--upgrade",
            "-e",
            esptool_dir,
            "click>=8.0.4,<8.4",
        ]
    )

# pioarduino's Windows RISC-V package follows Espressif's nested directory
# layout. PlatformIO's binary-embedding action invokes objcopy by name, so add
# that directory explicitly before embed_files are converted to object files.
toolchain = platform.get_package_dir("toolchain-riscv32-esp")
if toolchain:
    env.PrependENVPath("PATH", env.subst(toolchain + "/riscv32-esp-elf/bin"))
