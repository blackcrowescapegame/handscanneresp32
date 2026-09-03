Import("env")

# esptool 5.4 renders a Unicode progress bar. PlatformIO's Windows output
# reader can still use a legacy code page, so suppress only that progress bar;
# completion and verification messages remain visible.
if env.subst("$UPLOAD_PROTOCOL") == "esptool":
    flags = list(env.get("UPLOADERFLAGS", []))
    try:
        command_index = flags.index("write-flash")
    except ValueError:
        command_index = -1

    if command_index >= 0 and "--no-progress" not in flags:
        flags.insert(command_index + 1, "--no-progress")
        env.Replace(UPLOADERFLAGS=flags)
