#
# Regular cron jobs for the unstrung package
#
0 4	* * *	root	[ -x /usr/bin/unstrung_maintenance ] && /usr/bin/unstrung_maintenance
