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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

struct evsignal_info {

	/* 信号事件列表, 上层调用event_add时候会将事件添加到该列表中 */
	struct event_list signalqueue;

	/* 为socket pair的读socket向event_base注册毒事件时使用的event结构体 */
	struct event ev_signal;

	/* 用于通知事件循环的socket对, 信号捕获时从一端socket写入一个标记, 
	 * 事件循环在另一端socket检测到读就绪则知道有信号在底层被捕获, 于是
	 * 触发上层信号处理回调, 事实上, 通过这一机制, 就巧妙地将事件的通知
	 * 转化为一个I/O事件了!!!
	 */
	int ev_signal_pair[2];

	/* 记录ev_signal事件是否已经注册了 */
	int ev_signal_added;

	/* 标识当前是否存在捕获的未处理的信号, volatile是因为它会在另一个线程中被修改 */
	volatile sig_atomic_t evsignal_caught;

	/* 一个数组, 以信号类型值为索引, 保存某个特定信号在上次事件循环处理后
	 * 到本次处理时捕获的次数, 即两次evsignal_process调用之间同一事件积累的次数
	 */
	sig_atomic_t evsigcaught[NSIG];

	/* 一个指针数组, 动态分配, 以信号类型值作为索引
	 *  用来保存信号原来的处理信息,
	 * 在sigaction系统调用中, 该信息是一个sigaction结构
	 * 而在较老的signal系统调用中, 该信息是一个void (*func)(int)类型的函数指针
	 * 这里通过HAVE_SIGACTION来分别定义两种情况
	 */
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;
#else
	ev_sighandler_t **sh_old;
#endif

	/* sh_old中能够保存的最大信号类型值, 也就是sh_old数组的最大索引 */
	int sh_old_max;
};
void evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
