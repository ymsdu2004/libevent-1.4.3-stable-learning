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
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"

/***
 * 抽象的IO复用机制接口, 相当于C++中的纯虚接口, 具体的各种IO复用机制都要实现这些接口, 
 * 在初始化时候, 会将这些函数指针分别指向选择的IO复用机制所定义的这些函数
 * libevent实际支持: select, epoll, kqueue, dev/poll等机制
 */
struct eventop {

	/* 机制名称: 如"select", "epoll" */
	const char *name;
	
	/* 初始化 */
	void *(*init)(struct event_base *);
	
	/* 注册事件 */
	int (*add)(void *, struct event *);
	
	/* 删除事件 */
	int (*del)(void *, struct event *);
	
	/* 事件分发, 相当于select机制的select调用 */
	int (*dispatch)(struct event_base *, void *, struct timeval *);
	
	/* 注销, 释放资源 */
	void (*dealloc)(struct event_base *, void *);
	
	/* set if we need to reinitialize the event base */
	int need_reinit;
};

/***
 * 相当于框架中的Reactor组件(反应器)
 * 三种类型的事件: I/O事件, 定时事件, 信号事件
 * 他们各自有一个注册列表, 以不同的形式存在:
 * 1)已注册I/O事件列表: 就是event_base中的eventqueue(一个链表)
 * 2)已注册定时事件列表: 就是event_base中的timeheap, 它是一个小根堆
 * 3)已注册信号事件列表: 就是event_base中的evsignal_info中的signalqueue(一个链表)
 * 
 * event_base中的另外存在一张表格, 就是已激活事件优先级列表, 它用于保存所有就绪的事件, 
 * 事件在就绪(poll调用来检测)时候会根据其优先级添加到该表格的对应的优先级列表, 三种类型
 * 的事件就绪都是存放在该表格中的
 */
struct event_base {

	/* 特定I/O复用机制对eventop抽象接口的实例化, 如select模型下指向selectops */
	const struct eventop *evsel;
	
	/* 每种I/O复用机制的内部数据结构体, 调用evsel抽象接口时要传入该参数
	 * 在调用eventop的init时会为其赋值, 如在select模型下, evbase指向
	 * select_init返回的一个selectop结构体实例
	 */
	void *evbase;
	
	/* 注册的事件总数(三种类型事件都算, 信号机制使用的内部事件不算) */
	int event_count;		/* counts number of total events */
	
	/* 激活的事件数 */
	int event_count_active;	/* counts number of active events */

	/* 标记结束事件循环 */
	int event_gotterm;		/* Set to terminate loop */
	
	/* 标记立即结束事件循环 */
	int event_break;		/* Set to terminate loop immediately */

	/* 已激活事件表格, 相当于指针数组, 数组的元素为一个链表
	 * 每个链表保存某个优先级的所有激活事件, 数组的索引是priority
	 * 它叫做EVLIST_ACTIVE列表
	 * 其中event_list由宏TAILQ_HEAD(event_list, event)定义为如下
	 * struct event_list {
	 *	   event* tqh_first;
	 *	   event** tqh_last
	 * }
	 */
	/* active event management */
	struct event_list **activequeues;
	
	/* 激活事件优先级队列的总数 */
	int nactivequeues;

	/* 已注册信号事件表(当然它还包含信号处理的其他信息) */
	/* signal handling info */
	struct evsignal_info sig;

	/* 已注册的I/O事件表(信号机制内部的IO事件也在其中, 该事件用EVLIST_INTERNAL标记来区分) */
	struct event_list eventqueue;
	
	struct timeval event_tv;

	/* 已注册定时时间表: 管理所有定时事件的小根堆*/
	struct min_heap timeheap;
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
	for((var) = TAILQ_FIRST(head);					\
	    (var) != TAILQ_END(head);					\
	    (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
	(elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
	(elm)->field.tqe_next = (listelm);				\
	*(listelm)->field.tqe_prev = (elm);				\
	(listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsignal_set_handler(struct event_base *base, int evsignal,
			  void (*fn)(int));
int _evsignal_restore_handler(struct event_base *base, int evsignal);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
