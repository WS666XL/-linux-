// softirq_monitor: 统计每类软中断的累计触发次数（kstat，GPL 导出符号）
// read 返回一行 10 个整数，顺序同 softirq 枚举：
// HI TIMER NET_TX NET_RX BLOCK IRQ_POLL TASKLET SCHED HRTIMER RCU
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

static ssize_t softirq_read(struct file *f, char __user *buf, size_t len,
                            loff_t *off) {
  char kbuf[224];
  unsigned int cpu;
  u64 cnt[NR_SOFTIRQS] = {};

  for_each_online_cpu(cpu) {
    for (int i = 0; i < NR_SOFTIRQS; i++)
      cnt[i] += kstat_cpu(cpu).softirqs[i];
  }

  u64 total = 0;
  for (int i = 0; i < NR_SOFTIRQS; i++) total += cnt[i];

  int n = scnprintf(kbuf, sizeof(kbuf),
                    "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
                    total, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5],
                    cnt[6], cnt[7], cnt[8], cnt[9]);
  if (*off > 0 || len < (size_t)n) return 0;
  if (copy_to_user(buf, kbuf, n)) return -EFAULT;
  *off = n;
  return n;
}

static const struct file_operations softirq_fops = {
    .owner = THIS_MODULE,
    .read = softirq_read,
};

static struct miscdevice softirq_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "softirq_monitor",
    .fops = &softirq_fops,
    .mode = 0444,
};

static int __init softirq_monitor_init(void) {
  int ret = misc_register(&softirq_dev);
  if (ret == 0) pr_info("softirq_monitor: /dev/softirq_monitor ready\n");
  return ret;
}

static void __exit softirq_monitor_exit(void) { misc_deregister(&softirq_dev); }

module_init(softirq_monitor_init);
module_exit(softirq_monitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Per-softirq cumulative counters via kstat (GPL symbols)");
