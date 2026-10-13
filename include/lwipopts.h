/* lwipopts.h - lwIP configuration for the TI-84 Plus (Z80, 32 KiB data RAM) */
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

#include <stdint.h>
#include "time84.h"

#define NO_SYS 1
#define LWIP_TIMERS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ETHERNET 1
#define LWIP_ARP 1
#define LWIP_CALLBACK_API 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_NETIF_API 0
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 0
#define LWIP_NETIF_REMOVE_CALLBACK 1
#define LWIP_HAVE_LOOPIF 0
#define LWIP_NETIF_LOOPBACK 0

#define LWIP_ICMP 1
#define LWIP_RAW 1
#define LWIP_UDP 1
#define LWIP_UDPLITE 0
#define LWIP_TCP 1
#define LWIP_ALTCP 1
#define LWIP_ALTCP_TLS 0                /* lib/net/tls84.c is used directly */
#define LWIP_IGMP 0
#define LWIP_DHCP 1
#define LWIP_DHCP_PROVIDE_DNS_SERVERS 1
#define LWIP_DHCP_GET_NTP_SRV 0
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_ACD 0
#define LWIP_AUTOIP 0
#define LWIP_DNS 1
#define DNS_TABLE_SIZE 2
#define DNS_MAX_NAME_LENGTH 64
#define DNS_MAX_SERVERS 2
#define LWIP_MDNS_RESPONDER 0
#define LWIP_NUM_NETIF_CLIENT_DATA 0
#define IP_FORWARD 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define ARP_TABLE_SIZE 4
#define ARP_QUEUEING 1

#define LWIP_STATS 0
#define LWIP_STATS_DISPLAY 0

/* memory: fixed pools for pcbs, segments and received frames (fast, no
 * fragmentation); lwIP's heap (PBUF_RAM, e.g. data being sent) is the OS
 * heap */
#define MEM_LIBC_MALLOC 1
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 1U

#define MEMP_NUM_PBUF 6
#define MEMP_NUM_RAW_PCB 1
#define MEMP_NUM_UDP_PCB 4
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_ALTCP_PCB 4
#define MEMP_NUM_ARP_QUEUE 4
#define MEMP_NUM_NETBUF 0
#define MEMP_NUM_NETCONN 0
#define MEMP_NUM_TCPIP_MSG_API 0
#define MEMP_NUM_TCPIP_MSG_INPKT 0
#define PBUF_POOL_BUFSIZE 256
#define PBUF_POOL_SIZE 12

#define TCP_TTL 64
#define TCP_QUEUE_OOSEQ 0
#define TCP_MSS 536
#define TCP_OVERSIZE TCP_MSS
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_SNDLOWAT (TCP_SND_BUF / 2)
#define TCP_WND (4 * TCP_MSS)
#define TCP_MSL 15000UL
#define TCP_MAXRTX 6
#define TCP_SYNMAXRTX 6
#define TCP_LISTEN_BACKLOG 1
#define UDP_TTL 64
#define ICMP_TTL 64

#define LWIP_CHKSUM_ALGORITHM 2
/* experimental speed mode: received checksums are not checked (USB packets
 * carry a CRC of their own); about 7% of the CPU time while downloading */
#define CHECKSUM_CHECK_IP 0
#define CHECKSUM_CHECK_TCP 0
#define CHECKSUM_CHECK_UDP 0
#define CHECKSUM_CHECK_ICMP 0
#define LWIP_RAND() sys_rand32()
#define SNTP_SET_SYSTEM_TIME(sec) time84_set((uint32_t)(sec))
#define SNTP_SERVER_DNS LWIP_DNS
#define SNTP_SERVER_ADDRESS "pool.ntp.org"
#define SNTP_STARTUP_DELAY 0

#define LWIP_DBG_TYPES_ON (LWIP_DBG_ON | LWIP_DBG_TRACE | LWIP_DBG_STATE | LWIP_DBG_FRESH | LWIP_DBG_HALT)
#endif
