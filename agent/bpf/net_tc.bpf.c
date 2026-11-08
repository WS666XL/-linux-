// TC(clsact) 网络流量采集：ingress/egress 各挂一个 SCHED_CLS 分类器
// 与 kprobe 版相比：只统计指定网卡，且收发方向由挂载点天然区分
// 使用 CO-RE 直接读 skb->len，无需针对内核版本重新编译
#include "vmlinux.h"

#define __TARGET_ARCH_x86
#include <bpf/bpf_helpers.h>

// 键: 0 = RX(ingress), 1 = TX(egress)；percpu 数组避免跨 CPU 锁竞争
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 2);
  __type(key, __u32);
  __type(value, __u64);
} tc_bytes SEC(".maps");

static __always_inline void count_skb(struct __sk_buff *skb, __u32 dir) {
  __u64 *v = bpf_map_lookup_elem(&tc_bytes, &dir);
  if (v) *v += skb->len;  // percpu 槽位，仅当前 CPU 访问
}

SEC("classifier")
int tc_rx(struct __sk_buff *skb) {
  count_skb(skb, 0);
  return 0;  // TC_ACT_OK
}

SEC("classifier")
int tc_tx(struct __sk_buff *skb) {
  count_skb(skb, 1);
  return 0;  // TC_ACT_OK
}

char LICENSE[] SEC("license") = "GPL";
