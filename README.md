errclosefs -- a sample broken filesystem

1) setenforce 0 (or else EPERM on open())

2) exim config: set split_spool_directory = false (no subdirectory support yet)

3) ./errclosefs -o allow_other -s -f -d /var/spool/exim/input
