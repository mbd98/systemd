/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/wait.h>

#include "sd-event.h"

#include "alloc-util.h"
#include "exec-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "log.h"
#include "macro.h"
#include "missing_syscall.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "random-util.h"
#include "rm-rf.h"
#include "signal-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "util.h"

static int prepare_handler(sd_event_source *s, void *userdata) {
        log_info("preparing %c", PTR_TO_INT(userdata));
        return 1;
}

static bool got_a, got_b, got_c, got_unref;
static unsigned got_d;

static int unref_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        sd_event_source_unref(s);
        got_unref = true;
        return 0;
}

static int io_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {

        log_info("got IO on %c", PTR_TO_INT(userdata));

        if (userdata == INT_TO_PTR('a')) {
                assert_se(sd_event_source_set_enabled(s, SD_EVENT_OFF) >= 0);
                assert_se(!got_a);
                got_a = true;
        } else if (userdata == INT_TO_PTR('b')) {
                assert_se(!got_b);
                got_b = true;
        } else if (userdata == INT_TO_PTR('d')) {
                got_d++;
                if (got_d < 2)
                        assert_se(sd_event_source_set_enabled(s, SD_EVENT_ONESHOT) >= 0);
                else
                        assert_se(sd_event_source_set_enabled(s, SD_EVENT_OFF) >= 0);
        } else
                assert_not_reached();

        return 1;
}

static int child_handler(sd_event_source *s, const siginfo_t *si, void *userdata) {

        assert_se(s);
        assert_se(si);

        assert_se(si->si_uid == getuid());
        assert_se(si->si_signo == SIGCHLD);
        assert_se(si->si_code == CLD_EXITED);
        assert_se(si->si_status == 78);

        log_info("got child on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('f'));

        assert_se(sd_event_exit(sd_event_source_get_event(s), 0) >= 0);
        sd_event_source_unref(s);

        return 1;
}

static int signal_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        sd_event_source *p = NULL;
        pid_t pid;
        siginfo_t plain_si;

        assert_se(s);
        assert_se(si);

        log_info("got signal on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('e'));

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, SIGUSR2, -1) >= 0);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                sigset_t ss;

                assert_se(sigemptyset(&ss) >= 0);
                assert_se(sigaddset(&ss, SIGUSR2) >= 0);

                zero(plain_si);
                assert_se(sigwaitinfo(&ss, &plain_si) >= 0);

                assert_se(plain_si.si_signo == SIGUSR2);
                assert_se(plain_si.si_value.sival_int == 4711);

                _exit(78);
        }

        assert_se(sd_event_add_child(sd_event_source_get_event(s), &p, pid, WEXITED, child_handler, INT_TO_PTR('f')) >= 0);
        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);
        assert_se(sd_event_source_set_child_process_own(p, true) >= 0);

        /* We can't use structured initialization here, since the structure contains various unions and these
         * fields lie in overlapping (carefully aligned) unions that LLVM is allergic to allow assignments
         * to */
        zero(plain_si);
        plain_si.si_signo = SIGUSR2;
        plain_si.si_code = SI_QUEUE;
        plain_si.si_pid = getpid();
        plain_si.si_uid = getuid();
        plain_si.si_value.sival_int = 4711;

        assert_se(sd_event_source_send_child_signal(p, SIGUSR2, &plain_si, 0) >= 0);

        sd_event_source_unref(s);

        return 1;
}

static int defer_handler(sd_event_source *s, void *userdata) {
        sd_event_source *p = NULL;

        assert_se(s);

        log_info("got defer on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('d'));

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGUSR1, -1) >= 0);

        assert_se(sd_event_add_signal(sd_event_source_get_event(s), &p, SIGUSR1, signal_handler, INT_TO_PTR('e')) >= 0);
        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);
        raise(SIGUSR1);

        sd_event_source_unref(s);

        return 1;
}

static bool do_quit;

static int time_handler(sd_event_source *s, uint64_t usec, void *userdata) {
        log_info("got timer on %c", PTR_TO_INT(userdata));

        if (userdata == INT_TO_PTR('c')) {

                if (do_quit) {
                        sd_event_source *p;

                        assert_se(sd_event_add_defer(sd_event_source_get_event(s), &p, defer_handler, INT_TO_PTR('d')) >= 0);
                        assert_se(sd_event_source_set_enabled(p, SD_EVENT_ONESHOT) >= 0);
                } else {
                        assert_se(!got_c);
                        got_c = true;
                }
        } else
                assert_not_reached();

        return 2;
}

static bool got_exit = false;

static int exit_handler(sd_event_source *s, void *userdata) {
        log_info("got quit handler on %c", PTR_TO_INT(userdata));

        got_exit = true;

        return 3;
}

static bool got_post = false;

static int post_handler(sd_event_source *s, void *userdata) {
        log_info("got post handler");

        got_post = true;

        return 2;
}

static void test_basic(bool with_pidfd) {
        sd_event *e = NULL;
        sd_event_source *w = NULL, *x = NULL, *y = NULL, *z = NULL, *q = NULL, *t = NULL;
        static const char ch = 'x';
        int a[2] = { -1, -1 }, b[2] = { -1, -1}, d[2] = { -1, -1}, k[2] = { -1, -1 };
        uint64_t event_now;
        int64_t priority;

        log_info("/* %s(pidfd=%s) */", __func__, yes_no(with_pidfd));

        assert_se(setenv("SYSTEMD_PIDFD", yes_no(with_pidfd), 1) >= 0);

        assert_se(pipe(a) >= 0);
        assert_se(pipe(b) >= 0);
        assert_se(pipe(d) >= 0);
        assert_se(pipe(k) >= 0);

        assert_se(sd_event_default(&e) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) > 0);

        assert_se(sd_event_set_watchdog(e, true) >= 0);

        /* Test whether we cleanly can destroy an io event source from its own handler */
        got_unref = false;
        assert_se(sd_event_add_io(e, &t, k[0], EPOLLIN, unref_handler, NULL) >= 0);
        assert_se(write(k[1], &ch, 1) == 1);
        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(got_unref);

        got_a = false, got_b = false, got_c = false, got_d = 0;

        /* Add a oneshot handler, trigger it, reenable it, and trigger
         * it again. */
        assert_se(sd_event_add_io(e, &w, d[0], EPOLLIN, io_handler, INT_TO_PTR('d')) >= 0);
        assert_se(sd_event_source_set_enabled(w, SD_EVENT_ONESHOT) >= 0);
        assert_se(write(d[1], &ch, 1) >= 0);
        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(got_d == 1);
        assert_se(write(d[1], &ch, 1) >= 0);
        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(got_d == 2);

        assert_se(sd_event_add_io(e, &x, a[0], EPOLLIN, io_handler, INT_TO_PTR('a')) >= 0);
        assert_se(sd_event_add_io(e, &y, b[0], EPOLLIN, io_handler, INT_TO_PTR('b')) >= 0);

        do_quit = false;
        assert_se(sd_event_add_time(e, &z, CLOCK_MONOTONIC, 0, 0, time_handler, INT_TO_PTR('c')) >= 0);
        assert_se(sd_event_add_exit(e, &q, exit_handler, INT_TO_PTR('g')) >= 0);

        assert_se(sd_event_source_set_priority(x, 99) >= 0);
        assert_se(sd_event_source_get_priority(x, &priority) >= 0);
        assert_se(priority == 99);
        assert_se(sd_event_source_set_enabled(y, SD_EVENT_ONESHOT) >= 0);
        assert_se(sd_event_source_set_prepare(x, prepare_handler) >= 0);
        assert_se(sd_event_source_set_priority(z, 50) >= 0);
        assert_se(sd_event_source_set_enabled(z, SD_EVENT_ONESHOT) >= 0);
        assert_se(sd_event_source_set_prepare(z, prepare_handler) >= 0);

        /* Test for floating event sources */
        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGRTMIN+1, -1) >= 0);
        assert_se(sd_event_add_signal(e, NULL, SIGRTMIN+1, NULL, NULL) >= 0);

        assert_se(write(a[1], &ch, 1) >= 0);
        assert_se(write(b[1], &ch, 1) >= 0);

        assert_se(!got_a && !got_b && !got_c);

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);

        assert_se(!got_a && got_b && !got_c);

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);

        assert_se(!got_a && got_b && got_c);

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);

        assert_se(got_a && got_b && got_c);

        sd_event_source_unref(x);
        sd_event_source_unref(y);

        do_quit = true;
        assert_se(sd_event_add_post(e, NULL, post_handler, NULL) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) == 0);
        assert_se(sd_event_source_set_time(z, event_now + 200 * USEC_PER_MSEC) >= 0);
        assert_se(sd_event_source_set_enabled(z, SD_EVENT_ONESHOT) >= 0);

        assert_se(sd_event_loop(e) >= 0);
        assert_se(got_post);
        assert_se(got_exit);

        sd_event_source_unref(z);
        sd_event_source_unref(q);

        sd_event_source_unref(w);

        sd_event_unref(e);

        safe_close_pair(a);
        safe_close_pair(b);
        safe_close_pair(d);
        safe_close_pair(k);

        assert_se(unsetenv("SYSTEMD_PIDFD") >= 0);
}

static void test_sd_event_now(void) {
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        uint64_t event_now;

        log_info("/* %s */", __func__);

        assert_se(sd_event_new(&e) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) > 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME, &event_now) > 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME_ALARM, &event_now) > 0);
        if (clock_boottime_supported()) {
                assert_se(sd_event_now(e, CLOCK_BOOTTIME, &event_now) > 0);
                assert_se(sd_event_now(e, CLOCK_BOOTTIME_ALARM, &event_now) > 0);
        }
        assert_se(sd_event_now(e, -1, &event_now) == -EOPNOTSUPP);
        assert_se(sd_event_now(e, 900 /* arbitrary big number */, &event_now) == -EOPNOTSUPP);

        assert_se(sd_event_run(e, 0) == 0);

        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &event_now) == 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME, &event_now) == 0);
        assert_se(sd_event_now(e, CLOCK_REALTIME_ALARM, &event_now) == 0);
        if (clock_boottime_supported()) {
                assert_se(sd_event_now(e, CLOCK_BOOTTIME, &event_now) == 0);
                assert_se(sd_event_now(e, CLOCK_BOOTTIME_ALARM, &event_now) == 0);
        }
        assert_se(sd_event_now(e, -1, &event_now) == -EOPNOTSUPP);
        assert_se(sd_event_now(e, 900 /* arbitrary big number */, &event_now) == -EOPNOTSUPP);
}

static int last_rtqueue_sigval = 0;
static int n_rtqueue = 0;

static int rtqueue_handler(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        last_rtqueue_sigval = si->ssi_int;
        n_rtqueue++;
        return 0;
}

static void test_rtqueue(void) {
        sd_event_source *u = NULL, *v = NULL, *s = NULL;
        sd_event *e = NULL;

        log_info("/* %s */", __func__);

        assert_se(sd_event_default(&e) >= 0);

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGRTMIN+2, SIGRTMIN+3, SIGUSR2, -1) >= 0);
        assert_se(sd_event_add_signal(e, &u, SIGRTMIN+2, rtqueue_handler, NULL) >= 0);
        assert_se(sd_event_add_signal(e, &v, SIGRTMIN+3, rtqueue_handler, NULL) >= 0);
        assert_se(sd_event_add_signal(e, &s, SIGUSR2, rtqueue_handler, NULL) >= 0);

        assert_se(sd_event_source_set_priority(v, -10) >= 0);

        assert_se(sigqueue(getpid_cached(), SIGRTMIN+2, (union sigval) { .sival_int = 1 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGRTMIN+3, (union sigval) { .sival_int = 2 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGUSR2, (union sigval) { .sival_int = 3 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGRTMIN+3, (union sigval) { .sival_int = 4 }) >= 0);
        assert_se(sigqueue(getpid_cached(), SIGUSR2, (union sigval) { .sival_int = 5 }) >= 0);

        assert_se(n_rtqueue == 0);
        assert_se(last_rtqueue_sigval == 0);

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(n_rtqueue == 1);
        assert_se(last_rtqueue_sigval == 2); /* first SIGRTMIN+3 */

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(n_rtqueue == 2);
        assert_se(last_rtqueue_sigval == 4); /* second SIGRTMIN+3 */

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(n_rtqueue == 3);
        assert_se(last_rtqueue_sigval == 3); /* first SIGUSR2 */

        assert_se(sd_event_run(e, UINT64_MAX) >= 1);
        assert_se(n_rtqueue == 4);
        assert_se(last_rtqueue_sigval == 1); /* SIGRTMIN+2 */

        assert_se(sd_event_run(e, 0) == 0); /* the other SIGUSR2 is dropped, because the first one was still queued */
        assert_se(n_rtqueue == 4);
        assert_se(last_rtqueue_sigval == 1);

        sd_event_source_unref(u);
        sd_event_source_unref(v);
        sd_event_source_unref(s);

        sd_event_unref(e);
}

#define CREATE_EVENTS_MAX (70000U)

struct inotify_context {
        bool delete_self_handler_called;
        unsigned create_called[CREATE_EVENTS_MAX];
        unsigned create_overflow;
        unsigned n_create_events;
};

static void maybe_exit(sd_event_source *s, struct inotify_context *c) {
        unsigned n;

        assert_se(s);
        assert_se(c);

        if (!c->delete_self_handler_called)
                return;

        for (n = 0; n < 3; n++) {
                unsigned i;

                if (c->create_overflow & (1U << n))
                        continue;

                for (i = 0; i < c->n_create_events; i++)
                        if (!(c->create_called[i] & (1U << n)))
                                return;
        }

        sd_event_exit(sd_event_source_get_event(s), 0);
}

static int inotify_handler(sd_event_source *s, const struct inotify_event *ev, void *userdata) {
        struct inotify_context *c = userdata;
        const char *description;
        unsigned bit, n;

        assert_se(sd_event_source_get_description(s, &description) >= 0);
        assert_se(safe_atou(description, &n) >= 0);

        assert_se(n <= 3);
        bit = 1U << n;

        if (ev->mask & IN_Q_OVERFLOW) {
                log_info("inotify-handler <%s>: overflow", description);
                c->create_overflow |= bit;
        } else if (ev->mask & IN_CREATE) {
                if (streq(ev->name, "sub"))
                        log_debug("inotify-handler <%s>: create on %s", description, ev->name);
                else {
                        unsigned i;

                        assert_se(safe_atou(ev->name, &i) >= 0);
                        assert_se(i < c->n_create_events);
                        c->create_called[i] |= bit;
                }
        } else if (ev->mask & IN_DELETE) {
                log_info("inotify-handler <%s>: delete of %s", description, ev->name);
                assert_se(streq(ev->name, "sub"));
        } else
                assert_not_reached();

        maybe_exit(s, c);
        return 1;
}

static int delete_self_handler(sd_event_source *s, const struct inotify_event *ev, void *userdata) {
        struct inotify_context *c = userdata;

        if (ev->mask & IN_Q_OVERFLOW) {
                log_info("delete-self-handler: overflow");
                c->delete_self_handler_called = true;
        } else if (ev->mask & IN_DELETE_SELF) {
                log_info("delete-self-handler: delete-self");
                c->delete_self_handler_called = true;
        } else if (ev->mask & IN_IGNORED) {
                log_info("delete-self-handler: ignore");
        } else
                assert_not_reached();

        maybe_exit(s, c);
        return 1;
}

static void test_inotify(unsigned n_create_events) {
        _cleanup_(rm_rf_physical_and_freep) char *p = NULL;
        sd_event_source *a = NULL, *b = NULL, *c = NULL, *d = NULL;
        struct inotify_context context = {
                .n_create_events = n_create_events,
        };
        sd_event *e = NULL;
        const char *q;
        unsigned i;

        log_info("/* %s(%u) */", __func__, n_create_events);

        assert_se(sd_event_default(&e) >= 0);

        assert_se(mkdtemp_malloc("/tmp/test-inotify-XXXXXX", &p) >= 0);

        assert_se(sd_event_add_inotify(e, &a, p, IN_CREATE|IN_ONLYDIR, inotify_handler, &context) >= 0);
        assert_se(sd_event_add_inotify(e, &b, p, IN_CREATE|IN_DELETE|IN_DONT_FOLLOW, inotify_handler, &context) >= 0);
        assert_se(sd_event_source_set_priority(b, SD_EVENT_PRIORITY_IDLE) >= 0);
        assert_se(sd_event_source_set_priority(b, SD_EVENT_PRIORITY_NORMAL) >= 0);
        assert_se(sd_event_add_inotify(e, &c, p, IN_CREATE|IN_DELETE|IN_EXCL_UNLINK, inotify_handler, &context) >= 0);
        assert_se(sd_event_source_set_priority(c, SD_EVENT_PRIORITY_IDLE) >= 0);

        assert_se(sd_event_source_set_description(a, "0") >= 0);
        assert_se(sd_event_source_set_description(b, "1") >= 0);
        assert_se(sd_event_source_set_description(c, "2") >= 0);

        q = strjoina(p, "/sub");
        assert_se(touch(q) >= 0);
        assert_se(sd_event_add_inotify(e, &d, q, IN_DELETE_SELF, delete_self_handler, &context) >= 0);

        for (i = 0; i < n_create_events; i++) {
                char buf[DECIMAL_STR_MAX(unsigned)+1];
                _cleanup_free_ char *z;

                xsprintf(buf, "%u", i);
                assert_se(z = path_join(p, buf));

                assert_se(touch(z) >= 0);
        }

        assert_se(unlink(q) >= 0);

        assert_se(sd_event_loop(e) >= 0);

        sd_event_source_unref(a);
        sd_event_source_unref(b);
        sd_event_source_unref(c);
        sd_event_source_unref(d);

        sd_event_unref(e);
}

static int pidfd_handler(sd_event_source *s, const siginfo_t *si, void *userdata) {
        assert_se(s);
        assert_se(si);

        assert_se(si->si_uid == getuid());
        assert_se(si->si_signo == SIGCHLD);
        assert_se(si->si_code == CLD_EXITED);
        assert_se(si->si_status == 66);

        log_info("got pidfd on %c", PTR_TO_INT(userdata));

        assert_se(userdata == INT_TO_PTR('p'));

        assert_se(sd_event_exit(sd_event_source_get_event(s), 0) >= 0);
        sd_event_source_unref(s);

        return 0;
}

static void test_pidfd(void) {
        sd_event_source *s = NULL, *t = NULL;
        sd_event *e = NULL;
        int pidfd;
        pid_t pid, pid2;

        log_info("/* %s */", __func__);

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, -1) >= 0);

        pid = fork();
        if (pid == 0)
                /* child */
                _exit(66);

        assert_se(pid > 1);

        pidfd = pidfd_open(pid, 0);
        if (pidfd < 0) {
                /* No pidfd_open() supported or blocked? */
                assert_se(ERRNO_IS_NOT_SUPPORTED(errno) || ERRNO_IS_PRIVILEGE(errno));
                (void) wait_for_terminate(pid, NULL);
                return;
        }

        pid2 = fork();
        if (pid2 == 0)
                freeze();

        assert_se(pid > 2);

        assert_se(sd_event_default(&e) >= 0);
        assert_se(sd_event_add_child_pidfd(e, &s, pidfd, WEXITED, pidfd_handler, INT_TO_PTR('p')) >= 0);
        assert_se(sd_event_source_set_child_pidfd_own(s, true) >= 0);

        /* This one should never trigger, since our second child lives forever */
        assert_se(sd_event_add_child(e, &t, pid2, WEXITED, pidfd_handler, INT_TO_PTR('q')) >= 0);
        assert_se(sd_event_source_set_child_process_own(t, true) >= 0);

        assert_se(sd_event_loop(e) >= 0);

        /* Child should still be alive */
        assert_se(kill(pid2, 0) >= 0);

        t = sd_event_source_unref(t);

        /* Child should now be dead, since we dropped the ref */
        assert_se(kill(pid2, 0) < 0 && errno == ESRCH);

        sd_event_unref(e);
}

static int ratelimit_io_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        unsigned *c = (unsigned*) userdata;
        *c += 1;
        return 0;
}

static int ratelimit_time_handler(sd_event_source *s, uint64_t usec, void *userdata) {
        int r;

        r = sd_event_source_set_enabled(s, SD_EVENT_ON);
        if (r < 0)
                log_warning_errno(r, "Failed to turn on notify event source: %m");

        r = sd_event_source_set_time(s, usec + 1000);
        if (r < 0)
                log_error_errno(r, "Failed to restart watchdog event source: %m");

        unsigned *c = (unsigned*) userdata;
        *c += 1;

        return 0;
}

static void test_ratelimit(void) {
        _cleanup_close_pair_ int p[2] = {-1, -1};
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        _cleanup_(sd_event_source_unrefp) sd_event_source *s = NULL;
        uint64_t interval;
        unsigned count, burst;

        log_info("/* %s */", __func__);

        assert_se(sd_event_default(&e) >= 0);
        assert_se(pipe2(p, O_CLOEXEC|O_NONBLOCK) >= 0);

        assert_se(sd_event_add_io(e, &s, p[0], EPOLLIN, ratelimit_io_handler, &count) >= 0);
        assert_se(sd_event_source_set_description(s, "test-ratelimit-io") >= 0);
        assert_se(sd_event_source_set_ratelimit(s, 1 * USEC_PER_SEC, 5) >= 0);
        assert_se(sd_event_source_get_ratelimit(s, &interval, &burst) >= 0);
        assert_se(interval == 1 * USEC_PER_SEC && burst == 5);

        assert_se(write(p[1], "1", 1) == 1);

        count = 0;
        for (unsigned i = 0; i < 10; i++) {
                log_debug("slow loop iteration %u", i);
                assert_se(sd_event_run(e, UINT64_MAX) >= 0);
                assert_se(usleep(250 * USEC_PER_MSEC) >= 0);
        }

        assert_se(sd_event_source_is_ratelimited(s) == 0);
        assert_se(count == 10);
        log_info("ratelimit_io_handler: called %d times, event source not ratelimited", count);

        assert_se(sd_event_source_set_ratelimit(s, 0, 0) >= 0);
        assert_se(sd_event_source_set_ratelimit(s, 1 * USEC_PER_SEC, 5) >= 0);

        count = 0;
        for (unsigned i = 0; i < 10; i++) {
                log_debug("fast event loop iteration %u", i);
                assert_se(sd_event_run(e, UINT64_MAX) >= 0);
                assert_se(usleep(10) >= 0);
        }
        log_info("ratelimit_io_handler: called %d times, event source got ratelimited", count);
        assert_se(count < 10);

        s = sd_event_source_unref(s);
        safe_close_pair(p);

        count = 0;
        assert_se(sd_event_add_time_relative(e, &s, CLOCK_MONOTONIC, 1000, 1, ratelimit_time_handler, &count) >= 0);
        assert_se(sd_event_source_set_ratelimit(s, 1 * USEC_PER_SEC, 10) == 0);

        do {
                assert_se(sd_event_run(e, UINT64_MAX) >= 0);
        } while (!sd_event_source_is_ratelimited(s));

        log_info("ratelimit_time_handler: called %d times, event source got ratelimited", count);
        assert_se(count == 10);

        /* In order to get rid of active rate limit client needs to disable it explicitly */
        assert_se(sd_event_source_set_ratelimit(s, 0, 0) >= 0);
        assert_se(!sd_event_source_is_ratelimited(s));

        assert_se(sd_event_source_set_ratelimit(s, 1 * USEC_PER_SEC, 10) >= 0);

        do {
                assert_se(sd_event_run(e, UINT64_MAX) >= 0);
        } while (!sd_event_source_is_ratelimited(s));

        log_info("ratelimit_time_handler: called 10 more times, event source got ratelimited");
        assert_se(count == 20);
}

static void test_simple_timeout(void) {
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        usec_t f, t, some_time;

        some_time = random_u64_range(2 * USEC_PER_SEC);

        assert_se(sd_event_default(&e) >= 0);

        assert_se(sd_event_prepare(e) == 0);

        f = now(CLOCK_MONOTONIC);
        assert_se(sd_event_wait(e, some_time) >= 0);
        t = now(CLOCK_MONOTONIC);

        /* The event loop may sleep longer than the specified time (timer accuracy, scheduling latencies, …),
         * but never shorter. Let's check that. */
        assert_se(t >= usec_add(f, some_time));
}

static int inotify_self_destroy_handler(sd_event_source *s, const struct inotify_event *ev, void *userdata) {
        sd_event_source **p = userdata;

        assert_se(ev);
        assert_se(p);
        assert_se(*p == s);

        assert_se(FLAGS_SET(ev->mask, IN_ATTRIB));

        assert_se(sd_event_exit(sd_event_source_get_event(s), 0) >= 0);

        *p = sd_event_source_unref(*p); /* here's what we actually intend to test: we destroy the event
                                         * source from inside the event source handler */
        return 1;
}

static void test_inotify_self_destroy(void) {
        _cleanup_(sd_event_source_unrefp) sd_event_source *s = NULL;
        _cleanup_(sd_event_unrefp) sd_event *e = NULL;
        char path[] = "/tmp/inotifyXXXXXX";
        _cleanup_close_ int fd = -1;

        /* Tests that destroying an inotify event source from its own handler is safe */

        assert_se(sd_event_default(&e) >= 0);

        fd = mkostemp_safe(path);
        assert_se(fd >= 0);
        assert_se(sd_event_add_inotify_fd(e, &s, fd, IN_ATTRIB, inotify_self_destroy_handler, &s) >= 0);
        fd = safe_close(fd);
        assert_se(unlink(path) >= 0); /* This will trigger IN_ATTRIB because link count goes to zero */
        assert_se(sd_event_loop(e) >= 0);
}

int main(int argc, char *argv[]) {
        test_setup_logging(LOG_DEBUG);

        test_simple_timeout();

        test_basic(true);   /* test with pidfd */
        test_basic(false);  /* test without pidfd */

        test_sd_event_now();
        test_rtqueue();

        test_inotify(100); /* should work without overflow */
        test_inotify(33000); /* should trigger a q overflow */

        test_pidfd();

        test_ratelimit();

        test_inotify_self_destroy();

        return 0;
}
