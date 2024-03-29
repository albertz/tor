/* Copyright (c) 2009-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file compat_libevent.c
 * \brief Wrappers to handle porting between different versions of libevent.
 *
 * In an ideal world, we'd just use Libevent 2.0 from now on.  But as of June
 * 2009, Libevent 2.0 is still in alpha, and we will have old versions of
 * Libevent for the forseeable future.
 **/

#include "orconfig.h"
#include "compat.h"
#include "compat_libevent.h"

#include "util.h"
#include "torlog.h"

#ifdef HAVE_EVENT2_EVENT_H
#include <event2/event.h>
#else
#include <event.h>
#endif

/** A number representing a version of Libevent.

    This is a 4-byte number, with the first three bytes representing the
    major, minor, and patchlevel respectively of the library.  The fourth
    byte is unused.

    This is equivalent to the format of LIBEVENT_VERSION_NUMBER on Libevent
    2.0.1 or later.  For versions of Libevent before 1.4.0, which followed the
    format of "1.0, 1.0a, 1.0b", we define 1.0 to be equivalent to 1.0.0, 1.0a
    to be equivalent to 1.0.1, and so on.
*/
typedef uint32_t le_version_t;

/** @{ */
/** Macros: returns the number of a libevent version as a le_version_t */
#define V(major, minor, patch) \
  (((major) << 24) | ((minor) << 16) | ((patch) << 8))
#define V_OLD(major, minor, patch) \
  V((major), (minor), (patch)-'a'+1)
/** @} */

/** Represetns a version of libevent so old we can't figure out what version
 * it is. */
#define LE_OLD V(0,0,0)
/** Represents a version of libevent so weird we can't figure out what version
 * it is. */
#define LE_OTHER V(0,0,99)

static le_version_t tor_get_libevent_version(const char **v_out);

#ifdef HAVE_EVENT_SET_LOG_CALLBACK
/** A string which, if it appears in a libevent log, should be ignored. */
static const char *suppress_msg = NULL;
/** Callback function passed to event_set_log() so we can intercept
 * log messages from libevent. */
static void
libevent_logging_callback(int severity, const char *msg)
{
  char buf[1024];
  size_t n;
  if (suppress_msg && strstr(msg, suppress_msg))
    return;
  n = strlcpy(buf, msg, sizeof(buf));
  if (n && n < sizeof(buf) && buf[n-1] == '\n') {
    buf[n-1] = '\0';
  }
  switch (severity) {
    case _EVENT_LOG_DEBUG:
      log(LOG_DEBUG, LD_NOCB|LD_NET, "Message from libevent: %s", buf);
      break;
    case _EVENT_LOG_MSG:
      log(LOG_INFO, LD_NOCB|LD_NET, "Message from libevent: %s", buf);
      break;
    case _EVENT_LOG_WARN:
      log(LOG_WARN, LD_NOCB|LD_GENERAL, "Warning from libevent: %s", buf);
      break;
    case _EVENT_LOG_ERR:
      log(LOG_ERR, LD_NOCB|LD_GENERAL, "Error from libevent: %s", buf);
      break;
    default:
      log(LOG_WARN, LD_NOCB|LD_GENERAL, "Message [%d] from libevent: %s",
          severity, buf);
      break;
  }
}
/** Set hook to intercept log messages from libevent. */
void
configure_libevent_logging(void)
{
  event_set_log_callback(libevent_logging_callback);
}
/** Ignore any libevent log message that contains <b>msg</b>. */
void
suppress_libevent_log_msg(const char *msg)
{
  suppress_msg = msg;
}
#else
void
configure_libevent_logging(void)
{
}
void
suppress_libevent_log_msg(const char *msg)
{
  (void)msg;
}
#endif

#ifndef HAVE_EVENT2_EVENT_H
/** Work-alike replacement for event_new() on pre-Libevent-2.0 systems. */
struct event *
tor_event_new(struct event_base *base, int sock, short what,
              void (*cb)(int, short, void *), void *arg)
{
  struct event *e = tor_malloc_zero(sizeof(struct event));
  event_set(e, sock, what, cb, arg);
  if (! base)
    base = tor_libevent_get_base();
  event_base_set(base, e);
  return e;
}
/** Work-alike replacement for evtimer_new() on pre-Libevent-2.0 systems. */
struct event *
tor_evtimer_new(struct event_base *base,
                void (*cb)(int, short, void *), void *arg)
{
  return tor_event_new(base, -1, 0, cb, arg);
}
/** Work-alike replacement for evsignal_new() on pre-Libevent-2.0 systems. */
struct event *
tor_evsignal_new(struct event_base * base, int sig,
                 void (*cb)(int, short, void *), void *arg)
{
  return tor_event_new(base, sig, EV_SIGNAL|EV_PERSIST, cb, arg);
}
/** Work-alike replacement for event_free() on pre-Libevent-2.0 systems. */
void
tor_event_free(struct event *ev)
{
  event_del(ev);
  tor_free(ev);
}
#endif

/** Global event base for use by the main thread. */
struct event_base *the_event_base = NULL;

/* This is what passes for version detection on OSX.  We set
 * MACOSX_KQUEUE_IS_BROKEN to true iff we're on a version of OSX before
 * 10.4.0 (aka 1040). */
#ifdef __APPLE__
#ifdef __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__
#define MACOSX_KQUEUE_IS_BROKEN \
  (__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1040)
#else
#define MACOSX_KQUEUE_IS_BROKEN 0
#endif
#endif

/** Initialize the Libevent library and set up the event base. */
void
tor_libevent_initialize(void)
{
  tor_assert(the_event_base == NULL);

#ifdef __APPLE__
  if (MACOSX_KQUEUE_IS_BROKEN ||
      tor_get_libevent_version(NULL) < V_OLD(1,1,'b')) {
    setenv("EVENT_NOKQUEUE","1",1);
  }
#endif

#ifdef HAVE_EVENT2_EVENT_H
  {
    struct event_config *cfg = event_config_new();
    tor_assert(cfg);

    /* In 0.2.2, we don't use locking at all.  Telling Libevent not to try to
     * turn it on can avoid a needless socketpair() attempt.
     */
    event_config_set_flag(cfg, EVENT_BASE_FLAG_NOLOCK);

    the_event_base = event_base_new_with_config(cfg);

    event_config_free(cfg);
  }
#else
  the_event_base = event_init();
#endif

  if (!the_event_base) {
    log_err(LD_GENERAL, "Unable to initialize Libevent: cannot continue.");
    exit(1);
  }

#if defined(HAVE_EVENT_GET_VERSION) && defined(HAVE_EVENT_GET_METHOD)
  /* Making this a NOTICE for now so we can link bugs to a libevent versions
   * or methods better. */
  log(LOG_NOTICE, LD_GENERAL,
      "Initialized libevent version %s using method %s. Good.",
      event_get_version(), tor_libevent_get_method());
#else
  log(LOG_NOTICE, LD_GENERAL,
      "Initialized old libevent (version 1.0b or earlier).");
  log(LOG_WARN, LD_GENERAL,
      "You have a *VERY* old version of libevent.  It is likely to be buggy; "
      "please build Tor with a more recent version.");
#endif
}

/** Return the current Libevent event base that we're set up to use. */
struct event_base *
tor_libevent_get_base(void)
{
  return the_event_base;
}

#ifndef HAVE_EVENT_BASE_LOOPEXIT
/** Replacement for event_base_loopexit on some very old versions of Libevent
 * that we are not yet brave enough to deprecate. */
int
tor_event_base_loopexit(struct event_base *base, struct timeval *tv)
{
  tor_assert(base == the_event_base);
  return event_loopexit(tv);
}
#endif

/** Return the name of the Libevent backend we're using. */
const char *
tor_libevent_get_method(void)
{
#ifdef HAVE_EVENT2_EVENT_H
  return event_base_get_method(the_event_base);
#elif defined(HAVE_EVENT_GET_METHOD)
  return event_get_method();
#else
  return "<unknown>";
#endif
}

/** Return the le_version_t for the version of libevent specified in the
 * string <b>v</b>.  If the version is very new or uses an unrecognized
 * version, format, return LE_OTHER. */
static le_version_t
tor_decode_libevent_version(const char *v)
{
  unsigned major, minor, patchlevel;
  char c, e, extra;
  int fields;

  /* Try the new preferred "1.4.11-stable" format.
   * Also accept "1.4.14b-stable". */
  fields = sscanf(v, "%u.%u.%u%c%c", &major, &minor, &patchlevel, &c, &e);
  if (fields == 3 ||
      ((fields == 4 || fields == 5 ) && (c == '-' || c == '_')) ||
      (fields == 5 && TOR_ISALPHA(c) && (e == '-' || e == '_'))) {
    return V(major,minor,patchlevel);
  }

  /* Try the old "1.3e" format. */
  fields = sscanf(v, "%u.%u%c%c", &major, &minor, &c, &extra);
  if (fields == 3 && TOR_ISALPHA(c)) {
    return V_OLD(major, minor, c);
  } else if (fields == 2) {
    return V(major, minor, 0);
  }

  return LE_OTHER;
}

/** Return an integer representing the binary interface of a Libevent library.
 * Two different versions with different numbers are sure not to be binary
 * compatible.  Two different versions with the same numbers have a decent
 * chance of binary compatibility.*/
static int
le_versions_compatibility(le_version_t v)
{
  if (v == LE_OTHER)
    return 0;
  if (v < V_OLD(1,0,'c'))
    return 1;
  else if (v < V(1,4,0))
    return 2;
  else if (v < V(1,4,99))
    return 3;
  else if (v < V(2,0,1))
    return 4;
  else /* Everything 2.0 and later should be compatible. */
    return 5;
}

/** Return the version number of the currently running version of Libevent.
 * See le_version_t for info on the format.
 */
static le_version_t
tor_get_libevent_version(const char **v_out)
{
  const char *v;
  le_version_t r;
#if defined(HAVE_EVENT_GET_VERSION_NUMBER)
  v = event_get_version();
  r = event_get_version_number();
#elif defined (HAVE_EVENT_GET_VERSION)
  v = event_get_version();
  r = tor_decode_libevent_version(v);
#else
  v = "pre-1.0c";
  r = LE_OLD;
#endif
  if (v_out)
    *v_out = v;
  return r;
}

/** Return a string representation of the version of the currently running
 * version of Libevent. */
const char *
tor_libevent_get_version_str(void)
{
#ifdef HAVE_EVENT_GET_VERSION
  return event_get_version();
#else
  return "pre-1.0c";
#endif
}

/**
 * Compare the current Libevent method and version to a list of versions
 * which are known not to work.  Warn the user as appropriate.
 */
void
tor_check_libevent_version(const char *m, int server,
                           const char **badness_out)
{
  int buggy = 0, iffy = 0, slow = 0, thread_unsafe = 0;
  le_version_t version;
  const char *v = NULL;
  const char *badness = NULL;
  const char *sad_os = "";

  version = tor_get_libevent_version(&v);

  /* It would be better to disable known-buggy methods rather than warning
   * about them.  But the problem is that with older versions of Libevent,
   * it's not trivial to get them to change their methods once they're
   * initialized... and with newer versions of Libevent, they aren't actually
   * broken.  But we should revisit this if we ever find a post-1.4 version
   * of Libevent where we need to disable a given method. */
  if (!strcmp(m, "kqueue")) {
    if (version < V_OLD(1,1,'b'))
      buggy = 1;
  } else if (!strcmp(m, "epoll")) {
    if (version < V(1,1,0))
      iffy = 1;
  } else if (!strcmp(m, "poll")) {
    if (version < V_OLD(1,0,'e'))
      buggy = 1;
    if (version < V(1,1,0))
      slow = 1;
  } else if (!strcmp(m, "select")) {
    if (version < V(1,1,0))
      slow = 1;
  } else if (!strcmp(m, "win32")) {
    if (version < V_OLD(1,1,'b'))
      buggy = 1;
  }

  /* Libevent versions before 1.3b do very badly on operating systems with
   * user-space threading implementations. */
#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
  if (server && version < V_OLD(1,3,'b')) {
    thread_unsafe = 1;
    sad_os = "BSD variants";
  }
#elif defined(__APPLE__) || defined(__darwin__)
  if (server && version < V_OLD(1,3,'b')) {
    thread_unsafe = 1;
    sad_os = "Mac OS X";
  }
#endif

  if (thread_unsafe) {
    log(LOG_WARN, LD_GENERAL,
        "Libevent version %s often crashes when running a Tor server with %s. "
        "Please use the latest version of libevent (1.3b or later)",v,sad_os);
    badness = "BROKEN";
  } else if (buggy) {
    log(LOG_WARN, LD_GENERAL,
        "There are serious bugs in using %s with libevent %s. "
        "Please use the latest version of libevent.", m, v);
    badness = "BROKEN";
  } else if (iffy) {
    log(LOG_WARN, LD_GENERAL,
        "There are minor bugs in using %s with libevent %s. "
        "You may want to use the latest version of libevent.", m, v);
    badness = "BUGGY";
  } else if (slow && server) {
    log(LOG_WARN, LD_GENERAL,
        "libevent %s can be very slow with %s. "
        "When running a server, please use the latest version of libevent.",
        v,m);
    badness = "SLOW";
  }

  *badness_out = badness;
}

#if defined(LIBEVENT_VERSION)
#define HEADER_VERSION LIBEVENT_VERSION
#elif defined(_EVENT_VERSION)
#define HEADER_VERSION _EVENT_VERSION
#endif

/** See whether the headers we were built against differ from the library we
 * linked against so much that we're likely to crash.  If so, warn the
 * user. */
void
tor_check_libevent_header_compatibility(void)
{
  (void) le_versions_compatibility;
  (void) tor_decode_libevent_version;

  /* In libevent versions before 2.0, it's hard to keep binary compatibility
   * between upgrades, and unpleasant to detect when the version we compiled
   * against is unlike the version we have linked against. Here's how. */
#if defined(HEADER_VERSION) && defined(HAVE_EVENT_GET_VERSION)
  /* We have a header-file version and a function-call version. Easy. */
  if (strcmp(HEADER_VERSION, event_get_version())) {
    le_version_t v1, v2;
    int compat1 = -1, compat2 = -1;
    int verybad;
    v1 = tor_decode_libevent_version(HEADER_VERSION);
    v2 = tor_decode_libevent_version(event_get_version());
    compat1 = le_versions_compatibility(v1);
    compat2 = le_versions_compatibility(v2);

    verybad = compat1 != compat2;

    log(verybad ? LOG_WARN : LOG_NOTICE,
        LD_GENERAL, "We were compiled with headers from version %s "
        "of Libevent, but we're using a Libevent library that says it's "
        "version %s.", HEADER_VERSION, event_get_version());
    if (verybad)
      log_warn(LD_GENERAL, "This will almost certainly make Tor crash.");
    else
      log_info(LD_GENERAL, "I think these versions are binary-compatible.");
  }
#elif defined(HAVE_EVENT_GET_VERSION)
  /* event_get_version but no _EVENT_VERSION.  We might be in 1.4.0-beta or
     earlier, where that's normal.  To see whether we were compiled with an
     earlier version, let's see whether the struct event defines MIN_HEAP_IDX.
  */
#ifdef HAVE_STRUCT_EVENT_MIN_HEAP_IDX
  /* The header files are 1.4.0-beta or later. If the version is not
   * 1.4.0-beta, we are incompatible. */
  {
    if (strcmp(event_get_version(), "1.4.0-beta")) {
      log_warn(LD_GENERAL, "It's a little hard to tell, but you seem to have "
               "Libevent 1.4.0-beta header files, whereas you have linked "
               "against Libevent %s.  This will probably make Tor crash.",
               event_get_version());
    }
  }
#else
  /* Our headers are 1.3e or earlier. If the library version is not 1.4.x or
     later, we're probably fine. */
  {
    const char *v = event_get_version();
    if ((v[0] == '1' && v[2] == '.' && v[3] > '3') || v[0] > '1') {
      log_warn(LD_GENERAL, "It's a little hard to tell, but you seem to have "
               "Libevent header file from 1.3e or earlier, whereas you have "
               "linked against Libevent %s.  This will probably make Tor "
               "crash.", event_get_version());
    }
  }
#endif

#elif defined(HEADER_VERSION)
#warn "_EVENT_VERSION is defined but not get_event_version(): Libevent is odd."
#else
  /* Your libevent is ancient. */
#endif
}

/*
  If possible, we're going to try to use Libevent's periodic timer support,
  since it does a pretty good job of making sure that periodic events get
  called exactly M seconds apart, rather than starting each one exactly M
  seconds after the time that the last one was run.
 */
#ifdef HAVE_EVENT2_EVENT_H
#define HAVE_PERIODIC
#define PERIODIC_FLAGS EV_PERSIST
#else
#define PERIODIC_FLAGS 0
#endif

/** Represents a timer that's run every N microseconds by Libevent. */
struct periodic_timer_t {
  /** Underlying event used to implement this periodic event. */
  struct event *ev;
  /** The callback we'll be invoking whenever the event triggers */
  void (*cb)(struct periodic_timer_t *, void *);
  /** User-supplied data for the callback */
  void *data;
#ifndef HAVE_PERIODIC
  /** If Libevent doesn't know how to invoke events every N microseconds,
   * we'll need to remember the timeout interval here. */
  struct timeval tv;
#endif
};

/** Libevent callback to implement a periodic event. */
static void
periodic_timer_cb(evutil_socket_t fd, short what, void *arg)
{
  periodic_timer_t *timer = arg;
  (void) what;
  (void) fd;
#ifndef HAVE_PERIODIC
  /** reschedule the event as needed. */
  event_add(timer->ev, &timer->tv);
#endif
  timer->cb(timer, timer->data);
}

/** Create and schedule a new timer that will run every <b>tv</b> in
 * the event loop of <b>base</b>.  When the timer fires, it will
 * run the timer in <b>cb</b> with the user-supplied data in <b>data</b>. */
periodic_timer_t *
periodic_timer_new(struct event_base *base,
                   const struct timeval *tv,
                   void (*cb)(periodic_timer_t *timer, void *data),
                   void *data)
{
  periodic_timer_t *timer;
  tor_assert(base);
  tor_assert(tv);
  tor_assert(cb);
  timer = tor_malloc_zero(sizeof(periodic_timer_t));
  if (!(timer->ev = tor_event_new(base, -1, PERIODIC_FLAGS,
                                  periodic_timer_cb, timer))) {
    tor_free(timer);
    return NULL;
  }
  timer->cb = cb;
  timer->data = data;
#ifndef HAVE_PERIODIC
  memcpy(&timer->tv, tv, sizeof(struct timeval));
#endif
  event_add(timer->ev, (struct timeval *)tv); /*drop const for old libevent*/
  return timer;
}

/** Stop and free a periodic timer */
void
periodic_timer_free(periodic_timer_t *timer)
{
  if (!timer)
    return;
  tor_event_free(timer->ev);
  tor_free(timer);
}

