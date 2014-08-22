/*
 * Copyright (c) 2006 Maxim Yegorushkin <maxim.yegorushkin@gmail.com>
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
#ifndef _MIN_HEAP_H_
#define _MIN_HEAP_H_

#include "event.h"

/* 最小堆, 堆元素是事件指针 */
typedef struct min_heap
{
	/* 用一个动态数组保存作为最小堆 */
    struct event** p;

	/* n记录队中当前元素的个数, a记录动态数组的大小, 两者的关系n <= a */
    unsigned n, a;
} min_heap_t;

static inline void           min_heap_ctor(min_heap_t* s);
static inline void           min_heap_dtor(min_heap_t* s);
static inline void           min_heap_elem_init(struct event* e);
static inline int            min_heap_elem_greater(struct event *a, struct event *b);
static inline int            min_heap_empty(min_heap_t* s);
static inline unsigned       min_heap_size(min_heap_t* s);
static inline struct event*  min_heap_top(min_heap_t* s);
static inline int            min_heap_reserve(min_heap_t* s, unsigned n);
static inline int            min_heap_push(min_heap_t* s, struct event* e);
static inline struct event*  min_heap_pop(min_heap_t* s);
static inline int            min_heap_erase(min_heap_t* s, struct event* e);
static inline void           min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void           min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e);

/* 堆中元素关键字的比较原则, 对于定时event来说, 肯定是比较定时时间, 定时值最小的处于堆顶 */
int min_heap_elem_greater(struct event *a, struct event *b)
{
    return timercmp(&a->ev_timeout, &b->ev_timeout, >);
}

/* 构造一个最小堆, 没有分配内存, 指针值为NULL */
void min_heap_ctor(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }

/* 释放最小堆, 就是释放堆空间 */
void min_heap_dtor(min_heap_t* s) { free(s->p); }

/* 没有放入堆中的事件中的min_heap_idx置为-1, 事件结构体中的该成员就是事件在堆中数组的索引 */
void min_heap_elem_init(struct event* e) { e->min_heap_idx = -1; }

/* 判断最小堆是否为空 */
int min_heap_empty(min_heap_t* s) { return 0u == s->n; }

/* 获取堆的大小, 也就是元素的个数 */
unsigned min_heap_size(min_heap_t* s) { return s->n; }

/* 返回堆顶元素, 不存在则返回NULL */
struct event* min_heap_top(min_heap_t* s) { return s->n ? *s->p : 0; }

/***
 * 往堆中添加事件元素 
 * @s[IN]: 要插入到的堆结构
 * @e[IN]: 要添加的事件指针
 * @return: 成功返回0, 失败返回-1
 */
int min_heap_push(min_heap_t* s, struct event* e)
{
	/* 确认堆中具备再容纳一个元素的空间, 不够的话min_heap_reserve会扩大数组空间 */
    if(min_heap_reserve(s, s->n + 1))
        return -1;

	/* 将元素放在第一个空间位置, 并执行向上调整 */
    min_heap_shift_up_(s, s->n++, e);
    return 0;
}

/* 从堆中弹出堆顶元素 */
struct event* min_heap_pop(min_heap_t* s)
{
	/* 如果存在元素, 则执行动作, 否则直接返回空指针 */
    if(s->n)
    {
        struct event* e = *s->p;

		/* 弹出的元素索引置为无效索引-1 */
        e->min_heap_idx = -1;
        min_heap_shift_down_(s, 0u, s->p[--s->n]);
        return e;
    }
    return 0;
}

int min_heap_erase(min_heap_t* s, struct event* e)
{
    if(((unsigned int)-1) != e->min_heap_idx)
    {
        min_heap_shift_down_(s, e->min_heap_idx, s->p[--s->n]);
        e->min_heap_idx = -1;
        return 0;
    }
    return -1;
}

/* 保证堆中有至少有n个空间, 已有元素空间+空闲空间之和
 * @s[IN]: 堆结构
 * @n[IN]: 要保证的堆空间大小
 * @return: 成功返回0, 失败返回-1
 */
int min_heap_reserve(min_heap_t* s, unsigned n)
{
	/* 如果堆空间大小小于要保证的空间数目, 则需要重新分配空间, 如果堆数组空间不小于n则不动作 */
    if(s->a < n)
    {
        struct event** p;

		/* 如果最初数组大小为0(堆刚刚构造的时候), 默认设置数组大小为8, 否则将数组现在的大小扩大两倍
		 * 如果这样的数值还是小于n, 则直接将数组大小设置为n 
		 */
        unsigned a = s->a ? s->a * 2 : 8;
        if(a < n)
            a = n;

		/* 调整空间大小, 注意库函数realloc的功能*/
        if(!(p = (struct event**)realloc(s->p, a * sizeof *p)))
            return -1;
        s->p = p;

		/* 记录堆新数组的大小 */
        s->a = a;
    }
    return 0;
}

/* 将元素e放到hole_index索引处, 如果破坏了小根堆结构, 则上将hole_index与其父节点对调,
 * 注意这里说的对调起始仅仅是将父节点指针及min_heap_idx修改, 并没有把新插入的节点放入堆中,
 * 而是直到找到重新满足小根堆结构的位置时才赋值, 这样就节省了lgN次指针赋值操作
 */
void min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    unsigned parent = (hole_index - 1) / 2;
    while(hole_index && min_heap_elem_greater(s->p[parent], e))
    {
        (s->p[hole_index] = s->p[parent])->min_heap_idx = hole_index;
        hole_index = parent;
        parent = (hole_index - 1) / 2;
    }
    (s->p[hole_index] = e)->min_heap_idx = hole_index;
}

/* 将索引hole_index处的元素下调, 直到满足小根堆的结构, 小根堆元素下移时, 也将本节点与其左右子节点比较,
 * 将本节点与较小的子节点调换位置
 * @hole_index[IN]: 索引, 从0开始
 */
void min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e)
{
	/* 假设最小节点是本节点的右节点 */
    unsigned min_child = 2 * (hole_index + 1);

	/* */
    while(min_child <= s->n)
	{
        min_child -= min_child == s->n || min_heap_elem_greater(s->p[min_child], s->p[min_child - 1]);
        if(!(min_heap_elem_greater(e, s->p[min_child])))
            break;
        (s->p[hole_index] = s->p[min_child])->min_heap_idx = hole_index;
        hole_index = min_child;
        min_child = 2 * (hole_index + 1);
	}
    min_heap_shift_up_(s, hole_index,  e);
}

#endif /* _MIN_HEAP_H_ */
