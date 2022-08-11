/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res ; \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	/* struct m_inode * inode;*/
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev); //检查是否换公盘，并做相应处理
	if ((s = get_super(dev))) //如果已经加载过了，直接使用，倒霉蛋没有加载过，需要继续初始化
		return s;

    //NR_SUPER是8
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break;
	}
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s); //锁定超级块
    //调用bread（）函数，把超级块从虚拟盘上读进缓冲区，并从缓冲区复制到super_block[8]的第一项。
    //给硬盘发送操作命令，do_hd_request()
    //当前给虚拟盘发送命令，do_rd_request()
    //超级块复制到缓冲块中后，将缓冲块中的超级块数据复制到super_block[8]的第一项，虚拟盘这个根设备就由super_block[8]的第一项管理
    //之后调用brelse(bh);释放这个缓冲块
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =      //将缓冲区中的超级块复制到
		*((struct d_super_block *) bh->b_data); //super_block[8]第一项
	brelse(bh); //释放这个缓冲块
    //判断超级块的魔术数字是否正确，正确释放超级块
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}

    // 初始化super_block[8]中的虚拟盘超级块中的i节点位图s_imap、逻辑块位图s_zmap，
    // 并把虚拟盘上i节点位图、逻辑块位图所占用的所有逻辑块读到缓冲区，
    // 将这些缓冲块分别挂接到s_imap[8]和s_zmap[8]上。
    // 由于对它们的操作会比较频繁，所以这些占用的缓冲块并不被释放，它们将常驻在缓冲区内。
    // 超级块通过指针与s_imap和s_zmap实现挂接

	// 初始化s_imap[8]
    for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;

    // 初始化s_zmap[8]
    for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;

    //虚拟盘的第一块super_block[0]是超级块，第二块开始是inode位图和逻辑块位图
	block=2;

	for (i=0 ; i < s->s_imap_blocks ; i++) //把虚拟盘上inode位图所占用的所有逻辑块
		if ((s->s_imap[i]=bread(dev,block))) //读到缓冲区，分别挂接到s_imap[8]上
			block++;
		else
			break;

	for (i=0 ; i < s->s_zmap_blocks ; i++) //把虚拟盘上逻辑块位图所占用的所有逻辑块
		if ((s->s_zmap[i]=bread(dev,block)))  //读到缓冲区，分别挂接到s_zmap[8]上
			block++;
		else
			break;

    // 如果i节点位图、逻辑块图锁占用的块数不对，说明操作系统有问题，则应释放前面获得的缓冲块及超级块
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
    //牺牲一个i节点，以防止查找算法返回0
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1; //与0号i节点混淆?
	free_super(s); //超级块设置完毕，解除对超级块项的保护
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name))) //获取hd1设备文件inode
		return -ENOENT;

	dev = dev_i->i_zone[0]; //通过inode获取设备号

    //如果hd1文件不是块设备文件
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i); //就释放掉它的inode
		return -EPERM;
	}

    //释放hd1设备文件inode
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;

	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}

    //通过设备号，读取设备的超级块
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i; //将超级块中s_imount与根文件系统中dir_i挂接
	dir_i->i_mount=1;   //给dir_i做标记，表明该i节点上已经挂接了文件系统
	dir_i->i_dirt=1;	//给dir_i做标记，表明i节点上的信息已经被更改

                        /* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");

    //初始化file_table[64]
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;

    //2代表软盘，根设备是虚拟盘为1
    if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}

    //初始化super_block[8]
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
    //从虚拟盘中读取设备的超级块，并复制到super_block[8]数组中
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");

    //调用iget()函数，从虚拟盘上读取根节点inode。通过根inode可以到文件系统中任何指定的inode，即能找到任何指定的文件。
    //将inode指针返回并赋值给mi指针
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");

	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
    //将代表虚拟盘根节点的inode指针，赋值到super_block[8]超级块结构体对应的字段s_sisup,s_imount指针上。
	p->s_isup = p->s_imount = mi;

	current->pwd = mi;  //当前进程（进程1）掌控根文件系统的根i节点，
	current->root = mi; //父子进程创建机制将这个特性遗传给子进程
	free=0;
    //找到了超级块，就可以根据超级块中"逻辑块位图"里记载的信息，计算出虚拟盘上数据块的占用和空闲情况，
	i=p->s_nzones;
	while (-- i >= 0) //计算虚拟盘中空闲的逻辑块的总数
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0) //计算虚拟盘中空闲的i节点的总数
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
