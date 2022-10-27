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
//#include <linux/prip.h>
//#include <net/prip.h>
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
 struct proc_dir_entry * prip_status_entry;
 struct proc_dir_entry * prip_alarm_entry;
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

    printk("PRIP modules insmod success.\n");
    return 0;

}
static void __exit exit_prip(void){

    return;
}
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

module_init(init_prip);
module_exit(exit_prip);