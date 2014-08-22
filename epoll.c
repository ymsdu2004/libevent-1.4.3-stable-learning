/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
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

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
/***
 * 实现中将evepoll传递到epoll_data中
 */
struct evepoll {
	/* 针对读事件保存事件数据 */
	struct event *evread;
	
	/* 针对读事件保存事件数据 */
	struct event *evwrite;
};

/* epoll机制内部私有数据, 由epoll_init创建名赋值给event_base->evbase */
struct epollop {

	/* 用于保存感兴趣的事件, 它是一个数组, 大小为nfds, 数组的索引为文件描述符值 */
	struct evepoll *fds;
	
	/* 当前支持处理的最大事件描述符, 它指示fds数组的大小 */
	int nfds;
	
	/* 保存epoll_wait返回的就绪事件, 这是一个数组, 其大小至少应该为最多感兴趣的事件数目的大小 
	 * 注意, 与fds数组不同, 它的可不是用fd来索引的!!!
	 * 如何知道它对应那个事件? 查看返回的event_data.ptr吧, 在添加事件的时候已经把event结构
	 * 的指针以evepoll的形式保存在里面了
	 */
	struct epoll_event *events;
	
	/* 最大可监控事件数目, 它是events数组的大小 */
	int nevents;
	
	/* epoll文件描述符, 由epoll_create创建 */
	int epfd;
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

/* eventop抽象接口的epoll实例化, 初始化时赋值给event_base->evsel */
struct eventop epollops = {
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

#define NEVENT	32000

/***
 * 为Reactor初始化epoll机制
 * @base[IN]: Reactor组件
 * @return: 成功返回epoll内部结构指针, 失败返回NULL
 */
static void *
epoll_init(struct event_base *base)
{
	int epfd, nfiles = NEVENT;	/* nfiles作为处理的最大事件的指导性参数 */
	struct rlimit rl;
	struct epollop *epollop;
	
	/* 如果定义了EVENT_NOEPOLL环境变量, 则无法使用epoll机制 */
	/* Disable epollueue when this environment variable is set */
	if (getenv("EVENT_NOEPOLL"))
		return (NULL);

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
	    rl.rlim_cur != RLIM_INFINITY) {
		/*
		 * Solaris is somewhat retarded - it's important to drop
		 * backwards compatibility when making changes.  So, don't
		 * dare to put rl.rlim_cur here.
		 */
		nfiles = rl.rlim_cur - 1;
	}

	/* Initalize the kernel queue */

	/* 创建epoll, 在较新的linux版本中epoll_create中的size参数不起作用 */
	if ((epfd = epoll_create(nfiles)) == -1) {
                event_warn("epoll_create");
		return (NULL);
	}

	/* 为epoll文件描述符设置close-on-close标志 */
	FD_CLOSEONEXEC(epfd);

	/* 为epoll私有数据分配空间 */
	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	/* 保存epoll文件描述符 */
	epollop->epfd = epfd;

	/* Initalize fields */
	
	/* 为事件就绪数组分配空间, 数组的大小是支持的最大事件的数目 */
	epollop->events = malloc(nfiles * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	
	/* 最大监控的事件数目 */
	epollop->nevents = nfiles;

	/* 分配感兴趣的事件数组空间 */
	epollop->fds = calloc(nfiles, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	
	/* 记录fds数组的大小 */
	epollop->nfds = nfiles;

	/* 初始化base的信号处理*/
	evsignal_init(base);

	return (epollop);
}

/***
 * 重新分配fds的内存空间, 以容纳描述符比当前允许容纳的最大描述符还要大的描述符
 * @base[IN]: Reactor组件
 * @arg[IN]: epoll的内部数据结构epollop
 * @max[IN]: 新的可以容纳的最大描述符
 * @return: 成功返回0, 失败返回-1, 注意, 如果返回失败则保证不会修改原来的epollop
 */
static int
epoll_recalc(struct event_base *base, void *arg, int max)
{
	/* 参数中保存的是epollop */
	struct epollop *epollop = arg;

	/* 如果max参数确实大于epollop得以容纳的最大描述符, 则重新分配fds, 否则不动作 */
	if (max > epollop->nfds) {
		struct evepoll *fds;
		int nfds;

		/* 这里单独用一个nfds变量的作用有二: 
	     * 1)保证成功了才会修改epollop;
		 * 2)后面还需要用到原来的nfds大小, 如memset除代码
		 */
		nfds = epollop->nfds;
		
		/* 这里重新分配的原则是不断地将epollop->nfds翻倍, 直到新的epollop->nfds大于max为止 */
		while (nfds < max)
			nfds <<= 1;

		/* 重新分配epollop->fds空间
		 * 注意void *realloc(void *mem_address, unsigned int newsize)库函数先
		 * 判断当前的内存块是否有足够的连续未用空间, 如果有, 则扩大mem_address指向的地址,
		 * 并且将mem_address返回, 如果空间不够, 先按照newsize指定的大小分配空间, 
		 * 将原有数据从头到尾拷贝到新分配的内存区域, 而后释放原来mem_address所指内存区域
		 */
		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		
		/* 更新epollop->fds指向新分配的空间 */
		epollop->fds = fds;
		
		/* 将未用的元素初始化为0 */
		memset(fds + epollop->nfds, 0,
		    (nfds - epollop->nfds) * sizeof(struct evepoll));
			
		/* 更新epollop->nfds为新空间足以容纳的最大描述符数 */
		epollop->nfds = nfds;
	}

	return (0);
}

/***
 * 抽象接口dispatch的epoll实现, 执行一次poll操作
 * @base[IN]: Reactor组件
 * @arg[IN]: 特定IO机制的内部私有数据, epoll中为epollop
 * @tv[IN]: epoll_wait执行的超时时间
 * @return: 成功返回0, 失败返回-1
 */
static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)
{
	/* epoll实现当然为epollop类型数据 */
	struct epollop *epollop = arg;

	/* 用于保存已就绪的事件, 由epoll_wait填充 */
	struct epoll_event *events = epollop->events;
	struct evepoll *evep;
	int i, res, timeout = -1;

	/* 如果tv不为NULL, 则将事件转化为毫秒数 */
	if (tv != NULL)
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

	/* 执行一次poll操作 */
	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	/* epoll_wait系统调用返回-1表示出错或者中断, 如果是信号中断发生的话, 
	 *直接调用evsignal_process处理信号事件, 然后返回成功, 否则返回-1, 表示出错
	 */
	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base);
		return (0);
	} else if (base->sig.evsignal_caught) {	/* 如果返回成功, base中标识不活信号, 则首先调用evsignal_process处理信号 */
		/* 内部event_active(ev, EV_SIGNAL, ncalls); */
		evsignal_process(base);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));

	/* epoll_wait成功是返回值res表示就绪事件的数目, 就绪事件的信息填充在epollop->events中
	 * 此处逐个处理所有就绪的事件
	 */
	for (i = 0; i < res; i++) {
		int what = events[i].events;

		/* 用这两个指针来标记读或者写是否就绪, 就绪的话, 两个指针都指向代表该事件的event结构 */
		struct event *evread = NULL, *evwrite = NULL;

		/* 注意, 之前添加事件的时候就是将evepoll类型数据保存在里面的, 这个时候传回来了 */
		evep = (struct evepoll *)events[i].data.ptr;

		/* 如果该事件的读和写事件都就绪 */
		if (what & (EPOLLHUP|EPOLLERR)) {
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {

			/* 如果只有读就绪 */
			if (what & EPOLLIN) {
				evread = evep->evread;
			}

			/* 如果只有些就绪 */
			if (what & EPOLLOUT) {
				evwrite = evep->evwrite;
			}
		}

		/* 如果该事件没有读写就绪, 直接下一个就绪事件, 既然已经返回在就绪列表中了, 怎么可能读写没有就绪了?
		 * 当然, 上面还有个信号事件呢!!!
		 */
		if (!(evread||evwrite))
			continue;

		/* 如果读指针不为NULL, 说明读就绪, 将该事件加入到已激活链表中, 并叠加读就绪掩码
		 * event_active实现中, 如果该事件已经在激活列表中, 则仅仅需要叠加本次的就绪掩码即可 
		 */
		if (evread != NULL)
			event_active(evread, EV_READ, 1);

		/* 如果写指针不为NULL, 说明写就绪, 将该事件加入到已激活链表中, 并叠加写就绪掩码 */
		if (evwrite != NULL)
			event_active(evwrite, EV_WRITE, 1);
	}

	return (0);
}

/***
 * add抽象接口的epoll实现, 向epoll中添加/修改感兴趣的事件
 * @arg[IN]: 特定IO机制的内部私有数据, epoll中为epollop
 * @ev[IN]: 要添加的事件
 * @return: 成功返回0, 失败返回-1
 */
static int
epoll_add(void *arg, struct event *ev)
{
	/* epoll实现当然为epollop类型数据 */
	struct epollop *epollop = arg;
	
	/***
	 * linux中epoll_event的定义:
	 *
	 * struct epoll_event
	 * {
	 *     uint32_t events; 
	 *     epoll_data_t data;
	 * } __attribute__ ((__packed__));

	 * typedef union epoll_data
	 * {
	 *     void *ptr;
	 *     int fd;
	 *     uint32_t u32;
	 *     uint64_t u64;
	 * } epoll_data_t;
	*/

	/* linux定义的epoll事件表示 */
	struct epoll_event epev = {0, {0}};
	
	/* 本模块定义的epoll事件表示, 用它指向epollop->fds中的一个元素以进行操作 */
	struct evepoll *evep;
	
	int fd, op, events;

	/* 如果添加的事件是信号事件, 则直接交由evdsignal_add处理 */
	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(ev));

	/* 如果新添加事件的描述符大于初始化给出的最大监听描述符的大小, 则需要重新分配epollop->fds
 	 * 数组的空间大小, 以容纳较大描述符的事件
	 */
	fd = ev->ev_fd;
	if (fd >= epollop->nfds) {
	
		/* Extent the file descriptor array as necessary */
		/* 执行到这里也说明这个事件肯定新添加的事件, 而不可能是对已经添加事件的修改 
		 * 也就是说肯定是执行EPOLL_CTL_ADD操作 
		 */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
			return (-1);
	}
	
	/* 定位到epollop->fds数组中新增事件描述符fd所对应的数组元素 */
	evep = &epollop->fds[fd];
	
	/* 执行动作初始化为添加操作 */
	op = EPOLL_CTL_ADD;
	events = 0;
	
	/* 如果该事件之前已经对读感兴趣, 则叠加EPOLLIN掩码, 同时说明了要执行EPOLL_CTL_MOD操作 */
	if (evep->evread != NULL) {
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	
	/* 如果该事件之前已经对写感兴趣, 则叠加EPOLLOUT掩码, 同时说明了要执行EPOLL_CTL_MOD操作 */
	if (evep->evwrite != NULL) {
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}

	/* 新添加事件的事件掩码对读感兴趣, 则叠加EPOLLIN */
	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
		
	/* 新添加事件的事件掩码对写感兴趣, 则叠加EPOLLOUT */
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	/* 填充epoll内核需要使用的新增/修改事件的epoll_event数据
	 * linux中epoll_data用于保存用户的私有数据, epoll内部不会使用和修改该数据,
	 * 一般情况下, 用户使用epoll_data保存fd, 这里不同, 它用epoll_data当做指针使用保存
	 * 文件描述符对应的epollop->fds数组项
	 */
	epev.data.ptr = evep;
	epev.events = events;
	
	/* 调用内核提供的epoll_ctl系统调用 */
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)
			return (-1);

	/* Update events responsible */
	
	/* 填充该事件对应的epollop->fds数组项 */
	
	/* 如果新增事件对读感兴趣, 则将读事件指针指向本事件 */
	if (ev->ev_events & EV_READ)
		evep->evread = ev;
		
	/* 如果新增事件对写感兴趣, 则将写事件指针指向本事件 */
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev;

	return (0);
}

/***
 * del抽象接口的epoll实现, 删除某个事件
 * @arg[IN]: 特定IO机制的内部私有数据, epoll中为epollop
 * @ev[IN]: 要删除的事件
 * @return: 成功返回0, 失败返回-1
 */
static int
epoll_del(void *arg, struct event *ev)
{
	/* epoll实现当然为epollop类型数据 */
	struct epollop *epollop = arg;

	/* linux定义的epoll事件表示 */
	struct epoll_event epev = {0, {0}};

	/* 本模块定义的epoll事件表示, 用它指向epollop->fds中的一个元素以进行操作 */
	struct evepoll *evep;

	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	/* 如果添加的事件是信号事件, 则直接交由evsignal_del处理 */
	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_del(ev));

	/* 如果该事件的文件描述符大于epollop中所能容纳的最大文件描述符对应的事件, 
	 * 说明epollop中肯定不存在该事件, 也就不需要执行任何动作
	 */
	fd = ev->ev_fd;
	if (fd >= epollop->nfds)
		return (0);

	/* 指向本事件文件描述符对应的epollop->fds中的数组项, 以方便后面的操作 */
	evep = &epollop->fds[fd];

	/* 默认执行epoll内核的EPOLL_CTL_DEL删除操作, 后面可能修改 */
	op = EPOLL_CTL_DEL;
	events = 0;

	/* 本事件ev需要删除的事件类型 */
	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	/* 如果本事件需要删除事件不是同时包括读和写, 则需要进一步判断之前已有的感兴趣的事件类型,
	 * 以此来决定是否要执行的EPOLL_CTL_MOD
	 */
	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
		
		/* 如果事件要删除读事件类型并且之前事件已经对写感兴趣, 则必须保留写掩码, 
		 * 删除读掩码, 操作类型改为EPOLL_CTL_MOD 
		 */
		if ((events & EPOLLIN) && evep->evwrite != NULL) {
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} /* 如果事件要删除写事件类型且之前事件已经对读感兴趣, 则必须保留读掩码, 
		   * 删除写掩码, 操作类型改为EPOLL_CTL_MOD 
		   */ 
		else if ((events & EPOLLOUT) && evep->evread != NULL) {
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.ptr = evep;

	/* 该文件描述符数组对应项的读或者写指针指向空, 注意, 如果者两个指针都为NULL, 则说明该
	 * 文件描述符对应的事件不存在
	 */
	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;

	/* 执行内核epoll操作 
	 * 对于EPOLL_CTL_DEL操作, epev.events置位那些要删除的事件类型 
	 * 对于EPOLL_CTL_MOD操作, epev.events置位那些最终剩下的事件类型, 要删除的事件类型必须去掉 */
	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
		return (-1);

	return (0);
}

/***
 * dealloc抽象接口的epoll实现, 用于释放特定I/O复用机制内部使用的私有数据结构
 * @base[IN]: Reactor组件
 * @arg[IN]: 特定IO机制的内部私有数据, epoll中为epollop
 */
static void
epoll_dealloc(struct event_base *base, void *arg)
{
	/* epoll实现当然为epollop类型数据 */
	struct epollop *epollop = arg;

	/* 释放信号事件相关资源 */
	evsignal_dealloc(base);

	/* 如果事件数组存在, 则释放该数组空间 */
	if (epollop->fds)
		free(epollop->fds);

	/* 如果已就绪事件数组存在, 则释放该空间 */
	if (epollop->events)
		free(epollop->events);

	/* 如果epoll文件描述符有效, 则需要关闭该描述符 */
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	/* 释放epoll内部数据本身空间 */
	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
