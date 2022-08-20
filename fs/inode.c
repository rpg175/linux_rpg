/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h> 
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
    //遍历所有inode
	for(i=0 ; i<NR_INODE ; i++,inode++) {
        //如果遍历到inode正在使用就等待indoe解锁
		wait_on_inode(inode);
        //如果inode节点内容已经被改动过，而且不是管道文件的inode
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode); //将inode 同步到缓冲区
	}
}

static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

    //如果待操作文件数块号小于0
	if (block<0)
		panic("_bmap: block<0");

    //如果待操作文件数据块号大于允许的文件数据量最大值
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");

    //待操作数据块文件块号小于7
	if (block<7) {
        //如果是创建一个函数块
		if (create && !inode->i_zone[block])
			if ((inode->i_zone[block]=new_block(inode->i_dev))) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	block -= 7;

    //大于7、小于等于（7+512）个逻辑块的情况
	if (block<512) {
        //待操作数据块文件块号小于512，需要一级间接检索文件块号
		if (create && !inode->i_zone[7])
			if ((inode->i_zone[7]=new_block(inode->i_dev))) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
        //一级间接块中没有索引号，无法继续查找，直接返回0
		if (!inode->i_zone[7])
			return 0;
        //获取一级间接块
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
        //取该间接块上第block项中的逻辑块
		i = ((unsigned short *) (bh->b_data))[block];

        //如果是创建一个新数据块，执行下面代码
		if (create && !i)
			if ((i=new_block(inode->i_dev))) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
    //大于（7+512）、小于（7+512+512×512）个逻辑块的情况
	block -= 512;
	if (create && !inode->i_zone[8])
		if ((inode->i_zone[8]=new_block(inode->i_dev))) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}

    //一级间接块中没有索引号，无法继续查找，直接返回0
	if (!inode->i_zone[8])
		return 0;

    if (!(bh=bread(inode->i_dev,inode->i_zone[8]))) //获取一级间接块
		return 0;

    //取该间接块上第block/512项中的逻辑块号
    i = ((unsigned short *)bh->b_data)[block>>9];
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;

    //获取二级间接块
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block&511]=i;
			bh->b_dirt=1;
		}
    //create标志置位，不等于就要创建一个新数据块，必须确保文件的下一个文件块不存在，
    // 即！inode-＞i_zone[……]或！i成立，才能创建新数据块。比如本实例中加载目录项的内容，
    // 一个数据块中没有发现空闲项，很可能下一个数据块中就有，如果强行分配新数据块，
    // 就会把已有的块覆盖掉，导致目录文件管理混乱。
	brelse(bh);
	return i;
}

int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

int create_block(struct m_inode * inode, int block)
{
    //最后一个参数是创建标志位，变量为1，表示有可能创建数据块
	return _bmap(inode,block,1);
}
		
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	inode->i_count--;
	return;
}

struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
    //从inode_table[32]中申请一个空闲的inode位置，倒霉蛋拿到的是第一个inode_table[0]
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) { //查找与参数dev，nr相同的inode
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode); //找到相同的inode，等待解锁
		if (inode->i_dev != dev || inode->i_num != nr) { //解锁后发现inode和参数要求不匹配，需要再次遍历查找
			inode = inode_table;
			continue;
		}

		inode->i_count++;
		if (inode->i_mount) {
			int i;
            //如果是mount，则查找对应的超级块
			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;

			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev; //从超级块中获取设备号
			nr = ROOT_INO; //ROOT_INO为1，根inode
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev; //初始化
	inode->i_num = nr;
	read_inode(inode); //从虚拟盘上读出根inode
	return inode;
}

static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;
    //给参数inode加锁
	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	//通过inode所在的超级块，计算出inode所在的逻辑块号
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;
    //将inode所在的逻辑块整体读出，从中提取inode的信息，载入刚才加锁的inode位置上
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
    //释放缓冲块
	brelse(bh);
    //解锁inode
	unlock_inode(inode);
}

static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

    //先将inode加锁，保证写数据的原子性
	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}

    //获取外设超级块
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK; //确定inode位图在外设上的逻辑块号

    //将inode所在的逻辑块加载入缓冲区
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");

    //将inode同步到缓冲区
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
    //缓冲块设置为脏
	bh->b_dirt=1;
    //将inode设置为0
	inode->i_dirt=0;
	brelse(bh);
    //解锁inode
	unlock_inode(inode);
}
