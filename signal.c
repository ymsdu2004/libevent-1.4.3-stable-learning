/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#include <winsock2.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "evutil.h"
#include "log.h"

struct event_base *evsignal_base = NULL;

static void evsignal_handler(int sig);

/* Callback for when the signal handler write a byte to our signaling socket */
static void
evsignal_cb(int fd, short what, void *arg)
{
	static char signals[100];
#ifdef WIN32
	SSIZE_T n;
#else
	ssize_t n;
#endif

	n = recv(fd, signals, sizeof(signals), 0);
	if (n == -1)
		event_err(1, "%s: read", __func__);
}

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/***
 * 对event_base初始化信号处理
 * @base[IN]: event_base实例(即Reactor组件)
 */
void
evsignal_init(struct event_base *base)
{
	/* 
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.
	 */
	/* 创建一个UNXI域的socket对, 用于唤醒针对信号的事件循环, 其原理为, 一旦有信号捕获, 
	 * 在evsignal_handler函数中就会从一端socket写入一个通知信息, 事件循环一端会从
	 * 另一端socket读, 一旦该socket度就绪, 说明有信号被捕获
	 */
	if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1)
		event_err(1, "%s: socketpair", __func__);

	/* 为这对socket设置close-on-exec标记 */
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);

	/* sh_old数组初始化无任何元素*/
	base->sig.sh_old = NULL;
	base->sig.sh_old_max = 0;
	base->sig.evsignal_caught = 0;

	/* 信号捕获统计数组各项初始化为0 */
	memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);

    evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);

	/* 这里将信号处理转化为了一个I/O事件!!!, 这里做的工作这是设置信号事件, 但是
	 * 还没有执行注册, 只有等到用户add信号事件的时候才会这里设置的信号事件注册, 也
	 * 就是说用户注册信号事件第一次触发evsignal_add执行的时候才会去注册ev_signal,
	 * 这也相当于一种"Copy on write"思想
	 */
	event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],
		EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);

	base->sig.ev_signal.ev_base = base;

	/* 
	 * 添加内部事件的标记
	 */
	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
/***
 * 为event_base针对evsignal信号设置信号处理函数
 * @base[IN]: event_base对象(Reactor组件)
 * @evsignal[IN]: 信号类型
 * @handler[IN]: 信号处理函数指针
 * @return: 成功返回0, 失败返回-1
 */
int
_evsignal_set_handler(struct event_base *base,
		      int evsignal, void (*handler)(int))
{
	/* 类unix系统早期通过signal系统调用来设置信号处理, 较新的系统都有另外一个
	 * 功能更强大的sigaction系统调用来设置信号处理, 此处用HAVE_SIGACTION宏来
	 * 来控制这种情况
	 */
#ifdef HAVE_SIGACTION
	struct sigaction sa;
#else

	/* void (*ev_sighandler_t)(int); */
	ev_sighandler_t sh;
#endif

	/* 取出event_base中的信号处理信息 */
	struct evsignal_info *sig = &base->sig;
	void *p;

	/*
	 * resize saved signal handler array up to the highest signal number.
	 * a dynamic array is used to keep footprint on the low side.
	 */

	/* 如果当前处理的信号类型值比sh_old数组所能容纳的最大信号类型值还要打, 则需要动态扩展sh_old数组 */
	if (evsignal >= sig->sh_old_max) {
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig->sh_old_max));

		/* 将数组大小设置为新的信号类型值加1 */
		sig->sh_old_max = evsignal + 1;

		/* 扩展或者重新分配空间 */
		p = realloc(sig->sh_old, sig->sh_old_max * sizeof *sig->sh_old);
		if (p == NULL) {
			event_warn("realloc");
			return (-1);
		}
		sig->sh_old = p;
	}

	/* allocate space for previous handler out of dynamic array */
	sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]);
	if (sig->sh_old[evsignal] == NULL) {
		event_warn("malloc");
		return (-1);
	}

	/* save previous handler and setup new handler */
#ifdef HAVE_SIGACTION
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags |= SA_RESTART;
	sigfillset(&sa.sa_mask);

	/* 调用较新的sigaction系统调用
	 * linux中sigaction第一个参数时要设置的信号类型, 第二个参数是设置的信号的信息
	 * 第三个参数是调用返回的该信号原来的处理信息
	 */
	if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
		event_warn("sigaction");
		free(sig->sh_old[evsignal]);
		return (-1);
	}
#else

	/* 调用较老的signal系统调用
	 * 该系统调用的第一个参数时要设置的信号类型
	 * 第二个参数是新的处理器函数指针
	 * 返回值是该信号原来的处理函数指针
	 */
	if ((sh = signal(evsignal, handler)) == SIG_ERR) {
		event_warn("signal");
		free(sig->sh_old[evsignal]);
		return (-1);
	}
	*sig->sh_old[evsignal] = sh;
#endif

	return (0);
}

/***
 * 添加信号类型事件
 * @ev[IN]: 信号事件
 * @return: 成功返回0, 失败返回-1
 */
int
evsignal_add(struct event *ev)
{
	int evsignal;

	/* 事件关联的Reactor组件 */
	struct event_base *base = ev->ev_base;

	/* 信号事件信息 */
	struct evsignal_info *sig = &ev->ev_base->sig;

	/* 信号事件不能有IO事件标记 */
	if (ev->ev_events & (EV_READ|EV_WRITE))
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);

	/* 取出信号事件的信号类型 */
	evsignal = EVENT_SIGNAL(ev);

	event_debug(("%s: %p: changing signal handler", __func__, ev));

	/* 设置信号事件的处理函数 */
	if (_evsignal_set_handler(base, evsignal, evsignal_handler) == -1)
		return (-1);

	/* catch signals if they happen quickly */
	evsignal_base = base;

	/* 上面evsignal_init说的只有用户真正注册信号事件的时候才会注册ev_signal事件*/
	if (!sig->ev_signal_added) {
		sig->ev_signal_added = 1;

		/* 这里才是将信号处理的socket pair的读socket对应的事件注册到libevent中 */
		event_add(&sig->ev_signal, NULL);
	}

	return (0);
}

/* event_base中对evsignal信号恢复之前的处理方式 
 * @base[IN]: event_base实例
 * @evsignal[IN]: 要恢复的信号类型值
 * @return: 成功返回0, 出错则返回-1
 */
int
_evsignal_restore_handler(struct event_base *base, int evsignal)
{
	int ret = 0;

	/* event_base中维护的信号事件处理信息 */
	struct evsignal_info *sig = &base->sig;
#ifdef HAVE_SIGACTION
	struct sigaction *sh;
#else
	ev_sighandler_t *sh;
#endif

	/* restore previous handler */

	/* 从sh_old数组中取出此信号保存的原有的信号处理 */
	sh = sig->sh_old[evsignal];
	sig->sh_old[evsignal] = NULL;

	/* 以原来的处理信息调用系统调用sigactionl或者signal来恢复之前的处理方式 */
#ifdef HAVE_SIGACTION
	if (sigaction(evsignal, sh, NULL) == -1) {
		event_warn("sigaction");
		ret = -1;
	}
#else
	if (signal(evsignal, *sh) == SIG_ERR) {
		event_warn("signal");
		ret = -1;
	}
#endif
	free(sh);

	return ret;
}

/* 删除某个信号事件 */
int
evsignal_del(struct event *ev)
{
	event_debug(("%s: %p: restoring signal handler", __func__, ev));
	return _evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev));
}

/***
 * 统一信号处理函数, 信号发生时, 会调用该函数
 * @sig[IN]: 信号类型值
 */
static void
evsignal_handler(int sig)
{
	/* 在信号处理函数中调用某些系统调用时不安全的, 如它可能修改全局的errno值
	 * 所以工程实践中通常采取的方法是在进入信号处理函数的时候保存一下errno值, 在
	 * 离开处理函数时在将保存的值恢复给errno
	 */
	int save_errno = errno;

	/* 全局变量, 指向信号处理所属的event_base, 在初始化信号时会为该全局变量指定某个event_base实例, 
	 * 如果没有指定, 说明未初始化, 打印出错信息返回失败 
	 */
	if(evsignal_base == NULL) {
		event_warn(
			"%s: received signal %d, but have no base configured",
			__func__, sig);
		return;
	}

	/* 该函数调用说明sig信号发生了, 所以记录该信号发生次数值增1 */
	evsignal_base->sig.evsigcaught[sig]++;

	/* 标记捕获到了信号, 事件处理循环判断到该值会调用上层回调处理信号 */
	evsignal_base->sig.evsignal_caught = 1;

	/* 再次设置该信号 */
#ifndef HAVE_SIGACTION
	signal(sig, evsignal_handler);
#endif

	/* 向读写socket写一个字节数据，触发event_base的I/O事件，从而通知其有信号触发
	 * 需要处理
	 */
	/* Wake up our notification mechanism */
	send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0);

	/* 复原信号处理函数执行前的全局错误码 */
	errno = save_errno;
}

/* 上层信号处理函数, 在事件循环中发现有信号激活, 则会调用此函数来处理信号事件 */
void
evsignal_process(struct event_base *base)
{
	struct event *ev;
	sig_atomic_t ncalls;

	base->sig.evsignal_caught = 0;

	/* 遍历已注册事件列表(即signalqueue), 依次取出各个信号事件(每个信号事件对应着一个信号类型) */
	TAILQ_FOREACH(ev, &base->sig.signalqueue, ev_signal_next) {

		/* 查看该信号已经捕获的次数 */
		ncalls = base->sig.evsigcaught[EVENT_SIGNAL(ev)];

		/* 如果该信号事件已经捕获至少一次 */
		if (ncalls) {

			/* EV_PERSIST标记是否单次还是持续关注该信号, 如果不是持续, 则本次出发后就删除该信号事件
			 * libevent中的信号事件这一特性也秉承了linux中signal系统调用的特性, signal系统调用也是
			 * 单次有效, 如果希望继续关注该信号事件, 需要在信号处理函数中再次调用signal系统调用
			 * 不同的是, libevent中用EV_PERSIST标记来控制这一特性是否具备
			 */
			if (!(ev->ev_events & EV_PERSIST))
				event_del(ev);

			/* 针对该事件激活信号事件, 激活的次数为ncalls */
			event_active(ev, EV_SIGNAL, ncalls);

			/* 复位该信号捕获次数 */
			base->sig.evsigcaught[EVENT_SIGNAL(ev)] = 0;
		}
	}
}

void
evsignal_dealloc(struct event_base *base)
{
	if(base->sig.ev_signal_added) {
		event_del(&base->sig.ev_signal);
		base->sig.ev_signal_added = 0;
	}
	assert(TAILQ_EMPTY(&base->sig.signalqueue));

	EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[0]);
	base->sig.ev_signal_pair[0] = -1;
	EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[1]);
	base->sig.ev_signal_pair[1] = -1;
	base->sig.sh_old_max = 0;

	/* per index frees are handled in evsignal_del() */
	free(base->sig.sh_old);
}
