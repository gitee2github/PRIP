# 							priptool工具设计方案

## 1 main函数 priptool.c

1.1 实现priptool的用法描述Usage,具体如下：

```
            "[Options] : \n"
                    "\tset\t[dev]\t[devname xxx.xxx.xxx.xxx mask]\n"
                    "\tset\t[prip] [devname1 devname2 mask]\n"
                    "\tset\t[alarm] [value]\n"
                    "\tset\t[timeout] [value]\n"
                    "\tunset\t[prip]\n"
                    "\tshow\t[alarm]\n"
                    "\tshow\t[timeout]\n"
                    "\tshow\t[config]\n"
                    "\tshow\t[state]\n"
            "[Examples] :\n"
                    "\tset\t[dev]\t[eth1 192.168.122.80 24]\n"
                    "\tset\t[prip]\t[192.168.10.10 192.168.11.10 24]\n"
                    "\tset\t[alarm]\t[100]\n"
                    "\tshow\t[alarm]\n",

```

1.2  实现do_cmd函数，通过匹配对应的操作集（set unset show help version），调用相应的回调函数，操作集合如下：

```
static const struct cmd cmds[] =
{
    { "set",    do_set},
    { "unset",  do_unset},
    { "show",   do_show},
    { "help",   do_help},
    { "version",show_version},
    { 0 }
};

```



