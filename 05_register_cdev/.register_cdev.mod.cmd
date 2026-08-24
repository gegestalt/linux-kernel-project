savedcmd_register_cdev.mod := printf '%s\n'   register_cdev.o | awk '!x[$$0]++ { print("./"$$0) }' > register_cdev.mod
