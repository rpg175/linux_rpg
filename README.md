# Linux 0.11 debug by eclipse + gdb

## Linux start by qemu

Image, at root dir input commond:
```
make
``` 

Start linux
```
make start
```

Debug linux

```
make debug
```

## Eclipse + Gdb debug linux

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