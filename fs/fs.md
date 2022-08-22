# linux0.11的文件系统

操作系统对文件的一切操作，都可以分为两个方面：对super_block、d_super_block、m_inode、d_inode、i节点位图、逻辑块位图这类文件管理信息的操作以及对文件数据内容的操作。新建、打开、关闭、删除文件属于对文件管理信息的操作。读文件、写文件和修改文件则主要是操作文件数据内容。
操作文件管理信息就是建立或解除进程与文件的关系链条，链条的主干为task_struct中的*filp[20]——file_table[64]——inode_table[32]。进程就可以沿着关系链条，依托缓冲区与硬盘进行数据交互。当关系链条解除后，进程则不再具备操作指定文件的能力。如果文件管理信息被更改，则操作系统要将此更改落实在硬盘上，以免失去对文件数据内容的控制

## 数据同步到外设的两种方法
数据从缓冲区同步到硬盘有两种方法。
* 通过updata定期同步；
* 因缓冲区使用达到极限，操作系统强行同步。

### 第一种方法：通过updata定期同步
shell进程在第一次执行时，启动了一个updata进程。
这个进程常驻于内存，功能就是将缓冲区中的数据同步到外设上。
该进程会调用pause（）函数，这个函数会映射到sys_pause（）函数中，使该进程被设置为可中断等待状态。
每隔一段时间，操作系统就将updata进程唤醒。
它执行后，调用sync（）函数，将缓冲区中的数据同步到外设上。
sync（）函数最终映射到sys_sync（）系统调用函数去执行。
为了保证文件内容同步的完整性，需要将文件i节点位图、文件i节点、文件数据块、数据块对应的逻辑块位图，全都同步到外设。
sys_sync（）函数先将改动过的文件i节点写入缓冲区（其余内容已经在缓冲区中了），之后，遍历整个缓冲区，
只要发现其中缓冲块内容被改动过（b_dirt被置1），就全部同步到外设上。

### 第二种方法：写入数据超过缓冲区，比如超过10mb
要写入的数据将达到10 MB以上，而缓冲区，肯定不可能超过10 MB。
因此，当前进程要写入数据的话，很可能在updata进程被唤醒之前，就已经将缓冲区写满。
若继续写入，就需要强行将缓冲区中的数据同步到硬盘，为续写腾出空间。
此任务是由getblk（）函数完成的。
当在缓冲区中找到的空闲块都已经无法继续写入信息（b_dirt都是1）时，就说明需要腾空间了

```
struct buffer_head * getblk(int dev,int block) {
    ....
    while（bh-＞b_dirt）{
        //虽然找到空闲缓冲块，但b_dirt仍是1，说明缓冲区中已无可用的缓冲块了，需要同步腾空　　
        sync _dev（bh-＞b_dev);
        //同步数据　　
        wait _on_buffer（bh;
    　　if（bh-＞b_count）　　
           goto repeat；　　
    }
    ....
}
```

## 修改文件
修改文件的本质就是可以在文件的任意位置插入数据、删除数据，且不影响文件已有数据。
此问题的处理方案是：将sys_read（）、sys_write（）以及sys_lseek（）几个函数组合使用。
sys_read（）和sys_write（）

```c
#include<fcntl.h>
#include<stdio.h>
#include<string.h>
#define LOCATION 6

int main(char argc,char ** argv)
{
    char str1[]="Linux";
    char str2[1024];
    int fd,size;
    memset(str2,0,sizeof(str2));
    fd=open("hello.txt",O_RDWR,0644);
    lseek(fd,LOCATION,SEEK_SET);
    strcpy(str2,str1);
    size=read(fd,str2+5,6);
    lseek(fd,LOCATION,SEEK_SET);
    size=write(fd,str2,strlen(str2));
    close(fd);
    return 0:
}
```

## 关闭文件
1. 当前进程的file与file_table[64]脱勾。
   1. 通过fd找到file_table中的file，
   2. 将file_table[fd]重置为null，
   3. 将file->f_count 引用计数器减1，
   4. iput(file->f_inode)脱勾
2. 文件inode被释放：先要对inode进行检查，然后将inode的i_count减1，当inode的引用计数变为0，这个inode_table[32]中的表项为空闲项。

## 删除文件
1. 删除的文件，需要保证其他进程都无法访问
2. 删除文件，需要将逻辑块位图清空，inode节点位图清空，目录项清空
