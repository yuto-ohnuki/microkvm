# Step 1: Execute `hlt`

> **Phase A: CPU Virtualization**
>
> Phase A covers the fundamentals of hardware-assisted virtualization: creating a VM, running guest code on a real CPU via KVM, and handling transitions between guest and host (VM exits). Each step adds one CPU virtualization concept.

## Goal

Run a single guest instruction (`hlt`) on a real CPU using the KVM API. With no I/O or mode transitions, this is a minimal demonstration that guest code executes natively through hardware virtualization.

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

After creating a vCPU, map its fd with `mmap` to obtain a shared page (`struct kvm_run`). After `KVM_RUN` returns, this page provides the exit reason:

```
┌─────────────────────────┐
│  exit_reason            │  ← exit reason (HLT, IO, MMIO, …)
│  io / mmio / …          │  ← exit-specific details
└─────────────────────────┘
```

The data is already available in userspace through the shared mapping, so reading exit information requires no additional `read()` or `write()` syscall.

### The `hlt` instruction

`hlt` (opcode `0xF4`) halts the CPU until the next interrupt. In this minimal guest, executing `hlt` causes `KVM_RUN` to return with `KVM_EXIT_HLT` (value 5). The hypervisor decides how to respond; here, it prints a success message and exits.

## Execution flow

```
Guest (hlt only)          KVM                            VMM (microkvm)
────────────────         ───                            ──────────────
                                                        1. open("/dev/kvm")
                                                        2. ioctl(KVM_CREATE_VM)
                                                        3. mmap(1MB anonymous)
                                                        4. KVM_SET_USER_MEMORY_REGION
                                                           → register guest memory
                                                        5. memcpy(0xF4 to GPA 0)
                                                        6. KVM_CREATE_VCPU
                                                        7. mmap(vcpufd) → kvm_run
                                                        8. KVM_SET_SREGS
                                                           (CS.base=0, CS.selector=0)
                                                        9. KVM_SET_REGS
                                                           (RIP=0, RFLAGS=2)
Begin execution ←─────── Enter the guest ←────────────── 10. ioctl(KVM_RUN)
  │
hlt at GPA 0
  └── VM exit ─────────→ Record exit reason in kvm_run
                         Return from KVM_RUN ──────────→ 11. run->exit_reason == 5
                                                        12. Print success message
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

This tells KVM to associate guest physical address 0 with this host virtual address. The memory slot identifies the backing store for guest physical memory and is used to set up second-stage address translation, such as EPT/NPT.

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
regs.rflags = 0x2;  /* bit 1 is reserved and must be set */
ioctl(vcpufd, KVM_SET_REGS, &regs);
```

The effective instruction address is `CS.base + RIP = 0 + 0 = 0`, which points to our `hlt` instruction.

Why read sregs before setting it? It contains many fields (DS, SS, CR0, CR4, EFER…). Zeroing all of them could invalidate the state needed to run the guest. Read the current state, change only the required fields, and write it back.

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

`Starting guest...` is printed before guest execution begins. `Exit reason: 5` identifies `KVM_EXIT_HLT`, and the final line confirms that the guest executed `hlt`.

## Key insight

This step establishes the basic structure: the VMM prepares the VM state, runs the guest with `KVM_RUN`, and checks the exit reason. Here, `KVM_RUN` is called only once. Step 2 extends this into a loop that repeatedly runs the guest.

## What this step teaches

| Concept | How it appears here |
|---------|-------------------|
| KVM fd hierarchy | system → VM → vCPU, each with its own ioctl set |
| Guest memory setup | `mmap` + `KVM_SET_USER_MEMORY_REGION` → backing store for guest physical memory |
| struct kvm_run | Shared page for zero-copy exit information |
| VM exit | Guest `hlt` → hardware VMEXIT → KVM fills kvm_run → ioctl returns |
| Register initialization | sregs (mode/segments) vs regs (execution state) |

## Next step

[Step 2: I/O port character output](step02_io-port.md) — add an exit handler loop and use PIO (`out` instruction) to send characters from guest to host.
