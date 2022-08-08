/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* #include <string.h> */
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <utime.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/kernel.h>
#include <asm/segment.h>

int sys_ustat(int dev, struct ustat * ubuf)
{
	return -ENOSYS;
}

int sys_utime(char * filename, struct utimbuf * times)
{
	struct m_inode * inode;
	long actime,modtime;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (times) {
		actime = get_fs_long((unsigned long *) &times->actime);
		modtime = get_fs_long((unsigned long *) &times->modtime);
	} else
		actime = modtime = CURRENT_TIME;
	inode->i_atime = actime;
	inode->i_mtime = modtime;
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */
int sys_access(const char * filename,int mode)
{
	struct m_inode * inode;
	int res, i_mode;

	mode &= 0007;
	if (!(inode=namei(filename)))
		return -EACCES;
	i_mode = res = inode->i_mode & 0777;
	iput(inode);
	if (current->uid == inode->i_uid)
		res >>= 6;
	else if (current->gid == inode->i_gid)
		res >>= 6;
	if ((res & 0007 & mode) == mode)
		return 0;
	/*
	 * XXX we are doing this test last because we really should be
	 * swapping the effective with the real user id (temporarily),
	 * and then calling suser() routine.  If we do call the
	 * suser() routine, it needs to be called last. 
	 */
	if ((!current->uid) &&
	    (!(mode & 1) || (i_mode & 0111)))
		return 0;
	return -EACCES;
}

int sys_chdir(const char * filename)
{
	struct m_inode * inode;

	if (!(inode = namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->pwd);
	current->pwd = inode;
	return (0);
}

int sys_chroot(const char * filename)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->root);
	current->root = inode;
	return (0);
}

int sys_chmod(const char * filename,int mode)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if ((current->euid != inode->i_uid) && !suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
	inode->i_dirt = 1;
	iput(inode);
	return 0;
}

int sys_chown(const char * filename,int uid,int gid)
{
	struct m_inode * inode;

	if (!(inode=namei(filename)))
		return -ENOENT;
	if (!suser()) {
		iput(inode);
		return -EACCES;
	}
	inode->i_uid=uid;
	inode->i_gid=gid;
	inode->i_dirt=1;
	iput(inode);
	return 0;
}

int sys_open(const char * filename,int flag,int mode)
{
	struct m_inode * inode;
	struct file * f;
	int i,fd;

	mode &= 0777 & ~current->umask;
    for(fd=0 ; fd<NR_OPEN ; fd++)
		if (!current->filp[fd]) //遍历进程1的filp，直到获取一个空闲项，fd就是这个空闲项的索引
			break;
    //如果此条件成立，说明 filp[20] 中没有空闲项了，直接返回
	if (fd>=NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);

    //获取file_table[64]首地址
	f=0+file_table;

    //遍历file_table[64],直到获取到一个空闲项，f就是这个空闲项的指针
	for (i=0 ; i<NR_FILE ; i++,f++)
		if (!f->f_count) break;

    //如果此条件成立，说明file_table[20]中没有空闲项了，直接返回
	if (i>=NR_FILE)
		return -EINVAL;

    //将进程1的filp[20]与file_table[64]挂接，并增加引用计数
	(current->filp[fd]=f)->f_count++;

    //获取文件inode，即标准输入设备文件的inode，此时的filename就是路径/dev/tty0的指针
    //open("/dev/tty0",O_RDWR,0);
	if ((i=open_namei(filename,flag,mode,&inode))<0) {
		current->filp[fd]=NULL;
		f->f_count=0;
		return i;
	}
/* ttys are somewhat special (ttyxx major==4, tty major==5) */
    //通过检测tty0文件的i节点属性，得知它是设备文件
	if (S_ISCHR(inode->i_mode)) {
        //得知设备号是4
		if (MAJOR(inode->i_zone[0])==4) {
			if (current->leader && current->tty<0) {
                //设置当前进程的tty号为该i节点的子设备号
				current->tty = MINOR(inode->i_zone[0]);
                //设置当前进程tty对应的tty表项的父进程组号为进程的父进程组号　
				tty_table[current->tty].pgrp = current->pgrp;
			}
		} else if (MAJOR(inode->i_zone[0])==5)
			if (current->tty<0) {
				iput(inode);
				current->filp[fd]=NULL;
				f->f_count=0;
				return -EPERM;
			}
	}
/* Likewise with block-devices: check for floppy_change */
	if (S_ISBLK(inode->i_mode))
		check_disk_change(inode->i_zone[0]);
	f->f_mode = inode->i_mode; //用该i节点属性，设置文件属性
	f->f_flags = flag; //用flag参数，设置文件标识
	f->f_count = 1; //将文件引用计数加1
	f->f_inode = inode; //文件与i节点建立关系
	f->f_pos = 0; //将文件读写指针设置为0
	return (fd);
}

int sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

// 由于进程2继承了进程1的管理信息，因此其filp[20]中文件指针存储情况与进程1是一致的。
// close（0）就是要将filp[20]第一项清空（就是关闭标准输入设备文件tty0），
// 并递减file_table[64]中f_count的引用计数。
int sys_close(unsigned int fd)
{	
	struct file * filp;

	if (fd >= NR_OPEN)
		return -EINVAL;
	current->close_on_exec &= ~(1<<fd);
	if (!(filp = current->filp[fd])) //获取进程2标准输入设备文件的指针
		return -EINVAL;
	current->filp[fd] = NULL; //进程2与该设备文件解除关系
	if (filp->f_count == 0)
		panic("Close: file count is 0");
	if (--filp->f_count) //该设备文件引用计数递减
		return (0);
	iput(filp->f_inode);
	return (0);
}
