#ifndef KVM_STATS_H
#define KVM_STATS_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

/*
 * KVM binary stats reader (Step 19: KVM MMU stats explorer)
 *
 * Uses KVM_GET_STATS_FD to read per-VM and per-vCPU counters.
 * Stat names are kernel-version-dependent; we filter by prefix
 * rather than hardcoding specific names.
 */

#define KVM_STATS_MAX_ENTRIES 64

struct kvm_stat_entry {
    char name[48];
    uint64_t value;
    uint32_t flags;
};

struct kvm_stats_reading {
    unsigned int count;
    struct kvm_stat_entry entries[KVM_STATS_MAX_ENTRIES];
};

/* Take a snapshot of all stats from a stats fd */
static int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap) {
    struct kvm_stats_header hdr;
    if (pread(stats_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr))
        return -1;

    size_t one_desc = sizeof(struct kvm_stats_desc) + hdr.name_size;
    char *descs = malloc(one_desc * hdr.num_desc);
    if (!descs) return -1;
    pread(stats_fd, descs, one_desc * hdr.num_desc, hdr.desc_offset);

    /* Read data block */
    size_t data_size = 8 * 1024;
    char *data = malloc(data_size);
    if (!data) { free(descs); return -1; }
    pread(stats_fd, data, data_size, hdr.data_offset);

    unsigned int n = 0;
    for (unsigned int i = 0; i < hdr.num_desc && n < KVM_STATS_MAX_ENTRIES; i++) {
        struct kvm_stats_desc *d = (struct kvm_stats_desc *)(descs + i * one_desc);
        /* Only capture scalar stats (size == 1 means one uint64_t) */
        if (d->size != 1) continue;
        strncpy(snap->entries[n].name, d->name, 47);
        snap->entries[n].name[47] = '\0';
        memcpy(&snap->entries[n].value, data + d->offset, sizeof(uint64_t));
        snap->entries[n].flags = d->flags;
        n++;
    }
    snap->count = n;

    free(descs);
    free(data);
    return 0;
}

/* Check if a stat name is MMU/memory/exit related */
static int kvm_stats_is_interesting(const char *name) {
    return strstr(name, "pf_") ||
           strstr(name, "mmu_") ||
           strstr(name, "pages_") ||
           strstr(name, "tlb") ||
           strcmp(name, "exits") == 0 ||
           strcmp(name, "mmio_exits") == 0 ||
           strcmp(name, "io_exits") == 0 ||
           strcmp(name, "halt_exits") == 0 ||
           strcmp(name, "irq_injections") == 0 ||
           strstr(name, "nx_lpage");
}

/* Print delta between two snapshots (only interesting stats with non-zero delta) */
static void kvm_stats_print_delta(const char *label,
                                  const struct kvm_stats_reading *before,
                                  const struct kvm_stats_reading *after) {
    fprintf(stderr, "\n--- %s ---\n", label);
    int printed = 0;
    for (unsigned int i = 0; i < after->count; i++) {
        if (!kvm_stats_is_interesting(after->entries[i].name))
            continue;

        /* Find matching entry in before */
        uint64_t before_val = 0;
        for (unsigned int j = 0; j < before->count; j++) {
            if (strcmp(before->entries[j].name, after->entries[i].name) == 0) {
                before_val = before->entries[j].value;
                break;
            }
        }

        uint64_t after_val = after->entries[i].value;
        uint32_t flags = after->entries[i].flags;
        /* flags & 0xF == type: 0=cumulative, 1=instant, 2=peak */
        int type = flags & 0xF;

        if (type == 1 || type == 2) {
            /* Instant/peak: show current value */
            if (after_val != 0) {
                fprintf(stderr, "  %-32s %llu (current)\n",
                        after->entries[i].name, (unsigned long long)after_val);
                printed++;
            }
        } else {
            /* Cumulative: show delta */
            uint64_t delta = after_val - before_val;
            if (delta != 0) {
                fprintf(stderr, "  %-32s +%llu\n",
                        after->entries[i].name, (unsigned long long)delta);
                printed++;
            }
        }
    }
    if (!printed)
        fprintf(stderr, "  (no changes)\n");
}

#endif /* KVM_STATS_H */