# Linux 0.11 debug by eclipse + gdb

实验使用的软件版本
* ubuntu-16.04.7-desktop-amd64
* jdk-8u91-linux-x64
* eclipse-cpp-2019-03-R-linux-gtk-x86_64
* 使用的linux版本[Running_Linux0.11](https://github.com/Original-Linux/Running_Linux0.11)，该版本支持ubuntu-16.04
* 安装qemu `sudo apt-get install qemu`

## Linux start by qemu

Build Linux-0.11
```
make
``` 

Boot Linux-0.11 on qemu
```
make start
```

Debug Linux-0.11 in GDB
```
make debug
```

## Eclipse + Gdb debug linux

### Eclipse的配置

Debugger Console
```
(gdb)file tools/system
(gdb)directory ./
(gdb)set architectur i8086
(gdb)set disassembly-flavor intel
(gdb)b *0x7c00
(gdb)layout split
(gdb)c
```

```
(gdb)x/16xb 0x7DF0
```

```
si
```

```
b main
```
