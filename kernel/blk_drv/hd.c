/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7
#define MAX_HD		2

static void recal_intr(void);

static int recalibrate = 0;
static int reset = 0;

/*
 *  This struct defines the HD's and their types.
 */
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

static struct hd_struct {
	long start_sect; //起始扇区号
	long nr_sects;   //总扇区数
} hd[5*MAX_HD]={{0,0},};

#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr))

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr))

extern void hd_interrupt(void);
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
int sys_setup(void * BIOS)
{
	static int callable = 1;
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	if (!callable) //控制只能调用一次
		return -1;
	callable = 0;
#ifndef HD_TYPE
	for (drive=0 ; drive<2 ; drive++) { //读取drive_info设置hd_info
		hd_info[drive].cyl = *(unsigned short *) BIOS; //柱面数
		hd_info[drive].head = *(unsigned char *) (2+BIOS); //磁头数
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
		hd_info[drive].sect = *(unsigned char *) (14+BIOS); //每磁道扇区数
		BIOS += 16;
	}
	if (hd_info[1].cyl) //判断有几个硬盘
		NR_HD=2;
	else
		NR_HD=1;
#endif
    //一个物理硬盘最多可以分4个逻辑盘，0是物理盘，1～4是逻辑盘，
    // 共5个，第1个物理盘是0*5，第2个物理盘是1*
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}

    //第1个物理盘设备号是0x300，第2个是0x305，读每个物理硬盘的0号块，即引导块，有分区
	for (drive=0 ; drive<NR_HD ; drive++) {
        //进程1：
        // 1.读取硬盘的引导块到缓冲区：进入bread()后，调用getblk(),申请一个新空闲的缓冲块；
        // 在getblk()函数中，先调用get_hash_table()查找哈希表，检查此前是否有程序吧要读的硬盘逻辑块读到缓冲区。如果已经读了，无需再读。
        // 进入get_hash_table函数后，调用find_buffer查找缓冲区是否有指定的设备号，块号的缓冲块。第一次返回null，退出find_buffer,退出get_hash_table,
        // 返回到getblk()，在free_list中心申请新的缓冲块，然后进行初始化设置，并挂接到hash_table中,挂接过程比较复杂（remove_from_queues,insert_into_queues），
        // 挂接hash_table完成后，就执行完了getblk()，返回到bread()，
        // 在bread函数中，调用ll_rw_block()这个函数，将缓冲块与请求项结构进行挂接：调用make_request()，注意：make_request需要先将缓冲块加锁
        // 目的是保护这个缓冲块在解锁之前将不再被任何进程操作，lock_buffer(),初始化request完成后，调用add_request()向请求项队列中加载该请求项。
        // 对当前硬盘工作进行分析，然后设置请求项为当前请求项，并调用硬盘处理函数(dev->request_fn),即do_hd_request给硬盘发送读盘命令。
        // 发送读盘命令（使用电梯算法让磁盘的移动距离最小）hd_out()，注意hd_out函数的win_read,read_intr的两个参数，
        // 当前为读命令，所以是read_intr，硬盘开始读的时候，程序调用结束返回：hd_out,do_hd_request,add_request,make_request,ll_rw_block
        // 一直到bread()，硬盘数据还没有读完，调用wait_on_buffer，因为锁还没释放，调用sleep_on，挂起等待！
        // 进程1当前状态为：不可中断等待状态。调用schedule()函数，切换到进程0，进程0进行怠速运行。硬盘继续读扇区数据，某个时刻硬盘数据读完，产生硬盘中断。
        // CPU接到中断指令后，终止正在执行的程序，开始执行中断服务代码即read_intr，read_intr将硬盘缓存复制到锁定的缓冲块中。
        // 引导块的数据是1024，当前只读了一半数据512，需要继续读。这个时候进程1仍处在被挂起状态，
        // pause（）、sys_pause（）、schedule（）、switch_to（0）循环从刚才硬盘中断打断的地方继续循环，硬盘继续读盘……
        // 某个时刻硬盘数据读完，产生硬盘中断。再次进入read_intr函数后，此时数据已经读完，跳转到end_request()
        // end_request中，数据已经全部读取完成，更新标准b_uptodata为1。

        // 2.硬盘数据被加载到内存后，调用unlock_buffer()->wake_up()函数将进程1设置为就绪态，
        // 由schedule函数调用switch_to切换进程1执行ljmp %0切走CPU执行流,
        // 返回切换者（进程1）sleep_on()函数，最终返回到bread()函数，此时b_uptodata为1，函数执行完毕。
        // 7.读盘操作完成后，进程调度切换到进程1执行
		if (!(bh = bread(0x300 + drive*5,0))) {
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
        //如果扇区的最后2字节不是'55AA',表示数据无效。
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
        //读取的数据是有效的，根据引导块中的分区信息设置hd[]
		p = 0x1BE + (void *)bh->b_data;
		for (i=1;i<5;i++,p++) {
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh); //释放缓冲区（引用计数减1）
	}
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	rd_load();
	mount_root();
	return (0);
}

static int controller_ready(void)
{
	int retries=100000;

	while (--retries && (inb_p(HD_STATUS)&0x80));
	return (retries);
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);
	return (1);
}

//进入hd_out（）函数中去执行读盘的最后一步：下达读盘指令
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void)) //对比调用的传参WIN_READ，＆read_intr
{
	register int port asm("dx");

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	do_hd = intr_addr;//根据调用的实参决定是read_intr还是write_intr，第一次是read_intr
                      //把读盘服务程序与硬盘中断操作程序相挂接
                      //这里面的do_hd是system_call.s中_hd_interrupt下面xchgl_do_hd，%edx这一行所描述的内容。
                      //现在要做读盘操作，所以挂接的就是实参read_intr，如果是写盘，挂接的就应该是write_intr（）函数。
	outb_p(hd_info[drive].ctl,HD_CMD);
	port=HD_DATA;
	outb_p(hd_info[drive].wpcom>>2,++port);
	outb_p(nsect,++port);
	outb_p(sect,++port);
	outb_p(cyl,++port);
	outb_p(cyl>>8,++port);
	outb_p(0xA0|(drive<<4)|head,++port);
	outb(cmd,++port);
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS);
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == (READY_STAT | SEEK_STAT))
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);
	for(i = 0; i < 100; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);
}

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}

static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

//read_intr（）函数会将已经读到硬盘缓存中的数据复制到刚才被锁定的那个缓冲块中
// （注意：锁定是阻止进程方面的操作，而不是阻止外设方面的操作），
// 这时1个扇区256字（512字节）的数据读入前面申请到的缓冲块，如图3-27中的第二步所示。执行代码如下
static void read_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256);
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	if (--CURRENT->nr_sectors) {
        //引导块的数据是1024字节，
        //请求项要求的也是1024字节，如果没有读完1024，硬盘会继续读盘
		do_hd = &read_intr;
		return;
	}
    //请求项要求的数据量(1024)全部读完了，执行end_request
	end_request(1);
	do_hd_request();
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr;
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

//进入do_hd_request（）函数去执行，为读盘做最后准备工作。
void do_hd_request(void)
{
	int i,r = 0;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;
	dev /= 5;
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;
	nsect = CURRENT->nr_sectors;
	if (reset) {
		reset = 0; //置位，防止多次执行if(reset)
		recalibrate = 1; //置位，确保执行下面的if
		reset_hd(CURRENT_DEV); //将通过调用hd_out向硬盘发送WIN_SPECIFY，
                                   // 建立硬盘读盘必要的参数
		return;
	}
	if (recalibrate) {
		recalibrate = 0; //置位，防止多次执行if(recalibrate)
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr); //将向硬盘发送WIN_RESTORE命令，将磁头移动到0柱面，以便从硬盘上读取数据
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr); //注意这两个参数
        //进入hd_out（）函数中去执行读盘的最后一步：下达读盘指令
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

//代码路径：kernel/blk_dev/hd.c：
//与rd_init类似，参看rd_init的注释
//硬盘的初始化为进程与硬盘这种块设备进行I/O通信建立了环境基础
void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST; //挂接do_hd_request()
	set_intr_gate(0x2E,&hd_interrupt); //设置硬盘中断
	outb_p(inb_p(0x21)&0xfb,0x21); //允许8259A发出中断请求
	outb(inb_p(0xA1)&0xbf,0xA1); //允许硬盘发送中断请求
}
