
#ifndef _UTILS_H_
#define _UTILS_H_

#define MAX_NET_QUEUE_NUM 64 

#define SOCKET_BUF 536870912L
#define WBUF_MAX     "/proc/sys/net/core/wmem_max"
#define RBUF_MAX     "/proc/sys/net/core/rmem_max"
#define WBUF_DEFAULT "/proc/sys/net/core/wmem_default"
#define RBUF_DEFAULT "/proc/sys/net/core/rmem_default"

#define TCP_RMEM "/proc/sys/net/ipv4/tcp_rmem"
#define TCP_WMEM "/proc/sys/net/ipv4/tcp_wmem"

#define TCP_SOCKET_BUF_MIN      314572800L
#define TCP_SOCKET_BUF_DEFAULT  536870912L
#define TCP_SOCKET_BUF_MAX      838860800L

#define PATH_PRIP_CONFIG "/proc/prip/prip_config"
#define PATH_PRIP_ALARM "/proc/prip/prip_alarm"
#define PATH_PRIP_TIMEOUT "/proc/prip/prip_cache_timeout"
#define PATH_PRIP_STATE "/proc/prip/prip_state"
#define PATH_PRIP_SET "/proc/sys/net/ipv4/prip_set"

extern void get_bond_file(int file, char *bondname, char *filename, size_t namelen);
extern void exec_cmd(char *cmd);
extern void exec_config_cmd(char *cmd, char *str, int flag);
extern void set_socket_buff();
extern void set_net_card_irq(char *name);
extern int check_ip(char *str);
extern int strrcmp(char *s1, char *s2, size_t len);
#endif
