/*
 * refclock_gpsdjson.c - clock driver as GPSD JSON client
 * Copyright Juergen Perlinger <perlinger@ntp.org>
 * Copyright the NTPsec project contributors
 * SPDX-License-Identifier: NTP
 *
 *	Heavily inspired by refclock_nmea.c
 *
 * Special thanks to Gary Miller and Hal Murray for their comments and
 * ideas.
 *
 * ---------------------------------------------------------------------
 *
 * This driver works slightly different from most others, as the PPS
 * information (if available) is also coming from GPSD via the data
 * connection. This makes using both the PPS data and the serial data
 * easier, but OTOH it's not possible to use the PPS driver to feed a
 * raw PPS stream to the core of NTPD.
 *
 * To go around this, the driver can use a secondary clock unit
 * (units>=128) that operate in tandem with the primary clock unit
 * (unit%128). The primary clock unit does all the IO stuff and data
 * decoding; if a a secondary unit is attached to a primary unit, this
 * secondary unit is feed with the PPS samples only and can act as a PPS
 * source to the clock selection.
 *
 * The drawback is that the primary unit must be present for the
 * secondary unit to work.
 *
 * This design is a compromise to reduce the IO load for both NTPD and
 * GPSD; it also ensures that data is transmitted and evaluated only
 * once on the side of NTPD.
 *
 * ---------------------------------------------------------------------
 *
 * trouble shooting hints:
 *
 *   Enable and check the clock stats. Check if there are bad replies;
 *   there should be none. If there are actually bad replies, then the
 *   driver cannot parse all JSON records from GPSD, and some record
 *   types are vital for the operation of the driver. This indicates a
 *   problem on the protocol level.
 *
 *   When started on the command line with a debug level >= 2, the
 *   driver dumps the raw received data and the parser input to
 *   stdout. Since the debug level is global, NTPD starts to create a
 *   *lot* of output. It makes sense to pipe it through '(f)grep
 *   GPSD_JSON' before writing the result to disk.
 *
 *   A bit less intrusive is using netcat or telnet to connect to GPSD
 *   and snoop what NTPD would get. If you try this, you have to send a
 *   WATCH command to GPSD:
 *
 * ?WATCH={"device":"/dev/gps0","enable":true,"json":true,"pps":true};<CRLF>
 *
 *   should show you what GPSD has to say to NTPD. Replace "/dev/gps0"
 *   with the device link used by GPSD, if necessary.
 *
 */


#include "config.h"
#include "ntp.h"
#include "ntp_types.h"
#include "ntp_debug.h"

/* =====================================================================
 * Get the little JSMN library directly into our guts. Use the 'parent
 * link' feature for maximum speed.
 */
#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "jsmn.h"

/* =====================================================================
 * JSON parsing stuff
 */

#define JSMN_MAXTOK	350
#define INVALID_TOKEN (-1)

typedef struct json_ctx {
	char        * buf;
	int           ntok;
	jsmntok_t     tok[JSMN_MAXTOK];
} json_ctx;

typedef int tok_ref;

/* We roll our own integer number parser.
 */
typedef signed   long int json_int;
typedef unsigned long int json_uint;
#define JSON_INT_MAX LONG_MAX
#define JSON_INT_MIN LONG_MIN

/* =====================================================================
 * header stuff we need
 */

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/tcp.h>

#include <sys/select.h>

#include "ntpd.h"
#include "ntp_io.h"
#include "ntp_refclock.h"
#include "ntp_stdlib.h"
#include "ntp_calendar.h"
#include "timespecops.h"

/* get operation modes from mode word.

 * + SERIAL (default) evaluates only in-band time ('IBT') as
 *   provided by TPV and TOFF records. TPV evaluation suffers from a
 *   bigger jitter than TOFF, sine it does not contain the receive time
 *   from GPSD and therefore the receive time of NTPD must be
 *   substituted for it. The network latency makes this a second rate
 *   guess.
 *
 *   If TOFF records are detected in the data stream, the timing
 *   information is gleaned from this record -- it contains the local
 *   receive time stamp from GPSD and therefore eliminates the
 *   transmission latency between GPSD and NTPD. The timing information
 *   from TPV is ignored once a TOFF is detected or expected.
 *
 *   TPV is still used to check the fix status, so the driver can stop
 *   feeding samples when GPSD says that the time information is
 *   effectively unreliable.
 *
 * + STRICT means only feed clock samples when a valid IBT/PPS pair is
 *   available. Combines the reference time from IBT with the pulse time
 *   from PPS. Masks the serial data jitter as long PPS is available,
 *   but can rapidly deteriorate once PPS drops out.
 *
 * + AUTO tries to use IBT/PPS pairs if available for some time, and if
 *   this fails for too long switches back to IBT only until the PPS
 *   signal becomes available again. See the HTML docs for this driver
 *   about the gotchas and why this is not the default.
 */
#define MODE_OP_MASK   0x03
#define MODE_OP_IBT    0
#define MODE_OP_STRICT 1
#define MODE_OP_AUTO   2
#define MODE_OP_MAXVAL 2
#define MODE_OP_MODE(x)		((x) & MODE_OP_MASK)

#define	PRECISION	(-9)	/* precision assumed (about 2 ms) */
#define	PPS_PRECISION	(-20)	/* precision assumed (about 1 us) */
#define	REFID		"GPSD"	/* reference id */
#define	NAME		"GPSD"	/* shortname */
#define	DESCRIPTION	"GPSD JSON client clock" /* who we are */

/* MAX_PDU_LEN needs to be bigger than GPS_JSON_RESPONSE_MAX from gpsd.
 * As of March 2019 that is 4096 */
#define MAX_PDU_LEN	8192

#define TICKOVER_LOW	10
#define TICKOVER_HIGH	120
#define LOGTHROTTLE	SECSPERHR

/* Primary channel PPS availability dance:
 * Every good PPS sample gets us a credit of PPS_INCCOUNT points, every
 * bad/missing PPS sample costs us a debit of PPS_DECCOUNT points. When
 * the account reaches the upper limit we change to a mode where only
 * PPS-augmented samples are fed to the core; when the account drops to
 * zero we switch to a mode where TPV-only timestamps are fed to the
 * core.
 * This reduces the chance of rapid alternation between raw and
 * PPS-augmented time stamps.
 */
#define PPS_MAXCOUNT	60	/* upper limit of account  */
#define PPS_INCCOUNT     3	/* credit for good samples */
#define PPS_DECCOUNT     1	/* debit for bad samples   */

/* The secondary (PPS) channel uses a different strategy to avoid old
 * PPS samples in the median filter.
 */
#define PPS2_MAXCOUNT 10

#define PROTO_VERSION(hi,lo) \
	    ((((uint32_t)(hi) << 16) & 0xFFFF0000u) | \
	     ((uint32_t)(lo) & 0x0FFFFu))

/* some local typedefs: The NTPD formatting style cries for short type
 * names, and we provide them locally. Note:the suffix '_t' is reserved
 * for the standard; I use a capital T instead.
 */
typedef struct peer         peerT;
typedef struct refclockproc clockprocT;
typedef struct addrinfo     addrinfoT;

/* =====================================================================
 * We use the same device name scheme as does the NMEA driver; since
 * GPSD supports the same links, we can select devices by a fixed name.
 */
#define	DEVICE		"/dev/gps%d"	/* GPS serial device */

/* =====================================================================
 * forward declarations for transfer vector and the vector itself
 */

static	void	gpsd_init	(void);
static	bool	gpsd_start	(int, peerT *);
static	void	gpsd_shutdown	(struct refclockproc *);
static	void	gpsd_receive	(struct recvbuf *);
static	void	gpsd_poll	(int, peerT *);
static	void	gpsd_control	(int, const struct refclockstat *,
				 struct refclockstat *, peerT *);
static	void	gpsd_timer	(int, peerT *);

static  int     myasprintf(char**, char const*, ...) NTP_PRINTF(2, 3);

static void     enter_opmode(peerT *peer, int mode);
static void	leave_opmode(peerT *peer, int mode);

struct refclock refclock_gpsdjson = {
	NAME,			/* basename of driver */
	gpsd_start,		/* start up driver */
	gpsd_shutdown,		/* shut down driver */
	gpsd_poll,		/* transmit poll message */
	gpsd_control,		/* fudge and option control */
	gpsd_init,		/* initialize driver */
	gpsd_timer		/* called once per second */
};

/* =====================================================================
 * our local clock unit and data
*/

struct gpsd_unit;
typedef struct gpsd_unit gpsd_unitT;

struct gpsd_unit {
	/* links for sharing between master/slave units */
	gpsd_unitT *next_unit;
	size_t      refcount;

	/* data for the secondary PPS channel */
	peerT      *pps_peer;

	/* unit and operation modes */
	int      unit;
	int      mode;
	char    *logname;	/* cached name for log/print */
	char    *device;	/* device name of unit */

	/* current line protocol version */
	uint32_t proto_version;

	/* PPS time stamps primary + secondary channel */
	l_fp pps_local;	/* when we received the PPS message */
	l_fp pps_stamp;	/* related reference time */
	l_fp pps_recvt;	/* when GPSD detected the pulse */
	l_fp pps_stamp2;/* related reference time (secondary) */
	l_fp pps_recvt2;/* when GPSD detected the pulse (secondary)*/
	int  ppscount;	/* PPS counter (primary unit) */
	int  ppscount2;	/* PPS counter (secondary unit) */

	/* TPV or TOFF serial time information */
	l_fp ibt_local;	/* when we received the TPV/TOFF message */
	l_fp ibt_stamp;	/* effective GPS time stamp */
	l_fp ibt_recvt;	/* when GPSD got the fix */

	/* precision estimates */
	int16_t	    ibt_prec;	/* serial precision based on EPT */
	int16_t     pps_prec;	/* PPS precision from GPSD or above */

	/* fudge values for correction, mirrored as 'l_fp' */
	l_fp pps_fudge;		/* PPS fudge primary channel */
	l_fp pps_fudge2;	/* PPS fudge secondary channel */
	l_fp ibt_fudge;		/* TPV/TOFF serial data fudge */

	/* Flags to indicate available data */
	bool fl_nosync: true;	/* GPSD signals bad quality */
	bool fl_ibt   : true;	/* valid TPV/TOFF seen (have time) */
	bool fl_pps   : true;	/* valid pulse seen */
	bool fl_pps2  : true;	/* valid pulse seen for PPS channel */
	bool fl_rawibt: true;	/* permit raw TPV/TOFF time stamps */
	bool fl_vers  : true;	/* have protocol version */
	bool fl_watch : true;	/* watch reply seen */
	/* protocol flags */
	bool pf_nsec  : true;	/* have nanosec PPS info */
	bool pf_toff  : true;	/* have TOFF record for timing */

	/* admin stuff for sockets and device selection */
	int         fdt;	/* current connecting socket */
	addrinfoT * addr;	/* next address to try */
	unsigned int       tickover;	/* timeout countdown */
	unsigned int       tickpres;	/* timeout preset */

	/* tallies for the various events */
	unsigned int       tc_recv;	/* received known records */
	unsigned int       tc_breply;	/* bad replies / parsing errors */
	unsigned int       tc_nosync;	/* TPV / sample cycles w/o fix */
	unsigned int       tc_ibt_recv;/* received serial time info records */
	unsigned int       tc_ibt_used;/* used        --^-- */
	unsigned int       tc_pps_recv;/* received PPS timing info records */
	unsigned int       tc_pps_used;/* used        --^-- */

	/* log bloat throttle */
	unsigned int       logthrottle;/* seconds to next log slot */

	/* The parse context for the current record */
	json_ctx    json_parse;

	/* record assembly buffer and saved length */
	int  buflen;
	char buffer[MAX_PDU_LEN];
};

/* =====================================================================
 * static local helpers forward decls
 */
static void gpsd_init_socket(peerT * const peer);
static void gpsd_test_socket(peerT * const peer);
static void gpsd_stop_socket(peerT * const peer);

static void gpsd_parse(peerT * const peer,
		       const l_fp  * const rtime);
static bool convert_ascii_time(l_fp * fp, const char * gps_time);
static void save_ltc(clockprocT * const pp, const char * const tc);
static bool syslogok(clockprocT * const pp, gpsd_unitT * const up);
static void log_data(peerT *peer, const char *what,
		     const char *buf, size_t len);
static int16_t clamped_precision(int rawprec);

/* =====================================================================
 * local / static stuff
 */

/* The logon string is actually the ?WATCH command of GPSD, using JSON
 * data and selecting the GPS device name we created from our unit
 * number. We have an old a newer version that request PPS (and TOFF)
 * transmission.
 * Note: These are actually format strings!
 */
static const char * const s_req_watch[2] = {
	"?WATCH={\"device\":\"%s\",\"enable\":true,\"json\":true};\r\n",
	"?WATCH={\"device\":\"%s\",\"enable\":true,\"json\":true,\"pps\":true};\r\n"
};

static const char * const s_req_version =
    "?VERSION;\r\n";

/* We keep a static list of network addresses for 'localhost:gpsd' or a
 * fallback alias of it, and we try to connect to them in round-robin
 * fashion. The service lookup is done during the driver init
 * function to minmise the impact of 'getaddrinfo()'.
 *
 * Alas, the init function is called even if there are no clocks
 * configured for this driver. So it makes sense to defer the logging of
 * any errors or other notifications until the first clock unit is
 * started -- otherwise there might be syslog entries from a driver that
 * is not used at all.
 */
static addrinfoT  *s_gpsd_addr;
static gpsd_unitT *s_clock_units;

/* list of service/socket names we want to resolve against */
static const char * const s_svctab[][2] = {
	{ "localhost", "gpsd" },
	{ "localhost", "2947" },
	{ "127.0.0.1", "2947" },
	{ NULL, NULL }
};

/* list of address resolution errors and index of service entry that
 * finally worked.
 */
static int s_svcerr[sizeof(s_svctab)/sizeof(s_svctab[0])];
static int s_svcidx;

/* =====================================================================
 * log throttling
 */
static bool
syslogok(
	clockprocT * const pp,
	gpsd_unitT * const up)
{
	int res = (0 != (pp->sloppyclockflag & CLK_FLAG3))
	       || (0           == up->logthrottle )
	       || (LOGTHROTTLE == up->logthrottle );
	if (res)
		up->logthrottle = LOGTHROTTLE;
	return res;
}

/* =====================================================================
 * the clock functions
 */

/* ---------------------------------------------------------------------
 * Init: This currently just gets the socket address for the GPS daemon
 */
static void
gpsd_init(void)
{
	addrinfoT   hints;
	int         idx;

	memset(s_svcerr, 0, sizeof(s_svcerr));
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_socktype = SOCK_STREAM;

	for (idx = 0; s_svctab[idx][0] && !s_gpsd_addr; idx++) {
		int rc = getaddrinfo(s_svctab[idx][0], s_svctab[idx][1],
				     &hints, &s_gpsd_addr);
		s_svcerr[idx] = rc;
		if (0 == rc) {
			break;
		}
		s_gpsd_addr = NULL;
	}
	s_svcidx = idx;
}

/* ---------------------------------------------------------------------
 * Init Check: flush pending log messages and check if we can proceed
 */
static bool
gpsd_init_check(void)
{
	int idx;

	/* Check if there is something to log */
	if (s_svcidx == 0) {
		return (s_gpsd_addr != NULL);
	}

	/* spool out the resolver errors */
	for (idx = 0; idx < s_svcidx; ++idx) {
		msyslog(LOG_WARNING,
			"REFCLOCK: GPSD_JSON: failed to resolve '%s:%s', rc=%d (%s)",
			s_svctab[idx][0], s_svctab[idx][1],
			s_svcerr[idx], gai_strerror(s_svcerr[idx]));
	}

	/* check if it was fatal, or if we can proceed */
	if (s_gpsd_addr == NULL)
		msyslog(LOG_ERR,
			"REFCLOCK: GPSD_JSON: failed to get socket address, giving up.");
	else if (idx != 0)
		msyslog(LOG_WARNING,
			"REFCLOCK: GPSD_JSON: using '%s:%s' instead of '%s:%s'",
			s_svctab[idx][0], s_svctab[idx][1],
			s_svctab[0][0], s_svctab[0][1]);

	/* make sure this gets logged only once and tell if we can
	 * proceed or not
	 */
	s_svcidx = 0;
	return (s_gpsd_addr != NULL);
}

/* ---------------------------------------------------------------------
 * Start: allocate a unit pointer and set up the runtime data
 */
static bool
gpsd_start(
	int     unit,
	peerT * peer)
{
	clockprocT  * const pp = peer->procptr;
	gpsd_unitT  * up;
	gpsd_unitT ** uscan    = &s_clock_units;

	struct stat sb;
        int ret;

	/* check if we can proceed at all or if init failed */
	if ( ! gpsd_init_check())
		return false;

	/* search for matching unit */
	while ((up = *uscan) != NULL && up->unit != (unit & 0x7F)) {
		uscan = &up->next_unit;
	}
	if (up == NULL) {
		/* alloc unit, add to list and increment use count ASAP. */
		up = emalloc_zero(sizeof(*up));
		*uscan = up;
		++up->refcount;

		/* initialize the unit structure */
		pp->clockname     = NAME; /* Hack, needed by refclock_name */
		up->logname  = estrdup(refclock_name(peer));
		up->unit     = unit & 0x7F;
		up->fdt      = -1;
		up->addr     = s_gpsd_addr;
		up->tickpres = TICKOVER_LOW;

		/* Create the device name and check for a Character
		 * Device. It's assumed that GPSD was started with the
		 * same link, so the names match. (If this is not
		 * practicable, we will have to read the symlink, if
		 * any, so we can get the true device file.)
		 */
                if ( peer->cfg.path ) {
                    /* use the ntp.conf path name */
		    ret = myasprintf(&up->device, "%s", peer->cfg.path);
                } else {
                    ret = myasprintf(&up->device, DEVICE, up->unit);
                }
		if (-1 == ret ) {
                        /* more likely out of RAM */
			msyslog(LOG_ERR,
                                "REFCLOCK: %s: clock device name too long",
				up->logname);
			goto dev_fail;
		}
		if (-1 == stat(up->device, &sb) || !S_ISCHR(sb.st_mode)) {
			msyslog(LOG_ERR,
                                "REFCLOCK: %s: '%s' is not a character device",
				up->logname, up->device);
			goto dev_fail;
		}
	} else {
		/* All set up, just increment use count. */
		++up->refcount;
	}

	/* setup refclock processing */
	pp->unitptr       = (void *)up;
	pp->io.fd         = -1;
	pp->io.clock_recv = gpsd_receive;
	pp->io.srcclock   = peer;
	pp->io.datalen    = 0;
	pp->a_lastcode[0] = '\0';
	pp->lencode       = 0;
	pp->clockname     = NAME;
	pp->clockdesc     = DESCRIPTION;
	memcpy(&pp->refid, REFID, REFIDLEN);
	peer->sstclktype = CTL_SST_TS_UHF;

	/* Initialize miscellaneous variables */
	if (unit >= 128)
		peer->precision = PPS_PRECISION;
	else
		peer->precision = PRECISION;

	/* If the daemon name lookup failed, just give up now. */
	if (NULL == up->addr) {
		msyslog(LOG_ERR,
			"REFCLOCK: %s: no GPSD socket address, giving up",
			up->logname);
		goto dev_fail;
	}

	LOGIF(CLOCKINFO,
	      (LOG_NOTICE, "%s: startup, device is '%s'",
	       refclock_name(peer), up->device));
	up->mode = MODE_OP_MODE(peer->cfg.mode);
	if (up->mode > MODE_OP_MAXVAL) {
		up->mode = 0;
	}
	if (unit >= 128) {
		up->pps_peer = peer;
	} else {
		enter_opmode(peer, up->mode);
	}
	return true;

dev_fail:
	/* On failure, remove all UNIT resources and declare defeat. */

	INSIST (up);
	if (!--up->refcount) {
		*uscan = up->next_unit;
		free(up->device);
		free(up);
	}

	pp->unitptr = NULL;
	return false;
}

/* ------------------------------------------------------------------ */

static void
gpsd_shutdown(
	struct refclockproc *pp)
{
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;
	gpsd_unitT ** uscan   = &s_clock_units;

	/* The unit pointer might have been removed already. */
	if (up == NULL) {
		return;
	}

	if (up->pps_peer == NULL) {
		/* This is NULL if no related PPS */
		DPRINT(1, ("%s: pps_peer found NULL", up->logname));
	} else if (pp != up->pps_peer->procptr) {
		/* now check if we must close IO resources */
		if (-1 != pp->io.fd) {
			DPRINT(1, ("%s: closing clock, fd=%d\n",
				   up->logname, pp->io.fd));
			io_closeclock(&pp->io);
			pp->io.fd = -1;
		}
		if (up->fdt != -1) {
			close(up->fdt);
		}
	}
	/* decrement use count and eventually remove this unit. */
	if (!--up->refcount) {
		/* unlink this unit */
		while (*uscan != NULL) {
			if (*uscan == up) {
				*uscan = up->next_unit;
			} else {
				uscan = &(*uscan)->next_unit;
			}
		}
		free(up->logname);
		free(up->device);
		free(up);
	}
	pp->unitptr = NULL;
	LOGIF(CLOCKINFO,
	      (LOG_NOTICE, "shutdown: gpsd_json(%d)", (int)pp->refclkunit));
}

/* ------------------------------------------------------------------ */

static void
gpsd_receive(
	struct recvbuf * rbufp)
{
	/* declare & init control structure ptrs */
	peerT	   * const peer = rbufp->recv_peer;
	clockprocT * const pp   = peer->procptr;
	gpsd_unitT * const up   = (gpsd_unitT *)pp->unitptr;

	const char *psrc, *esrc;
	char       *pdst, *edst, ch;

	/* log the data stream, if this is enabled */
	log_data(peer, "recv", (const char*)rbufp->recv_buffer,
		 (size_t)rbufp->recv_length);


	/* Since we're getting a raw stream data, we must assemble lines
	 * in our receive buffer. We can't use neither 'refclock_gtraw'
	 * not 'refclock_gtlin' here...  We process chars until we reach
	 * an EoL (that is, line feed) but we truncate the message if it
	 * does not fit the buffer.  GPSD might truncate messages, too,
	 * so dealing with truncated buffers is necessary anyway.
	 */
	psrc = (const char*)rbufp->recv_buffer;
	esrc = psrc + rbufp->recv_length;

	pdst = up->buffer + up->buflen;
	edst = pdst + sizeof(up->buffer) - 1; /* for trailing NUL */

	while (psrc < esrc) {
		ch = *psrc++;
		if (ch == '\n') {
			/* trim trailing whitespace & terminate buffer */
			while (pdst != up->buffer && pdst[-1] <= ' ') {
				--pdst;
			}
			*pdst = '\0';
			/* process data and reset buffer */
			up->buflen = (int)(pdst - up->buffer);
			gpsd_parse(peer, &rbufp->recv_time);
			pdst = up->buffer;
		} else if (pdst < edst) {
			/* add next char, ignoring leading whitespace */
			if (ch > ' ' || pdst != up->buffer) {
				*pdst++ = ch;
			}
		}
	}
	up->buflen = (int)(pdst - up->buffer);
	up->tickover = TICKOVER_LOW;
}

/* ------------------------------------------------------------------ */

static void
poll_primary(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	if (pp->coderecv != pp->codeproc) {
		/* all is well */
		pp->lastref = pp->lastrec;
		refclock_report(peer, CEVNT_NOMINAL);
		refclock_receive(peer);
	} else {
		/* Not working properly, admit to it. If we have no
		 * connection to GPSD, declare the clock as faulty. If
		 * there were bad replies, this is handled as the major
		 * cause, and everything else is just a timeout.
		 */
		peer->precision = PRECISION;
		if (-1 == pp->io.fd)
			refclock_report(peer, CEVNT_FAULT);
		else if (0 != up->tc_breply)
			refclock_report(peer, CEVNT_BADREPLY);
		else
			refclock_report(peer, CEVNT_TIMEOUT);
	}

	if (pp->sloppyclockflag & CLK_FLAG4)
		mprintf_clock_stats(
			peer,"%u %u %u %u %u %u %u",
			up->tc_recv,
			up->tc_breply, up->tc_nosync,
			up->tc_ibt_recv, up->tc_ibt_used,
			up->tc_pps_recv, up->tc_pps_used);

	/* clear tallies for next round */
	up->tc_breply   = 0;
	up->tc_recv     = 0;
	up->tc_nosync   = 0;
	up->tc_ibt_recv = 0;
	up->tc_ibt_used = 0;
	up->tc_pps_recv = 0;
	up->tc_pps_used = 0;
}

static void
poll_secondary(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	UNUSED_ARG(up);
	if (pp->coderecv != pp->codeproc) {
		/* all is well */
		pp->lastref = pp->lastrec;
		refclock_report(peer, CEVNT_NOMINAL);
		refclock_receive(peer);
	} else {
		peer->precision = PPS_PRECISION;
		peer->cfg.flags &= ~FLAG_PPS;
		refclock_report(peer, CEVNT_TIMEOUT);
	}
}

static void
gpsd_poll(
	int     unit,
	peerT * peer)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	UNUSED_ARG(unit);

	++pp->polls;
	if (peer == up->pps_peer) {
		poll_secondary(peer, pp, up);
	} else {
		poll_primary(peer, pp, up);
	}
}

/* ------------------------------------------------------------------ */

static void
gpsd_control(
	int                         unit,
	const struct refclockstat * in_st,
	struct refclockstat       * out_st,
	peerT                     * peer  )
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	UNUSED_ARG(unit);
	UNUSED_ARG(in_st);
	UNUSED_ARG(out_st);

	if (peer == up->pps_peer) {
		up->pps_fudge2 = dtolfp(pp->fudgetime1);
		if ( ! (pp->sloppyclockflag & CLK_FLAG1))
			peer->cfg.flags &= ~FLAG_PPS;
	} else {
		/* save preprocessed fudge times */
		up->pps_fudge = dtolfp(pp->fudgetime1);
		up->ibt_fudge = dtolfp(pp->fudgetime2);

		if (MODE_OP_MODE((uint32_t)up->mode ^ peer->cfg.mode)) {
			leave_opmode(peer, up->mode);
			up->mode = MODE_OP_MODE(peer->cfg.mode);
			enter_opmode(peer, up->mode);
		}
	}
 }

/* ------------------------------------------------------------------ */

static void
timer_primary(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	int rc;

	/* This is used for timeout handling. Nothing that needs
	 * sub-second precision happens here, so receive/connect/retry
	 * timeouts are simply handled by a count down, and then we
	 * decide what to do by the socket values.
	 *
	 * Note that the timer stays at zero here, unless some of the
	 * functions set it to another value.
	 */
	if (up->logthrottle) {
		--up->logthrottle;
	}
	if (up->tickover) {
		--up->tickover;
	}
	switch (up->tickover) {
	case 4:
		/* If we are connected to GPSD, try to get a live signal
		 * by querying the version. Otherwise just check the
		 * socket to become ready.
		 */
		if (-1 != pp->io.fd) {
			size_t rlen = strlen(s_req_version);
			DPRINT(2, ("%s: timer livecheck: '%s'\n",
				   up->logname, s_req_version));
			log_data(peer, "send", s_req_version, rlen);
			rc = write(pp->io.fd, s_req_version, rlen);
			(void)rc;
		} else if (-1 != up->fdt) {
			gpsd_test_socket(peer);
		}
		break;

	case 0:
		if (-1 != pp->io.fd)
			gpsd_stop_socket(peer);
		else if (-1 != up->fdt) {
			gpsd_test_socket(peer);
		} else if (NULL != s_gpsd_addr) {
			gpsd_init_socket(peer);
		}
		break;

	default:
		if (-1 == pp->io.fd && -1 != up->fdt)
			gpsd_test_socket(peer);
	}
}

static void
timer_secondary(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	/* Reduce the count by one. Flush sample buffer and clear PPS
	 * flag when this happens.
	 */
	up->ppscount2 = max(0, (up->ppscount2 - 1));
	if (0 == up->ppscount2) {
		if (pp->coderecv != pp->codeproc) {
			refclock_report(peer, CEVNT_TIMEOUT);
			pp->coderecv = pp->codeproc;
		}
		peer->cfg.flags &= ~FLAG_PPS;
	}
}

static void
gpsd_timer(
	int     unit,
	peerT * peer)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	UNUSED_ARG(unit);

	if (peer == up->pps_peer) {
		timer_secondary(peer, pp, up);
	} else {
		timer_primary(peer, pp, up);
	}
}

/* =====================================================================
 * handle opmode switches
 */

static void
enter_opmode(
	peerT *peer,
	int    mode)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	DPRINT(1, ("%s: enter operation mode %d\n",
		   up->logname, MODE_OP_MODE(mode)));

	if (MODE_OP_MODE(mode) == MODE_OP_AUTO) {
		up->fl_rawibt = false;
		up->ppscount  = PPS_MAXCOUNT / 2;
	}
	up->fl_pps = false;
	up->fl_ibt = false;
}

/* ------------------------------------------------------------------ */

static void
leave_opmode(
	peerT *peer,
	int    mode)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	DPRINT(1, ("%s: leaving operation mode %d\n",
		   up->logname, MODE_OP_MODE(mode)));

	if (MODE_OP_MODE(mode) == MODE_OP_AUTO) {
		up->fl_rawibt = false;
		up->ppscount  = 0;
	}
	up->fl_pps = false;
	up->fl_ibt = false;
}

/* =====================================================================
 * operation mode specific evaluation
 */

static void
add_clock_sample(
	peerT      * const peer ,
	clockprocT * const pp   ,
	l_fp               stamp,
	l_fp               recvt)
{
	pp->lastref = stamp;
	if (pp->coderecv == pp->codeproc)
		refclock_report(peer, CEVNT_NOMINAL);
	refclock_process_offset(pp, stamp, recvt, 0.0);
}

/* ------------------------------------------------------------------ */

static void
eval_strict(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	if (up->fl_ibt && up->fl_pps) {
		/* use TPV reference time + PPS receive time */
		add_clock_sample(peer, pp, up->ibt_stamp, up->pps_recvt);
		peer->precision = (int8_t)up->pps_prec;
		/* both packets consumed now... */
		up->fl_pps = false;
		up->fl_ibt = false;
		++up->tc_ibt_used;
	}
}

/* ------------------------------------------------------------------ */
/* PPS processing for the secondary channel. GPSD provides us with full
 * timing information, so there's no danger of PLL-locking to the wrong
 * second. The belts and suspenders needed for the raw ATOM clock are
 * unnecessary here.
 */
static void
eval_pps_secondary(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	if (up->fl_pps2) {
		/* feed data */
		add_clock_sample(peer, pp, up->pps_stamp2, up->pps_recvt2);
		peer->precision = (int8_t)up->pps_prec;
		/* PPS peer flag logic */
		up->ppscount2 = min(PPS2_MAXCOUNT, (up->ppscount2 + 2));
		if ((PPS2_MAXCOUNT == up->ppscount2) &&
		    (pp->sloppyclockflag & CLK_FLAG1) )
			peer->cfg.flags |= FLAG_PPS;
		/* mark time stamp as burned... */
		up->fl_pps2 = false;
		++up->tc_pps_used;
	}
}

/* ------------------------------------------------------------------ */

static void
eval_serial(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	if (up->fl_ibt) {
		add_clock_sample(peer, pp, up->ibt_stamp, up->ibt_recvt);
		peer->precision = (int8_t)up->ibt_prec;
		/* mark time stamp as burned... */
		up->fl_ibt = false;
		++up->tc_ibt_used;
	}
}

/* ------------------------------------------------------------------ */
static void
eval_auto(
	peerT      * const peer ,
	clockprocT * const pp   ,
	gpsd_unitT * const up   )
{
	/* If there's no TPV available, stop working here... */
	if (!up->fl_ibt)
		return;

	/* check how to handle IBT+PPS: Can PPS be used to augment IBT
	 * (or vice versae), do we drop the sample because there is a
	 * temporary missing PPS signal, or do we feed on IBT time
	 * stamps alone?
	 *
	 * Do a counter/threshold dance to decide how to proceed.
	 */
	if (up->fl_pps) {
		up->ppscount = min(PPS_MAXCOUNT,
				   (up->ppscount + PPS_INCCOUNT));
		if ((PPS_MAXCOUNT == up->ppscount) && up->fl_rawibt) {
			up->fl_rawibt = false;
			msyslog(LOG_INFO,
				"REFCLOCK: %s: expect valid PPS from now",
				up->logname);
		}
	} else {
		up->ppscount = max(0, (up->ppscount - PPS_DECCOUNT));
		if ((0 == up->ppscount) && !up->fl_rawibt) {
			up->fl_rawibt = true;
			msyslog(LOG_WARNING,
				"REFCLOCK: %s: use TPV alone from now",
				up->logname);
		}
	}

	/* now eventually feed the sample */
	if (up->fl_rawibt)
		eval_serial(peer, pp, up);
	else
		eval_strict(peer, pp, up);
}

/* =====================================================================
 * JSON parsing stuff
 */

/* ------------------------------------------------------------------ */
/* Parse a decimal integer with a possible sign. Works like 'strtoll()'
 * or 'strtol()', but with a fixed base of 10 and without eating away
 * leading whitespace. For the error codes, the handling of the end
 * pointer and the return values see 'strtol()'.
 */
static json_int
strtojint(
	const char *cp, char **ep)
{
	json_uint     accu, limit_lo, limit_hi;
	int           flags; /* bit 0: overflow; bit 1: sign */
	const char  * hold;

	/* pointer union to circumvent a tricky/sticky const issue */
	union {	const char * c; char * v; } vep;

	/* store initial value of 'cp' -- see 'strtol()' */
	vep.c = cp;

	/* Eat away an optional sign and set the limits accordingly: The
	 * high limit is the maximum absolute value that can be returned,
	 * and the low limit is the biggest value that does not cause an
	 * overflow when multiplied with 10. Avoid negation overflows.
	 */
	if (*cp == '-') {
		cp += 1;
		flags    = 2;
		limit_hi = (json_uint)-(JSON_INT_MIN + 1) + 1;
	} else {
		cp += (*cp == '+');
		flags    = 0;
		limit_hi = (json_uint)JSON_INT_MAX;
	}
	limit_lo = limit_hi / 10;

	/* Now try to convert a sequence of digits. */
	hold = cp;
	accu = 0;
	while (isdigit(*(const unsigned char*)cp)) {
	    flags |= (accu > limit_lo);
	    accu = accu * 10 + (json_uint)(*(const unsigned char*)cp++ - '0');
	    flags |= (accu > limit_hi);
	}
	/* Check for empty conversion (no digits seen). */
	if (hold != cp) {
		vep.c = cp;
	} else {
		errno = EINVAL;	/* accu is still zero */
	}
	/* Check for range overflow */
	if (flags & 1) {
		errno = ERANGE;
		accu  = limit_hi;
	}
	/* If possible, store back the end-of-conversion pointer */
	if (ep) {
		*ep = vep.v;
	}
	/* If negative, return the negated result if the accu is not
	 * zero. Avoid negation overflows.
	 */
	if ((flags & 2) && accu) {
		return -(json_int)(accu - 1) - 1;
	} else {
		return (json_int)accu;
	}
}

/* ------------------------------------------------------------------ */

static tok_ref
json_token_skip(
	const json_ctx * ctx,
	tok_ref          tid)
{
	if (tid >= 0 && tid < ctx->ntok) {
		int len = ctx->tok[tid].size;
		/* For arrays and objects, the size is the number of
		 * ITEMS in the compound. That's the number of objects in
		 * the array, and the number of key/value pairs for
		 * objects. In theory, the key must be a string, and we
		 * could simply skip one token before skipping the
		 * value, which can be anything. We're a bit paranoid
		 * and lazy at the same time: We simply double the
		 * number of tokens to skip and fall through into the
		 * array processing when encountering an object.
		 */
		switch (ctx->tok[tid].type) {
		case JSMN_OBJECT:
			len *= 2;
			/* FALLTHROUGH */
		case JSMN_ARRAY:
			for (++tid; len; --len)
				tid = json_token_skip(ctx, tid);
			break;

                case JSMN_PRIMITIVE:
                case JSMN_STRING:
                case JSMN_UNDEFINED:
		default:
			++tid;
			break;
		}
		if (tid > ctx->ntok) { /* Impossible? Paranoia rulez. */
			tid = ctx->ntok;
		}
	}
	return tid;
}

/* ------------------------------------------------------------------ */

static int
json_object_lookup(
	const json_ctx * ctx ,
	tok_ref          tid ,
	const char     * key ,
	int              what)
{
	int len;

	if (tid < 0 || tid >= ctx->ntok ||
	    ctx->tok[tid].type != JSMN_OBJECT)
		return INVALID_TOKEN;

	len = ctx->tok[tid].size;
	for (++tid; len && tid+1 < ctx->ntok; --len) {
		if (ctx->tok[tid].type != JSMN_STRING) { /* Blooper! */
			tid = json_token_skip(ctx, tid); /* skip key */
			tid = json_token_skip(ctx, tid); /* skip val */
		} else if (strcmp(key, ctx->buf + ctx->tok[tid].start)) {
			tid = json_token_skip(ctx, tid+1); /* skip key+val */
		} else if (what < 0 || what == (int)ctx->tok[tid+1].type) {
			return tid + 1;
		} else {
			break;
		}
		/* if skipping ahead returned an error, bail out here. */
		if (tid < 0) {
			break;
		}
	}
	return INVALID_TOKEN;
}

/* ------------------------------------------------------------------ */

static const char*
json_object_lookup_primitive(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key)
{
	tid = json_object_lookup(ctx, tid, key, JSMN_PRIMITIVE);
	if (INVALID_TOKEN  != tid) {
		return ctx->buf + ctx->tok[tid].start;
	} else {
		return NULL;
	}
}
/* ------------------------------------------------------------------ */
/* look up a boolean value. This essentially returns a tribool:
 * 0->false, 1->true, (-1)->error/undefined
 */
static int
json_object_lookup_bool(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key)
{
	const char *cp;
	cp  = json_object_lookup_primitive(ctx, tid, key);
	switch ( cp ? *cp : '\0') {
	case 't': return  1;
	case 'f': return  0;
	default : return -1;
	}
}

/* ------------------------------------------------------------------ */

static const char*
json_object_lookup_string(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key)
{
	tid = json_object_lookup(ctx, tid, key, JSMN_STRING);
	if (INVALID_TOKEN != tid)
		return ctx->buf + ctx->tok[tid].start;
	return NULL;
}

static const char*
json_object_lookup_string_default(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key,
	const char     * def)
{
	tid = json_object_lookup(ctx, tid, key, JSMN_STRING);
	if (INVALID_TOKEN != tid)
		return ctx->buf + ctx->tok[tid].start;
	return def;
}

/* ------------------------------------------------------------------ */

static json_int
json_object_lookup_int(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key)
{
	json_int     ret;
	const char * cp;
	char       * ep;

	cp = json_object_lookup_primitive(ctx, tid, key);
	if (NULL != cp) {
		ret = strtojint(cp, &ep);
		if (cp != ep && '\0' == *ep) {
			return ret;
		}
	} else {
		errno = EINVAL;
	}
	return 0;
}

static json_int
json_object_lookup_int_default(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key,
	json_int         def)
{
	json_int     ret;
	const char * cp;
	char       * ep;

	cp = json_object_lookup_primitive(ctx, tid, key);
	if (NULL != cp) {
		ret = strtojint(cp, &ep);
		if (cp != ep && '\0' == *ep) {
			return ret;
		}
	}
	return def;
}

/* ------------------------------------------------------------------ */

static double
json_object_lookup_float_default(
	const json_ctx * ctx,
	tok_ref          tid,
	const char     * key,
	double           def)
{
	double       ret;
	const char * cp;
	char       * ep;

	cp = json_object_lookup_primitive(ctx, tid, key);
	if (NULL != cp) {
		ret = strtod(cp, &ep);
		if (cp != ep && '\0' == *ep) {
			return ret;
		}
	}
	return def;
}

/* ------------------------------------------------------------------ */

static bool
json_parse_record(
	json_ctx * ctx,
	char     * buf,
	size_t     len)
{
	jsmn_parser jsm;
	int         idx, rc;

	jsmn_init(&jsm);
	rc = jsmn_parse(&jsm, buf, len, ctx->tok, JSMN_MAXTOK);
	if (rc <= 0)
		return false;
	ctx->buf  = buf;
	ctx->ntok = rc;

	if (JSMN_OBJECT != ctx->tok[0].type)
		return false; /* not object!?! */

	/* Make all tokens NUL terminated by overwriting the
	 * terminator symbol. Makes string compares and number parsing a
	 * lot easier!
	 */
	for (idx = 0; idx < ctx->ntok; ++idx)
		if (ctx->tok[idx].end > ctx->tok[idx].start)
			ctx->buf[ctx->tok[idx].end] = '\0';
	return true;
}


/* =====================================================================
 * static local helpers
 */
static bool
get_binary_time(
	l_fp       * const dest     ,
	json_ctx   * const jctx     ,
	const char * const time_name,
	const char * const frac_name,
	long               fscale   )
{
	bool            retv = false;
	struct timespec ts;

	errno = 0;
	ts.tv_sec  = (time_t)json_object_lookup_int(jctx, 0, time_name);
	ts.tv_nsec = (long  )json_object_lookup_int(jctx, 0, frac_name);
	if (0 == errno) {
		ts.tv_nsec *= fscale;
		*dest = tspec_stamp_to_lfp(ts);
		retv  = true;
	}
	return retv;
}

/* ------------------------------------------------------------------ */
/* Process a WATCH record
 *
 * Currently this is only used to recognise that the device is present
 * and that we're listed subscribers.
 */
static void
process_watch(
	peerT      * const peer ,
	json_ctx   * const jctx ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	const char * path;

	UNUSED_ARG(rtime);

	path = json_object_lookup_string(jctx, 0, "device");
	if (NULL == path || strcmp(path, up->device)) {
		return;
	}

	if (json_object_lookup_bool(jctx, 0, "enable") > 0 &&
	    json_object_lookup_bool(jctx, 0, "json"  ) > 0  )
		up->fl_watch = true;
	else
		up->fl_watch = false;
	DPRINT(2, ("%s: process_watch, enabled=%d\n",
		   up->logname, up->fl_watch));
}

/* ------------------------------------------------------------------ */

static void
process_version(
	peerT      * const peer ,
	json_ctx   * const jctx ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	size_t len;
	ssize_t ret;
	char * buf;
	const char *revision;
	const char *release;
	uint16_t    pvhi, pvlo;

	UNUSED_ARG(rtime);

	/* get protocol version number */
	revision = json_object_lookup_string_default(
		jctx, 0, "rev", "(unknown)");
	release  = json_object_lookup_string_default(
		jctx, 0, "release", "(unknown)");
	errno = 0;
	pvhi = (uint16_t)json_object_lookup_int(jctx, 0, "proto_major");
	pvlo = (uint16_t)json_object_lookup_int(jctx, 0, "proto_minor");

	if (0 == errno) {
		if ( ! up->fl_vers)
			msyslog(LOG_INFO,
				"REFCLOCK: %s: GPSD revision=%s release=%s "
				"protocol=%u.%u",
				up->logname, revision, release,
				pvhi, pvlo);
		up->proto_version = PROTO_VERSION(pvhi, pvlo);
		up->fl_vers = true;
	} else {
		if (syslogok(pp, up))
			msyslog(LOG_INFO,
				"REFCLOCK: %s: could not evaluate version data",
				up->logname);
		return;
	}
	/* With the 3.9 GPSD protocol, '*_musec' vanished from the PPS
	 * record and was replace by '*_nsec'.
	 */
	up->pf_nsec = up->proto_version >= PROTO_VERSION(3,9);

	/* With the 3.10 protocol we can get TOFF records for better
	 * timing information.
	 */
	up->pf_toff = up->proto_version >= PROTO_VERSION(3,10);

	/* request watch for our GPS device if not yet watched.
	 *
	 * The version string is also sent as a life signal, if we have
	 * seen usable data. So if we're already watching the device,
	 * skip the request.
	 *
	 * Reuse the input buffer, which is no longer needed in the
	 * current cycle. Also assume that we can write the watch
	 * request in one sweep into the socket; since we do not do
	 * output otherwise, this should always work.  (Unless the
	 * TCP/IP window size gets lower than the length of the
	 * request. We handle that when it happens.)
	 */
	if (up->fl_watch)
		return;

	snprintf(up->buffer, sizeof(up->buffer),
		 s_req_watch[up->pf_toff], up->device);
	buf = up->buffer;
	len = strlen(buf);
	log_data(peer, "send", buf, len);
	ret = write(pp->io.fd, buf, len);
	if ( (ret < 0 || (size_t)ret != len) && (syslogok(pp, up))) {
		/* Note: if the server fails to read our request, the
		 * resulting data timeout will take care of the
		 * connection!
		 */
		msyslog(LOG_ERR,
                        "REFCLOCK: %s: failed to write watch request (%s)",
			up->logname, strerror(errno));
	}
}

/* ------------------------------------------------------------------ */

static void
process_tpv(
	peerT      * const peer ,
	json_ctx   * const jctx ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	const char * gps_time;
	int          gps_mode;
	double       ept;
	int          xlog2;

	gps_mode = (int)json_object_lookup_int_default(
		jctx, 0, "mode", 0);

	gps_time = json_object_lookup_string(
		jctx, 0, "time");

	/* accept time stamps only in 2d or 3d fix */
	if (gps_mode < 2 || NULL == gps_time) {
		/* receiver has no fix; tell about and avoid stale data */
		if ( ! up->pf_toff)
			++up->tc_ibt_recv;
		++up->tc_nosync;
		up->fl_ibt    = false;
		up->fl_pps    = false;
		up->fl_nosync = true;
		return;
	}
	up->fl_nosync = false;

	/* convert clock and set resulting ref time, but only if the
	 * TOFF sentence is *not* available
	 */
	if ( ! up->pf_toff) {
		++up->tc_ibt_recv;
		/* save last time code to clock data */
		save_ltc(pp, gps_time);
		/* now parse the time string */
		if (convert_ascii_time(&up->ibt_stamp, gps_time)) {
			DPRINT(2, ("%s: process_tpv, stamp='%s',"
				   " recvt='%s' mode=%d\n",
				   up->logname,
				   prettydate(up->ibt_stamp),
				   prettydate(up->ibt_recvt),
				   gps_mode));

			/* have to use local receive time as substitute
			 * for the real receive time: TPV does not tell
			 * us.
			 */
			up->ibt_local = *rtime;
			up->ibt_recvt = *rtime;
			up->ibt_recvt -= up->ibt_fudge;
			up->fl_ibt = true;
		} else {
			++up->tc_breply;
			up->fl_ibt = false;
		}
	}

	/* Set the precision from the GPSD data
	 * Use the ETP field for an estimation of the precision of the
	 * serial data. If ETP is not available, use the default serial
	 * data precision instead. (Note: The PPS branch has a different
	 * precision estimation, since it gets the proper value directly
	 * from GPSD!)
	 */
	ept = json_object_lookup_float_default(jctx, 0, "ept", 2.0e-3);
	ept = frexp(fabs(ept)*0.70710678, &xlog2); /* ~ sqrt(0.5) */
	if (ept < 0.25)
		xlog2 = INT_MIN;
	if (ept > 2.0)
		xlog2 = INT_MAX;
	up->ibt_prec = clamped_precision(xlog2);
}

/* ------------------------------------------------------------------ */

static void
process_pps(
	peerT      * const peer ,
	json_ctx   * const jctx ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	int xlog2;

	++up->tc_pps_recv;

	/* Bail out if there's indication that time sync is bad or
	 * if we're explicitly requested to ignore PPS data.
	 */
	if (up->fl_nosync)
		return;

	up->pps_local = *rtime;
	/* Now grab the time values. 'clock_*' is the event time of the
	 * pulse measured on the local system clock; 'real_*' is the GPS
	 * reference time GPSD associated with the pulse.
	 */
	if (up->pf_nsec) {
		if ( ! get_binary_time(&up->pps_recvt2, jctx,
				       "clock_sec", "clock_nsec", 1))
			goto fail;
		if ( ! get_binary_time(&up->pps_stamp2, jctx,
				       "real_sec", "real_nsec", 1))
			goto fail;
	} else {
		if ( ! get_binary_time(&up->pps_recvt2, jctx,
				       "clock_sec", "clock_musec", 1000))
			goto fail;
		if ( ! get_binary_time(&up->pps_stamp2, jctx,
				       "real_sec", "real_musec", 1000))
			goto fail;
	}

	/* Try to read the precision field from the PPS record. If it's
	 * not there, take the precision from the serial data.
	 */
	xlog2 = (int)json_object_lookup_int_default(
			jctx, 0, "precision", up->ibt_prec);
	up->pps_prec = clamped_precision(xlog2);

	/* Get fudged receive times for primary & secondary unit */
	up->pps_recvt = up->pps_recvt2;
	up->pps_recvt -= up->pps_fudge;
	up->pps_recvt2 -= up->pps_fudge2;
	pp->lastrec = up->pps_recvt;

	/* Map to nearest full second as reference time stamp for the
	 * primary channel. Sanity checks are done in evaluation step.
	 */
	up->pps_stamp = up->pps_recvt;
	up->pps_stamp += 0x80000000U;
	setlfpfrac(up->pps_stamp, 0);

	if (NULL != up->pps_peer)
		save_ltc(up->pps_peer->procptr, prettydate(up->pps_stamp2));
	DPRINT(2, ("%s: PPS record processed,"
		   " stamp='%s', recvt='%s'\n",
		   up->logname,
		   prettydate(up->pps_stamp2),
		   prettydate(up->pps_recvt2)));

	up->fl_pps  = !(pp->sloppyclockflag & CLK_FLAG2);
	up->fl_pps2 = true;
	return;

  fail:
	DPRINT(1, ("%s: PPS record processing FAILED\n",
		   up->logname));
	++up->tc_breply;
}

/* ------------------------------------------------------------------ */

static void
process_toff(
	peerT      * const peer ,
	json_ctx   * const jctx ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	++up->tc_ibt_recv;

	/* remember this! */
	up->pf_toff = true;

	/* bail out if there's indication that time sync is bad */
	if (up->fl_nosync)
		return;

	if ( ! get_binary_time(&up->ibt_recvt, jctx,
			       "clock_sec", "clock_nsec", 1))
			goto fail;
	if ( ! get_binary_time(&up->ibt_stamp, jctx,
			       "real_sec", "real_nsec", 1))
			goto fail;
	up->ibt_recvt -= up->ibt_fudge;
	up->ibt_local = *rtime;
	up->fl_ibt    = true;

	save_ltc(pp, prettydate(up->ibt_stamp));
	DPRINT(2, ("%s: TOFF record processed,"
		   " stamp='%s', recvt='%s'\n",
		   up->logname,
		   prettydate(up->ibt_stamp),
		   prettydate(up->ibt_recvt)));
	return;

  fail:
	DPRINT(1, ("%s: TOFF record processing FAILED\n",
		   up->logname));
	++up->tc_breply;
}

/* ------------------------------------------------------------------ */

static void
gpsd_parse(
	peerT      * const peer ,
	const l_fp * const rtime)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	const char * clsid;

        DPRINT(2, ("%s: gpsd_parse: time %s '%.*s'\n",
		   up->logname, ulfptoa(*rtime, 6),
		   up->buflen, up->buffer));

	/* See if we can grab anything potentially useful. JSMN does not
	 * need a trailing NUL, but it needs the number of bytes to
	 * process. */
	if (!json_parse_record(&up->json_parse, up->buffer,
                               (size_t)up->buflen)) {
		++up->tc_breply;
		return;
	}

	/* Now dispatch over the objects we know */
	clsid = json_object_lookup_string(&up->json_parse, 0, "class");
	if (NULL == clsid) {
		++up->tc_breply;
		return;
	}

	if      (!strcmp("TPV", clsid)) {
		process_tpv(peer, &up->json_parse, rtime);
	} else if (!strcmp("PPS", clsid)) {
		process_pps(peer, &up->json_parse, rtime);
	} else if (!strcmp("TOFF", clsid)) {
		process_toff(peer, &up->json_parse, rtime);
	} else if (!strcmp("VERSION", clsid)) {
		process_version(peer, &up->json_parse, rtime);
	} else if (!strcmp("WATCH", clsid)) {
		process_watch(peer, &up->json_parse, rtime);
	} else {
		return; /* nothing we know about... */
	}
	++up->tc_recv;

	/* if possible, feed the PPS side channel */
	if (up->pps_peer)
		eval_pps_secondary(
			up->pps_peer, up->pps_peer->procptr, up);

	/* check PPS vs. IBT receive times:
	 * If IBT is before PPS, then clearly the IBT is too old. If PPS
	 * is before IBT by more than one second, then PPS is too old.
	 * Weed out stale time stamps & flags.
	 */
	if (up->fl_pps && up->fl_ibt) {
		l_fp diff;
		diff = up->ibt_local;
		diff -= up->pps_local;
		if (lfpsint(diff) > 0)
			up->fl_pps = false; /* pps too old */
		else if (lfpsint(diff) < 0)
			up->fl_ibt = false; /* serial data too old */
	}

	/* dispatch to the mode-dependent processing functions */
	switch (up->mode) {
	default:
	case MODE_OP_IBT:
		eval_serial(peer, pp, up);
		break;

	case MODE_OP_STRICT:
		eval_strict(peer, pp, up);
		break;

	case MODE_OP_AUTO:
		eval_auto(peer, pp, up);
		break;
	}
}

/* ------------------------------------------------------------------ */

static void
gpsd_stop_socket(
	peerT * const peer)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	if (-1 != pp->io.fd) {
		if (syslogok(pp, up))
			msyslog(LOG_INFO,
				"REFCLOCK: %s: closing socket to GPSD, fd=%d",
				up->logname, pp->io.fd);
		else
			DPRINT(1, ("%s: closing socket to GPSD, fd=%d\n",
				   up->logname, pp->io.fd));
		io_closeclock(&pp->io);
		pp->io.fd = -1;
	}
	up->tickover = up->tickpres;
	up->tickpres = min(up->tickpres + 5, TICKOVER_HIGH);
	up->fl_vers  = false;
	up->fl_ibt   = false;
	up->fl_pps   = false;
	up->fl_watch = false;
}

/* ------------------------------------------------------------------ */

static void
gpsd_init_socket(
	peerT * const peer)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;
	addrinfoT  * ai;
	int          rc;
	int          ov;

	/* draw next address to try */
	if (NULL == up->addr) {
		up->addr = s_gpsd_addr;
	}
	ai = up->addr;
	up->addr = ai->ai_next;

	/* try to create a matching socket */
	up->fdt = socket(
		ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (-1 == up->fdt) {
		if (syslogok(pp, up))
			msyslog(LOG_ERR,
				"REFCLOCK: %s: cannot create GPSD socket: %s",
				up->logname, strerror(errno));
		goto no_socket;
	}

	/* Make sure the socket is non-blocking. Connect/reconnect and
	 * IO happen in an event-driven environment, and synchronous
	 * operations wreak havoc on that.
	 */
	rc = fcntl(up->fdt, F_SETFL, O_NONBLOCK, 1);
	if (-1 == rc) {
		if (syslogok(pp, up))
			msyslog(LOG_ERR,
				"REFCLOCK: %s: cannot set GPSD socket "
                                "to non-blocking: %s",
				up->logname, strerror(errno));
		goto no_socket;
	}
	/* Disable nagling. The way both GPSD and NTPD handle the
	 * protocol makes it record-oriented, and in most cases
	 * complete records (JSON serialised objects) will be sent in
	 * one sweep. Nagling gives not much advantage but adds another
	 * delay, which can worsen the situation for some packets.
	 */
	ov = 1;
	rc = setsockopt(up->fdt, IPPROTO_TCP, TCP_NODELAY,
			(char*)&ov, sizeof(ov));
	if (-1 == rc) {
		if (syslogok(pp, up))
			msyslog(LOG_INFO,
				"REFCLOCK: %s: cannot disable TCP nagle: %s",
				up->logname, strerror(errno));
	}

	/* Start a non-blocking connect. There might be a synchronous
	 * connection result we have to handle.
	 */
	rc = connect(up->fdt, ai->ai_addr, ai->ai_addrlen);
	if (-1 == rc) {
		if (errno == EINPROGRESS) {
			DPRINT(1, ("%s: async connect pending, fd=%d\n",
				   up->logname, up->fdt));
			return;
		}

		if (syslogok(pp, up))
			msyslog(LOG_ERR,
				"REFCLOCK: %s: cannot connect GPSD socket: %s",
				up->logname, strerror(errno));
		goto no_socket;
	}

	/* We had a successful synchronous connect, so we add the
	 * refclock processing ASAP. We still have to wait for the
	 * version string and apply the watch command later on, but we
	 * might as well get the show on the road now.
	 */
	DPRINT(1, ("%s: new socket connection, fd=%d\n",
		   up->logname, up->fdt));

	pp->io.fd = up->fdt;
	up->fdt   = -1;
	if (0 == io_addclock(&pp->io)) {
		if (syslogok(pp, up))
			msyslog(LOG_ERR,
				"REFCLOCK: %s: failed to register "
                                "with I/O engine",
				up->logname);
		goto no_socket;
	}

	return;

  no_socket:
	if (-1 != pp->io.fd)
		close(pp->io.fd);
	if (-1 != up->fdt) {
		close(up->fdt);
	}
	pp->io.fd    = -1;
	up->fdt      = -1;
	up->tickover = up->tickpres;
	up->tickpres = min(up->tickpres + 5, TICKOVER_HIGH);
}

/* ------------------------------------------------------------------ */

static void
gpsd_test_socket(
	peerT * const peer)
{
	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	int       ec, rc;
	socklen_t lc;

	/* Check if the non-blocking connect was finished by testing the
	 * socket for writeability. Use the 'poll()' API if available
	 * and 'select()' otherwise.
	 */
	DPRINT(2, ("%s: check connect, fd=%d\n",
		   up->logname, up->fdt));

	{
		struct timespec tout;
		fd_set         wset;

		memset(&tout, 0, sizeof(tout));
		FD_ZERO(&wset);
		FD_SET(up->fdt, &wset);
		rc = pselect(up->fdt+1, NULL, &wset, NULL, &tout, NULL);
		if (0 == rc || !(FD_ISSET(up->fdt, &wset))) {
			return;
		}
	}

	/* next timeout is a full one... */
	up->tickover = TICKOVER_LOW;

	/* check for socket error */
	ec = 0;
	lc = sizeof(ec);
	rc = getsockopt(up->fdt, SOL_SOCKET, SO_ERROR, &ec, &lc);
	if (-1 == rc || 0 != ec) {
		const char *errtxt;
		if (0 == ec)
			ec = errno;
		errtxt = strerror(ec);
		if (syslogok(pp, up))
			msyslog(LOG_ERR,
				"REFCLOCK: %s: async connect to GPSD failed,"
				" fd=%d, ec=%d(%s)",
				up->logname, up->fdt, ec, errtxt);
		else
			DPRINT(1, ("%s: async connect to GPSD failed,"
				   " fd=%d, ec=%d(%s)\n",
				   up->logname, up->fdt, ec, errtxt));
		goto no_socket;
	} else {
		DPRINT(1, ("%s: async connect to GPSD succeeded, fd=%d\n",
			   up->logname, up->fdt));
	}

	/* swap socket FDs, and make sure the clock was added */
	pp->io.fd = up->fdt;
	up->fdt   = -1;
	if (0 == io_addclock(&pp->io)) {
		if (syslogok(pp, up))
			msyslog(LOG_ERR,
			    "REFCLOCK: %s: failed to register with I/O engine",
			    up->logname);
		goto no_socket;
	}
	return;

  no_socket:
	if (-1 != up->fdt) {
		DPRINT(1, ("%s: closing socket, fd=%d\n",
			   up->logname, up->fdt));
		close(up->fdt);
	}
	up->fdt      = -1;
	up->tickover = up->tickpres;
	up->tickpres = min(up->tickpres + 5, TICKOVER_HIGH);
}

/* =====================================================================
 * helper stuff
 */

/* -------------------------------------------------------------------
 * store a properly clamped precision value
 */
static int16_t
clamped_precision(
	int rawprec)
{
	if (rawprec > 0) {
		rawprec = 0;
	}
	if (rawprec < -32) {
		rawprec = -32;
	}
	return (int16_t)rawprec;
}

/* -------------------------------------------------------------------
 * Convert a GPSD timestamp (ISO 8601 Format) to an l_fp
 */
static bool
convert_ascii_time(
	l_fp       * fp      ,
	const char * gps_time)
{
	char           *ep;
	struct tm       gd;
	struct timespec ts;
	long   dw;

	/* Use 'strptime' to take the brunt of the work, then parse
	 * the fractional part manually, starting with a digit weight of
	 * 10^8 nanoseconds.
	 */
	ts.tv_nsec = 0;
	ep = strptime(gps_time, "%Y-%m-%dT%H:%M:%S", &gd);
	if (NULL == ep)
		return false; /* could not parse the mandatory stuff! */
	if (*ep == '.') {
		dw = 100000000;
		while (isdigit(*(unsigned char*)++ep)) {
		    ts.tv_nsec += (long)(*(unsigned char*)ep - '0') * dw;
		    dw /= 10;
		}
	}
	if (ep[0] != 'Z' || ep[1] != '\0')
		return false; /* trailing garbage */

	/* Now convert the whole thing into a 'l_fp'. We do not use
	 * 'mkgmtime()' since its not standard and going through the
	 * calendar routines is not much effort, either.
	 */
	ts.tv_sec = (ntpcal_tm_to_rd(&gd) - DAY_NTP_STARTS) * SECSPERDAY
	          + ntpcal_tm_to_daysec(&gd);
	*fp = tspec_intv_to_lfp(ts);

	return true;
}

/* -------------------------------------------------------------------
 * Save the last timecode string, making sure it's properly truncated
 * if necessary and NUL terminated in any case.
 */
static void
save_ltc(
	clockprocT * const pp,
	const char * const tc)
{

        if (NULL == tc) {
	    pp->a_lastcode[0] = '\0';
        } else {
	    strlcpy(pp->a_lastcode, tc, sizeof(pp->a_lastcode));
	}
}

/* -------------------------------------------------------------------
 * asprintf replacement... it's not available everywhere...
 */
static int
myasprintf(
	char      ** spp,
	char const * fmt,
	...             )
{
	size_t alen, plen;

	alen = 32;
	*spp = NULL;
	do {
		va_list va;

		alen += alen;
		free(*spp);
		*spp = (char*)malloc(alen);
		if (NULL == *spp) {
			return -1;
		}

		va_start(va, fmt);
		plen = (size_t)vsnprintf(*spp, alen, fmt, va);
		va_end(va);
	} while (plen >= alen);

	return (int)plen;
}

/* -------------------------------------------------------------------
 * dump a raw data buffer
 *
 * Maybe this could be used system wide?
 */

static void
log_data(
	peerT      *peer,
	const char *what,
	const char *buf ,
	size_t      len )
{
#ifndef DEBUG
	UNUSED_ARG(peer);
	UNUSED_ARG(what);
	UNUSED_ARG(buf);
	UNUSED_ARG(len);
#else
	char s_lbuf[MAX_PDU_LEN];

	clockprocT * const pp = peer->procptr;
	gpsd_unitT * const up = (gpsd_unitT *)pp->unitptr;

	if (debug > 1) { /* SPECIAL DEBUG */
		const char *sptr = buf;
		const char *stop = buf + len;
		char       *dptr = s_lbuf;
                /* leave room for hex (\\x23) + NUL */
		char       *dtop = s_lbuf + sizeof(s_lbuf) - 10;

		while (sptr != stop && dptr < dtop) {
			if (*sptr == '\\') {
                                /* replace with two \ */
				*dptr++ = '\\';
				*dptr++ = '\\';
			} else if (isprint((unsigned char)*sptr)) {
				*dptr++ = *sptr;
			} else {
                               dptr += snprintf(dptr, dtop - dptr, "\\%#.2x",
                                                *(const uint8_t*)sptr);

			}
			sptr++;
		}
		*dptr = '\0';
		printf("%s[%s]: '%s'\n", up->logname, what, s_lbuf);
	}
#endif
}

