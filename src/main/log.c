/*
 * log.c	Logging module.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2000  The FreeRADIUS server project
 * Copyright 2000  Miquel van Smoorenburg <miquels@cistron.nl>
 * Copyright 2000  Alan DeKok <aland@ox.com>
 * Copyright 2000  Chad Miller <cmiller@surfsouth.com>
 */

static const char rcsid[] = "$Id$";

#include	"autoconf.h"
#include	"libradius.h"

#include	<stdlib.h>
#include	<string.h>
#include	<stdarg.h>

#include	"radiusd.h"

#if HAVE_SYSLOG_H
#include	<syslog.h>
#endif

/*
 *	Log the message to the logfile. Include the severity and
 *	a time stamp.
 */
static int do_log(int lvl, const char *fmt, va_list ap)
{
	FILE	*msgfd = NULL;
	const char  *s = ": ";
	unsigned char *p;
	char	buffer[8192];
	time_t	timeval;
	int	len;
#if HAVE_SYSLOG_H
	int	use_syslog = FALSE;
#endif
	if ((lvl & L_CONS) || radlog_dir == NULL || debug_flag) {
		lvl &= ~L_CONS;
		if (!debug_flag) fprintf(stdout, "%s: ", progname);
		vfprintf(stdout, fmt, ap);
		fprintf(stdout, "\n");
	}
	if (radlog_dir == NULL || debug_flag) return 0;

	if (strcmp(radlog_dir, "stdout") == 0) {
		msgfd = stdout;

#if HAVE_SYSLOG_H
	} else if (strcmp(radlog_dir, "syslog") == 0) {
		use_syslog = TRUE;
#endif
	} else {
		sprintf(buffer, "%.1000s/%.1000s", radlog_dir, RADIUS_LOG);
		if ((msgfd = fopen(buffer, "a")) == NULL) {
			fprintf(stderr, "%s: Couldn't open %s for logging: %s\n",
				progname, buffer, strerror(errno));
			return -1;
		}
	}

	timeval = time(NULL);
#if HAVE_SYSLOG_H
	if (use_syslog)
		*buffer = '\0';
	else {
		strcpy(buffer, ctime(&timeval));

		switch(lvl) {
			case L_DBG:
			  s = ": Debug: ";
			  break;
			case L_AUTH:
			  s = ": Auth: ";
			  break;
			case L_PROXY:
			  s = ": Proxy: ";
			  break;
			case L_INFO:
			  s = ": Info: ";
			  break;
			case L_ERR:
			  s = ": Error: ";
			  break;
		}
		strcat(buffer, s);
	}
#endif
	len = strlen(buffer);

#ifdef HAVE_VSNPRINTF
	vsnprintf(buffer + len, sizeof(buffer) - len -1, fmt, ap);
#else
	vsprintf(buffer + len, fmt, ap);
	if (strlen(buffer) >= sizeof(buffer) - 1)
		/* What can we do? */
		_exit(42);
#endif

	/*
	 *	Filter out characters not in Latin-1.
	 */
	for (p = (unsigned char *)buffer; *p != '\0'; p++) {
		if (*p == '\r' || *p == '\n')
			*p = ' ';
		else if (*p < 32 || (*p >= 128 && *p <= 160))
			*p = '?';
	}
	strcat(buffer, "\n");

#if HAVE_SYSLOG_H
	if (!use_syslog) {
		fputs(buffer, msgfd);
#endif
		if (msgfd != stdout)
			fclose(msgfd);
		else
			fflush(stdout);
#if HAVE_SYSLOG_H
	} else {
	  switch(lvl) {
	  	case L_DBG:
			lvl = LOG_DEBUG;
			break;
		case L_AUTH:
			lvl = LOG_NOTICE;
			break;
		case L_PROXY:
			lvl = LOG_NOTICE;
			break;
		case L_INFO:
			lvl = LOG_INFO;
			break;
		case L_ERR:
			lvl = LOG_ERR;
			break;
	  }
	  syslog(lvl, "%s", buffer);
	}
#endif

	return 0;
}

int log_debug(const char *msg, ...)
{
	va_list ap;
	int r;

	va_start(ap, msg);
	r = do_log(L_DBG, msg, ap);
	va_end(ap);

	return r;
}

int radlog(int lvl, const char *msg, ...)
{
	va_list ap;
	int r;

	va_start(ap, msg);
	r = do_log(lvl, msg, ap);
	va_end(ap);

	return r;
}

