savedcmd_printk_log_levels.mod := printf '%s\n'   printk_log_levels.o | awk '!x[$$0]++ { print("./"$$0) }' > printk_log_levels.mod
