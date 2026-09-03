Import("env")

# Keep OTA authentication out of debug logs while leaving both USB and OTA
# progress animations visible in the VS Code terminal.
protocol = env.subst("$UPLOAD_PROTOCOL")

if protocol == "espota":
    # pioarduino enables espota debug output by default. Besides producing a
    # very large per-chunk log, that output includes the plaintext OTA auth
    # argument. Disable debug output but retain the safe progress animation.
    flags = [
        flag
        for flag in list(env.get("UPLOADERFLAGS", []))
        if flag not in ("-d", "--debug")
    ]
    env.Replace(UPLOADERFLAGS=flags)
