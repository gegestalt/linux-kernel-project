savedcmd_better_hello.mod := printf '%s\n'   better_hello.o | awk '!x[$$0]++ { print("./"$$0) }' > better_hello.mod
