# Step 1: Execute `hlt`

## Goal

Run a single guest instruction (`hlt`) on a real CPU using the KVM API.
This is the absolute minimum hypervisor - no I/O, no mode transitions, just proof that guest code executes natively via hardware virtualization.

## Background

### KVM API hierarchy

The KVM API is a set of `ioctl` calls on file descriptors arranged in three levels:

```
/dev/kvm  (system fd)
  └── VM fd        (ioctl KVM_CREATE_VM)
        └── vCPU fd  (ioctl KVM_CREATE_VCPU)
```

Each level controls a different scope:
- **System fd**: query capabilities, get global parameters
- **VM fd**: manage memory regions, create vCPUs
- **vCPU fd**: set registers, run guest code, read exit info

### struct kvm_run (shared page)

When you create a vCPU, you `mmap` its fd to get a shared page (`struct kvm_run`).
After `KVM_RUN` returns, this page tells you *why* the guest exited:

```
┌─────────────────────────┐
│  exit_reason            │  ← why the VM exited (HLT, IO, MMIO, …)
│  io / mmio / …          │  ← exit-specific details
└─────────────────────────┘
```

This avoids a `read()`/`write()` syscall per exit - the data is already in userspace via the shared mapping.

### The `hlt` instruction

`hlt` (opcode `0xF4`) halts the CPU until the next interrupt.
In this minimal guest, executing `hlt` causes `KVM_RUN` to return with `KVM_EXIT_HLT` (value 5). The hypervisor decides what to do — in our case, we simply print success and exit.

## Execution flow

```
microkvm (host)                              KVM                    Hardware
───────────────                              ───                    ────────
1. open("/dev/kvm")
2. ioctl(KVM_CREATE_VM)
3. mmap(1MB anonymous)
4. ioctl(KVM_SET_USER_MEMORY_REGION)
     → KVM uses this memory slot to translate guest physical memory accesses.
5. memcpy(0xF4 to GPA 0)
6. ioctl(KVM_CREATE_VCPU)
7. mmap(vcpufd) → struct kvm_run
8. KVM_SET_SREGS  (CS.base=0)
9. KVM_SET_REGS   (RIP=0, RFLAGS=2)
10. ioctl(KVM_RUN) ─────────────→  VMLAUNCH ──────→  guest mode
                                                      │
                                                      │ hlt at GPA 0
                                                      ↓
11. run->exit_reason == 5 ←─────  kvm_run filled ←─  VMEXIT (HLT)
12. printf("Success!")
```

## Implementation

### 1. Open /dev/kvm and create a VM

```c
kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);
```

`kvmfd` is the system-level handle. `KVM_CREATE_VM` returns a VM fd — an empty container with no CPUs or memory yet.

### 2. Allocate and register guest memory

```c
mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = GUEST_MEM_SIZE,   /* 1 MB */
    .userspace_addr = (unsigned long)mem,
};
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
```

This tells KVM: "map guest physical address 0 to this host virtual address."
KVM uses this memory slot as the backing store for guest physical memory.
On hardware virtualization, this eventually participates in second-stage address translation such as EPT/NPT.

### 3. Load guest code

```c
static const unsigned char guest_code[] = { 0xf4 /* hlt */ };
memcpy(mem, guest_code, sizeof(guest_code));
```

The `hlt` instruction is placed at GPA 0 — exactly where the vCPU will start executing.

### 4. Create vCPU and map kvm_run

```c
vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
```

The `mmap` gives us zero-copy access to exit information. The size is queried at runtime because it may vary across kernel versions.

### 5. Initialize registers

```c
/* Special registers — get first, modify CS only, set back */
ioctl(vcpufd, KVM_GET_SREGS, &sregs);
sregs.cs.base = 0;
sregs.cs.selector = 0;
ioctl(vcpufd, KVM_SET_SREGS, &sregs);

/* General registers — start from zero */
memset(&regs, 0, sizeof(regs));
regs.rip = 0;
regs.rflags = 0x2;   /* bit 1 must always be set on x86 */
ioctl(vcpufd, KVM_SET_REGS, &regs);
```

The effective instruction address is `CS.base + RIP = 0 + 0 = 0`, which points to our `hlt` instruction.

Why GET then SET for sregs: sregs contains many fields (DS, SS, CR0, CR4, EFER…). Zeroing them all would crash the CPU. We preserve defaults and only change what we need.

### 6. Run and check exit reason

```c
ioctl(vcpufd, KVM_RUN, NULL);

switch (run->exit_reason) {
case KVM_EXIT_HLT:
    printf("Guest executed HLT. Success!\n");
    break;
}
```

## Output

```
$ ./microkvm
Starting guest...
Exit reason: 5
Guest executed HLT. Success!
```

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| KVM fd hierarchy | system → VM → vCPU, each with its own ioctl set |
| Guest memory setup | `mmap` + `KVM_SET_USER_MEMORY_REGION` → backing store for guest physical memory |
| struct kvm_run | Shared page for zero-copy exit information |
| VM exit | Guest `hlt` → hardware VMEXIT → KVM fills kvm_run → ioctl returns |
| Register initialization | sregs (mode/segments) vs regs (execution state) |

## Why this step matters

This step establishes the smallest possible KVM execution loop: host userspace prepares VM state, enters the guest with `KVM_RUN`, and regains control when the guest exits.

All later steps keep this structure and only add one new concept at a time.

## Next step

[Step 2: I/O port character output](step02_io-port.md) — add an exit handler loop and use PIO (`out` instruction) to send characters from guest to host.
