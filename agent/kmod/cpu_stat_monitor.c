// cpu_stat_monitor: 聚合所有 CPU 的 jiffies 级时间统计（kcpustat，GPL 导出符号）
// read 返回一行 8 个整数：user nice system idle iowait irq softirq steal
// 单位与 /proc/stat 一致（tick），agent 用差值算使用率
#include <linux/cpumask.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

static ssize_t cpu_stat_read(struct file *f, char __user *buf, size_t len,
                             loff_t *off) {
  char kbuf[192];
  struct kernel_cpustat kcs;
  unsigned int cpu;
  u64 s[8] = {};  // user nice system idle iowait irq softirq steal

  for_each_online_cpu(cpu) {
    kcpustat_cpu_fetch(&kcs, cpu);
    s[0] += kcs.cpustat[CPUTIME_USER];
    s[1] += kcs.cpustat[CPUTIME_NICE];
    s[2] += kcs.cpustat[CPUTIME_SYSTEM];
    s[3] += kcs.cpustat[CPUTIME_IDLE];
    s[4] += kcs.cpustat[CPUTIME_IOWAIT];
    s[5] += kcs.cpustat[CPUTIME_IRQ];
    s[6] += kcs.cpustat[CPUTIME_SOFTIRQ];
    s[7] += kcs.cpustat[CPUTIME_STEAL];
  }

  int n = scnprintf(kbuf, sizeof(kbuf), "%llu %llu %llu %llu %llu %llu %llu %llu\n",
                    s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
  if (*off > 0 || len < (size_t)n) return 0;  // 单次读语义：读一次即完
  if (copy_to_user(buf, kbuf, n)) return -EFAULT;
  *off = n;
  return n;
}

static const struct file_operations cpu_stat_fops = {
    .owner = THIS_MODULE,
    .read = cpu_stat_read,
};

static struct miscdevice cpu_stat_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "cpu_stat_monitor",
    .fops = &cpu_stat_fops,
    .mode = 0444,
};

static int __init cpu_stat_init(void) {
  int ret = misc_register(&cpu_stat_dev);
  if (ret == 0) pr_info("cpu_stat_monitor: /dev/cpu_stat_monitor ready\n");
  return ret;
}

static void __exit cpu_stat_exit(void) { misc_deregister(&cpu_stat_dev); }

module_init(cpu_stat_init);
module_exit(cpu_stat_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Aggregate CPU time statistics via kcpustat (GPL symbols)");
