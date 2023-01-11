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



## 3 开启prip，设置prip参数 set.c set.h

主函数

```
int do_set(int argc, char **argv)
```

​	根据地一个参数，判断是设置ip地址还是设置prip

​	如果是设置prip，再根据第二个参数判断是设置prip的配置信息，或者告警阈值，或者超时时间



```
//设置网口ip地址和掩码
static int set_dev(char *devname, char *value, int mask)
```



```
//开启prip功能，并设置主从链路的ip地址对
static int set_config(char *ip1, char *ip2, int mask)
```



```
//	设置告警阈值的大小，默认为500，如果主链路或者从链路连续500次没有收到包，则进行报警
static int set_alarm(int value)
```



```
//设置超时时间，默认是3秒
static int set_timeout(int value)
```



## 4 关闭prip

主函数

```
int do_unset(int argc, char **argv)
```

通过exec_cmd函数将/proc/sys/net/ipv4/prip_set置为0，将/proc/prip/prip_config 置为空

```
static int unset_prip(void)
```



## 5 prip的显示功能

主函数

```
int do_show(int argc, char **argv)
```

根据第一个参数，确认出需要显示输出的配置项，包括：alarm timeout config state，并调用相应的函数实现

```
//输出告警阈值信息
static void show_alarm(void)
{
    char cmd[1024];
    char str[16];
    strcpy(str, "PRIP_ALARM"); 
    snprintf(cmd, sizeof(cmd), "cat %s", PATH_PRIP_ALARM);
    exec_config_cmd(cmd, str, 1);
    return;
}
```

