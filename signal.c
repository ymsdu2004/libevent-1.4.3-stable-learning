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
 * ��event_base��ʼ���źŴ���
 * @base[IN]: event_baseʵ��(��Reactor���)
 */
void
evsignal_init(struct event_base *base)
{
	/* 
	 * Our signal handler is going to write to one end of the socket
	 * pair to wake up our event loop.  The event loop then scans for
	 * signals that got delivered.
	 */
	/* ����һ��UNXI���socket��, ���ڻ�������źŵ��¼�ѭ��, ��ԭ��Ϊ, һ�����źŲ���, 
	 * ��evsignal_handler�����оͻ��һ��socketд��һ��֪ͨ��Ϣ, �¼�ѭ��һ�˻��
	 * ��һ��socket��, һ����socket�Ⱦ���, ˵�����źű�����
	 */
	if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1)
		event_err(1, "%s: socketpair", __func__);

	/* Ϊ���socket����close-on-exec��� */
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);
	FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);

	/* sh_old�����ʼ�����κ�Ԫ��*/
	base->sig.sh_old = NULL;
	base->sig.sh_old_max = 0;
	base->sig.evsignal_caught = 0;

	/* �źŲ���ͳ����������ʼ��Ϊ0 */
	memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);

    evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);

	/* ���ｫ�źŴ���ת��Ϊ��һ��I/O�¼�!!!, �������Ĺ������������ź��¼�, ����
	 * ��û��ִ��ע��, ֻ�еȵ��û�add�ź��¼���ʱ��Ż��������õ��ź��¼�ע��, Ҳ
	 * ����˵�û�ע���ź��¼���һ�δ���evsignal_addִ�е�ʱ��Ż�ȥע��ev_signal,
	 * ��Ҳ�൱��һ��"Copy on write"˼��
	 */
	event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],
		EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);

	base->sig.ev_signal.ev_base = base;

	/* 
	 * ����ڲ��¼��ı��
	 */
	base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
/***
 * Ϊevent_base���evsignal�ź������źŴ�����
 * @base[IN]: event_base����(Reactor���)
 * @evsignal[IN]: �ź�����
 * @handler[IN]: �źŴ�����ָ��
 * @return: �ɹ�����0, ʧ�ܷ���-1
 */
int
_evsignal_set_handler(struct event_base *base,
		      int evsignal, void (*handler)(int))
{
	/* ��unixϵͳ����ͨ��signalϵͳ�����������źŴ���, ���µ�ϵͳ��������һ��
	 * ���ܸ�ǿ���sigactionϵͳ�����������źŴ���, �˴���HAVE_SIGACTION����
	 * �������������
	 */
#ifdef HAVE_SIGACTION
	struct sigaction sa;
#else

	/* void (*ev_sighandler_t)(int); */
	ev_sighandler_t sh;
#endif

	/* ȡ��event_base�е��źŴ�����Ϣ */
	struct evsignal_info *sig = &base->sig;
	void *p;

	/*
	 * resize saved signal handler array up to the highest signal number.
	 * a dynamic array is used to keep footprint on the low side.
	 */

	/* �����ǰ������ź�����ֵ��sh_old�����������ɵ�����ź�����ֵ��Ҫ��, ����Ҫ��̬��չsh_old���� */
	if (evsignal >= sig->sh_old_max) {
		event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
			    __func__, evsignal, sig->sh_old_max));

		/* �������С����Ϊ�µ��ź�����ֵ��1 */
		sig->sh_old_max = evsignal + 1;

		/* ��չ�������·���ռ� */
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

	/* ���ý��µ�sigactionϵͳ����
	 * linux��sigaction��һ������ʱҪ���õ��ź�����, �ڶ������������õ��źŵ���Ϣ
	 * �����������ǵ��÷��صĸ��ź�ԭ���Ĵ�����Ϣ
	 */
	if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
		event_warn("sigaction");
		free(sig->sh_old[evsignal]);
		return (-1);
	}
#else

	/* ���ý��ϵ�signalϵͳ����
	 * ��ϵͳ���õĵ�һ������ʱҪ���õ��ź�����
	 * �ڶ����������µĴ���������ָ��
	 * ����ֵ�Ǹ��ź�ԭ���Ĵ�����ָ��
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
 * ����ź������¼�
 * @ev[IN]: �ź��¼�
 * @return: �ɹ�����0, ʧ�ܷ���-1
 */
int
evsignal_add(struct event *ev)
{
	int evsignal;

	/* �¼�������Reactor��� */
	struct event_base *base = ev->ev_base;

	/* �ź��¼���Ϣ */
	struct evsignal_info *sig = &ev->ev_base->sig;

	/* �ź��¼�������IO�¼���� */
	if (ev->ev_events & (EV_READ|EV_WRITE))
		event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);

	/* ȡ���ź��¼����ź����� */
	evsignal = EVENT_SIGNAL(ev);

	event_debug(("%s: %p: changing signal handler", __func__, ev));

	/* �����ź��¼��Ĵ����� */
	if (_evsignal_set_handler(base, evsignal, evsignal_handler) == -1)
		return (-1);

	/* catch signals if they happen quickly */
	evsignal_base = base;

	/* ����evsignal_init˵��ֻ���û�����ע���ź��¼���ʱ��Ż�ע��ev_signal�¼�*/
	if (!sig->ev_signal_added) {
		sig->ev_signal_added = 1;

		/* ������ǽ��źŴ����socket pair�Ķ�socket��Ӧ���¼�ע�ᵽlibevent�� */
		event_add(&sig->ev_signal, NULL);
	}

	return (0);
}

/* event_base�ж�evsignal�źŻָ�֮ǰ�Ĵ���ʽ 
 * @base[IN]: event_baseʵ��
 * @evsignal[IN]: Ҫ�ָ����ź�����ֵ
 * @return: �ɹ�����0, �����򷵻�-1
 */
int
_evsignal_restore_handler(struct event_base *base, int evsignal)
{
	int ret = 0;

	/* event_base��ά�����ź��¼�������Ϣ */
	struct evsignal_info *sig = &base->sig;
#ifdef HAVE_SIGACTION
	struct sigaction *sh;
#else
	ev_sighandler_t *sh;
#endif

	/* restore previous handler */

	/* ��sh_old������ȡ�����źű����ԭ�е��źŴ��� */
	sh = sig->sh_old[evsignal];
	sig->sh_old[evsignal] = NULL;

	/* ��ԭ���Ĵ�����Ϣ����ϵͳ����sigactionl����signal���ָ�֮ǰ�Ĵ���ʽ */
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

/* ɾ��ĳ���ź��¼� */
int
evsignal_del(struct event *ev)
{
	event_debug(("%s: %p: restoring signal handler", __func__, ev));
	return _evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev));
}

/***
 * ͳһ�źŴ�����, �źŷ���ʱ, ����øú���
 * @sig[IN]: �ź�����ֵ
 */
static void
evsignal_handler(int sig)
{
	/* ���źŴ������е���ĳЩϵͳ����ʱ����ȫ��, ���������޸�ȫ�ֵ�errnoֵ
	 * ���Թ���ʵ����ͨ����ȡ�ķ������ڽ����źŴ�������ʱ�򱣴�һ��errnoֵ, ��
	 * �뿪������ʱ�ڽ������ֵ�ָ���errno
	 */
	int save_errno = errno;

	/* ȫ�ֱ���, ָ���źŴ���������event_base, �ڳ�ʼ���ź�ʱ��Ϊ��ȫ�ֱ���ָ��ĳ��event_baseʵ��, 
	 * ���û��ָ��, ˵��δ��ʼ��, ��ӡ������Ϣ����ʧ�� 
	 */
	if(evsignal_base == NULL) {
		event_warn(
			"%s: received signal %d, but have no base configured",
			__func__, sig);
		return;
	}

	/* �ú�������˵��sig�źŷ�����, ���Լ�¼���źŷ�������ֵ��1 */
	evsignal_base->sig.evsigcaught[sig]++;

	/* ��ǲ������ź�, �¼�����ѭ���жϵ���ֵ������ϲ�ص������ź� */
	evsignal_base->sig.evsignal_caught = 1;

	/* �ٴ����ø��ź� */
#ifndef HAVE_SIGACTION
	signal(sig, evsignal_handler);
#endif

	/* ���дsocketдһ���ֽ����ݣ�����event_base��I/O�¼����Ӷ�֪ͨ�����źŴ���
	 * ��Ҫ����
	 */
	/* Wake up our notification mechanism */
	send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0);

	/* ��ԭ�źŴ�����ִ��ǰ��ȫ�ִ����� */
	errno = save_errno;
}

/* �ϲ��źŴ�����, ���¼�ѭ���з������źż���, �����ô˺����������ź��¼� */
void
evsignal_process(struct event_base *base)
{
	struct event *ev;
	sig_atomic_t ncalls;

	base->sig.evsignal_caught = 0;

	/* ������ע���¼��б�(��signalqueue), ����ȡ�������ź��¼�(ÿ���ź��¼���Ӧ��һ���ź�����) */
	TAILQ_FOREACH(ev, &base->sig.signalqueue, ev_signal_next) {

		/* �鿴���ź��Ѿ�����Ĵ��� */
		ncalls = base->sig.evsigcaught[EVENT_SIGNAL(ev)];

		/* ������ź��¼��Ѿ���������һ�� */
		if (ncalls) {

			/* EV_PERSIST����Ƿ񵥴λ��ǳ�����ע���ź�, ������ǳ���, �򱾴γ������ɾ�����ź��¼�
			 * libevent�е��ź��¼���һ����Ҳ������linux��signalϵͳ���õ�����, signalϵͳ����Ҳ��
			 * ������Ч, ���ϣ��������ע���ź��¼�, ��Ҫ���źŴ��������ٴε���signalϵͳ����
			 * ��ͬ����, libevent����EV_PERSIST�����������һ�����Ƿ�߱�
			 */
			if (!(ev->ev_events & EV_PERSIST))
				event_del(ev);

			/* ��Ը��¼������ź��¼�, ����Ĵ���Ϊncalls */
			event_active(ev, EV_SIGNAL, ncalls);

			/* ��λ���źŲ������ */
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
