# prip协议启用与关闭功能

这项功能是对系统原有的接口函数进行了扩展，在setsockopt函数里的IPPROTO_IP的相关操作里做了修改，增加了PRIP协议的IP选项的相关操作，TCP以及UDP协议都要调用setsockopt函数来完成IP选项的设置，在该函数里又需要调用do_ip_setsockopt来完成。

所以在do_ip_setsockopt函数完成ip选项设置，该函数位于net/ipv4/ip_sockglue.c

```
static int do_ip_setsockopt(struct sock *sk, int level,
			    int optname, char __user *optval, unsigned int optlen)
{
	struct inet_sock *inet = inet_sk(sk);
	int val = 0, err;

	if (((1<<optname) & ((1<<IP_PKTINFO) | (1<<IP_RECVTTL) |
			     (1<<IP_RECVOPTS) | (1<<IP_RECVTOS) |
			     (1<<IP_RETOPTS) | (1<<IP_TOS) |
			     (1<<IP_TTL) | (1<<IP_HDRINCL) |
			     (1<<IP_MTU_DISCOVER) | (1<<IP_RECVERR) |
			     (1<<IP_ROUTER_ALERT) | (1<<IP_FREEBIND) |
			     (1<<IP_PASSSEC) | (1<<IP_TRANSPARENT))) ||
	    optname == IP_MULTICAST_TTL ||
	    optname == IP_MULTICAST_ALL ||
	    optname == IP_MULTICAST_LOOP ||
	    optname == IP_RECVORIGDSTADDR) {
		if (optlen >= sizeof(int)) {
			if (get_user(val, (int __user *) optval))
				return -EFAULT;
		} else if (optlen >= sizeof(char)) {
			unsigned char ucval;

			if (get_user(ucval, (unsigned char __user *) optval))
				return -EFAULT;
			val = (int) ucval;
		}
	}

	/* If optlen==0, it is equivalent to val == 0 */

	if (ip_mroute_opt(optname))
		return ip_mroute_setsockopt(sk, optname, optval, optlen);

	err = 0;
	lock_sock(sk);
	//根据setsockopt函数中设置的optname 来进入不同的处理代码段，ip选项则是进入case IP_OPTIONS
	switch (optname) {
	case IP_OPTIONS:
	{
		struct ip_options *opt = NULL;
		if (optlen > 40 || optlen < 0)
			goto e_inval;

#ifdef CONFIG_PRIP
		
#endif
		//从用户空间获取ip选项设置
		err = ip_options_get_from_user(sock_net(sk), &opt,
					       optval, optlen);
		if (err)
			break;
#ifdef CONFIG_PRIP
	...
#endif
```

(1)保存原 socket 中对 PRIP 的设置情况,将 PRIP 的偏移赋值给变量 prip。

(2)解析用户传入的选项,将其保存在 opt 变量中。

(3)将 opt 变量中的__data 中的主从标志置 0。

(4)根据 prip 的值来决定是否增加 PRIP 计数:

如果原 socket 已经添加 PRIP 选项,且 opt 中包含 PRIP 选项,则将 PRIP 计数加一;

如果原 socket 未添加 PRIP 选项,则增加 PRIP 计数(添加 PRIP 选项),如果增加计数失

败,则将 opt 的 prip 偏移值置为 1,并将返回值置为错误。

