// eBPF 网络流量采集：kprobe 挂载内核协议栈收发路径，按字节累计
// TX: __dev_queue_xmit（dev_queue_xmit 在新内核已内联进此函数）
// RX: __netif_receive_skb（所有收包必经路径，含回环与 NAPI/GRO 汇聚后）
// 使用 CO-RE 读取 skb->len，无需针对内核版本重新编译
#include "vmlinux.h"

#define __TARGET_ARCH_x86
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

// 键: 0 = RX(收), 1 = TX(发)；percpu 数组避免跨 CPU 锁竞争
struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(max_entries, 2);
  __type(key, __u32);
  __type(value, __u64);
} net_bytes SEC(".maps");

static __always_inline void count_skb(__u32 dir, __u64 len) {
  __u64 *v = bpf_map_lookup_elem(&net_bytes, &dir);
  if (v) *v += len;  // percpu 槽位，仅当前 CPU 访问，无需原子操作
}

SEC("kprobe/__dev_queue_xmit")
int BPF_KPROBE(k_tx, struct sk_buff *skb) {
  count_skb(1, BPF_CORE_READ(skb, len));
  return 0;
}

SEC("kprobe/__netif_receive_skb")
int BPF_KPROBE(k_rx, struct sk_buff *skb) {
  count_skb(0, BPF_CORE_READ(skb, len));
  return 0;
}

char LICENSE[] SEC("license") = "GPL";
