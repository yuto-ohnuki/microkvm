#ifndef KVM_STATS_H
#define KVM_STATS_H

#include <stdint.h>
#include <linux/kvm.h>

#define KVM_STATS_MAX_ENTRIES 64

/* Single scalar stat from KVM's binary stats interface */
struct kvm_stat_entry {
    char name[48];
    uint64_t value;
    uint32_t flags;
};

/* Snapshot of all scalar stats from one stats fd */
struct kvm_stats_reading {
    unsigned int count;
    struct kvm_stat_entry entries[KVM_STATS_MAX_ENTRIES];
};

int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap);
int kvm_stats_is_interesting(const char *name);
void kvm_stats_print_delta(const char *label,
    const struct kvm_stats_reading *before,
    const struct kvm_stats_reading *after);

#endif /* KVM_STATS_H */