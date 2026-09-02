Import("env")

# pioarduino's Windows RISC-V package follows Espressif's nested directory
# layout. PlatformIO's binary-embedding action invokes objcopy by name, so add
# that directory explicitly before embed_files are converted to object files.
toolchain = env.PioPlatform().get_package_dir("toolchain-riscv32-esp")
if toolchain:
    env.PrependENVPath("PATH", env.subst(toolchain + "/riscv32-esp-elf/bin"))

