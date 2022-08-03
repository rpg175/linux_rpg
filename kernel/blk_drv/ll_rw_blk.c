/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;
    //先对当前硬盘的工作情况进行分析，然后设置该请求项为当前请求项，
    // 并调用硬盘请求项处理函数（dev-＞request_fn）（），
    // 即do_hd_request（）函数去给硬盘发送读盘命令。
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		sti();
		(dev->request_fn)();
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next)
        //电梯算法的作用是让磁盘磁头的移动距离最小
		if ((IN_ORDER(tmp,req) || 
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next; //挂接请求项队
	tmp->next=req;
	sti();
}

// 进程1继续执行，进入make_request（）函数后，先要将这个缓冲块加锁，
// 目的是保护这个缓冲块在解锁之前将不再被任何进程操作，
// 这是因为这个缓冲块现在已经被使用，
// 如果此后再被挪作他用，里面的数据就会发生混乱。
static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if ((rw_ahead = (rw == READA || rw == WRITEA))) {
		if (bh->b_lock) //还没有加锁b_lock = 0
			return;
		if (rw == READA) //放弃预读写，改为普通读写
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	lock_buffer(bh); //加锁
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
        //第一次还没有使用
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	if (rw == READ) //读直接从尾端开始
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3); //写从2/3处开始
/* find an empty request */
	while (--req >= request) //从后向前搜索空闲请求项，在blk_dev_init中，dev初始化为-1，即空闲
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	req->dev = bh->b_dev; //设置请求项
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
    //调用add_request（）函数，向请求项队列中加载该请求项
	add_request(major+blk_dev,req);
}

void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

    //先判断缓冲块对应的设备是否存在或这个设备的请求项函数是否挂接正常。
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
        //NR_BLK_DEV是7，主设备号0-6，＞=7意味着不存在
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
    //操作这个缓冲块
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
