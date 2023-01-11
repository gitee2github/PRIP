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



## 2 通用函数 utils.c utils.h

2.1 utils.h中定义各个prip相关文件的路径，声明通用函数，如下：

```
#define PATH_PRIP_CONFIG "/proc/prip/prip_config"
#define PATH_PRIP_ALARM "/proc/prip/prip_alarm"
#define PATH_PRIP_TIMEOUT "/proc/prip/prip_cache_timeout"
#define PATH_PRIP_STATE "/proc/prip/prip_state"
#define PATH_PRIP_SET "/proc/sys/net/ipv4/prip_set"

```

2.2 utils.c 中对通用函数进行实现，主要有如下函数：

```
void exec_config_cmd(char *cmd, char *str, int flag)
```

​	在程序中执行cmd指令对文件进行查看，并将查看结果打印输出，关键函数popen



```
void exec_cmd(char *cmd)
```

​	在程序中执行cmd指令，对文件进行修改或者设置网口ip地址



```
int check_ip(char *str)
```

​	检查ip地址的合法性

