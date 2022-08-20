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