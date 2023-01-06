# prip协议启用与关闭功能

这项功能是对系统原有的接口函数进行了扩展，在setsockopt函数里的IPPROTO_IP的相关操作里做了修改，增加了PRIP协议的IP选项的相关操作，TCP以及UDP协议都要调用setsockopt函数来完成IP选项的设置，在该函数里又需要调用do_ip_setsockopt来完成。

所以在do_ip_setsockopt函数完成ip选项设置，该函数位于net/ipv4/ip_sockglue.c



(1)保存原 socket 中对 PRIP 的设置情况,将 PRIP 的偏移赋值给变量 prip。

(2)解析用户传入的选项,将其保存在 opt 变量中。

(3)将 opt 变量中的__data 中的主从标志置 0。

(4)根据 prip 的值来决定是否增加 PRIP 计数:

如果原 socket 已经添加 PRIP 选项,且 opt 中包含 PRIP 选项,则将 PRIP 计数加一;

如果原 socket 未添加 PRIP 选项,则增加 PRIP 计数(添加 PRIP 选项),如果增加计数失

败,则将 opt 的 prip 偏移值置为 1,并将返回值置为错误。

如果原 socket 已添加 PRIP 选项,且 opt 中不包含 PRIP 选项(去除 PRIP 选项),则将

PRIP 计数减一。