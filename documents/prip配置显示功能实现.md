# prip配置显示功能实现



由于互为冗余的两条链路处于不同的局域网,为实现正常的网络通信,因此在通信本机上需要用户配置对端计算机 PRIP 设备的主从网卡的子网掩码。

PRIP 协议的配置功能通过 PROC 虚拟文件系统实现的。在内核启动后,会在/proc 目录下创建PRIP 所需文件节点,用户和内核程序通过对文件节点的读写来实现的数据交互。

| 名称                 | 对应项         | 所在目录              | 属性     |
| -------------------- | -------------- | --------------------- | -------- |
| 主从网段子网掩码     | prip_config    | /proc/prip/           | 用户属性 |
| 链路告警阀值         | prip_alarm     | /proc/prip            | 用户属性 |
| PRIP 功能            | PRIP_ON/OFF    | /proc/prip/prip_state | 功能显示 |
| 本地ip               | local_ip       | /proc/prip/prip_state | 功能显示 |
| 引用计数             | refcnt         | /proc/prip/prip_state | 功能显示 |
| 对端主ip             | peer_master_ip | /proc/prip/prip_state | 功能显示 |
| 对端从ip             | peer_slave_ip  | /proc/prip/prip_state | 功能显示 |
| 主链路状态           | master_state   | /proc/prip/prip_state | 功能显示 |
| 从链路状态           | slave_state    | /proc/prip/prip_state | 功能显示 |
| 主链路发送数据包数目 | master_send    | /proc/prip/prip_state | 功能显示 |
| 从链路发送数据包数目 | slave_send     | /proc/prip/prip_state | 功能显示 |
| 主链路接收数据包数目 | master_recv    | /proc/prip/prip_state | 功能显示 |
| 从链路接收数据包数目 | slave_recv     | /proc/prip/prip_state | 功能显示 |



proc文件的创建使用 create_proc_entry 来创建文件，提供应用程序与内核之间交互数据，示意图如图4.2

![](图片/prip配置显示功能实现/1.png)

## PRIP配置功能模块代码实现

PRIP 协议配置功能模块是以静态模块的形式编译实现的,实现代码添加到内核的具体路径是net/prip/

在net/prip/目录有下有 Makefile Kconfig prip.c 三个文件，其中Makefile负责prip.c编译生成prip.o ，Kconfig 是将prip.o 选择编译进内核，在源码顶层目录下的.config 中有CONFIG_prip的内核编译项，prip.c文件则是PRIP协议初始化，创建/proc/prip/下的一系列文件

PRIP所涉及到的数据结构在3.1节中已经做出说明，PRIP_CONFIG_T 结构体包含了用户要获取或者配置的 PRIP 相关变量信息。模块的实现主要是在net/prip/prip.c 文件中。先从PRIP的源头，init_prip开始

```
static int __init init_prip(void)
{
	int err;
	int i;
	//配置信息结构体的初始化

.....
.....
.....

err_5:
	remove_proc_entry("prip_alarm", dir_prip);
err_4:
	remove_proc_entry("prip_state", dir_prip);
err_3:
	remove_proc_entry("prip_config", dir_prip);
err_2:
	remove_proc_entry("prip", NULL);
err_1:
	return err;
}
```

