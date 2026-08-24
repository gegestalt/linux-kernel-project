savedcmd_gpioctrl.mod := printf '%s\n'   gpioctrl.o | awk '!x[$$0]++ { print("./"$$0) }' > gpioctrl.mod
