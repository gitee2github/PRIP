/*
 * =====================================================================================
 *
 *       Filename:  prip.c
 *
 *    Description:  
 *
 *        Version:  1.0
 *        Created:  2014年08月28日 14时50分51秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  dwang(), dwang@linx-info.com
 *        Company:  Linx-info
 *
 * =====================================================================================
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <asm/atomic.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <net/sock.h>
#include <net/inet_sock.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
#include <linux/timer.h>
#include <net/prip.h>
#include <linux/nsproxy.h>
#include <asm/bitops.h>
#define BUFF_SIZE 1024

struct prip_hash_list prip_hash[PRIP_HASHSZ];
PRIP_CONFIG_T prip_config;
atomic_t prip_alarm;
atomic_t prip_cache_timeout;
atomic_t prip_netmask;
EXPORT_SYMBOL(prip_netmask);
struct proc_dir_entry * dir_prip;
struct proc_dir_entry * prip_config_entry;
struct proc_dir_entry *prip_status_entry;
struct proc_dir_entry *prip_alarm_entry;
struct proc_dir_entry * prip_cache_timeout_entry;

static char config_buff[BUFF_SIZE];
static u32 jhash_prip_initval __read_mostly;
 unsigned long prip_reboot;
__u32 prip_net_one;
__u32 prip_net_two;

static void deinit_prip_struct(struct prip_priv *priv)
{
    if(!priv) return;
    kfree(priv->link->seq_in_top);
    kfree(priv->link->seq_in_bottom);
    kfree(priv->link->clear_map);
    kfree(priv->link);
    kfree(priv);
}

static struct prip_priv * init_prip_struct(__u32 localip,__u32 peerip,int hash)
{
    struct prip_priv *priv=(struct  prip_priv*)kzalloc(sizeof(struct prip_priv),GFP_ATOMIC);
    if(!priv) return priv;
    spin_lock_init(&priv->seqnr_out_lock);
    priv->sequence_nr_out=0;
    spin_lock_init(&priv->seqnr_in_lock);
    spin_lock_init(&priv->alarm_lock);
    priv->link = (struct link_entry *)kzalloc(sizeof(struct link_entry),GFP_ATOMIC);
    if(!priv->link) {
	kfree(priv);
	priv=NULL;
	return priv;
    }
    priv->link->seq_in_top = (char *)kmalloc(4096,GFP_ATOMIC);
    if(!priv->link->seq_in_top){
        kfree(priv->link);
        kfree(priv);
        priv=NULL;
        return priv;
    }
    priv->link->seq_in_bottom = (char *)kmalloc(4096,GFP_ATOMIC);
    if(!priv->link->seq_in_bottom){
    	kfree(priv->link->seq_in_top);
        kfree(priv->link);
        kfree(priv);
        priv=NULL;
        return priv;
    }
    priv->link->clear_map = (unsigned long *)kzalloc(4096,GFP_ATOMIC);
    if(!priv->link->clear_map){
	    kfree(priv->link->seq_in_top);
	    kfree(priv->link->seq_in_bottom);
        kfree(priv->link);
        kfree(priv);
        priv=NULL;
        return priv;
    }
 
 
    priv->peerip=peerip;
    priv->localip=localip;
    spin_lock_init(&priv->timer_lock);
    atomic_set(&priv->refcnt,0);
    atomic_set(&priv->master_status,0);
    atomic_set(&priv->slave_status,0);
    atomic64_set(&priv->master_send_num,0); 
    atomic64_set(&priv->slave_send_num,0);
    atomic64_set(&priv->master_recv_num,0);
    atomic64_set(&priv->slave_recv_num,0);
    priv->prip_single_head=&prip_single_list;
    priv->prip_hash_head=&prip_hash[hash];
    priv->peer_slave_ip=master_to_slave(peerip);
    timer_setup(&priv->timer,prip_priv_timeout, 0);
    return priv;
}

u16 get_pripid(struct prip_priv *priv,unsigned long * snd_start)
{   
    u16 ret;
    if(!priv) return 0;
    spin_lock(&priv->seqnr_out_lock);
    if(priv->sequence_nr_out==0) ++(priv->sequence_nr_out);
    ret = priv->sequence_nr_out;
    if(ret==1) priv->snd_start=jiffies;
    if(ret==32768) priv->snd_start=jiffies;
    *snd_start=priv->snd_start;
    ++(priv->sequence_nr_out);
    spin_unlock(&priv->seqnr_out_lock);
    return ret;
}
EXPORT_SYMBOL(get_pripid);


static int inet_aton(const char *p){
    int dot=0;
    __u32 val=0,base=10,addr=0;
    unsigned char c;
    if(unlikely(!p))
        return 0;
    do{
        c=*p;
        switch(c){
            case '0':case '1':
            case '2':case '3':
            case '4':case '5':
            case '6':case '7':
            case '8':case '9':
                val=(val * base)+(c - '0');
                break;
            case '.':
                if(++dot > 3)
                    return 0;
            case '\0':
                if(val > 255)
                    return 0;
                addr = addr << 8 | val;
                val=0;
                break;
            default :
                return 0;
        }
    }while(*p++);
    if(dot > 0 && dot <= 3){
        addr <<= 8 * (3 - dot);
        return addr;
    }
    if(dot == 0){
        if(addr < 32)
            return addr;//netmask
        else
            return 0;
    }
    return 0;
}

static int inet_ntoa(char *buff,__u32 ip){
    IP_ADDR tmp;
    tmp.ip=ip;
    if(unlikely(!buff))
        return 0;
#if defined(__LITTLE_ENDIAN_BITFIELD)
    sprintf(buff,"%d.%d.%d.%d",tmp.buff[3],tmp.buff[2],tmp.buff[1],tmp.buff[0]);
#elif defined(__BIG_ENDIAN_BITFIELD)
    sprintf(buff,"%d.%d.%d.%d",tmp.buff[0],tmp.buff[1],tmp.buff[2],tmp.buff[3]);
#endif
    return 1;
}

static void * prip_status_seq_start (struct seq_file * fd,loff_t * pos){

    if(*pos==0)
        return &prip_single_list;
    else
        return NULL;
}
static void * prip_status_seq_next (struct seq_file * fd ,void * v,loff_t *pos){

    (*pos)++;
    return NULL;
}
static void prip_status_seq_stop (struct seq_file * fd ,void *v){

    return;
}
static int prip_status_seq_show (struct seq_file *fd,void *v){
    int on=0;
    char m_stat[6];
    char s_stat[6];
    int m_stat_flag=0;
    int s_stat_flag=0;
    unsigned int m_net=0;
    unsigned int s_net=0;
    char master_net[16];
    char slave_net[16];
    char local_net[16];
    struct prip_priv * q;
    struct prip_hash_list * tmp = (struct prip_hash_list *)v;

    read_lock_bh(&prip_config.rwlock);
    if(prip_config.valid <= 0){
        read_unlock_bh(&prip_config.rwlock);
        return 0;
    }else{
       if(atomic_read(&prip_config.reference)>0){
           on=1;
	   m_net=prip_config.net_one;
           s_net=prip_config.net_two;
       }
    }
    read_unlock_bh(&prip_config.rwlock);
    if(!on){
        seq_printf(fd,"PRIP_ON/OFF \n");
        seq_printf(fd,"%8s\n","off");
    }else{
        inet_ntoa(master_net,m_net);
        inet_ntoa(slave_net,s_net);
        seq_printf(fd,"PRIP_ON/OFF \tPRIP_NET_ONE\tPRIP_NET_TWO\tPRIP_REFCNT \n");
        seq_printf(fd,"%8s \t%8s \t%8s \t%8u\n\n","on",master_net,slave_net,atomic_read(&prip_config.reference));
    }

    seq_printf(fd,"local_ip \trefcnt\tpeer_master_ip\tpeer_slave_ip\tmaster_state\tslave_state\tmaster_sent\tslave_sent\tmaster_recv\tslave_recv\t\n");
    read_lock_bh(&tmp->rwlock);
    hlist_for_each_entry(q,&tmp->head,single_list){
           m_stat_flag=atomic_read(&q->master_status);
           s_stat_flag=atomic_read(&q->slave_status);
        if(m_stat_flag)
            strcpy(m_stat,"up");
        else
            strcpy(m_stat,"down");
        if(s_stat_flag)
            strcpy(s_stat,"up");
        else
            strcpy(s_stat,"down");
        inet_ntoa(master_net,ntohl(q->peerip));
        inet_ntoa(slave_net,ntohl(q->peer_slave_ip));
        inet_ntoa(local_net,ntohl(q->localip));

        seq_printf(fd,"%8s\t%d \t%8s \t%8s \t%8s \t%8s \t%8lld \t%8lld \t%8lld \t%8lld \n",
                        local_net,atomic_read(&q->refcnt),master_net,slave_net,m_stat,s_stat,atomic64_read(&q->master_send_num),atomic64_read(&q->slave_send_num),
                        atomic64_read(&q->master_recv_num),
                        atomic64_read(&q->slave_recv_num));
    }
    read_unlock_bh(&tmp->rwlock);
    return 0;
}

static struct seq_operations prip_status_seq_ops = {
    .start = prip_status_seq_start,
    .stop = prip_status_seq_stop,
    .next = prip_status_seq_next,
    .show = prip_status_seq_show,
};

static int prip_status_open (struct inode * inode, struct file *file){
    return seq_open(file,&prip_status_seq_ops);
}

static struct file_operations prip_status_ops = {
    .owner = THIS_MODULE,
    .open = prip_status_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = seq_release,
};

void prip_priv_put(struct prip_priv *q){
    atomic_dec(&q->refcnt);
}
EXPORT_SYMBOL(prip_priv_put);


void prip_priv_timeout(struct timer_list *t){

    struct prip_priv *q = from_timer(q, t, timer);

    if(atomic_dec_and_test(&q->refcnt)){
        write_lock_bh(&q->prip_hash_head->rwlock);
        if(atomic_read(&q->refcnt) == 0){
            hlist_del(&q->hash_list);
            write_lock_bh(&q->prip_single_head->rwlock);
            hlist_del(&q->single_list);
            write_unlock_bh(&q->prip_single_head->rwlock);
            deinit_prip_struct(q);
            write_unlock_bh(&q->prip_hash_head->rwlock);
            return;
        }
        write_unlock_bh(&q->prip_hash_head->rwlock);
    }
    atomic_inc(&q->refcnt);
    spin_lock(&q->timer_lock);
    mod_timer(&q->timer,jiffies+atomic_read(&prip_cache_timeout)*60*HZ);
    spin_unlock(&q->timer_lock);
    
}
EXPORT_SYMBOL(prip_priv_timeout);

static int prip_hashfn(__u32 localip,__u32 peerip){
    return jhash_2words((__force u32) localip,(__force u32) peerip,jhash_prip_initval) & (PRIP_HASHSZ-1);
}

static struct prip_priv * prip_priv_intern(struct prip_priv * qp_in,int hash){
    struct prip_priv *q;
    write_lock_bh(&prip_hash[hash].rwlock);
#ifdef CONFIG_SMP
    hlist_for_each_entry(q,&prip_hash[hash].head,hash_list){
        if((q->peerip == qp_in->peerip) && (q->localip == qp_in->localip)){
            atomic_inc(&q->refcnt);
            spin_lock(&q->timer_lock);
            mod_timer_pending(&q->timer,jiffies+atomic_read(&prip_cache_timeout)*60*HZ);
            spin_unlock(&q->timer_lock);
            write_unlock_bh(&prip_hash[hash].rwlock);
            deinit_prip_struct(qp_in);
            return q;
        }
    }
#endif
    q=qp_in;
    if(!mod_timer(&q->timer,jiffies+atomic_read(&prip_cache_timeout)*60*HZ))
        atomic_inc(&q->refcnt);
    atomic_inc(&q->refcnt);
    hlist_add_head(&q->hash_list,&prip_hash[hash].head);
    write_unlock_bh(&prip_hash[hash].rwlock);
    write_lock_bh(&prip_single_list.rwlock);
    hlist_add_head(&q->single_list,&prip_single_list.head);
    write_unlock_bh(&prip_single_list.rwlock);
    return q;
}

static struct prip_priv * prip_priv_create(__u32 localip,__u32 peerip,int hash){
    struct prip_priv * p = init_prip_struct(localip,peerip,hash);
    if(p == NULL)
        return NULL;
    return prip_priv_intern(p,hash);

}

*
 *
 *localip and peerip :net byte order
 *
 */
struct prip_priv * prip_priv_find(__u32 localip, __u32 peerip){
    int hash;
    struct prip_priv * q;
    hash=prip_hashfn(localip,peerip);
    read_lock_bh(&prip_hash[hash].rwlock);
    hlist_for_each_entry(q,&prip_hash[hash].head,hash_list){
        if((q->peerip == peerip) && (q->localip == localip)){
            atomic_inc(&q->refcnt);
            spin_lock(&q->timer_lock);
            mod_timer_pending(&q->timer,jiffies+atomic_read(&prip_cache_timeout)*60*HZ);
            spin_unlock(&q->timer_lock);
            read_unlock_bh(&prip_hash[hash].rwlock);
            return q;
        }
    }
    read_unlock_bh(&prip_hash[hash].rwlock);
    return prip_priv_create(localip,peerip,hash);
}
EXPORT_SYMBOL(prip_priv_find);
struct prip_priv * prip_priv_only_find(__u32 localip, __u32 peerip){
    int hash;
    struct prip_priv * q;
    hash=prip_hashfn(localip,peerip);
    read_lock_bh(&prip_hash[hash].rwlock);
    hlist_for_each_entry(q,&prip_hash[hash].head,hash_list){
        if((q->peerip == peerip) && (q->localip == localip)){
            atomic_inc(&q->refcnt);
            spin_lock(&q->timer_lock);
            mod_timer_pending(&q->timer,jiffies+atomic_read(&prip_cache_timeout)*60*HZ);
            spin_unlock(&q->timer_lock);
            read_unlock_bh(&prip_hash[hash].rwlock);
            return q;
        }
    }
    read_unlock_bh(&prip_hash[hash].rwlock);
    return NULL;
}
EXPORT_SYMBOL(prip_priv_only_find);

static int get_config_ip(const char * config,char ip[3][16]){
    int i;
    int j= 0;
    int k=0;
    int len;
    int flag=0;
    unsigned char c;
    if(config == NULL || ip ==NULL)
        return 0;
    len=strlen(config);
    for(i=0;i<(len+1);i++){
        c = *(config+i);
        switch(c){
            case ' ':
                if(flag){
                    if ((j==2) && (k <16)){
                        ip[j][k]='\0';
                        return 1;
                    }
                    if(j >2 || k > 15)
                        return 0; 
                    ip[j][k]='\0';
                    j++;
                    k=0;
                    flag = 0;
                }
                break;
            case '0': case '1': 
            case '2': case '3':
            case '4': case '5':
            case '6': case '7':
            case '8':case '9':
            case '.':
                flag = 1;
                if(k > 15 || j >2)
                    return 0;
                ip[j][k] = c;
                k++;
                break;
            case '\n':
            case '\0':
                if((j==2)&&(k<16)){
                    ip[j][k] = '\0';
                    return 1;
                }else
                    return 0;
            default:
                return 0;
        }
    }
    return 1;
}

static ssize_t write_prip_config( struct file *file,const char __user * buffer,size_t len, loff_t *f_pos) 
{
    char data[BUFF_SIZE];
    char ip[3][16];
    __u32 net_one;
    __u32 net_two;
    __u32 netmask;
    __u32 tmp_mask=0;
    char *cache;
    int i;
    int clean=0;
    unsigned long tmp=0;
    unsigned char tmp_buff[16];
    struct net_device *dev;
    struct in_device *in_dev;
    int net_one_flag=0;
    int net_two_flag=0;
    int net_one_ip=0;
    int net_two_ip=0;

    if( len > BUFF_SIZE-1)
        return -E2BIG;
    memset(config_buff,0,BUFF_SIZE);
    memset(tmp_buff,0,sizeof(tmp_buff));
    if(copy_from_user(config_buff,buffer,len)){
        return -EINVAL;
    }
    for(i=0;i<len;i++){
        if(config_buff[i]==' ' || config_buff[i]=='\n'){
            tmp++;
        }
    }
    if(tmp == len)
        clean = 1;
    read_lock_bh(&prip_config.rwlock);
    if(atomic_read(&(prip_config.reference)) > 0){
        read_unlock_bh(&prip_config.rwlock);
        return -EBUSY;
    }
    read_unlock_bh(&prip_config.rwlock);
    strcpy((char *)data,config_buff);
    if(!clean) {
        if( len < 17 ) 
            return -EINVAL;
        if(!get_config_ip(config_buff,ip))
            return -EINVAL;
        cache =ip[0];
        if(!(net_one=inet_aton(cache)))
            return -EINVAL;
        cache =ip[1];
        if(!(net_two=inet_aton(cache)))
            return -EINVAL;
        cache =ip[2];
        if(!(netmask=inet_aton(cache)))
            return -EINVAL;
        if((netmask <=0) || (netmask >=32))
            return -EINVAL;
        for(i=1;i<=netmask;i++){
            tmp_mask |= 1<< (32-i);
        }
		prip_net_one = net_one;
		prip_net_two = net_two;
        net_one = net_one & tmp_mask;
        net_two = net_two & tmp_mask;

        read_lock_bh(&dev_base_lock);
        list_for_each_entry(dev,&(current->nsproxy->net_ns->dev_base_head),dev_list){
            if(dev){
                in_dev = (struct in_device *)(dev->ip_ptr);
                if(in_dev && in_dev->ifa_list){
                    printk("dwang debug :in_dev->ifa_list->ifa_mask=0x%x,0x%x\n",in_dev->ifa_list->ifa_mask,ntohl(in_dev->ifa_list->ifa_mask));
                    if((ntohl(in_dev->ifa_list->ifa_address) & tmp_mask)== net_one){
                        net_one_flag=1;
                        printk("net_one_flag[%d]two_flag[%d]\n",net_one_flag,net_two_flag);
                        net_one_ip=(ntohl(in_dev->ifa_list->ifa_address) & (~ tmp_mask));
                        if(net_two_flag)
                            break;
                    }else{
                        if((ntohl(in_dev->ifa_list->ifa_address) & tmp_mask)== net_two){
                            net_two_flag=1;
                            printk("net_one_flag[%d]two_flag[%d]\n",net_one_flag,net_two_flag);
                            net_two_ip=(ntohl(in_dev->ifa_list->ifa_address) & (~ tmp_mask));
                            if(net_one_flag)
                                break;
                        }
                    }
                }
            }
        }
        read_unlock_bh(&dev_base_lock);
printk("net_one_ip =[%d]two_ip[%d]\n",net_one_ip,net_two_ip);
        if(!(net_one_flag && net_two_flag))
            return -ENXIO;
        if((net_one_ip != net_two_ip) || (net_one_ip == 0) ||(net_two_ip==0))
            return -ENXIO;
        write_lock_bh(&(prip_config.rwlock));
        if(atomic_read(&(prip_config.reference)) > 0){
            write_unlock_bh(&prip_config.rwlock);
            return -EBUSY;
        }
        prip_config.valid=1;
        prip_config.net_one=net_one;
        prip_config.net_two=net_two;
        prip_config.mask=tmp_mask;
        atomic_set(&prip_config.reference,0);
        write_unlock_bh(&(prip_config.rwlock));
        atomic_set(&prip_netmask,tmp_mask);

        memset((char *)data,0,BUFF_SIZE);
        inet_ntoa(tmp_buff,net_one);
        i=sprintf((char *)data,"%s ",tmp_buff);
        inet_ntoa(tmp_buff,net_two);
        i+=sprintf(((char *)data + i),"%s ",tmp_buff);
        sprintf(((char *)data+i),"%d\n",netmask);
	    strcpy(config_buff,data);
    } else {
        write_lock_bh(&(prip_config.rwlock));
		if(init_net.ipv4.sysctl_prip_set){
			write_unlock_bh(&prip_config.rwlock);
			return -EBUSY;
		}
        if(atomic_read(&(prip_config.reference)) > 0){
            write_unlock_bh(&prip_config.rwlock);
            return -EBUSY;
        }
        prip_config.valid=0;
        atomic_set(&prip_config.reference,0);
        write_unlock_bh(&(prip_config.rwlock));
        atomic_set(&prip_netmask,0);
       	*((char *)data)='\0';
		strcpy(config_buff,data);
    }
    return len;
}

static ssize_t write_prip_alarm( struct file *file,const char __user * buffer,size_t len, loff_t *f_pos)
{
    int val=0;
    char cache[128];
    char c;
    int space_flag=0;
    int base=10;
    char *p=cache;
    if(len > 128)
        return -EINVAL;
    //get_user(val,(int __user *)buffer);
    if(copy_from_user(cache,buffer,len))
        return -EINVAL;
    do{
        c=*p;
        switch(c){
            case '0':case '1':case '2':case '3':case '4':
            case '5':case '6':case '7':case '8':case '9':
                val=val*base + (c - '0');
                space_flag=1;
                break;
            case ' ':
                if(space_flag)
                    *p='\0';
                break;
            case '\n':
                *p='\0';
                break;
            default:
                return -EINVAL;
        }
    }while(*p++);
    if(val > 0)
        atomic_set(&prip_alarm,val);
    else
        return -EINVAL;
    return len;
}

static ssize_t write_prip_cache_timeout( struct file *file,const char __user * buffer,size_t len, loff_t *f_pos)
{
    int val=0;
    char cache[128];
    char c;
    int space_flag=0;
    int base=10;
    char *p=cache;
    if(len > 128)
        return -EINVAL;
    //get_user(val,(int __user *)buffer);
    if(copy_from_user(cache,buffer,len))
        return -EINVAL;
    do{
        c=*p;
        switch(c){
            case '0':case '1':case '2':case '3':case '4':
            case '5':case '6':case '7':case '8':case '9':
                val=val*base + (c - '0');
                space_flag=1;
                break;
            case ' ':
                if(space_flag)
                    *p='\0';
                break;
            case '\n':
                *p='\0';
                break;
            default:
                return -EINVAL;
        }
    }while(*p++);
    if(val > 0)
        atomic_set(&prip_cache_timeout,val);
    else
        return -EINVAL;
    return len;
}

static int read_prip_config(struct seq_file *seq,void *v)
{
	seq_printf(seq,"%s",config_buff);
	return 0;
}

static int read_prip_alarm(struct seq_file *seq,void *v)
{
	seq_printf(seq,"%d\n",atomic_read(&prip_alarm));
	return 0;
}
static int read_prip_cache_timeout(struct seq_file *seq,void *v)
{
	seq_printf(seq,"%d\n",atomic_read(&prip_cache_timeout));
	return 0;
}

static int seq_open_prip_config(struct inode *inode, struct file *file)
{
	return single_open(file,read_prip_config,inode->i_private);
}

static int seq_open_prip_alarm(struct inode *inode, struct file *file)
{
	return single_open(file,read_prip_alarm,inode->i_private);
}

static int seq_open_prip_cache_timeout(struct inode *inode, struct file *file)
{
	return single_open(file,read_prip_cache_timeout,inode->i_private);
}


static struct file_operations prip_config_ops = {
	.open	=	seq_open_prip_config,
	.read	=	seq_read,
	.llseek	=	seq_lseek,
	.write	= 	write_prip_config,
	.owner	=	THIS_MODULE,
	.release	=	single_release,
};

static struct file_operations prip_alarm_ops = {
	.open	=	seq_open_prip_alarm,
	.read	=	seq_read,
	.write	= 	write_prip_alarm,
	.owner	=	THIS_MODULE,
};

static struct file_operations prip_cache_timeout_ops = {
	.open	=	seq_open_prip_cache_timeout,
	.read	=	seq_read,
	.write	= 	write_prip_cache_timeout,
	.owner	=	THIS_MODULE,
};

int set_prip_mode(struct sock * sk,int mode)
{
    struct inet_sock *inet;
    if(unlikely(!sk))
        return -1;
    read_lock_bh(&prip_config.rwlock);
    if(prip_config.valid <= 0){
        read_unlock_bh(&prip_config.rwlock);
        return -1;
    }
    read_unlock_bh(&prip_config.rwlock);
    inet=inet_sk(sk);
    if(mode){
           if(inet->inet_rcv_saddr && !ipv4_is_multicast(inet->inet_rcv_saddr)){
                write_lock_bh(&prip_config.rwlock);
                if((ntohl(inet->inet_rcv_saddr) & prip_config.mask)!=prip_config.net_one){
		    if((ntohl(inet->inet_rcv_saddr) & prip_config.mask)!=prip_config.net_two){
                            write_unlock_bh(&prip_config.rwlock);
                            return -1;
		    }
		}
	    }else{
		write_lock_bh(&prip_config.rwlock);
            }
            atomic_inc(&prip_config.reference);
            write_unlock_bh(&prip_config.rwlock);
    }else{
        write_lock_bh(&prip_config.rwlock);
        if(atomic_read(&prip_config.reference)==0){
            write_unlock_bh(&prip_config.rwlock);
            return -1;
        }
        atomic_dec(&prip_config.reference);
#ifdef CONFIG_PRIP
    if(sk->priv){
        prip_priv_put(sk->priv);
        sk->priv=NULL;
    }
#endif
        write_unlock_bh(&prip_config.rwlock);
    }
    return 0;
}
EXPORT_SYMBOL(set_prip_mode);
/* 
 *@flag : 1: get master network
 *          0: get slave network
 *
 * retrun: master or slave network.(net byte order).
 *         if prip_config.reference==0 or prip_config.master==0,return 0.
 * */
 __u32  get_master_or_slave(int flag){
    __u32 d_addr=0;
    read_lock_bh(&prip_config.rwlock);
    if(prip_config.valid <= 0||(atomic_read(&prip_config.reference)<=0)){
        read_unlock_bh(&prip_config.rwlock);
        return 0;
    }else{
        if(atomic_read(&prip_config.reference) > 0){
            if(flag){
                if(prip_config.net_one_flag){
                    d_addr=prip_config.net_one;
                    read_unlock_bh(&prip_config.rwlock);
                    return htonl(d_addr);
                }else{
                    d_addr=prip_config.net_two;
                    read_unlock_bh(&prip_config.rwlock);
                    return htonl(d_addr);
                }
            }else{
                if(prip_config.net_one_flag){
                    d_addr=prip_config.net_two;
                    read_unlock_bh(&prip_config.rwlock);
                    return htonl(d_addr);
                }else{
                    d_addr=prip_config.net_one;
                    read_unlock_bh(&prip_config.rwlock);
                    return htonl(d_addr);
                }
            }
        }
    }
    read_unlock_bh(&prip_config.rwlock);
    return htonl(d_addr);
    
}
EXPORT_SYMBOL(get_master_or_slave);
/* 
 *saddr  is  net byte order ip.
 *flag :1:master to slave. 0 :slave to master.
 * the return value: 
 *                   0 :ip unmatched or prip_config not set.
 *                   >0: slave ip(net byte order).
 * */
//unsigned int master_to_slave(unsigned int master_ip){
static __u32  __ip_addr_trans(__u32 s_addr){
    __u32 d_addr=0;		
    __u32 tmp_ip= 0;

    if(ipv4_is_multicast(s_addr)) 
	    return s_addr;

    tmp_ip= ntohl(s_addr);
    read_lock_bh(&prip_config.rwlock);
    if(prip_config.valid <= 0){
        read_unlock_bh(&prip_config.rwlock);
        return 0;
    }else{
	if((tmp_ip & prip_config.mask)==prip_config.net_one){
	    d_addr = prip_config.net_two | (tmp_ip & (~prip_config.mask));
                        read_unlock_bh(&prip_config.rwlock);
                        return htonl(d_addr);
	}
	else if((tmp_ip & prip_config.mask)==prip_config.net_two)
	{
	    d_addr = prip_config.net_one | (tmp_ip & (~prip_config.mask));
            read_unlock_bh(&prip_config.rwlock);
            return htonl(d_addr);
        }else{
				read_unlock_bh(&prip_config.rwlock);
            	return 0;
		}
    }
}
/* 
 *master_ip is  net byte order
 * the return value: 
 *                   0 :ip unmatched or prip_config not set.
 *                   >0: slave ip(net byte order).
 * */
__u32  master_to_slave(__u32 master_ip){
    return __ip_addr_trans(master_ip);
}
EXPORT_SYMBOL(master_to_slave);
/* 
 * slave_ip is  net byte order
 * the return value: 
 *                   0 :ip unmatched or prip_config not set.
 *                   >0: master ip(net byte order).
 * */
__u32 slave_to_master(__u32 slave_ip){
    return __ip_addr_trans(slave_ip);
}
EXPORT_SYMBOL(slave_to_master);
/*
 *stat : 1 to up ;0 to down
 * */
void set_master_stat(struct prip_priv *q,__u32 stat){
    if(stat > 0)
        atomic_set(&q->master_status,1);
    else
        atomic_set(&q->master_status,0);
}
EXPORT_SYMBOL(set_master_stat);
/*
 *stat : 1 to up ;0 to down
 * */
void set_slave_stat(struct prip_priv *q,__u32 stat){
    if(stat > 0)
        atomic_set(&q->slave_status,1);
    else
        atomic_set(&q->slave_status,0);
}
EXPORT_SYMBOL(set_slave_stat);
void master_send_inc(struct prip_priv *q){
    atomic64_inc(&q->master_send_num);
    if(atomic64_read(&q->master_send_num)==0){
        atomic64_set(&q->master_recv_num,0);
        atomic64_set(&q->slave_send_num,0);
        atomic64_set(&q->slave_recv_num,0);
    }
}
EXPORT_SYMBOL(master_send_inc);
void master_recv_inc(struct prip_priv *q){
    atomic64_inc(&q->master_recv_num);
    if(atomic64_read(&q->master_recv_num)==0){
        atomic64_set(&q->master_send_num,0);
        atomic64_set(&q->slave_send_num,0);
        atomic64_set(&q->slave_recv_num,0);
    }
}
EXPORT_SYMBOL(master_recv_inc);
void slave_send_inc(struct prip_priv *q){
    atomic64_inc(&q->slave_send_num);
    if(atomic64_read(&q->slave_send_num)==0){
        atomic64_set(&q->master_send_num,0);
        atomic64_set(&q->master_recv_num,0);
        atomic64_set(&q->slave_recv_num,0);
    }
}
EXPORT_SYMBOL(slave_send_inc);
void slave_recv_inc(struct prip_priv *q){
    atomic64_inc(&q->slave_recv_num);
    if(atomic64_read(&q->slave_recv_num)==0){
        atomic64_set(&q->master_send_num,0);
        atomic64_set(&q->slave_send_num,0);
        atomic64_set(&q->master_recv_num,0);
    }
}
EXPORT_SYMBOL(slave_recv_inc);

static int __init init_prip(void) 
{
    int err;
    int i;

    memset(&prip_config,0,sizeof(PRIP_CONFIG_T));
    memset(config_buff,0,BUFF_SIZE);
    atomic_set(&(prip_config.reference),0);
    atomic_set(&prip_netmask,0);
    rwlock_init(&(prip_config.rwlock));
    atomic_set(&prip_alarm,500);
    atomic_set(&prip_cache_timeout,1);
    for(i=0;i<PRIP_HASHSZ;i++){
        prip_hash[i].head.first=NULL;
        rwlock_init(&(prip_hash[i].rwlock));
    }
    prip_reboot=msecs_to_jiffies(10000);
    prip_single_list.head.first=NULL;
    rwlock_init(&prip_single_list.rwlock);
    get_random_bytes(&jhash_prip_initval,sizeof(jhash_prip_initval));

    dir_prip = proc_mkdir("prip",NULL);
    if(!dir_prip){
        printk("PRIP ERROR: Cannot create /proc/prip .\n");
        err = -1;
        goto err_1;
    }
    prip_config_entry = proc_create("prip_config",S_IRUGO|S_IWUSR,dir_prip, &prip_config_ops);
    if(!prip_config_entry){
        printk("PRIP ERROR: Cannot create /proc/prip/prip_config .\n");
        err = -1;
        goto err_2;
    }

    prip_status_entry = proc_create("prip_state",S_IRUGO,dir_prip, &prip_status_ops);
    if(!prip_status_entry){
        printk("PRIP ERROR: Cannot create /proc/prip/prip_state .\n");
        err = -1;
        goto err_3;
    }

    prip_alarm_entry = proc_create("prip_alarm",S_IRUGO|S_IWUSR,dir_prip, &prip_alarm_ops);
    if(!prip_alarm_entry){
        printk("PRIP ERROR: Cannot create /proc/prip/prip_alarm .\n");
        err = -1;
        goto err_4;
    }

    prip_cache_timeout_entry = proc_create("prip_cache_timeout",S_IRUGO|S_IWUSR,dir_prip,&prip_cache_timeout_ops);
    if(!prip_cache_timeout_entry) {
        printk("PRIP ERROR: Cannot create /proc/prip/prip_cache_timeout .\n");
        err = -1;
        goto err_5;
    }

    printk("PRIP modules insmod success.\n");
    return 0;

err_5:
    remove_proc_entry("prip_alarm",dir_prip);
err_4:

    remove_proc_entry("prip_state",dir_prip);
err_3:
    remove_proc_entry("prip_config",dir_prip);
err_2:
    remove_proc_entry("prip",NULL);
err_1:
}
static void __exit exit_prip(void){
    remove_proc_entry("prip_cache_timeout",dir_prip);
    remove_proc_entry("prip_alarm",dir_prip);
    remove_proc_entry("prip_state",dir_prip);
    remove_proc_entry("prip_config",dir_prip);
    remove_proc_entry("prip",NULL);
    return;
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

module_init(init_prip);
module_exit(exit_prip);