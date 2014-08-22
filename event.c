/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include "misc.h"
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else 
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* 编译器通过这些宏定义将系统提供的I/O复用机制都保存到该数组中
 * 注意, 有些系统可能存在多种机制, 比如linux系统就同时存在
 * select, poll及epoll等机制, 按照效率分别排序保存在数组中
 * 后面初始化的时候会从头遍历该数组, 初始化选择的机制, 在存在
 * 多种IO机制的系统中, 如果前面的初始化失败, 则会继续尝试初始化
 * 下一种类型的机制
 */
/* In order of preference */
const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	
	/* evport模型*/
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE

	/* BSD支持的kqueue模型 */
	&kqops,
#endif
#ifdef HAVE_EPOLL

	/* epoll模型 */
	&epollops,
#endif
#ifdef HAVE_DEVPOLL

	/* dev/poll模型 */
	&devpollops,
#endif
#ifdef HAVE_POLL

	/* poll模型 */
	&pollops,
#endif
#ifdef HAVE_SELECT

	/* select模型 */
	&selectops,
#endif
#ifdef WIN32

	/* windows的iocp模型 */
	&win32ops,
#endif

	/* 这里加一个NULL指针式为了方便遍历数组时的停止条件, 这样for循环就不需要知道数组的大小 */
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

/* 检测系统是否支持monotonic时间(即系统自boot以来的时间)
 *  在定时事件中优先选用这个系统时间, 因为这个是最准确的, 如果采用系统时间的话,
 * 需要校准定时器时间, 因为用户可能随时调整了系统时间, 导致定时事件时间不准确
 */
static void
detect_monotonic(void)
{
/* 通过这两个宏确定是否支持获取系统的monotonic时间 */
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}

/* 获取系统当前的时间 */
static int
gettime(struct timeval *tp)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	/* 如果支持monotonic时间, 直接获取monotonic时间即可 */
	if (use_monotonic) {
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	/* 否则只要能获取系统的UTC时间(这个时间可能是不准的, 后面需要进行校准测试) */
	return (gettimeofday(tp, NULL));
}

/* 初始化libevent-api, 内部调用event_base_new创建一个event_base实例
 * 并将该实例赋值给全局指针current_base, 作为默认的event_base实例, 
 * 在事件没有指定关联的event_base时会使用此默认的event_base
 */
struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

/***
 * 创建一个libevent实例(即创建一个event_base结构), 并进行初始化
 * @return: 成功, 返回新创建的event_base实例, 失败, 返回NULL
 */
struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	/* 为event_base分配内存, 同malloc(1*sizeof(struct event_base)), 区别在于前者会内存内容会初始化为0 */
	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	event_sigcb = NULL;
	event_gotsig = 0;

	/* 检测系统是否支持monotonic时间, 如果支持的就会置位全局的monotonic时间标记, 后面获取系统
	 * 时间都会使用monotonic时间*/
	detect_monotonic();
	gettime(&base->event_tv);
	

	/* 构造一个最小堆, 用于定时事件的时间管理 */
	min_heap_ctor(&base->timeheap);

	/* 初始化已注册事件队列 */
	TAILQ_INIT(&base->eventqueue);

	/* 初始化信号事件队列 */
	TAILQ_INIT(&base->sig.signalqueue);

	/* 信号捕获通知的socket_pair初始化为无效值 */
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;
	
	/* 这里是选择最合适的I/O复用机制*/
	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		/* 注意这里初始化机制失败的话, for循环会继续尝试下一种机制 */
		base->evbase = base->evsel->init(base);
	}

	/* 如果所有可能的机制都失败, 则报错, 并直接退出进程(内部会调用exit(1)退出进程) */
	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	/* 如果定义了EVENT_SHOW_METHOD环境变量, 则日志记录使用的IO机制的名称 */
	if (getenv("EVENT_SHOW_METHOD")) 
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* 初始化已就绪IO事件队列 */
	/* allocate a single active event queue */
	event_base_priority_init(base, 1);

	return (base);
}

/***
 * 释放event_base实例
 * @base[IN]: event_base对象, 如果传入NULL值, 则删除默认的event_base
 */
void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */

	/* 删除已注册的所有非内部I/O事件 */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}

	/* 删除所有的定时事件 */
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	/* 这里提示删除event_base时还存在多少注册的未就绪的事件 */
	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
					 __func__, n_deleted));

	/* 释放初始化创建的特定I/O复用机制 */
	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	/* ??????????????????????? */
	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	/* 此时定时事件堆此时肯定时空的, 将堆数据结构释放 */
	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	/* 释放各个已就绪IO事件优先级列表 */
	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);

	/* 释放已就绪列表本身 */
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);
	base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

/***
 * 初始化或者修改event_base中的激活IO事件优先级列表
 * @base[IN]: event_base实例
 * @npriorities[IN]: 总共的优先级数
 * @return: 成功返回0, 失败返回-1
 */
int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;

	/* 如果event_base中已经存在激活的IO事件, 则不允许该操作 */
	if (base->event_count_active)
		return (-1);

	/* 如果之前已经有存在优先级队列, 并且本次修改需要的优先级队列数与之前的数目不同, 则先释放之前的队列 */
	if (base->nactivequeues && npriorities != base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			/* 依次释放每个优先级队列 */
			free(base->activequeues[i]);
		}

		/* 最后释放之前分配的整个优先级队列动态数组空间 */
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
	base->activequeues = (struct event_list **)calloc(base->nactivequeues,
	    npriorities * sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

/* 判断event_base中是否存在注册的I/O事件 */
int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */

/***
 * 处理event_base(Reactor组件)中所有激活的事件(包括I/O事件, 信号事件及定时事件), 
 * 正是在这里调用上层用户提供的事件处理回调
 */
static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;

	/* 每次仅仅处理优先级最高的就绪事件, 次优先级的事件会在后续的调用中处理(下次poll操作),
	 * 这样就可以保证优先级高的总被优先处理 
	 */
	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	/* 进入这个函数说明至少有一个事件就绪, 所以这里可以断言, 否则就是逻辑错误 */
	assert(activeq != NULL);

	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);
		
		/* Allows deletes to work */
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
			if (event_gotsig || base->event_break)
				return;
		}
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

/* 获取选用I/O复用机制名称, 如"select", "epoll"等 */
const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */
/***
 * 针对默认的全局event_base启动事件循环, event_base_dispatch可以通过参数指定特定的event_base
 * 它们的区别仅仅在于针对全局event_base还是指定的event_base
 * @flags[IN]: 
 */
int
event_loop(int flags)
{
	/* current_base为全局默认的event_base, 必须事先调用event_init才会初始化current_base */
	return event_base_loop(current_base, flags);
}

/***
 * 框架底层统一的事件循环函数, event_base_dispatch和event_loop都调用该函数
 * 该函数不会返回, 除非满足下列条件之一:
 * 1)、没有了监控的事件
 * 2)、调用了event_base_loopbreak或event_base_loopexit
 * 3)、出现错误
 *
 * @base[IN]: 事件循环的Reactor组件
 * @flags[IN]: 行为控制
 * 			   EVLOOP_ONCE: 阻塞直到有一个活跃的event, 然后执行完活跃的事件回调就退出
 *			   EVLOOP_NONBLOCK: 不阻塞, 检查哪个事件准备好, 调用优先级最高的那个然后推出
 * @return: 0->正常退出, -1->发生错误, 1->没有了注册的事件了
 *
 */
int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	if(!TAILQ_EMPTY(&base->sig.signalqueue))
		evsignal_base = base;
	done = 0;
	while (!done) {
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		/* 应用程序调用event_base_loopbreak指示推出事件处理循环, 属于正常退出 */
		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}

		/* 校准系统时间 */
		timeout_correct(base, &tv);

		tv_p = &tv;
		
		/* 如果没有激活的事件并且不是非阻塞式的行为, 则计算IO等待的事件, 实际上是
		 * 定时器下一个超时还有多长时间 
		 */
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			timeout_next(base, &tv_p);
		} else {
			/* 如果有激活事件或者处于非阻塞模式下, 则IO调用不需要等待 */
			/* 
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);
		}
		
		/* 如果没有了注册的事件, 则退出事件循环 */
		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* 调用具体的I/O复用机制的等待函数, 如调用select */
		res = evsel->dispatch(base, evbase, tv_p);

		/* IO调用返回-1表示调用出错, 这是严重的错误, 必须退出事件循环 */
		if (res == -1)
			return (-1);

		/* 开始处理可能的超时事件 */
		timeout_process(base);

		/* 如果此处poll后有激活(即就绪)的I/O事件*/
		if (base->event_count_active) {
			/* 处理激活的I/O事件 */
			event_process_active(base);
			
			/* 如果没有了激活事件, 并且采用EVLOOP_ONCE标记, 则退出事件循环 */
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK) {
			/* 此次poll没有激活事件, 并且设置了非阻塞标志, 则正常退出事件循环 */
			done = 1;
		}
	}

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;

	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}

/***
 * 设置事件, 相当于定义一个事件各方面的属性
 * @ev[IN]: 事件结构体
 * @fd[IN]: 事件关联的"描述符":
 *			对于I/O事件来说, 是IO文件描述符
 *			对于信号事件来说, 是操作系统定义的具体的信号类型值
 *			对于定时事件来说, 传入-1
 * @events[IN]: 该事件类型, 包括EV_SIGNAL, EV_READ, EV_WRITE, EV_TIMER以及EV_PERSIST
 * @callback[IN]: 应用层定义的事件就绪回调函数
 * @arg[IN]: 应用层回调函数参数
 */
void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	/* 初始化事件时, 将event关联的event_base设置为全局的默认event_base
	 * 后期, 用户可以调用event_base_set来设置自己想要的event_base, 一般情况下, 
	 * 使用默认的event_base就可以了
	 */
	ev->ev_base = current_base;

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	/* 默认情况下, 将新的事件的优先级定为当前所有优先级的中等 */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}

/***
 * 设置事件的event_base, 注意, 只有还未注册的事件才可以设置ev->ev_flags != EVLIST_INIT
 * @base[IN]: event_base实例
 * @ev[IN]: 要设置的事件
 * @return: 成功返回0, 失败-1
 */
int
event_base_set(struct event_base *base, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */

	/* 只有调用了event_set初始化的事件, 并且还没有注册的事件才可以设置 */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;

	/* 优先级任然是新设置的event_base中的中等优先级 */
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

/***
 * 设置事件的优先级, 如果不调用该函数设置, 则默认是添加事件时所有已有优先级的中间值
 * 注意: 只有I/O事件才有所谓的优先级, 定时和信号事件都没有
 * @ev[IN]: 要设置的事件
 * @pri[IN]: 优先级值
 * @return: 成功返回0, 失败-1
 */
int
event_priority_set(struct event *ev, int pri)
{
	/* 如果IO事件已经在激活列表中, 则无法设置 */
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);

	/* 优先级值不能小于0, 并且不能大于当前event_base中最大的优先级值 */
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

/***
 * 判断某个事件是否处于等待状态
 */
int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	/* 首先判断该事件是何种事件 */

	/* 如果事件在已注册列表中 */
	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE));

	/* 如果在激活IO事件列表中 */
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;

	/* 如果是在定时事件列表中 */
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	/* 如果在信号事件类表中 */
	if (ev->ev_flags & EVLIST_SIGNAL)
		flags |= EV_SIGNAL;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	/* 如果是定时事件, 并且函数参数给出了有效时间值 */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(&now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

/***
 * 注册事件
 */
int
event_add(struct event *ev, struct timeval *tv)
{
	struct event_base *base = ev->ev_base;
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* 如果tv参数不是NULL, 则说明注册一个定时事件, tv表示定时的时间值 */
	if (tv != NULL) {
		struct timeval now;

		/* 如果该定时时间已经在定时事件列表中, 则首先将该定时事件中列表中删除 */
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);
		else if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
		    return (-1);  /* ENOMEM == errno */

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}
			
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

		gettime(&now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
			 "event_add: timeout in %d seconds, call %p",
			 tv->tv_sec, ev->ev_callback));

		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}

	/* 如果是I/O事件, 并且该事件之前没有注册过(即不在EVLIST_INSERTED列表和EVLIST_ACTIVE列表中) */
	if ((ev->ev_events & (EV_READ|EV_WRITE)) &&
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {

		/* 添加到特定I/O复用机制中 */
		int res = evsel->add(evbase, ev);
		if (res != -1) /* 将事件插入到event_base的已注册I/O事件列表中 */
			event_queue_insert(base, ev, EVLIST_INSERTED);

		return (res);
	} /* 如果是信号事件, 并且该事件之前没有注册过(即不在EVLIST_SIGNAL列表中)*/ 
	else if ((ev->ev_events & EV_SIGNAL) &&
	    !(ev->ev_flags & EVLIST_SIGNAL)) {

		/* 添加到特定I/O复用机制中, 注意, 在特定IO复用实现中会对信号事件直接调用evsignal_add操作 */
		int res = evsel->add(evbase, ev);
		if (res != -1) /* 将事件插入到event_base的信号事件列表中, 当然这个列表不是直接保存在event_base中, 而是在evsignal_info中 */
			event_queue_insert(base, ev, EVLIST_SIGNAL);

		return (res);
	}

	return (0);
}

/* 删除事件 */
int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	/* 要删除的事件此时必定存在于某个列表中 */
	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	/* 如果是定时事件, 将该事件从定时列表中删除 */
	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	/* 如过是已经激活的事件, 将该事件从已就绪列表中删除 */
	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	/* 如果是已注册的IO事件, 将该事件从已注册IO事件列表中删除 */
	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	}
	else if (ev->ev_flags & EVLIST_SIGNAL) {
		event_queue_remove(base, ev, EVLIST_SIGNAL);
		return (evsel->del(evbase, ev));
	}

	return (0);
}

/***
 * 将事件加入其所属的event_base已激活事件链表中, 并标在ev->ev_res中叠加
 * 就绪的事件类型掩码, 如果执行该操作时候, 该事件已经处于激活列表中, 则
 * 仅仅需要叠加此处就绪的事件类型掩码即可
 * @ev[IN]: 激活的事件
 * @res[IN]: 就绪的事件类型掩码
 * @ncalls
 */
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
	/* 如果改事件已经处于激活链表中, 则仅需叠加就绪事件类型的掩码即可 */
	if (ev->ev_flags & EVLIST_ACTIVE) {
		ev->ev_res |= res;
		return;
	}

	/* 如果之前该事件没有加入到激活链表中*/
	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

/***
 * 获取定时事件堆中下一个到时距离现在的事件, 注意, 该函数主要用于确定IO复用机制最大等待的事件
 * 我们知道IO复用等待调用如epoll_wait在阻塞情况下, 会一直等待, 直到出现就绪的事件或者超过设定的
 * 超时值, 为什么要用最近的定时间作为超时值呢? 这是一个惯用技巧, 这样处理的好处是: 以最小的开销处理
 * 定时事件和IO事件, 在ACE, libevent, redis中的ae网络库都是这样处理的!!!
 * @base[IN]: event_base实例
 * @tv_p[OUT]: 输出时间值
 * @return: 成功返回0, 失败-1
 */
static int
timeout_next(struct event_base *base, struct timeval **tv_p)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p;

	/* 从队中获取最近的定时事件, 如果返回NULL, 说明堆中不存在任何事件, 此时返回成功
	 * 同时输出事件指针置为NULL
	 */
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL;
		return (0);
	}

	/* 获取当前的时间值 */
	if (gettime(&now) == -1)
		return (-1);

	/* 如果事件的超时时间值已经等于或者早于当前的事件, 说明定时事件就绪, 返回成功, 
	 * tv_p值置为0, IO复用机制不用等待 
	 */
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}

	/* 计算当前距离最近的定时事件还有多长时间 */
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %d seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	gettime(tv);
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
}

/* 处理定时事件, 注意: 每次在IO复用机制返回时, 首先调用该函数来处理可能就绪的定时事件 
 * @base[IN]: event_base实例
 */
void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;

	/* 如果定时事件最小堆没有任何元素, 说明没有任何注册的定时事件, 直接返回 */
	if (min_heap_empty(&base->timeheap))
		return;

	gettime(&now);

	/* 查看定时事件最小堆中的处于堆顶的事件的是否超时, 因为可能有多个事件时间
	 * 超时, 所以一直循环下去, 直到新的堆定事件未到超时
	 */
	while ((ev = min_heap_top(&base->timeheap))) {
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));

		/* 将定时事件插入event_base的已激活事件*/
		event_active(ev, EV_TIMEOUT, 1);
	}
}

/* 从某个队列中删除某个事件 */
void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	/* 如果该事件不在此队列中, 记录错误日志并退出 */
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	/* 如果不是内部使用的信号事件通知的辅助IO事件, 则递减1 */
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--;

	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_ACTIVE: /* 从已就绪事件列表中删除事件 */
		base->event_count_active--;
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_SIGNAL:	/* 从已注册信号事件列表中删除事件 */
		TAILQ_REMOVE(&base->sig.signalqueue, ev, ev_signal_next);
		break;
	case EVLIST_TIMEOUT: /* 从已注册定时事件列表中删除事件 */
		min_heap_erase(&base->timeheap, ev);
		break;
	case EVLIST_INSERTED: /* 从已注册I/O事件列表中删除事件 */
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/***
 * 将事件ev添加到event_base的某个queue列表中
 * @base[IN]: Reactor组件
 * @ev[IN]: 要插入的事件
 * @queue[IN]: 要插入到的列表类型, 如将事件插入到激活列表中则queue传入EVLIST_ACTIVE
 */
void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	/* 如果该事件的标志标明它已经加入到了queue指定的列表, 等于是重复插入到相同的列表 */
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		
		/* 如果是插入激活列表这种情况的话, 则直接返回, 这中情况是可能发生的 */
		if (queue & EVLIST_ACTIVE)
			return;
	
		/* 但是如果是重复插入其他的列表的话, 这是不可能的, 则会打印错误信息 */
		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	/* EVLIST_INTERNAL是内部使用的用来通知信号事件的辅助IO事件, 并非上层用户添加的事件, 所以
	 * 统计已注册的事件时, 不对该事件计数 
	 */
	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;

	/* 修改事件的标记, 说明该事件注册到某个列表了 */
	ev->ev_flags |= queue;

	switch (queue) {
	case EVLIST_ACTIVE: /* 如果是插入已激活事件列表 */
		base->event_count_active++;
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_SIGNAL:	/* 如果是插入信号列表 */
		TAILQ_INSERT_TAIL(&base->sig.signalqueue, ev, ev_signal_next);
		break;
	case EVLIST_TIMEOUT: {	/* 如果是插入定时器列表(实际是一个最小堆) */
		min_heap_push(&base->timeheap, ev);
		break;
	}
	case EVLIST_INSERTED: /* 如果是注册新IO事件, 则插入到已注册事件列表中 */
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/* 
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
