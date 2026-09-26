# Step 42: EPT Explorer—Relate Guest Memory to Host Backing

## Key point

**Registration in a RAM slot does not guarantee an existing EPT mapping.** Use `< Ctrl-A e >` to inspect GPA-to-microkvm-memory relationships and compare them with boot-time EPT faults.

Distinguish VMM memory registration, CPU address translation, and KVM fault handling.

## Background

### Guest paging and EPT

With guest paging enabled, memory accesses involve two translation stages.

```text
GVA (guest virtual address)
    │ Guest page tables: built by guest OS, rooted at guest CR3
    ▼
GPA (guest physical address)
    │ EPT: built by KVM, rooted at EPT pointer
    ▼
HPA (host physical address)
```

The CPU uses guest page tables, EPT, and translation caches. With the required translations and permissions available, that memory access does not need to return to KVM.

microkvm's `mmap()` instead returns an HVA. Host page tables connect the HVA to a host backing page, which KVM uses to build GPA→HPA EPT mappings. EPT translation does not pass through HVA.

```text
                  Memory slot registration
                 GPA             HVA
                  │ EPT           │ host page tables
                  ▼               ▼
                      host backing memory
```

### Connection to Parts 1 and 2

Step 5 created a hole in RAM registration for MMIO. Steps 19–20 retrieved MMU stats and dirty pages; Step 38 observed RAM-fault and MMIO paths. Add memory slot information here to connect these from an address-translation perspective.

## Implementation added in this step

Changes are confined to `microkvm.c`: an array retaining registered memory regions, a GPA lookup function, and monitor display handling.

### Use the same structures for registration and display

```c
#define MAX_MEMSLOTS 2

static struct kvm_userspace_memory_region g_memslots[MAX_MEMSLOTS];
static size_t g_nr_memslots = 0;
```

Set GPA, size, HVA, and `KVM_MEM_LOG_DIRTY_PAGES` in `g_memslots[0]` and `[1]`, using the same array for KVM registration and display.

```c
g_nr_memslots = 0;
for (size_t i = 0; i < MAX_MEMSLOTS; i++) {
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &g_memslots[i]) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 1;
    }
    g_nr_memslots++;
}
```

This displays information microkvm retains from its own registrations; it does not retrieve a slot list from KVM. Since this configuration does not change slot information after registration, `< Ctrl-A e >` displays it directly in the input thread.

### Find a slot and HVA from a GPA

```c
static const struct kvm_userspace_memory_region *find_memslot(uint64_t gpa)
{
    for (size_t i = 0; i < g_nr_memslots; i++) {
        uint64_t start = g_memslots[i].guest_phys_addr;
        uint64_t end   = start + g_memslots[i].memory_size;
        if (gpa >= start && gpa < end)
            return &g_memslots[i];
    }
    return NULL;
}
```

A slot range includes its start and excludes its end. If a slot matches, calculate HVA with:

```text
HVA = slot.userspace_addr + (GPA - slot.guest_phys_addr)
```

The monitor lists registered slots and calculates GPA gaps between adjacent slots. The array is maintained in ascending GPA order. It also looks up `0x00100000` and `0x000d0000`, showing their slot/HVA or unregistered status. These lookups do not issue guest memory accesses.

## Observation A: Display memory slots

Boot the Linux guest in Terminal A and invoke the monitor from the prompt.

```bash
cd ~/microkvm
./microkvm
```

```text
/ # < Ctrl-A e >
```

Existing measurements, presented in the current display format, are shown below. HVAs may differ on each run.

```text
=== KVM Memory Slots (Ctrl-A e) ===
Slot 0
  GPA  : [0x00000000, 0x000d0000)
  size : 0xd0000
  HVA  : [0x7f619e400000, 0x7f619e4d0000)
Slot 1
  GPA  : [0x000d1000, 0x08000000)
  size : 0x7f2f000
  HVA  : [0x7f619e4d1000, 0x7f61a6400000)
Unregistered GPA gap
  GPA  : [0x000d0000, 0x000d1000)  (no RAM memslot)
Query GPA 0x00100000
  slot : 1
  HVA  : 0x7f619e500000
Query GPA 0x000d0000
  slot : none (unregistered)
===================================
```

### Ranges registered as RAM

microkvm allocates a contiguous 128 MiB virtual address range with `mmap()` and registers two ranges with KVM, excluding a 4 KiB hole. This does not imply contiguous host physical pages.

GPA `0x00100000` is in slot 1. For the example display:

```text
Offset within slot = 0x00100000 - 0x000d1000 = 0x2f000
HVA = 0x7f619e4d1000 + 0x2f000 = 0x7f619e500000
```

The location corresponding to GPA `0x000d0000` is also within the original `mmap()` range but is not registered as guest RAM. **An existing HVA region and a GPA belonging to a RAM slot are separate facts.** This hole is also distinct from virtio-mmio at `0xd0000000`.

The display shows GPA→slot→HVA relationships, not HPA or EPT entry values.

## Observation B: Follow EPT faults during boot

Use boot traces to examine RAM mapping and MMIO handling. This observes faults naturally caused by Linux boot; it does not issue accesses to Observation A's two GPAs.

Stop Observation A's VM. Use Terminal A to start the VM and Terminal B for host tracing; stop other VMs and perf.

### 1. Configure tracing before boot

Check events in Terminal B.

```bash
trace_dir=/sys/kernel/tracing
sudo ls "$trace_dir/events/kvm/kvm_exit/enable" "$trace_dir/events/kvm/kvm_page_fault/enable"
```

Clear Step 41's CPUID filter and record exit events, including EPT, plus page-fault events.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_page_fault/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_page_fault/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. Stop and save after boot

Start in Terminal A.

```bash
./microkvm
```

When the guest prompt appears, stop/save in Terminal B before idle events overwrite the boot recording.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-ept-boot.txt
```

In Terminal A, inspect this run's slots with `< Ctrl-A e >`, then stop the VM and retain the final MMU stats. HVAs may differ from Observation A.

### 3. Check exit reasons and fault GPAs

Read the saved file in Terminal B.

```bash
grep -E 'reason EPT_VIOLATION|reason EPT_MISCONFIG' ~/kvm-ept-boot.txt | head -20
grep -c 'reason EPT_VIOLATION' ~/kvm-ept-boot.txt
grep -c 'reason EPT_MISCONFIG' ~/kvm-ept-boot.txt
grep 'kvm_page_fault:' ~/kvm-ept-boot.txt | head -20
```

Read `kvm_exit` reasons and `kvm_page_fault` addresses (GPAs in this VMX path). Compare the latter with slot GPA ranges. `head` shows only the beginning; use `less` to examine the full file.

An existing boot measurement recorded these trace counts.

```text
EPT_VIOLATION  1463
EPT_MISCONFIG  774
```

The same run's final report showed these values.

```text
pf_taken               +4321
pf_fixed               +4318
pf_emulate             +3
pf_mmio_spte_created   +3
mmio_exits             +163
```

The trace buffer's retained window and the stats collection period may differ, so do not map the two sets of counts one-to-one.

### RAM faults: Build or update mappings

Recorded `kvm_page_fault` GPAs ranged from `0x1eb800` to `0x07ffffff`, within slot 1. Even registered RAM can cause EPT violations when mappings are absent or permissions need handling.

```text
EPT violation
    ↓
KVM: find the memory slot for the GPA
    ↓
Resolve the host backing page from HVA
    ↓
Build/update EPT mapping and resume guest
```

`pf_fixed +4318` shows that KVM's MMU resolved many faults. Registering RAM and constructing actual EPT mappings are separate stages.

### MMIO: Identification through EPT misconfiguration

As seen in Step 38, KVM uses a special SPTE encoding to identify known MMIO accesses. On Intel EPT, accessing this encoding causes EPT misconfiguration, leading through `handle_ept_misconfig()` to MMIO handling.

When KVM handles an access outside RAM memslots as MMIO, as here, and MMIO caching is enabled, it creates an MMIO SPTE. Subsequent accesses while that entry remains valid can use EPT misconfiguration instead of EPT violation. This does not eliminate exits; it makes post-exit MMIO identification more efficient.

`pf_mmio_spte_created +3` and `EPT_MISCONFIG 774` help explain this path. These totals alone do not establish that Observation A's hole at `0x000d0000` was accessed.

## Checking the KVM implementation

### KVM resolves backing through slots

Linux v7.2's [`gfn_to_hva()`](https://github.com/torvalds/linux/blob/v7.2/virt/kvm/kvm_main.c#L2741) looks up a slot from a guest frame number (GFN) and obtains the HVA.

```c
unsigned long gfn_to_hva(struct kvm *kvm, gfn_t gfn)
{
    return gfn_to_hva_many(gfn_to_memslot(kvm, gfn), gfn, NULL);
}
```

This is KVM software processing; the CPU does not call it on every memory access. RAM-fault handling uses the slot and guest frame number to resolve a host backing page for constructing an EPT mapping.

[`kvm_mmu_faultin_pfn()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c#L4704) branches separately when no slot exists (function excerpt).

```c
struct kvm_memory_slot *slot = fault->slot;
/* Declarations and state checks omitted */
if (unlikely(!slot))
    return kvm_handle_noslot_fault(vcpu, fault, access);
```

### Distinguish page-fault events from hardware exits

[`handle_ept_violation()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5967) calls `trace_kvm_page_fault()`, while [`handle_ept_misconfig()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5983) reaches MMIO handling through another entry point. As Step 38 showed, reaching the same `kvm_mmu_page_fault()` does not imply recording the same tracepoint.

## Division of responsibilities between EPT and KVM

```text
Guest memory access
    ├─ Valid translation and permissions
    │      → CPU accesses through EPT / translation caches
    │
    ├─ EPT violation
    │      → KVM builds/updates RAM mappings or handles MMIO
    │
    └─ EPT misconfiguration from an MMIO SPTE
           → KVM handles MMIO
                 → KVM_EXIT_MMIO if userspace device handling is needed
```

`kvm_page_fault` is a tracepoint within KVM processing. VMX EPT violation and userspace `KVM_EXIT_MMIO` are events at different stages. The CPU checks EPT translations, permissions, and format; it does not consult memory slots. KVM and the VMM use registration information to handle accesses as RAM or devices.

## Stop tracing

Disable events in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## What we learned

- Memory slots define the mapping between GPAs and userspace backing memory.
- HVA/HPA and guest paging/EPT are different addresses and translations.
- With valid EPT mappings, KVM does not intervene on each memory access.
- EPT violations also occur for RAM; MMIO may use an EPT misconfiguration path.

## References

- [KVM API: KVM_SET_USER_MEMORY_REGION](https://docs.kernel.org/virt/kvm/api.html#kvm-set-user-memory-region)
- [Linux KVM MMU: Translation / Memory](https://docs.kernel.org/virt/kvm/x86/mmu.html): relationships among GPA, HVA, HPA, and mappings
- [Linux v7.2 KVM VMX implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): `handle_ept_violation()` / `handle_ept_misconfig()`
- [Linux v7.2 KVM MMU implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c): `kvm_mmu_page_fault()` and MMIO identification
- [Linux v7.2 KVM SPTE implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/spte.c): MMIO entry creation through `make_mmio_spte()`
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3, EPT, EPT Violations, EPT Misconfigurations

## Next step

[Step 43](step43_execution-controls.md) compares CPUID, HLT, and RDMSR, examining the VMX execution controls that determine when instructions cause VM exits.
