# prip

#### 介绍
PRIP（Parallel Redundancy Internet Protocol）并行冗余互联协议，是由北京凝思软件
股份有限公司提出并制定的适用于冗余网络通信协议，PRIP并行冗余互联协议可完全
满足冗余通信的基本功能要求和0延时切换的性能要求，并且完全基于软件实现。

#### 软件架构
amd64


#### 安装教程

1.  通过make menuconfig 打开PRIP选项，Networking support-->Networking options-->Parallel Redundancy Internet Protocol
2.  make
3.  make modules_install
4.  重启系统

#### 使用说明

1.  将本机的eth0网卡配置IP地址：ifconfig eth0 192.168.10.40/24 
2.  将本机的eth1网卡配置IP地址：ifconfig eth1 192.168.11.40/24
3.  echo 1 > /proc/sys/net/ipv4/prip_set
4.  echo "192.168.10.0 192.168.11.0 24" > /proc/prip/prip_config
#### 参与贡献

1.  Fork 本仓库
2.  新建 Feat_xxx 分支
3.  提交代码
4.  新建 Pull Request


### 目录结构
1. [code目录](https://gitee.com/anolis/prip/tree/develop/code)，用于存放PRIP各版本源码
2. [design目录](https://gitee.com/anolis/prip/tree/develop/design)，用于存放设计相关文档
3. [documents目录](https://gitee.com/anolis/prip/tree/develop/documents)，用于存放相关技术资料
4. [run目录](https://gitee.com/anolis/prip/tree/develop/run) ，用于存放用户手册、部署手册等
5. [test目录](https://gitee.com/anolis/prip/tree/develop/test)，用于存放测试相关文档，如测试用例、测试报告等 



#### 特技

1.  使用 Readme\_XXX.md 来支持不同的语言，例如 Readme\_en.md, Readme\_zh.md
2.  Gitee 官方博客 [blog.gitee.com](https://blog.gitee.com)
3.  你可以 [https://gitee.com/explore](https://gitee.com/explore) 这个地址来了解 Gitee 上的优秀开源项目
4.  [GVP](https://gitee.com/gvp) 全称是 Gitee 最有价值开源项目，是综合评定出的优秀开源项目
5.  Gitee 官方提供的使用手册 [https://gitee.com/help](https://gitee.com/help)
6.  Gitee 封面人物是一档用来展示 Gitee 会员风采的栏目 [https://gitee.com/gitee-stars/](https://gitee.com/gitee-stars/)
