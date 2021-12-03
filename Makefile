CC      = gcc
RM      = rm -f
CP      = cp -f
CFLAGS := -g0 -Wall
LDFLAGS := -lsystemd
LIBRELEASE = v4.8.0

default: interhapd

interhapd: interhapd.c
	$(CC) $(CFLAGS) -o interhapd interhapd.c $(LDFLAGS)

install:
	(systemctl stop    interhapd ; exit 0)
	(systemctl disable interhapd ; exit 0)
	($(CP) ./interhapd         /usr/local/sbin/ ; exit 0)
	($(CP) ./interhapd.py      /usr/local/sbin/ ; exit 0)
	($(CP) ./interhapd.service /etc/systemd/system/ ; exit 0)
	($(CP) ./interhapd_default /etc/default/interhapd ; exit 0)
	systemctl daemon-reload
	systemctl enable  interhapd
	systemctl start   interhapd
	systemctl status  interhapd --no-pager

remove:
	(systemctl stop    interhapd ; exit 0)
	(systemctl disable interhapd ; exit 0)
	$(RM) /usr/local/sbin/interhapd.py
	$(RM) /etc/systemd/system/interhapd.service
	$(RM) /usr/local/sbin/interhapd
	$(RM) /etc/default/interhapd

clean:
	$(RM) interhapd

