/*
 * rlm_radutmp.c	
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
 * FIXME add copyrights
 */

#include	"autoconf.h"

#include	<sys/types.h>
#include	<stdio.h>
#include	<string.h>
#include	<unistd.h>
#include	<fcntl.h>
#include	<time.h>
#include	<errno.h>
#include        <limits.h>

#include "config.h"

#if HAVE_MALLOC_H
#  include <malloc.h>
#endif

#include	"radiusd.h"
#include	"radutmp.h"
#include	"modules.h"

#define LOCK_LEN sizeof(struct radutmp)

static char porttypes[] = "ASITX";

/*
 *	used for caching radutmp lookups in the accounting component. The
 *	session (checksimul) component doesn't use it, but probably should.
 */
typedef struct nas_port {
	UINT4			nasaddr;
	int			port;
	off_t			offset;
	struct nas_port 	*next;
} NAS_PORT;

struct acct_instance {
  NAS_PORT *nas_port_list;
  char radutmp_fn[PATH_MAX];
  int permission;
  int callerid_ok:1;
};

struct sess_instance {
  char radutmp_fn[PATH_MAX];
};


/* RADDB/modules syntax for radutmp accounting:
 *  accounting rlm_radutmp.so [filename] [permission] [option...]
 * filename defaults to the old location (usually /var/log/radutmp)
 * permission defaults to 644, but the umask radiusd was started with applies
 * current options are "callerid" and "nocallerid". default is nocallerid
 * unless a restrictive permission is specified.
 *
 * For session component:
 *  session rlm_radutmp.so [filename] */
static int radutmp_instantiate(int component, int argc, char **argv,
			       void **instance)
{
	switch(component) {
		case RLM_COMPONENT_ACCT:
		{
			const char *fn=RADUTMP;
			unsigned int perm=0644;
			int callerid=-1;

			if(argc) {
				fn=argv[0];
				--argc,++argv;
			}
			if(argc) {
				if(sscanf(argv[0], "%o", &perm)!=1) {
					log(L_ERR, "Invalid permission %s",
					    argv[0]);
				}
				--argc,++argv;
			}
			while(argc) {
				if(!strcmp(argv[0], "callerid")) {
					callerid=1;
				} else if(!strcmp(argv[0], "nocallerid")) {
					callerid=0;
				} else {
					log(L_ERR, "Unknown option %s",
					    argv[0]);
					return -1;
				}
				--argc,++argv;
			}

			if(callerid==-1)
				callerid = !(perm & 004);

			*instance = malloc(sizeof(struct acct_instance));
			if (!*instance) {
				log(L_ERR|L_CONS, "Out of memory\n");
				return -1;
			}
#define ru_instance ((struct acct_instance *)(*instance))
			ru_instance->nas_port_list=0;
			if(*fn=='/')
				strNcpy(ru_instance->radutmp_fn, fn, PATH_MAX);
			else
				snprintf(ru_instance->radutmp_fn, PATH_MAX,
					 "%s/%s", LOGDIR, fn);
			ru_instance->permission=perm;
			ru_instance->callerid_ok=callerid;
#undef ru_instance
		}
			break;
		case RLM_COMPONENT_SESS:
		{
			const char *fn=RADUTMP;

			if(argc) {
				fn=argv[0];
				--argc,++argv;
			}
			if(argc) {
				log(L_ERR,
				    "Too many args in radutmp session config\n");
				return -1;
			}

			*instance = malloc(sizeof(struct sess_instance));
			if (!*instance) {
				log(L_ERR|L_CONS, "Out of memory\n");
				return -1;
			}
#define ru_instance ((struct sess_instance *)(*instance))
			if(*fn=='/')
				strNcpy(ru_instance->radutmp_fn, fn, PATH_MAX);
			else
				snprintf(ru_instance->radutmp_fn, PATH_MAX,
					 "%s/%s", LOGDIR, fn);
#undef ru_instance
		}
			break;
		default:
			*instance=0;
			break;
	}
	return 0;
}


/*
 *	Detach.
 */
static int radutmp_detach(int component, void *instance)
{
	switch(component) {
		case RLM_COMPONENT_ACCT:
		{
			NAS_PORT *p, *next;
			struct acct_instance *inst = instance;

			for(p=inst->nas_port_list ; p ; p=next) {
				next=p->next;
				free(p);
			}
			free(instance);
		}
			break;
		case RLM_COMPONENT_SESS:
			free(instance);
			break;
	}
	return 0;
}

/*
 *	Zap all users on a NAS from the radutmp file.
 */
static int radutmp_zap(struct acct_instance *inst, UINT4 nasaddr, time_t t)
{
	struct radutmp	u;
	int		fd;

	if (t == 0) time(&t);

	fd = open(inst->radutmp_fn, O_RDWR);
	if (fd >= 0) {
		/*
		 *	Lock the utmp file, prefer lockf() over flock().
		 */
#if defined(F_LOCK) && !defined(BSD)
		(void)lockf(fd, F_LOCK, LOCK_LEN);
#else
		(void)flock(fd, LOCK_EX);
#endif
		/*
		 *	Find the entry for this NAS / portno combination.
		 */
		while (read(fd, &u, sizeof(u)) == sizeof(u)) {
			if ((nasaddr != 0 && nasaddr != u.nas_address) ||
			      u.type != P_LOGIN)
				continue;
			/*
			 *	Match. Zap it.
			 */
			if (lseek(fd, -(off_t)sizeof(u), SEEK_CUR) < 0) {
				log(L_ERR, "Accounting: radutmp_zap: "
					   "negative lseek!\n");
				lseek(fd, (off_t)0, SEEK_SET);
			}
			u.type = P_IDLE;
			u.time = t;
			write(fd, &u, sizeof(u));
		}
		close(fd);
	}

	return 0;
}

/*
 *	Lookup a NAS_PORT in the nas_port_list
 */
static NAS_PORT *nas_port_find(NAS_PORT *nas_port_list, UINT4 nasaddr, int port)
{
	NAS_PORT	*cl;

	for(cl = nas_port_list; cl; cl = cl->next)
		if (nasaddr == cl->nasaddr &&
			port == cl->port)
			break;
	return cl;
}


/*
 *	Store logins in the RADIUS utmp file.
 */
static int radutmp_accounting(void *instance, REQUEST *request)
{
	struct radutmp	ut, u;
	VALUE_PAIR	*vp;
	int		rb_record = 0;
	int		status = -1;
	int		nas_address = 0;
	int		framed_address = 0;
	int		protocol = -1;
	time_t		t;
	int		fd;
	int		just_an_update = 0;
	int		port_seen = 0;
	int		nas_port_type = 0;
	int		off;
	struct acct_instance *inst = instance;

	/*
	 *	Which type is this.
	 */
	if ((vp = pairfind(request->packet->vps, PW_ACCT_STATUS_TYPE)) == NULL) {
		log(L_ERR, "Accounting: no Accounting-Status-Type record.");
		return RLM_ACCT_FAIL;
	}
	status = vp->lvalue;
	if (status == PW_STATUS_ACCOUNTING_ON ||
	    status == PW_STATUS_ACCOUNTING_OFF) rb_record = 1;

	if (!rb_record &&
	    (vp = pairfind(request->packet->vps, PW_USER_NAME)) == NULL) do {
		int check1 = 0;
		int check2 = 0;

		/*
		 *	ComOS (up to and including 3.5.1b20) does not send
		 *	standard PW_STATUS_ACCOUNTING_XXX messages.
		 *
		 *	Check for:  o no Acct-Session-Time, or time of 0
		 *		    o Acct-Session-Id of "00000000".
		 *
		 *	We could also check for NAS-Port, that attribute
		 *	should NOT be present (but we don't right now).
		 */
		if ((vp = pairfind(request->packet->vps, PW_ACCT_SESSION_TIME))
		     == NULL || vp->lvalue == 0)
			check1 = 1;
		if ((vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID))
		     != NULL && vp->length == 8 &&
		     memcmp(vp->strvalue, "00000000", 8) == 0)
			check2 = 1;
		if (check1 == 0 || check2 == 0) {
#if 0 /* Cisco sometimes sends START records without username. */
			log(L_ERR, "Accounting: no username in record");
			return RLM_ACCT_FAIL;
#else
			break;
#endif
		}
		log(L_INFO, "Accounting: converting reboot records.");
		if (status == PW_STATUS_STOP)
			status = PW_STATUS_ACCOUNTING_OFF;
		if (status == PW_STATUS_START)
			status = PW_STATUS_ACCOUNTING_ON;
		rb_record = 1;
	} while(0);

	time(&t);
	memset(&ut, 0, sizeof(ut));
	ut.porttype = 'A';

	/*
	 *	First, find the interesting attributes.
	 */
	for (vp = request->packet->vps; vp; vp = vp->next) {
		switch (vp->attribute) {
			case PW_USER_NAME:
				strncpy(ut.login, vp->strvalue, RUT_NAMESIZE);
				break;
			case PW_LOGIN_IP_HOST:
			case PW_FRAMED_IP_ADDRESS:
				framed_address = vp->lvalue;
				ut.framed_address = vp->lvalue;
				break;
			case PW_FRAMED_PROTOCOL:
				protocol = vp->lvalue;
				break;
			case PW_NAS_IP_ADDRESS:
				nas_address = vp->lvalue;
				ut.nas_address = vp->lvalue;
				break;
			case PW_NAS_PORT_ID:
				ut.nas_port = vp->lvalue;
				port_seen = 1;
				break;
			case PW_ACCT_DELAY_TIME:
				ut.delay = vp->lvalue;
				break;
			case PW_ACCT_SESSION_ID:
				/*
				 *	If length > 8, only store the
				 *	last 8 bytes.
				 */
				off = vp->length - sizeof(ut.session_id);
				/*
				 * 	Ascend is br0ken - it adds a \0
				 * 	to the end of any string.
				 * 	Compensate.
				 */
				if (vp->length > 0 &&
				    vp->strvalue[vp->length - 1] == 0)
					off--;
				if (off < 0) off = 0;
				memcpy(ut.session_id, vp->strvalue + off,
					sizeof(ut.session_id));
				break;
			case PW_NAS_PORT_TYPE:
				if (vp->lvalue >= 0 && vp->lvalue <= 4)
					ut.porttype = porttypes[vp->lvalue];
				nas_port_type = vp->lvalue;
				break;
			case PW_CALLING_STATION_ID:
				if(inst->callerid_ok)
					strNcpy(ut.caller_id, vp->strvalue,
						sizeof(ut.caller_id));
				break;
		}
	}

	/*
	 *	If we didn't find out the NAS address, use the
	 *	originator's IP address.
	 */
	if (nas_address == 0) {
		nas_address = request->packet->src_ipaddr;
		ut.nas_address = nas_address;
	}

	if (protocol == PW_PPP)
		ut.proto = 'P';
	else if (protocol == PW_SLIP)
		ut.proto = 'S';
	else
		ut.proto = 'T';
	ut.time = t - ut.delay;

	/*
	 *	See if this was a portmaster reboot.
	 */
	if (status == PW_STATUS_ACCOUNTING_ON && nas_address) {
		log(L_INFO, "NAS %s restarted (Accounting-On packet seen)",
			nas_name(nas_address));
		radutmp_zap(inst, nas_address, ut.time);
		return RLM_ACCT_OK;
	}
	if (status == PW_STATUS_ACCOUNTING_OFF && nas_address) {
		log(L_INFO, "NAS %s rebooted (Accounting-Off packet seen)",
			nas_name(nas_address));
		radutmp_zap(inst, nas_address, ut.time);
		return RLM_ACCT_OK;
	}

	/*
	 *	If we don't know this type of entry pretend we succeeded.
	 */
	if (status != PW_STATUS_START &&
	    status != PW_STATUS_STOP &&
	    status != PW_STATUS_ALIVE) {
		log(L_ERR, "NAS %s port %d unknown packet type %d)",
			nas_name(nas_address), ut.nas_port, status);
		return RLM_ACCT_FAIL_SOFT;
	}

	/*
	 *	Perhaps we don't want to store this record into
	 *	radutmp. We skip records:
	 *
	 *	- without a NAS-Port-Id (telnet / tcp access)
	 *	- with the username "!root" (console admin login)
	 */
	if (!port_seen || strncmp(ut.login, "!root", RUT_NAMESIZE) == 0)
		return RLM_ACCT_OK;

	/*
	 *	Enter into the radutmp file.
	 */
	fd = open(inst->radutmp_fn, O_RDWR|O_CREAT, inst->permission);
	if (fd >= 0) {
		NAS_PORT *cache;
		int r;

		/*
		 *	Lock the utmp file, prefer lockf() over flock().
		 */
#if defined(F_LOCK) && !defined(BSD)
		(void)lockf(fd, F_LOCK, LOCK_LEN);
#else
		(void)flock(fd, LOCK_EX);
#endif
		/*
		 *	Find the entry for this NAS / portno combination.
		 */
		if ((cache = nas_port_find(inst->nas_port_list, ut.nas_address,
					   ut.nas_port)) != NULL)
			lseek(fd, (off_t)cache->offset, SEEK_SET);

		r = 0;
		off = 0;
		while (read(fd, &u, sizeof(u)) == sizeof(u)) {
			off += sizeof(u);
			if (u.nas_address != ut.nas_address ||
			    u.nas_port	  != ut.nas_port)
				continue;

			if (status == PW_STATUS_STOP &&
			    strncmp(ut.session_id, u.session_id,
			     sizeof(u.session_id)) != 0) {
				/*
				 *	Don't complain if this is not a
				 *	login record (some clients can
				 *	send _only_ logout records).
				 */
				if (u.type == P_LOGIN)
					log(L_ERR,
		"Accounting: logout: entry for NAS %s port %d has wrong ID",
					nas_name(nas_address), u.nas_port);
				r = -1;
				break;
			}

			if (status == PW_STATUS_START &&
			    strncmp(ut.session_id, u.session_id,
			     sizeof(u.session_id)) == 0  &&
			    u.time >= ut.time) {
				if (u.type == P_LOGIN) {
					log(L_INFO,
		"Accounting: login: entry for NAS %s port %d duplicate",
					nas_name(nas_address), u.nas_port);
					r = -1;
					break;
				}
				log(L_ERR,
		"Accounting: login: entry for NAS %s port %d wrong order",
				nas_name(nas_address), u.nas_port);
				r = -1;
				break;
			}

			/*
			 *	FIXME: the ALIVE record could need
			 *	some more checking, but anyway I'd
			 *	rather rewrite this mess -- miquels.
			 */
			if (status == PW_STATUS_ALIVE &&
			    strncmp(ut.session_id, u.session_id,
			     sizeof(u.session_id)) == 0  &&
			    u.type == P_LOGIN) {
				/*
				 *	Keep the original login time.
				 */
				ut.time = u.time;
				if (u.login[0] != 0)
					just_an_update = 1;
			}

			if (lseek(fd, -(off_t)sizeof(u), SEEK_CUR) < 0) {
				log(L_ERR, "Accounting: negative lseek!\n");
				lseek(fd, (off_t)0, SEEK_SET);
				off = 0;
			} else
				off -= sizeof(u);
			r = 1;
			break;
		}

		if (r >= 0 &&  (status == PW_STATUS_START ||
				status == PW_STATUS_ALIVE)) {
			if (cache == NULL) {
			   if ((cache = malloc(sizeof(NAS_PORT))) != NULL) {
				   cache->nasaddr = ut.nas_address;
				   cache->port = ut.nas_port;
				   cache->offset = off;
				   cache->next = inst->nas_port_list;
				   inst->nas_port_list = cache;
			   }
			}
			ut.type = P_LOGIN;
			write(fd, &ut, sizeof(u));
		}
		if (status == PW_STATUS_STOP) {
			if (r > 0) {
				u.type = P_IDLE;
				u.time = ut.time;
				u.delay = ut.delay;
				write(fd, &u, sizeof(u));
			} else if (r == 0) {
				log(L_ERR,
		"Accounting: logout: login entry for NAS %s port %d not found",
				nas_name(nas_address), ut.nas_port);
				r = -1;
			}
		}
		close(fd);
	} else {
		log(L_ERR, "Accounting: %s: %s", inst->radutmp_fn, strerror(errno));
		return RLM_ACCT_FAIL;
	}

	return RLM_ACCT_OK;
}

/*
 *	See if a user is already logged in. Sets *count to the current
 *	session count for this user and sets *mpp to 2 if it looks like a
 *	multilink attempt based on the requested ipno, otherwise leaves it
 *	alone.
 */
static int radutmp_checksimul(void *instance, const char *name, int *count,
			      int doradcheck, UINT4 ipno, int *mpp)
{
	struct radutmp	u;
	int		fd;
	struct sess_instance *inst = instance;

	if ((fd = open(inst->radutmp_fn, O_RDWR)) < 0) {
		if(errno!=ENOENT)
			return RLM_CSIM_FAIL;
		*count=0;
		return RLM_CSIM_OK;
	}

	if(!doradcheck) {
		*count = 0;
		while(read(fd, &u, sizeof(u)) == sizeof(u))
			if (strncmp(name, u.login, RUT_NAMESIZE) == 0
			    && u.type == P_LOGIN)
				++*count;
		close(fd);
		return RLM_CSIM_OK;
	}

	/*
	 *	lockf() the file while reading/writing.
	 */
#if defined(F_LOCK) && !defined(BSD)
		(void)lockf(fd, F_LOCK, LOCK_LEN);
#else
		(void)flock(fd, LOCK_EX);
#endif

	/*
	 *	Check all registered logins by querying the
	 *	terminal server directly.
	 *	FIXME: rad_check_ts() runs with locked radutmp file!
	 */
	*count = 0;
	while (read(fd, &u, sizeof(u)) == sizeof(u)) {
		if (strncmp(name, u.login, RUT_NAMESIZE) == 0
		    && u.type == P_LOGIN) {
			char login[sizeof u.login+1];
			char session_id[sizeof u.session_id+1];
			strNcpy(login, u.login, sizeof login);
			strNcpy(session_id, u.session_id, sizeof session_id);
			if (rad_check_ts(u.nas_address, u.nas_port, login, 
					 session_id) == 1) {
				++*count;
				/*
				 *	Does it look like a MPP attempt?
				 */
				if (strchr("SCPA", u.proto) &&
				    ipno && u.framed_address == ipno)
					*mpp = 2;
			}
			else {
				/*
				 *	False record - zap it.
				 */

				session_zap(u.nas_address, u.nas_port, login,
					    session_id, u.framed_address,
					    u.proto, 0);
			}
		}
	}
	close(fd);

	return RLM_CSIM_OK;
}

/* globally exported name */
module_t rlm_radutmp = {
  "radutmp",
  0,                            /* type: reserved */
  NULL,                 	/* initialization */
  radutmp_instantiate,          /* initialization */
  NULL,                         /* authorization */
  NULL,                         /* authentication */
  radutmp_accounting,           /* accounting */
  radutmp_checksimul,		/* checksimul */
  radutmp_detach,               /* detach */
  NULL,         	        /* destroy */
};

