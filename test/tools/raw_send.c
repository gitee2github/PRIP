#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <linux/if_ether.h>
#include <linux/tcp.h>
//#include <linux/ip.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>

extern char *optarg;
extern int optind, opterr, optopt;
uint16_t ip_id = 0;

char packet_bytes[] = {
  0x5c, 0xdd, 0x70, 0x04, 0x1a, 0xc0, 0xe8, 0x61,
  0x1f, 0x2c, 0x3e, 0x58, 0x08, 0x00, 0x49, 0x00,
  0x05, 0xdc, 0x23, 0x29, 0x40, 0x00, 0x40, 0x06,
  0x8a, 0x89, 0xc0, 0xa8, 0xbf, 0x0b, 0xc0, 0xa8,
  0xc1, 0x0c, 0x12, 0x10, 0x04, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x95, 0x40, 0x8d, 0xab, 0x48, 0x00,
  0x00, 0x00, 0xe4, 0x06, 0x00, 0x16, 0x68, 0xb2,
  0xb8, 0xbd, 0x6d, 0x72, 0x1d, 0x28, 0x80, 0x10,
  0x00, 0x85, 0x07, 0x28, 0x00, 0x00, 0x01, 0x01,
  0x08, 0x0a, 0x04, 0xa0, 0x97, 0xcb, 0x04, 0xa0,
  0xaa, 0x89, 0xdc, 0x5f, 0x10, 0x52, 0xc2, 0x4e,
  0x66, 0x11, 0x6a, 0x79, 0x1e, 0x23, 0xc7, 0x40,
  0x42, 0x92, 0xbb, 0x56, 0x06, 0xb1, 0xc9, 0xec,
  0x69, 0x04, 0x7e, 0x21, 0xb9, 0x7d, 0xc6, 0x72,
  0xef, 0xe0, 0x60, 0xff, 0x00, 0x13, 0x04, 0x88,
  0xe0, 0x4c, 0x7f, 0x8d, 0xdd, 0x72, 0x51, 0xc1,
  0x28, 0x05, 0xbb, 0x06, 0xd0, 0xc4, 0xa3, 0x6b,
  0xbf, 0x19, 0x58, 0x48, 0xd3, 0x3d, 0x06, 0xe9,
  0x09, 0xa1, 0x25, 0xc6, 0x04, 0x6e, 0xdc, 0xbe,
  0x7a, 0x00, 0x4f, 0xe0, 0x25, 0x42, 0x9d, 0xf2,
  0x2b, 0xcc, 0x47, 0xb7, 0x1f, 0xe9, 0x7f, 0x05,
  0xa2, 0x48, 0xf3, 0x5b, 0x5c, 0xc7, 0x91, 0x51,
  0xf4, 0xd7, 0x02, 0x3d, 0x13, 0x93, 0xb4, 0x4a,
  0x17, 0x9a, 0x8a, 0xea, 0xdd, 0x05, 0x94, 0xd2,
  0x2c, 0xa4, 0x0c, 0x57, 0x68, 0x75, 0xbc, 0x83,
  0x04, 0x36, 0x10, 0x57, 0x66, 0xcf, 0x2d, 0x9a,
  0xa8, 0x7d, 0x07, 0xfc, 0xb5, 0xae, 0x2c, 0xec,
  0xa6, 0x2a, 0xcc, 0x6c, 0xf3, 0x5b, 0xdc, 0xbd,
  0xf2, 0x32, 0x8e, 0x98, 0x1f, 0xb9, 0xa6, 0xc2,
  0xbb, 0x40, 0x05, 0x9b, 0x77, 0x16, 0x18, 0xb3,
  0x93, 0xc8, 0xdf, 0xa2, 0x3b, 0x4c, 0x49, 0x05,
  0x30, 0x58, 0x8b, 0xaf, 0x51, 0xf0, 0x7b, 0xb2,
  0x70, 0x18, 0xf6, 0x02, 0x4f, 0xe9, 0x3e, 0xc3,
  0x14, 0xaa, 0xf8, 0x3d, 0x63, 0x48, 0x7b, 0x5c,
  0xba, 0x03, 0xcd, 0x63, 0x07, 0xc3, 0x6a, 0x8d,
  0xf9, 0x2f, 0x69, 0x1d, 0x1a, 0x6b, 0xa2, 0x58,
  0xbb, 0x38, 0x69, 0xaf, 0x77, 0x16, 0xdf, 0x27,
  0x23, 0x95, 0x17, 0xbc, 0x0c, 0xac, 0xc1, 0x42,
  0x1c, 0x21, 0x33, 0xcc, 0x74, 0x35, 0x55, 0x20,
  0x6f, 0x88, 0x32, 0x95, 0x2f, 0x60, 0x30, 0x2c,
  0xc4, 0x5f, 0x65, 0x38, 0x54, 0x92, 0xa0, 0x7b,
  0x5b, 0xf2, 0xea, 0x29, 0xab, 0x72, 0x22, 0x96,
  0x07, 0x63, 0x79, 0x6f, 0xed, 0x19, 0xaf, 0x21,
  0x99, 0x45, 0x2b, 0x72, 0x4a, 0xfe, 0xf7, 0x79,
  0x99, 0xcb, 0xeb, 0x9f, 0x39, 0x22, 0x37, 0x58,
  0x64, 0x26, 0xa5, 0x5c, 0xdc, 0x54, 0xe5, 0x64,
  0x68, 0x93, 0xd5, 0x70, 0x31, 0x34, 0xf3, 0x46,
  0x51, 0x5f, 0x09, 0xd2, 0xb4, 0xd5, 0x76, 0x09,
  0x6f, 0xdb, 0x55, 0xdc, 0xf8, 0xe3, 0x0a, 0xf2,
  0x9e, 0x7d, 0xf0, 0xe1, 0x60, 0x30, 0xc0, 0xca,
  0xfc, 0xcc, 0xdf, 0xc1, 0xc8, 0xe4, 0x2f, 0xa0,
  0x86, 0xc3, 0x52, 0x0f, 0x5b, 0x54, 0xd9, 0x8c,
  0x90, 0xc7, 0xac, 0x70, 0xb8, 0xe2, 0xb9, 0x6c,
  0xea, 0x09, 0x21, 0x07, 0x7a, 0xe3, 0xce, 0xcb,
  0xa2, 0x00, 0x52, 0x6a, 0x07, 0x5e, 0xe5, 0xd8,
  0x5d, 0x60, 0x1a, 0x31, 0x6c, 0xf3, 0xbe, 0x56,
  0xd4, 0x48, 0xce, 0x07, 0x25, 0x11, 0x14, 0xf6,
  0x93, 0xba, 0x1f, 0x0b, 0x07, 0x2e, 0x10, 0xac,
  0x4b, 0x83, 0x53, 0x34, 0x4e, 0xb9, 0xca, 0x04,
  0x61, 0x19, 0x90, 0x79, 0x8c, 0xf1, 0x9a, 0x15,
  0xa5, 0x3c, 0xea, 0xa1, 0x0a, 0xb8, 0x0b, 0xc6,
  0x47, 0x9c, 0x76, 0xa3, 0x4b, 0x57, 0x08, 0x6c,
  0xa1, 0x40, 0x15, 0x5b, 0xc6, 0x88, 0x60, 0x7c,
  0x1a, 0x62, 0xb7, 0x06, 0x9e, 0x39, 0x66, 0xef,
  0x50, 0xcf, 0x56, 0x2b, 0x54, 0x0f, 0xdd, 0x5c,
  0x32, 0xd4, 0x2e, 0xa4, 0xe3, 0xb7, 0xbe, 0xdc,
  0x7f, 0xb7, 0x95, 0x73, 0x27, 0x90, 0x09, 0x40,
  0x04, 0x38, 0x67, 0xfd, 0x41, 0xf0, 0x53, 0x7b,
  0x80, 0x06, 0x9f, 0x6f, 0x10, 0x70, 0x1f, 0xdb,
  0x6c, 0x89, 0x74, 0xee, 0x05, 0xeb, 0x41, 0x57,
  0xfc, 0x57, 0x6e, 0xae, 0xb0, 0xe1, 0x36, 0xff,
  0x34, 0xee, 0x34, 0xc1, 0x72, 0xb3, 0x60, 0x61,
  0x92, 0xe1, 0xe9, 0xd0, 0xae, 0x2d, 0x38, 0xe7,
  0x03, 0xc9, 0xb9, 0x7b, 0xd8, 0x5b, 0xb9, 0x2f,
  0x35, 0x62, 0x37, 0x98, 0x15, 0x2f, 0x47, 0x89,
  0xdb, 0x4f, 0x1e, 0xc3, 0xa0, 0x28, 0x3b, 0x43,
  0x08, 0x23, 0xc6, 0x8d, 0x3d, 0x67, 0xea, 0xaa,
  0xc0, 0x01, 0x1e, 0x51, 0xa6, 0xaa, 0xf2, 0xbf,
  0x07, 0x7f, 0x78, 0x47, 0xcb, 0x7b, 0xc6, 0x72,
  0xe7, 0xbd, 0x3d, 0xee, 0xba, 0xec, 0xbb, 0xbe,
  0x13, 0x98, 0x03, 0x3e, 0x1e, 0x3f, 0x24, 0x40,
  0xe4, 0x8c, 0x55, 0x26, 0xa0, 0xc8, 0xb4, 0x76,
  0x45, 0xf1, 0xc7, 0x36, 0x0d, 0x80, 0x40, 0x96,
  0xd6, 0x7f, 0xcf, 0x90, 0xfb, 0xbc, 0xc3, 0xb7,
  0x06, 0x0f, 0x7f, 0x24, 0x79, 0x69, 0x17, 0x0a,
  0x02, 0x2c, 0xa9, 0x73, 0x44, 0x6b, 0x39, 0x56,
  0x4a, 0x86, 0x48, 0x2c, 0xb9, 0x35, 0x93, 0x15,
  0x0b, 0x1e, 0x92, 0x4c, 0xeb, 0xf8, 0x30, 0x59,
  0xa8, 0x6a, 0x47, 0x7b, 0x69, 0x2c, 0x0a, 0xa1,
  0xf0, 0x04, 0x09, 0x98, 0xc1, 0x40, 0xb1, 0x13,
  0xab, 0xf2, 0x6e, 0x8e, 0x22, 0x32, 0x7b, 0x42,
  0x12, 0x2f, 0xaa, 0x53, 0x01, 0x60, 0x52, 0x60,
  0x18, 0xe6, 0x84, 0x0c, 0xd6, 0x3f, 0x54, 0x2b,
  0xcb, 0xb6, 0x9c, 0xdc, 0x82, 0x43, 0x08, 0xc3,
  0x55, 0x83, 0x30, 0xfe, 0xd0, 0xab, 0x49, 0x19,
  0x75, 0xa8, 0xf8, 0xc7, 0xfe, 0x4a, 0x36, 0xb9,
  0xb5, 0x24, 0x80, 0x43, 0xf6, 0xe6, 0xb1, 0xa5,
  0x3d, 0x80, 0xa1, 0x35, 0xf3, 0xc3, 0xa3, 0x92,
  0x8f, 0x0e, 0xf4, 0x88, 0x79, 0x8c, 0xf9, 0x91,
  0x37, 0xfe, 0xdf, 0x95, 0x7f, 0xb6, 0x17, 0x11,
  0x37, 0x83, 0x56, 0xa3, 0xef, 0x32, 0x40, 0x6d,
  0xc1, 0x3c, 0xc0, 0x4b, 0x4e, 0x99, 0x74, 0xb4,
  0xfb, 0xd2, 0xa6, 0x23, 0x2d, 0xd3, 0x90, 0xf2,
  0x76, 0x15, 0x91, 0xb7, 0xee, 0xde, 0x05, 0x89,
  0xc5, 0x53, 0x71, 0xa0, 0x39, 0x97, 0x00, 0xbb,
  0x80, 0x4f, 0xe3, 0xb6, 0xf8, 0xc9, 0x7e, 0xc2,
  0xd6, 0xa8, 0xee, 0xcb, 0x76, 0xbd, 0x96, 0x2e,
  0x9a, 0xbc, 0xab, 0xe9, 0x9d, 0x50, 0x94, 0xf1,
  0x7b, 0x31, 0x6d, 0x13, 0x9a, 0xbf, 0x98, 0xa1,
  0x6b, 0xc6, 0xbe, 0x7d, 0xfb, 0x0a, 0x22, 0x26,
  0xfc, 0x82, 0x93, 0xb2, 0x1b, 0x92, 0x9c, 0x0b,
  0xe9, 0xf4, 0x4b, 0xc4, 0xb9, 0xf5, 0xd9, 0x56,
  0xb2, 0x21, 0x00, 0xd8, 0x68, 0xf0, 0xa3, 0x2e,
  0xee, 0x14, 0x62, 0xeb, 0xa1, 0x25, 0xa1, 0xc8,
  0x6b, 0xea, 0x6f, 0x10, 0x8f, 0xc3, 0x87, 0x72,
  0x52, 0xdb, 0xe9, 0x2b, 0xbe, 0xec, 0x94, 0x66,
  0x34, 0xd3, 0xcc, 0x6f, 0xbe, 0xcd, 0x32, 0x82,
  0xa8, 0x03, 0xb8, 0xb6, 0x67, 0x02, 0x12, 0x13,
  0x20, 0xcc, 0x53, 0x9f, 0xed, 0x4e, 0x26, 0xbf,
  0x67, 0x88, 0x54, 0x34, 0x2b, 0xa1, 0x64, 0x11,
  0x15, 0x76, 0x7a, 0x57, 0x51, 0x42, 0xc3, 0xc9,
  0x21, 0xd4, 0xe1, 0x26, 0x22, 0x9d, 0x12, 0x6e,
  0x12, 0x92, 0x9d, 0x3f, 0xfa, 0xa8, 0x04, 0x66,
  0x26, 0x2c, 0xda, 0x6a, 0x4a, 0x36, 0xbc, 0xa9,
  0xb0, 0x1a, 0xce, 0x0b, 0x02, 0xf0, 0xa4, 0x33,
  0x74, 0x7e, 0xb4, 0x99, 0xb4, 0xa8, 0x6a, 0x66,
  0x6a, 0x86, 0x7e, 0xdf, 0x7a, 0xc9, 0xfe, 0xa3,
  0x01, 0x05, 0x10, 0xc5, 0x15, 0xd7, 0x8e, 0xe3,
  0xee, 0xa6, 0xe6, 0x06, 0xd2, 0x64, 0x98, 0x45,
  0x59, 0xe1, 0x06, 0xbc, 0xf5, 0xcf, 0x16, 0xab,
  0x3f, 0xc5, 0xa3, 0x4d, 0xe5, 0x6e, 0x46, 0x7c,
  0xa2, 0xb3, 0xaf, 0xa0, 0x46, 0x5e, 0xfb, 0x65,
  0x53, 0xba, 0xf5, 0xc5, 0xb4, 0xb3, 0xab, 0xba,
  0x0c, 0x83, 0xb3, 0x84, 0x66, 0xc8, 0x8e, 0x79,
  0x7e, 0x9e, 0x30, 0x29, 0xd6, 0x23, 0x44, 0x40,
  0x66, 0x27, 0xa2, 0x3f, 0x0a, 0x0d, 0x6e, 0xf7,
  0xca, 0x4b, 0xfa, 0xbf, 0x0e, 0x95, 0xe6, 0x9f,
  0x66, 0xc2, 0x5b, 0x4a, 0xb2, 0x5c, 0x7e, 0x1a,
  0x7f, 0xba, 0xa5, 0xaf, 0xc6, 0xb6, 0x10, 0x1b,
  0x16, 0x0d, 0x0c, 0xe8, 0x67, 0xbc, 0x6d, 0x94,
  0x2b, 0x0c, 0x41, 0xa4, 0xc2, 0x0a, 0xc2, 0xea,
  0xb5, 0x84, 0x27, 0x1d, 0x77, 0xc8, 0x9e, 0xa7,
  0x5e, 0x87, 0xd5, 0xa8, 0xec, 0x76, 0xee, 0xef,
  0xd4, 0x45, 0xb6, 0xd9, 0x80, 0x60, 0xbd, 0x1c,
  0x07, 0x91, 0x57, 0x6b, 0x4a, 0xea, 0xc5, 0x44,
  0xd1, 0x45, 0xc3, 0xde, 0xa7, 0x86, 0xee, 0x37,
  0xe2, 0xf1, 0x68, 0xaa, 0xc6, 0x52, 0x89, 0x59,
  0xb7, 0x5c, 0x13, 0x6a, 0x8e, 0x8f, 0xf3, 0x1c,
  0x1d, 0xdb, 0x67, 0x88, 0x1e, 0x52, 0x74, 0xdb,
  0xa6, 0xa5, 0x56, 0xd5, 0x0c, 0x9e, 0x8a, 0xf2,
  0x74, 0xd3, 0xba, 0x52, 0xbe, 0x13, 0x24, 0xb2,
  0x74, 0x13, 0x92, 0xa3, 0xb4, 0x0a, 0x6c, 0xb6,
  0x8b, 0x46, 0x9b, 0x01, 0x52, 0xba, 0x05, 0xbe,
  0xcf, 0xcd, 0x89, 0x3c, 0x6b, 0x2b, 0xdb, 0xdf,
  0xa0, 0x74, 0xdc, 0x5c, 0xa2, 0x80, 0x72, 0xdb,
  0x6d, 0x9e, 0x10, 0xaf, 0x65, 0xdb, 0x20, 0xbd,
  0x80, 0x44, 0xb8, 0xc2, 0x2c, 0xa3, 0x01, 0xa3,
  0x90, 0xc2, 0x20, 0x8c, 0x66, 0xad, 0x45, 0x54,
  0x3e, 0x47, 0x73, 0x8b, 0x79, 0x07, 0x80, 0xf0,
  0x31, 0xb4, 0x11, 0x33, 0x07, 0x61, 0x2e, 0x58,
  0xa2, 0x9d, 0x3a, 0x2f, 0xe2, 0xfb, 0x6a, 0x18,
  0x71, 0xe3, 0x4a, 0x76, 0xc4, 0x6b, 0x93, 0x57,
  0x0c, 0x98, 0x14, 0x1b, 0x0b, 0x65, 0x75, 0xc9,
  0x41, 0x55, 0x0b, 0xc5, 0xc6, 0x42, 0x06, 0x80,
  0xb5, 0x86, 0xc7, 0xb9, 0x53, 0xce, 0x28, 0x26,
  0xad, 0x5f, 0x9f, 0x76, 0xc6, 0xbd, 0xd5, 0x81,
  0x92, 0x44, 0xcb, 0x4e, 0x3e, 0x69, 0xa9, 0xd8,
  0x3b, 0x0c, 0xb1, 0xcf, 0xdb, 0x68, 0xf9, 0x16,
  0x9a, 0x30, 0x02, 0xef, 0xa4, 0x64, 0xd4, 0x70,
  0x3f, 0xe0, 0xea, 0x49, 0x6e, 0xa1, 0x84, 0x22,
  0xe0, 0x2d, 0x35, 0x7e, 0x20, 0x74, 0x09, 0x41,
  0xd2, 0x5c, 0x72, 0x27, 0x93, 0x5b, 0xb2, 0x68,
  0x85, 0x97, 0x30, 0x1e, 0xf3, 0x5e, 0xe0, 0xb9,
  0x16, 0x57, 0xc7, 0xc7, 0x32, 0x01, 0x15, 0x6a,
  0x7c, 0xe9, 0x47, 0x76, 0x0d, 0x76, 0x1d, 0xc9,
  0x55, 0x42, 0xca, 0x9a, 0xef, 0x92, 0xb8, 0x10,
  0x50, 0x2f, 0xc9, 0xb5, 0x77, 0x72, 0xf3, 0x33,
  0x00, 0x3c, 0x70, 0x9c, 0xf7, 0xeb, 0x64, 0x5e,
  0xc0, 0x73, 0x38, 0x7b, 0x39, 0x79, 0xa4, 0x2c,
  0xab, 0x3f, 0xce, 0xbb, 0x1e, 0x1a, 0x15, 0x34,
  0x5e, 0x55, 0xb1, 0x5d, 0xf0, 0x8c, 0xe2, 0xf0,
  0x80, 0xdc, 0xca, 0x89, 0x51, 0x73, 0x9f, 0x14,
  0x9e, 0x72, 0x02, 0x0d, 0xfa, 0x43, 0x3c, 0xa4,
  0x4a, 0x9f, 0x07, 0x90, 0x54, 0xa3, 0xfc, 0x19,
  0x2e, 0xb4, 0xc2, 0xed, 0x83, 0x2b, 0x9f, 0x47,
  0x5c, 0x2f, 0xcd, 0xc9, 0xf2, 0x73, 0x45, 0x78,
  0xdf, 0x59, 0xba, 0xea, 0xbf, 0xf8, 0x1b, 0xfe,
  0x41, 0x9f, 0x0d, 0x8f, 0x17, 0xfc, 0x4a, 0xe7,
  0x1f, 0xdf, 0xb3, 0xe9, 0xc6, 0xe3, 0xb0, 0x84,
  0x72, 0x7c
};

char *interface = NULL;
char src_ip[16] = {0};
char dst_ip[16]= {0};
char gw_ip[16] = {0};

struct sockaddr_in sinaddr;

static uint16_t CalculateIpChecksum(uint16_t *pkt, uint16_t hlen)
{
    uint32_t csum = pkt[0];

    csum += pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[6] + pkt[7] + pkt[8] +
        pkt[9];

    hlen -= 20;
    pkt += 10;

    if (hlen == 0) 
	{
        ;
    } 
	else if (hlen == 4) 
	{
        csum += pkt[0] + pkt[1];
    } 
	else if (hlen == 8) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3];
    } 
	else if (hlen == 12) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5];
    } 
	else if (hlen == 16) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7];
    } 
	else if (hlen == 20) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9];
    } 
	else if (hlen == 24) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11];
    } 
	else if (hlen == 28) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13];
    } 
	else if (hlen == 32) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13] +
            pkt[14] + pkt[15];
    } 
	else if (hlen == 36) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13] +
            pkt[14] + pkt[15] + pkt[16] + pkt[17];
    } 
	else if (hlen == 40) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13] +
            pkt[14] + pkt[15] + pkt[16] + pkt[17] + pkt[18] + pkt[19];
    }

    csum = (csum >> 16) + (csum & 0x0000FFFF);
    csum += (csum >> 16);

    return (uint16_t) ~csum;
}

static inline uint16_t TCPCalculateChecksum(uint16_t *shdr, uint16_t *pkt, uint16_t tlen)
{
    uint16_t pad = 0;
    uint32_t csum = shdr[0];

    csum += shdr[1] + shdr[2] + shdr[3] + htons(6) + htons(tlen);

    csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
        pkt[7] + pkt[9];

    tlen -= 20;
    pkt += 10;

    while (tlen >= 32) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13] +
            pkt[14] + pkt[15];
        tlen -= 32;
        pkt += 16;
    }

    while(tlen >= 8) 
	{
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3];
        tlen -= 8;
        pkt += 4;
    }

    while(tlen >= 4) 
	{
        csum += pkt[0] + pkt[1];
        tlen -= 4;
        pkt += 2;
    }

    while (tlen > 1) 
	{
        csum += pkt[0];
        pkt += 1;
        tlen -= 2;
    }

    if (tlen == 1) 
	{
        *(uint8_t *)(&pad) = (*(uint8_t *)pkt);
        csum += pad;
    }

    csum = (csum >> 16) + (csum & 0x0000FFFF);
    csum += (csum >> 16);

    return (uint16_t)~csum;
}
void refill_src_mac(unsigned char *hw_addr)
{
	memcpy(packet_bytes + 6, hw_addr, 6);

		return;
}
void refill_dst_mac(unsigned char *hw_addr)
{
	memcpy(packet_bytes, hw_addr, 6);

		return;
}

int  GetSysMacBySocket()
{
    struct ifreq        ifr;
	unsigned char pMac[6] = {0};

    int sock = socket(AF_INET,SOCK_DGRAM,0);
    if(sock <= 0)
    {
        perror("socket error!\n");
        return -1;
    }

    strcpy(ifr.ifr_name, interface);

    if(ioctl(sock,SIOCGIFHWADDR,&ifr) < 0)
    {
        perror("ioctl SIOCGIFHWADDR error\n");
        close(sock);
        return -1;
    }
    else
    {
        sprintf(pMac, "%02x:%02x:%02x:%02x:%02x:%02x",
                (unsigned char)ifr.ifr_hwaddr.sa_data[0],
                (unsigned char)ifr.ifr_hwaddr.sa_data[1],
                (unsigned char)ifr.ifr_hwaddr.sa_data[2],
                (unsigned char)ifr.ifr_hwaddr.sa_data[3],
                (unsigned char)ifr.ifr_hwaddr.sa_data[4],
                (unsigned char)ifr.ifr_hwaddr.sa_data[5]);
		memcpy(pMac, ifr.ifr_hwaddr.sa_data, 6);
		refill_src_mac(pMac);
    }

    close(sock);
    return 0;
}

static int arp_get(char* ip)
{
 struct arpreq arpreq;
 struct sockaddr_in *sin;
 struct in_addr ina;
 unsigned char *hw_addr;
 int rc;

    int sd = -1;
	sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0)
    {
        perror("socket() error\n");
        exit(1);
    }

   memset(&arpreq, 0, sizeof(struct arpreq));

  sin = (struct sockaddr_in *) &arpreq.arp_pa;
  memset(sin, 0, sizeof(struct sockaddr_in));
  sin->sin_family = AF_INET;
  ina.s_addr = inet_addr(ip);
  memcpy(&sin->sin_addr, (char *)&ina, sizeof(struct in_addr));
        
  strcpy(arpreq.arp_dev, interface);
  rc = ioctl(sd, SIOCGARP, &arpreq);
  if (rc < 0)
    {
      printf("%s\n", "Entry not available in cache...");
      return -1;
    }
  else
   {
		hw_addr = (unsigned char *) arpreq.arp_ha.sa_data;
	 	refill_dst_mac(hw_addr);
    }
  return 0;
}

void exec_ping(char *ip)
{
	char buf[512];

	snprintf(buf, sizeof(buf), "ping %s -W 2 -c 1 2>&1 > /dev/null", ip);
	
	system(buf);

	return;
}

struct sockaddr_ll ll_addr;

static int create_socket()
{
	int sockfd = -1;
	struct ifreq req;

	sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
	if (-1 == sockfd)
	{
		return sockfd;
	}

	memset(&ll_addr, 0, sizeof(ll_addr));
	ll_addr.sll_family = AF_PACKET;

	memset(&req, 0, sizeof(req));
	strncpy(req.ifr_name, interface, strlen(interface));

	if (ioctl(sockfd, SIOCGIFINDEX, &req) < 0)
	{
		perror("ioctl");
		close(sockfd);
		sockfd = -1;
		return sockfd;
	}

	ll_addr.sll_ifindex = req.ifr_ifindex;
	ll_addr.sll_protocol = htons(ETH_P_IP);

	return sockfd;
}
#if 0
static int create_socket()
{
	int sockfd = -1;
	struct ifreq ifr;
	int on = 1;

	sockfd = socket (AF_INET, SOCK_RAW, IPPROTO_TCP);
	if (-1 == sockfd)
	{
		perror("socket");
		return sockfd;
	}

	sinaddr.sin_family = AF_INET;
	sinaddr.sin_addr.s_addr = inet_network(dst_ip);

#if 1
	if (setsockopt(sockfd, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) < 0)
	{
		perror("setsockopt");
		close(sockfd);
		sockfd = -1;
		return sockfd;
	}
#endif
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, strlen(interface));

	if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0)
	{
		perror("ioctl");
		close(sockfd);
		sockfd = -1;
	}

	return sockfd;
}
#endif
void refill_ip_header(struct iphdr *pstIpHeader)
{
	pstIpHeader->saddr = htonl(inet_network(src_ip));
	pstIpHeader->daddr = htonl(inet_network(dst_ip));

	pstIpHeader->check = CalculateIpChecksum((uint16_t *)pstIpHeader, 36);

	return;
}

void refill_ip_id(struct iphdr *pstIpHeader)
{
	uint16_t ids = ntohs(pstIpHeader->id);
	ids += ip_id;
	ip_id++;

	pstIpHeader->id = htons(ids);
	pstIpHeader->check = CalculateIpChecksum((uint16_t *)pstIpHeader, 36);

	return;
}

void refill_tcp_header(struct tcphdr *pstTcpHeader)
{

	pstTcpHeader->check = TCPCalculateChecksum((uint16_t *) (packet_bytes + 26), (uint16_t *) (packet_bytes + 34 + 16), 1464); 

	return;
}


void refill_tcp_cnt(struct tcphdr *pstTcpHeader, int cnt)
{
	memcpy( packet_bytes+0x50, &cnt, 4 );

	pstTcpHeader->check = TCPCalculateChecksum((uint16_t *) (packet_bytes + 26), (uint16_t *) (packet_bytes + 34 + 16), 1464); 

	return;
}

int main(int argc, char *argv[])
{
	int c = 0;
	int sockfd = -1;
	struct iphdr *pstIpHeader = NULL;
	struct tcphdr *pstTcpHeader = NULL;
	int gw_flag = 0;

	while(1)
	{
		c = getopt(argc, argv, "i:s:d:g:");
		if (-1 == c)
		{
			break;	
		}

		switch (c)
		{
			case 'i':
			{	
				interface = malloc(strlen(optarg) + 1);
				if (NULL == interface)
				{
					printf("Not enough memory !\n");
					exit(1);
				}
				strncpy(interface, optarg, strlen(optarg));	
				break;
			}
			case 's':
			{
				strncpy(src_ip, optarg, strlen(optarg));		
				break;
			}
			case 'd':
			{
				strncpy(dst_ip, optarg, strlen(optarg));		
				break;
			}
			case 'g':
			{
				strncpy(gw_ip, optarg, strlen(optarg));		
				gw_flag =1;
				break;
			}
			default:
			{
				break;
			}
		}	
	}

	if (NULL == interface)
	{
		printf("Usage: ./sendrawpacket -i eth0 -s source_ip -d dst_ip  [-g gateway_ip]\n");
		exit(0);
	}

	if (0 == strlen(src_ip))
	{
		printf("Usage: ./sendrawpacket -i eth0 -s source_ip -d dst_ip  [-g gateway_ip]\n");
		exit(0);	
	}
	if (0 == strlen(dst_ip))
	{
		printf("Usage: ./sendrawpacket -i eth0 -s source_ip -d dst_ip  [-g gateway_ip]\n");
		exit(0);	
	}

	GetSysMacBySocket();

	if (1 == gw_flag)
	{
		exec_ping(gw_ip);	
	}
	else
	{
		exec_ping(dst_ip);
	}

	sleep(1);

	if (1 == gw_flag)
	{
		if (-1 == arp_get(gw_ip))
		{
			printf("Network can not reach \n");
			exit(1);
		}
	
	}
	else
	{
		if (-1 == arp_get(dst_ip))
		{
			printf("Network can not reach \n");
			exit(1);
		}
	}


	sockfd = create_socket();
	if (-1 == sockfd)
	{
		exit(0);
	}

	pstIpHeader = (struct iphdr *)(packet_bytes + 14);
	pstTcpHeader = (struct tcphdr *) (packet_bytes + 14 + 36);

	refill_ip_header(pstIpHeader);
	refill_tcp_header(pstTcpHeader);

	printf("Send packet ...\n");

	memset( &(packet_bytes[0x50]), 0 ,8 );
	int *count = (int *)(&(packet_bytes[0x50]));
	int cnt = 1;
	while (1)
	{
		refill_ip_id(pstIpHeader);
		refill_tcp_cnt(pstTcpHeader, ++cnt);
		sendto(sockfd, packet_bytes, 1514, 0, (struct sockaddr *)&ll_addr, sizeof(struct sockaddr_ll));
		usleep(1000);
	}
	


	return 0;
}
