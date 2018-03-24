/* fdwatch.c - fd watcher routines, either select() or poll()
**
** Copyright � 1999,2000 by Jef Poskanzer <jef@mail.acme.com>.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
** OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
** HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
*/

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <syslog.h>
#include <fcntl.h>

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#else /* HAVE_POLL_H */
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif /* HAVE_SYS_POLL_H */
#endif /* HAVE_POLL_H */

#ifdef HAVE_SYS_DEVPOLL_H
#include <sys/devpoll.h>
#ifndef HAVE_DEVPOLL
#define HAVE_DEVPOLL
#endif /* !HAVE_DEVPOLL */
#endif /* HAVE_SYS_DEVPOLL_H */

#ifdef HAVE_SYS_EVENT_H
#include <sys/event.h>
#endif /* HAVE_SYS_EVENT_H */

#include "fdwatch.h"

#ifdef HAVE_SELECT
#ifndef FD_SET
#define NFDBITS         32
#define FD_SETSIZE      32
#define FD_SET(n, p)    ((p)->fds_bits[(n)/NFDBITS] |= (1 << ((n) % NFDBITS)))//每一位标志一个描述符是否设置
#define FD_CLR(n, p)    ((p)->fds_bits[(n)/NFDBITS] &= ~(1 << ((n) % NFDBITS)))//清除标志位
#define FD_ISSET(n, p)  ((p)->fds_bits[(n)/NFDBITS] & (1 << ((n) % NFDBITS)))//检查标志位
#define FD_ZERO(p)      bzero((char*)(p), sizeof(*(p)))
#endif /* !FD_SET */
#endif /* HAVE_SELECT */

static int nfiles;
static long nwatches;
static int* fd_rw;
static void** fd_data;
static int nreturned, next_ridx;

#ifdef HAVE_KQUEUE

#define WHICH                  "kevent"
#define INIT( nf )         kqueue_init( nf )
#define ADD_FD( fd, rw )       kqueue_add_fd( fd, rw )
#define DEL_FD( fd )           kqueue_del_fd( fd )
#define WATCH( timeout_msecs ) kqueue_watch( timeout_msecs )
#define CHECK_FD( fd )         kqueue_check_fd( fd )
#define GET_FD( ridx )         kqueue_get_fd( ridx )

static int kqueue_init( int nf );
static void kqueue_add_fd( int fd, int rw );
static void kqueue_del_fd( int fd );
static int kqueue_watch( long timeout_msecs );
static int kqueue_check_fd( int fd );
static int kqueue_get_fd( int ridx );

#else /* HAVE_KQUEUE */
# ifdef HAVE_DEVPOLL

#define WHICH                  "devpoll"
#define INIT( nf )         devpoll_init( nf )
#define ADD_FD( fd, rw )       devpoll_add_fd( fd, rw )
#define DEL_FD( fd )           devpoll_del_fd( fd )
#define WATCH( timeout_msecs ) devpoll_watch( timeout_msecs )
#define CHECK_FD( fd )         devpoll_check_fd( fd )
#define GET_FD( ridx )         devpoll_get_fd( ridx )

static int devpoll_init( int nf );
static void devpoll_add_fd( int fd, int rw );
static void devpoll_del_fd( int fd );
static int devpoll_watch( long timeout_msecs );
static int devpoll_check_fd( int fd );
static int devpoll_get_fd( int ridx );

# else /* HAVE_DEVPOLL */
#  ifdef HAVE_POLL

#define WHICH                  "poll"
#define INIT( nf )         poll_init( nf )
#define ADD_FD( fd, rw )       poll_add_fd( fd, rw )
#define DEL_FD( fd )           poll_del_fd( fd )
#define WATCH( timeout_msecs ) poll_watch( timeout_msecs )
#define CHECK_FD( fd )         poll_check_fd( fd )
#define GET_FD( ridx )         poll_get_fd( ridx )

static int poll_init( int nf );
static void poll_add_fd( int fd, int rw );
static void poll_del_fd( int fd );
static int poll_watch( long timeout_msecs );
static int poll_check_fd( int fd );
static int poll_get_fd( int ridx );

#  else /* HAVE_POLL */
#   ifdef HAVE_SELECT

#define WHICH                  "select"
#define INIT( nf )         select_init( nf )
#define ADD_FD( fd, rw )       select_add_fd( fd, rw )
#define DEL_FD( fd )           select_del_fd( fd )
#define WATCH( timeout_msecs ) select_watch( timeout_msecs )
#define CHECK_FD( fd )         select_check_fd( fd )
#define GET_FD( ridx )         select_get_fd( ridx )

static int select_init( int nf );
static void select_add_fd( int fd, int rw );
static void select_del_fd( int fd );
static int select_watch( long timeout_msecs );
static int select_check_fd( int fd );
static int select_get_fd( int ridx );

#   endif /* HAVE_SELECT */
#  endif /* HAVE_POLL */
# endif /* HAVE_DEVPOLL */
#endif /* HAVE_KQUEUE */


/* Routines. */

/* Figure out how many file descriptors the system allows, and
** initialize the fdwatch data structures.  Returns -1 on failure.
*/
int
fdwatch_get_nfiles( void )
    {
    int i;
#ifdef RLIMIT_NOFILE
    struct rlimit rl;
#endif /* RLIMIT_NOFILE */

    /* Figure out how many fd's we can have. */
    nfiles = getdtablesize();/*进程所能打开的最大文件数*/
#ifdef RLIMIT_NOFILE
    /* If we have getrlimit(), use that, and attempt to raise the limit. */
    if ( getrlimit( RLIMIT_NOFILE, &rl ) == 0 )/*进程所能打开的最大文件数*/
	{
	nfiles = rl.rlim_cur;
	if ( rl.rlim_max == RLIM_INFINITY )
	    rl.rlim_cur = 8192;         /* arbitrary */
	else if ( rl.rlim_max > rl.rlim_cur )
	    rl.rlim_cur = rl.rlim_max;
	if ( setrlimit( RLIMIT_NOFILE, &rl ) == 0 )/*尝试设置*/
	    nfiles = rl.rlim_cur;
	}
#endif /* RLIMIT_NOFILE */

#if defined(HAVE_SELECT) && ! ( defined(HAVE_POLL) || defined(HAVE_DEVPOLL) || defined(HAVE_KQUEUE) )
    /* If we use select(), then we must limit ourselves to FD_SETSIZE. */
    nfiles = MIN( nfiles, FD_SETSIZE );
#endif /* HAVE_SELECT && ! ( HAVE_POLL || HAVE_DEVPOLL || HAVE_KQUEUE ) */

    /* Initialize the fdwatch data structures. */
    nwatches = 0;
    fd_rw = (int*) malloc( sizeof(int) * nfiles );/*为每个文件描述符分配空间*/
    fd_data = (void**) malloc( sizeof(void*) * nfiles );/*为每个文件描述符创建指向数据的指针空间*/
    if ( fd_rw == (int*) 0 || fd_data == (void**) 0 )
	return -1;
    for ( i = 0; i < nfiles; ++i )
	fd_rw[i] = -1;/*初始化文件描述符数组为-1*/
    if ( INIT( nfiles ) == -1 )
	return -1;

    return nfiles;
    }


/* Add a descriptor to the watch list.  rw is either FDW_READ or FDW_WRITE.  */
void
fdwatch_add_fd( int fd, void* client_data, int rw )
    {
    if ( fd < 0 || fd >= nfiles || fd_rw[fd] != -1 )
	{
	syslog( LOG_ERR, "bad fd (%d) passed to fdwatch_add_fd!", fd );
	return;
	}
    ADD_FD( fd, rw );//添加到描述符集
    fd_rw[fd] = rw;//记录是关注读还是写
    fd_data[fd] = client_data;//存储连接数据
    }


/* Remove a descriptor from the watch list. */
void
fdwatch_del_fd( int fd )
    {
    if ( fd < 0 || fd >= nfiles || fd_rw[fd] == -1 )
	{
	syslog( LOG_ERR, "bad fd (%d) passed to fdwatch_del_fd!", fd );
	return;
	}
    DEL_FD( fd );
    fd_rw[fd] = -1;
    fd_data[fd] = (void*) 0;
    }

/* Do the watch.  Return value is the number of descriptors that are ready,
** or 0 if the timeout expired, or -1 on errors.  A timeout of INFTIM means
** wait indefinitely.
*/
int
fdwatch( long timeout_msecs )
    {
    ++nwatches;
    nreturned = WATCH( timeout_msecs );//根据时间确定监控函数是立即返回还是等待一定时间
    next_ridx = 0;
    return nreturned;
    }


/* Check if a descriptor was ready. */
int
fdwatch_check_fd( int fd )
    {
    if ( fd < 0 || fd >= nfiles || fd_rw[fd] == -1 )
	{
	syslog( LOG_ERR, "bad fd (%d) passed to fdwatch_check_fd!", fd );
	return 0;
	}
    return CHECK_FD( fd );//检查描述符是否已添加
    }


void*
fdwatch_get_next_client_data( void )
    {
    int fd;

    if ( next_ridx >= nreturned )
	return (void*) -1;
    fd = GET_FD( next_ridx++ );
    if ( fd < 0 || fd >= nfiles )
	return (void*) 0;
    return fd_data[fd];
    }


/* Generate debugging statistics syslog message. */
void
fdwatch_logstats( long secs )//记录描述符情况
    {
    if ( secs > 0 )
	syslog(
	    LOG_NOTICE, "  fdwatch - %ld %ss (%g/sec)",
	    nwatches, WHICH, (float) nwatches / secs );
    nwatches = 0;
    }


#ifdef HAVE_KQUEUE

static int maxkqevents;
static struct kevent* kqevents;
static int nkqevents;
static struct kevent* kqrevents;
static int* kqrfdidx;
static int kq;


static int
kqueue_init( int nf )
    {
    kq = kqueue();
    if ( kq == -1 )
	return -1;
    maxkqevents = nf * 2;
    kqevents = (struct kevent*) malloc( sizeof(struct kevent) * maxkqevents );
    kqrevents = (struct kevent*) malloc( sizeof(struct kevent) * nf );
    kqrfdidx = (int*) malloc( sizeof(int) * nf );
    if ( kqevents == (struct kevent*) 0 || kqrevents == (struct kevent*) 0 ||
	 kqrfdidx == (int*) 0 )
	return -1;
    (void) memset( kqevents, 0, sizeof(struct kevent) * maxkqevents );
    (void) memset( kqrfdidx, 0, sizeof(int) * nf );
    return 0;
    }


static void
kqueue_add_fd( int fd, int rw )
    {
    if ( nkqevents >= maxkqevents )
	{
	syslog( LOG_ERR, "too many kqevents in kqueue_add_fd!" );
	return;
	}
    kqevents[nkqevents].ident = fd;
    kqevents[nkqevents].flags = EV_ADD;
    switch ( rw )
	{
	case FDW_READ: kqevents[nkqevents].filter = EVFILT_READ; break;
	case FDW_WRITE: kqevents[nkqevents].filter = EVFILT_WRITE; break;
	default: break;
	}
    ++nkqevents;
    }


static void
kqueue_del_fd( int fd )
    {
    if ( nkqevents >= maxkqevents )
	{
	syslog( LOG_ERR, "too many kqevents in kqueue_del_fd!" );
	return;
	}
    kqevents[nkqevents].ident = fd;
    kqevents[nkqevents].flags = EV_DELETE;
    switch ( fd_rw[fd] )
	{
	case FDW_READ: kqevents[nkqevents].filter = EVFILT_READ; break;
	case FDW_WRITE: kqevents[nkqevents].filter = EVFILT_WRITE; break;
	}
    ++nkqevents;
    }


static int
kqueue_watch( long timeout_msecs )
    {
    int i, r;

    if ( timeout_msecs == INFTIM )
	r = kevent(
	    kq, kqevents, nkqevents, kqrevents, nfiles, (struct timespec*) 0 );
    else
	{
	struct timespec ts;
	ts.tv_sec = timeout_msecs / 1000L;
	ts.tv_nsec = ( timeout_msecs % 1000L ) * 1000000L;
	r = kevent( kq, kqevents, nkqevents, kqrevents, nfiles, &ts );
	}
    nkqevents = 0;
    if ( r == -1 )
	return -1;

    for ( i = 0; i < r; ++i )
	kqrfdidx[kqrevents[i].ident] = i;

    return r;
    }


static int
kqueue_check_fd( int fd )
    {
    int ridx = kqrfdidx[fd];

    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in kqueue_check_fd!", ridx );
	return 0;
	}
    if ( ridx >= nreturned ) 
	return 0;
    if ( kqrevents[ridx].ident != fd )
	return 0;
    if ( kqrevents[ridx].flags & EV_ERROR )
	return 0;
    switch ( fd_rw[fd] )
	{
	case FDW_READ: return kqrevents[ridx].filter == EVFILT_READ;
	case FDW_WRITE: return kqrevents[ridx].filter == EVFILT_WRITE;
	default: return 0;
	}
    }


static int
kqueue_get_fd( int ridx )
    {
    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in kqueue_get_fd!", ridx );
	return -1;
	}
    return kqrevents[ridx].ident;
    }

#else /* HAVE_KQUEUE */


# ifdef HAVE_DEVPOLL

static int maxdpevents;
static struct pollfd* dpevents;
static int ndpevents;
static struct pollfd* dprevents;
static int* dp_rfdidx;
static int dp;


static int
devpoll_init( int nf )
    {
    dp = open( "/dev/poll", O_RDWR );
    if ( dp == -1 )
	return -1;
    (void) fcntl( dp, F_SETFD, 1 );
    maxdpevents = nf * 2;
    dpevents = (struct pollfd*) malloc( sizeof(struct pollfd) * maxdpevents );
    dprevents = (struct pollfd*) malloc( sizeof(struct pollfd) * nf );
    dp_rfdidx = (int*) malloc( sizeof(int) * nf );
    if ( dpevents == (struct pollfd*) 0 || dprevents == (struct pollfd*) 0 ||
	 dp_rfdidx == (int*) 0 )
	return -1;
    (void) memset( dp_rfdidx, 0, sizeof(int) * nf );
    return 0;
    }


static void
devpoll_add_fd( int fd, int rw )
    {
    if ( ndpevents >= maxdpevents )
	{
	syslog( LOG_ERR, "too many fds in devpoll_add_fd!" );
	return;
	}
    dpevents[ndpevents].fd = fd;
    switch ( rw )
	{
	case FDW_READ: dpevents[ndpevents].events = POLLIN; break;
	case FDW_WRITE: dpevents[ndpevents].events = POLLOUT; break;
	default: break;
	}
    ++ndpevents;
    }


static void
devpoll_del_fd( int fd )
    {
    if ( ndpevents >= maxdpevents )
	{
	syslog( LOG_ERR, "too many fds in devpoll_del_fd!" );
	return;
	}
    dpevents[ndpevents].fd = fd;
    dpevents[ndpevents].events = POLLREMOVE;
    ++ndpevents;
    }


static int
devpoll_watch( long timeout_msecs )
    {
    int i, r;
    struct dvpoll dvp;

    r = sizeof(struct pollfd) * ndpevents;
    if ( r > 0 && write( dp, dpevents, r ) != r )
	return -1;

    ndpevents = 0;
    dvp.dp_fds = dprevents;
    dvp.dp_nfds = nfiles;
    dvp.dp_timeout = (int) timeout_msecs;

    r = ioctl( dp, DP_POLL, &dvp );
    if ( r == -1 )
	return -1;

    for ( i = 0; i < r; ++i )
	dp_rfdidx[dprevents[i].fd] = i;

    return r;
    }


static int
devpoll_check_fd( int fd )
    {
    int ridx = dp_rfdidx[fd];

    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in devpoll_check_fd!", ridx );
	return 0;
	}
    if ( ridx >= nreturned )
	return 0;
    if ( dprevents[ridx].fd != fd )
	return 0;
    if ( dprevents[ridx].revents & POLLERR )
	return 0;
    switch ( fd_rw[fd] )
	{
	case FDW_READ: return dprevents[ridx].revents & ( POLLIN | POLLHUP | POLLNVAL );
	case FDW_WRITE: return dprevents[ridx].revents & ( POLLOUT | POLLHUP | POLLNVAL );
	default: return 0;
	}
    }


static int
devpoll_get_fd( int ridx )
    {
    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in devpoll_get_fd!", ridx );
	return -1;
	}
    return dprevents[ridx].fd;
    }


# else /* HAVE_DEVPOLL */


#  ifdef HAVE_POLL

static struct pollfd* pollfds;
static int npoll_fds;
static int* poll_fdidx;
static int* poll_rfdidx;


static int
poll_init( int nf )
    {
    int i;

    pollfds = (struct pollfd*) malloc( sizeof(struct pollfd) * nf );
    poll_fdidx = (int*) malloc( sizeof(int) * nf );
    poll_rfdidx = (int*) malloc( sizeof(int) * nf );
    if ( pollfds == (struct pollfd*) 0 || poll_fdidx == (int*) 0 ||
	 poll_rfdidx == (int*) 0 )
	return -1;
    for ( i = 0; i < nf; ++i )
	pollfds[i].fd = poll_fdidx[i] = -1;
    return 0;
    }


static void
poll_add_fd( int fd, int rw )
    {
    if ( npoll_fds >= nfiles )
	{
	syslog( LOG_ERR, "too many fds in poll_add_fd!" );
	return;
	}
    pollfds[npoll_fds].fd = fd;
    switch ( rw )
	{
	case FDW_READ: pollfds[npoll_fds].events = POLLIN; break;
	case FDW_WRITE: pollfds[npoll_fds].events = POLLOUT; break;
	default: break;
	}
    poll_fdidx[fd] = npoll_fds;
    ++npoll_fds;
    }


static void
poll_del_fd( int fd )
    {
    int idx = poll_fdidx[fd];

    if ( idx < 0 || idx >= nfiles )
	{
	syslog( LOG_ERR, "bad idx (%d) in poll_del_fd!", idx );
	return;
	}
    --npoll_fds;
    pollfds[idx] = pollfds[npoll_fds];
    poll_fdidx[pollfds[idx].fd] = idx;
    pollfds[npoll_fds].fd = -1;
    poll_fdidx[fd] = -1;
    }


static int
poll_watch( long timeout_msecs )
    {
    int r, ridx, i;

    r = poll( pollfds, npoll_fds, (int) timeout_msecs );
    if ( r <= 0 )
	return r;

    ridx = 0;
    for ( i = 0; i < npoll_fds; ++i )
	if ( pollfds[i].revents &
	     ( POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL ) )
	    {
	    poll_rfdidx[ridx++] = pollfds[i].fd;
	    if ( ridx == r )
		break;
	    }

    return ridx;	/* should be equal to r */
    }


static int
poll_check_fd( int fd )
    {
    int fdidx = poll_fdidx[fd];

    if ( fdidx < 0 || fdidx >= nfiles )
	{
	syslog( LOG_ERR, "bad fdidx (%d) in poll_check_fd!", fdidx );
	return 0;
	}
    if ( pollfds[fdidx].revents & POLLERR )
	return 0;
    switch ( fd_rw[fd] )
	{
	case FDW_READ: return pollfds[fdidx].revents & ( POLLIN | POLLHUP | POLLNVAL );
	case FDW_WRITE: return pollfds[fdidx].revents & ( POLLOUT | POLLHUP | POLLNVAL );
	default: return 0;
	}
    }


static int
poll_get_fd( int ridx )
    {
    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in poll_get_fd!", ridx );
	return -1;
	}
    return poll_rfdidx[ridx];
    }

#  else /* HAVE_POLL */


#   ifdef HAVE_SELECT

static fd_set master_rfdset;
static fd_set master_wfdset;
static fd_set working_rfdset;
static fd_set working_wfdset;
static int* select_fds;
static int* select_fdidx;/*描述符索引数组*/
static int* select_rfdidx;/*就绪描述符*/
static int nselect_fds;/*当前文件描述符个数*/
static int maxfd;/*当前最大的描述符*/
static int maxfd_changed;


static int
select_init( int nf )
    {
    int i;

    FD_ZERO( &master_rfdset );
    FD_ZERO( &master_wfdset );
    select_fds = (int*) malloc( sizeof(int) * nf );
    select_fdidx = (int*) malloc( sizeof(int) * nf );
    select_rfdidx = (int*) malloc( sizeof(int) * nf );
    if ( select_fds == (int*) 0 || select_fdidx == (int*) 0 ||
	 select_rfdidx == (int*) 0 )
	return -1;
    nselect_fds = 0;
    maxfd = -1;
    maxfd_changed = 0;
    for ( i = 0; i < nf; ++i )
	select_fds[i] = select_fdidx[i] = -1;
    return 0;
    }


static void
select_add_fd( int fd, int rw )
    {
    if ( nselect_fds >= nfiles )/*文件描述符数不能超过限制*/
	{
	syslog( LOG_ERR, "too many fds in select_add_fd!" );
	return;
	}
    select_fds[nselect_fds] = fd;
    switch ( rw )
	{
	case FDW_READ: FD_SET( fd, &master_rfdset ); break;/*根据读写将文件描述符加入到对应的描述符集中*/
	case FDW_WRITE: FD_SET( fd, &master_wfdset ); break;
	default: break;
	}
    if ( fd > maxfd )/*修改最大的文件描述符*/
	maxfd = fd;
    select_fdidx[fd] = nselect_fds;/*文件描述符的索引*/
    ++nselect_fds;
    }


static void
select_del_fd( int fd )
    {
    int idx = select_fdidx[fd];/*取得文件描述符索引*/

    if ( idx < 0 || idx >= nfiles )
	{
	syslog( LOG_ERR, "bad idx (%d) in select_del_fd!", idx );
	return;
	}

    --nselect_fds;
    select_fds[idx] = select_fds[nselect_fds];/*将最后一个文件描述符替换到被删除的文件描述符位置*/
    select_fdidx[select_fds[idx]] = idx;/*修改索引*/
    select_fds[nselect_fds] = -1;/*将最后一个文件描述符置为初始化状态*/
    select_fdidx[fd] = -1;

    FD_CLR( fd, &master_rfdset );/*从描述符集中清除掉该描述符*/
    FD_CLR( fd, &master_wfdset );

    if ( fd >= maxfd )/*如果该描述符为最大描述符，置最大描述符修改标志为true*/
	maxfd_changed = 1;
    }


static int
select_get_maxfd( void )/*返回最大的文件描述符，如果修改标志为true，那么先搜寻最大的文件描述符，然后在返回*/
    {
    if ( maxfd_changed )
	{
	int i;
	maxfd = -1;
	for ( i = 0; i < nselect_fds; ++i )
	    if ( select_fds[i] > maxfd )
		maxfd = select_fds[i];
	maxfd_changed = 0;
	}
    return maxfd;
    }


static int
select_watch( long timeout_msecs )
    {
    int mfd;
    int r, idx, ridx;

    working_rfdset = master_rfdset;/*监听的读描述符集*/
    working_wfdset = master_wfdset;/*监听的写描述符集*/
    mfd = select_get_maxfd();/*得到当前最大的描述度，select函数需*/
    if ( timeout_msecs == INFTIM )/*不等待，立即返回*/
       r = select(
           mfd + 1, &working_rfdset, &working_wfdset, (fd_set*) 0,
           (struct timeval*) 0 );
    else/*等待一段时间*/
	{
	struct timeval timeout;
	timeout.tv_sec = timeout_msecs / 1000L;
	timeout.tv_usec = ( timeout_msecs % 1000L ) * 1000L;
	r = select(
	   mfd + 1, &working_rfdset, &working_wfdset, (fd_set*) 0, &timeout );
	}
    if ( r <= 0 )/*超时为0，出错为-1*/
	return r;

    ridx = 0;
    for ( idx = 0; idx < nselect_fds; ++idx )/*所有描述符轮询一遍*/
	if ( select_check_fd( select_fds[idx] ) )/*就绪状态的描述符*/
	    {
	    select_rfdidx[ridx++] = select_fds[idx];/*记录就绪状态的描述符索引*/
	    if ( ridx == r )/*找到所有就绪描述符，跳出循环*/
		break;
	    }

    return ridx;	/* should be equal to r */ /*返回就绪描述符个数*/
    }


static int
select_check_fd( int fd )
    {
    switch ( fd_rw[fd] )
	{
	case FDW_READ: return FD_ISSET( fd, &working_rfdset );/*检查描述符集中是否有该描述符*/
	case FDW_WRITE: return FD_ISSET( fd, &working_wfdset );
	default: return 0;
	}
    }


static int
select_get_fd( int ridx )
    {
    if ( ridx < 0 || ridx >= nfiles )
	{
	syslog( LOG_ERR, "bad ridx (%d) in select_get_fd!", ridx );
	return -1;
	}
    return select_rfdidx[ridx];/*根据索引返回就绪描述符*/
    }

#   endif /* HAVE_SELECT */

#  endif /* HAVE_POLL */

# endif /* HAVE_DEVPOLL */

#endif /* HAVE_KQUEUE */
