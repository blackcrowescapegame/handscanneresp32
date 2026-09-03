Import("env")

# esptool 5.4 renders a Unicode progress bar. PlatformIO's Windows output
# reader can still use a legacy code page, so suppress only that progress bar;
# completion and verification messages remain visible.
protocol = env.subst("$UPLOAD_PROTOCOL")

if protocol == "esptool":
    flags = list(env.get("UPLOADERFLAGS", []))
    try:
        command_index = flags.index("write-flash")
    except ValueError:
        command_index = -1

    if command_index >= 0 and "--no-progress" not in flags:
        flags.insert(command_index + 1, "--no-progress")
        env.Replace(UPLOADERFLAGS=flags)
elif protocol == "espota":
    # pioarduino enables espota debug output by default. Besides producing a
    # very large per-chunk log, that output includes the plaintext OTA auth
    # argument. Keep OTA output quiet and credentials out of the terminal.
    flags = [
        flag
        for flag in list(env.get("UPLOADERFLAGS", []))
        if flag not in ("-d", "--debug", "-r", "--progress")
    ]
    env.Replace(UPLOADERFLAGS=flags)
