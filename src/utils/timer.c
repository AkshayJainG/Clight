#include <sys/timerfd.h>
#include "timer.h"

static time_t get_timeout_sec(int fd);

int start_timer(int clockid, int initial_s, int initial_ns) {
    int timerfd = timerfd_create(clockid, TFD_NONBLOCK);
    if (timerfd == -1) {
        ERROR("timerfd_create() failed: %s\n", strerror(errno));
    } 
    set_timeout(initial_s, initial_ns, timerfd, 0);
    return timerfd;
}

/*
 * Helper to set a new trigger on timerfd in sec seconds and nsec nanoseconds
 */
void set_timeout(int sec, int nsec, int fd, int flag) {
    struct itimerspec timerValue = {{0}};

    if (sec < 0) {
        sec = 0;
    }
    timerValue.it_value.tv_sec = sec;
    timerValue.it_value.tv_nsec = nsec;
    int r = timerfd_settime(fd, flag, &timerValue, NULL);
    if (r == -1) {
        ERROR("timerfd_settime(%d) failed: %s\n", fd, strerror(errno));
    }
    if (flag == 0) {
        if (sec != 0 || nsec != 0) {
            DEBUG("Set timeout of %ds %dns on fd %d.\n", sec, nsec, fd);
        } else {
            DEBUG("Disarmed timerfd on fd %d.\n", fd);
        }
    }
}

static time_t get_timeout_sec(int fd) {
    struct itimerspec curr_value;
    if (timerfd_gettime(fd, &curr_value) == 0) {
        return curr_value.it_value.tv_sec;
    }
    WARN("timerfd_gettime(%d) failed: %s\n", fd, strerror(errno));
    return 0;
}

void reset_timer(int fd, int old_timer, int new_timer) {
    if (old_timer <= 0) {
        /* We had a paused fd; resume it */
        set_timeout(new_timer, 0, fd, 0);
    } else {
        time_t fd_timeout = get_timeout_sec(fd);
        if (fd_timeout == 0 && new_timer > 0) {
            /* 
             * fd was ready to fire; we are not pausing it 
             * thus we can safely let it fire.
             * Note: this happens after eg: a long night suspend
             */
            return; 
        }
    
        unsigned int elapsed_time = old_timer - fd_timeout;
        /* if we still need to wait some seconds */
        if (new_timer > elapsed_time) {
            set_timeout(new_timer - elapsed_time, 0, fd, 0);
        } else if (new_timer > 0) {
            /* with new timeout, old_timeout would already have elapsed */
            set_timeout(0, 1, fd, 0);
        } else {
            /* pause fd as a timeout <= 0 has been set */
            set_timeout(0, 0, fd, 0);
        }
    }
}

void read_timer(int fd) {
    uint64_t t;
    read(fd, &t, sizeof(uint64_t));
}
