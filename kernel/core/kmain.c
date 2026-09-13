// SPDX-License-Identifier: MIT
// AfriyieOS — kmain, the kernel entry point
//
// =============================================================================
// Boot sequence
// =============================================================================
//   1.  Early architecture console (serial) — without this, failures are silent
//   2.  Validate the boot handoff from the boot bridge
//   3.  Dump the machine description
//   4.  CPU identification
//   5.  Framebuffer bring-up
//   6.  On-screen console (so a developer without a serial cable sees the log)
//   7.  Splash screen — proves the whole graphics path works
//   8.  Run the in-kernel self tests
//   9.  Announce AF_BOOT_OK and idle
//
// Everything from step 5 onward is what makes v0.1 demonstrable. Steps 7, 8 and
// 9 are the acceptance criteria from docs/AfriyieOS-Blueprint.md section 11.
// =============================================================================

#include "afriyie/config.h"
#include "afriyie/types.h"
#include "afriyie/status.h"
#include "afriyie/boot_info.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/io.h"
#include "afriyie/fb.h"
#include "afriyie/splash.h"
#include "afriyie/selftest.h"
#include "afriyie/hal.h"
#include "afriyie/arch_hooks.h"
#include "afriyie/kstring.h"
#include "afriyie/pmm.h"
#include "afriyie/heap.h"
#include "afriyie/thread.h"
#include "afriyie/sched.h"
#include "afriyie/pci.h"
#include "afriyie/block.h"
#include "afriyie/virtio_blk.h"
#include "afriyie/fs.h"
#include "afriyie/syscall.h"
#include "afriyie/elf.h"
#include "afriyie/process.h"

// Defined at the bottom of this file. Declared here because kmain creates the
// idle thread before the definition appears.
static void idle_thread_entry(void *arg);
static void usermode_stub_thread(void *arg);

// -----------------------------------------------------------------------------
// Recovery path
//
// If the boot handoff is unusable there is nothing sensible to do but say so as
// loudly as possible. This deliberately does not panic: a panic prints a
// register dump, and a bad boot_info is not a CPU fault.
// -----------------------------------------------------------------------------
static AF_NORETURN void boot_failed(af_status_t status, const char *what)
{
    af_log_raw("\n");
    af_log_raw("================================================================\n");
    af_log_raw("  AFRIYIEOS BOOT FAILURE\n");
    af_log_raw("================================================================\n");
    af_log_raw("  stage  : ");
    af_log_raw(what);
    af_log_raw("\n  status : ");
    af_log_raw(af_status_name(status));
    af_log_raw("\n\n");

    switch (status) {
    case AF_ERR_BOOT_MAGIC:
        af_log_raw("  The boot bridge did not leave a valid af_boot_info\n");
        af_log_raw("  structure where the kernel expected one. Check that the\n");
        af_log_raw("  UEFI application set `magic` to AF_BOOT_INFO_MAGIC and\n");
        af_log_raw("  that the pointer register was not clobbered on the jump.\n");
        break;
    case AF_ERR_BOOT_VERSION:
        af_log_raw("  The kernel and the boot bridge disagree about the\n");
        af_log_raw("  af_boot_info layout. Rebuild both from the same commit —\n");
        af_log_raw("  a stale BOOTX64.EFI on the ESP is the usual cause.\n");
        break;
    case AF_ERR_BOOT_MEMMAP:
        af_log_raw("  The memory map is unusable: empty, unaligned, or with no\n");
        af_log_raw("  AF_MEM_USABLE region at all. The firmware may have changed\n");
        af_log_raw("  the map after ExitBootServices was called.\n");
        break;
    case AF_ERR_BOOT_NOFB:
        af_log_raw("  No usable framebuffer was provided. The GOP mode or the\n");
        af_log_raw("  device tree framebuffer node is missing or malformed.\n");
        break;
    default:
        af_log_raw("  Unspecified boot failure.\n");
        break;
    }

    af_log_raw("================================================================\n");
    af_log_raw("  halted\n");
    af_log_raw("================================================================\n");

    af_interrupts_disable();
    for (;;) {
        af_halt();
    }
}

// -----------------------------------------------------------------------------
// Entry point
//
// The boot bridge calls this with interrupts disabled, on a private stack, with
// no runtime environment of any kind. It must never return.
// -----------------------------------------------------------------------------
void kmain(af_boot_info_t *boot_info)
{
    // -------------------------------------------------------------------------
    // 1. Earliest console
    //
    // The architecture layer brings up the debug UART if the boot bridge told
    // us where it is. This happens before any validation so that even a bad
    // handoff produces visible output.
    // -------------------------------------------------------------------------
    af_arch_early_console_init(boot_info);

    af_log_raw("\n");
    af_log_raw("AfriyieOS " AF_VERSION_STRING " (" AF_CODENAME ") booting\n");

    // -------------------------------------------------------------------------
    // 2. Validate the handoff
    // -------------------------------------------------------------------------
    af_status_t rc = af_boot_info_validate(boot_info);

    if (af_status_err(rc)) {
        // A NULL or unrecognised pointer may mean the register path was lost
        // during the jump. Before giving up, try the fixed fallback address
        // that the boot bridge is contractually required to populate.
        af_log_raw("boot_info invalid at the supplied pointer; trying the "
                   "backup location at 0x7000\n");

        const af_boot_info_t *backup =
            (const af_boot_info_t *)(af_uptr)AF_BOOT_INFO_BACKUP_ADDR;

        af_status_t rc2 = af_boot_info_validate(backup);
        if (af_status_ok(rc2)) {
            boot_info = (af_boot_info_t *)(af_uptr)backup;
            rc = AF_OK;
            af_log_raw("recovered boot_info from the backup location\n");
        }
    }

    if (af_status_err(rc)) {
        boot_failed(rc, "boot handoff validation");
    }

    // -------------------------------------------------------------------------
    // 3. Describe the machine
    // -------------------------------------------------------------------------
    af_boot_info_dump(boot_info);

    // -------------------------------------------------------------------------
    // 4. CPU initialisation
    //
    // GDT and IDT. Until the IDT is installed, ANY exception — including one
    // raised by the code below — triple-faults the machine with no output at
    // all. This is the earliest point at which a failure becomes debuggable.
    // -------------------------------------------------------------------------
    af_arch_cpu_dump();

    rc = hal_cpu_init();
    if (af_status_err(rc)) {
        boot_failed(rc, "CPU initialisation (GDT/IDT)");
    }

    // -------------------------------------------------------------------------
    // 4. Memory subsystem
    //
    // The PMM comes first and needs nothing but the boot handoff. It marks every
    // frame used, frees what the map says is usable, then re-reserves the kernel
    // image, the boot-info backup, its own metadata and the framebuffer.
    //
    // The kernel heap sits on top of it and is the only way anything else
    // allocates memory. Serial first, memory second: a failure in here is
    // otherwise invisible.
    // -------------------------------------------------------------------------
    rc = pmm_init(boot_info);
    if (af_status_err(rc)) {
        boot_failed(rc, "physical memory manager initialisation");
    }
    af_marker("AF_PMM_READY");
    pmm_dump_stats();

    rc = heap_init();
    if (af_status_err(rc)) {
        boot_failed(rc, "kernel heap initialisation");
    }
    af_marker("AF_HEAP_READY");

    // The process table is a fixed array, but process_create allocates an
    // address space through the PMM and the heap is used for thread stacks, so
    // this runs after both and before anything can create a process. It is not
    // a subsystem with its own marker: an empty table is not evidence of
    // anything, and the checks that matter are af_process_selftest's.
    process_init();

    // -------------------------------------------------------------------------
    // 4a. Paging
    //
    // The kernel has been executing on the page tables OVMF left behind since
    // ExitBootServices. They happen to identity-map everything we touch, which
    // is why nothing has broken — but the kernel does not own its address space
    // and cannot create a second one, so user mode is impossible until this runs.
    //
    // It needs the PMM to allocate tables, and it must run before anything
    // depends on a mapping the firmware happened to provide.
    // -------------------------------------------------------------------------
    hal_paging_init(boot_info);
    af_marker("AF_PAGING_READY");

    // -------------------------------------------------------------------------
    // 5. Framebuffer
    // -------------------------------------------------------------------------
    rc = af_fb_init(&boot_info->framebuffer);

    if (af_status_err(rc)) {
        // A missing framebuffer is not fatal in v0.1: the serial console still
        // shows everything. Warn clearly and continue.
        af_warn("fb", "framebuffer unavailable (%s) — continuing with serial only",
                af_status_name(rc));
    }

    // -------------------------------------------------------------------------
    // 6. On-screen console
    //
    // From here, the log is mirrored to the screen. Both sinks is the right
    // choice: serial for automation, screen for a human at the machine.
    // -------------------------------------------------------------------------
    if (af_fb_available()) {
        af_fb_console_init(AF_RGB(0xE8, 0xEC, 0xF2), AF_RGB(0x0E, 0x11, 0x16));
        af_fb_console_write("AfriyieOS " AF_VERSION_STRING " (" AF_CODENAME ")\n\n",
                            sizeof("AfriyieOS " AF_VERSION_STRING " (" AF_CODENAME ")\n\n") - 1);
    }

    // -------------------------------------------------------------------------
    // 7. Splash
    //
    // Drawn before the self tests so that a graphics failure is visible
    // immediately rather than being reported as a test failure.
    // -------------------------------------------------------------------------
    if (af_fb_available()) {
        af_splash_draw();
        af_marker("AF_FB_READY");
    }

    // -------------------------------------------------------------------------
    // 8. Self tests
    // -------------------------------------------------------------------------
    af_selftest_run_all();

    // -------------------------------------------------------------------------
    // 9. Ready
    //
    // This marker is the contract with CI (.github/workflows/ci.yml): if the
    // serial log contains AF_BOOT_OK and no AF_PANIC / AF_TEST_FAIL line, the
    // build boots.
    // -------------------------------------------------------------------------
    af_info("boot", "%s %s ready — kernel is alive", AF_NAME, AF_VERSION_STRING);
    af_marker(AF_BOOT_MARKER_OK);

    // -------------------------------------------------------------------------
    // 10. Multitasking
    //
    // Everything above this point ran on one context. From here the boot context
    // becomes the idle thread, the PIT drives a 100 Hz tick, and threads can be
    // created, preempted and scheduled.
    //
    // Order matters: the scheduler needs the boot context adopted as a thread
    // before it can switch away from it, and the timer IRQ must be registered
    // and unmasked only once the scheduler is ready to receive a tick.
    // -------------------------------------------------------------------------
    af_thread_t *boot_thread = thread_adopt_current("boot");
    if (boot_thread == NULL) {
        boot_failed(AF_ERR_NOMEM, "adopting the boot context as a thread");
    }

    // A SEPARATE idle thread, deliberately not the boot context. Blocking or
    // sleeping the idle thread is refused by the scheduler — it would deadlock
    // the machine — so if the boot context were also idle, every sleep in the
    // boot path would silently do nothing.
    af_thread_t *idle_thread = thread_create("idle", idle_thread_entry, NULL,
                                             AF_KERNEL_STACK_SIZE, AF_PRIO_IDLE);
    if (idle_thread == NULL) {
        boot_failed(AF_ERR_NOMEM, "creating the idle thread");
    }

    rc = sched_init(boot_thread, idle_thread);
    if (af_status_err(rc)) {
        boot_failed(rc, "scheduler initialisation");
    }

    sched_admit(idle_thread);

    // A coarse clock for log timestamps, now that something can count ticks.
    af_log_register_time_source(hal_time_ns);

    // hal_timer_init programs the PIT, registers the IRQ0 handler and unmasks
    // the line. The scheduler is already up, so the first tick finds somewhere
    // to go.
    rc = hal_timer_init(AF_SCHED_TICK_HZ);
    if (af_status_err(rc)) {
        boot_failed(rc, "timer initialisation");
    }

    af_marker("AF_SCHED_READY");

    // -------------------------------------------------------------------------
    // 11. Devices
    //
    // PCI enumeration comes first, then the storage driver claims the disk the
    // system booted from. Both are non-fatal: a machine with no PCI bus or no
    // virtio disk still boots, and says so.
    // -------------------------------------------------------------------------
    pci_init();
    pci_dump_devices();
    af_marker("AF_PCI_READY");

    rc = virtio_blk_init();
    if (af_status_err(rc)) {
        af_warn("boot", "no usable block device (%s) — continuing without "
                        "storage", af_status_name(rc));
    }

    // Interrupts have been off since the boot bridge called `cli`. The timer
    // tick, and therefore all preemption, needs them on — and so does the idle
    // `hlt` loop, which would otherwise stop the CPU permanently with no way to
    // wake it.
    af_interrupts_enable();

    // The v0.2 acceptance test: two threads printing A and B.
    af_sched_selftest();

    sched_dump_state();

    // The v0.3 acceptance test: read sector 0 from a real disk.
    virtio_blk_selftest();

    // ...then read a FILE from it: GPT -> FAT32 -> directory walk -> cluster
    // chain -> contents, checked against the exact expected string.
    fat32_selftest();

    // -------------------------------------------------------------------------
    // 12. User mode
    //
    // Two different things are tested here, and they fail independently, so both
    // are done rather than the newer one replacing the older.
    //
    //   usermode_selftest()  the ring transition itself: GDT user descriptors,
    //                        TSS RSP0, an interrupt-return to CPL 3, and the
    //                        system call gate back in. It runs a stub the kernel
    //                        copied into a page the kernel mapped.
    //
    //   elf_exec()           everything downstream of that: reading a file off
    //                        FAT32, parsing an ELF64 header, mapping each
    //                        program segment with its own permissions, zeroing
    //                        .bss, and entering ring 3 at e_entry.
    //
    // The stub runs on its own thread because af_x86_enter_user_mode never
    // returns — the ring-3 program's exit terminates whichever thread entered
    // it. Calling both in sequence from the boot thread would mean the second
    // one never ran.
    // -------------------------------------------------------------------------

    // Mount the volume init lives on.
    //
    // This deliberately duplicates the mount the FAT32 self test performs rather
    // than sharing its volume. A self test that reuses the object under test
    // proves less than one that builds it from scratch, and this mount exists
    // for a different reason: init needs somewhere to be read from.
    af_block_device_t *boot_dev = block_boot_device();
    if (boot_dev == NULL) {
        boot_failed(AF_ERR_NOENT, "no block device to load init from");
    }

    static fat32_volume_t init_volume;
    af_partition_table_t table;
    af_partition_t esp;

    rc = partition_table_read(boot_dev, &table);
    if (af_status_err(rc)) {
        boot_failed(rc, "reading the partition table to find init");
    }

    rc = partition_find_esp(&table, &esp);
    if (af_status_err(rc)) {
        boot_failed(rc, "no EFI System Partition to load init from");
    }

    rc = fat32_mount(&init_volume, boot_dev, &esp);
    if (af_status_err(rc)) {
        boot_failed(rc, "mounting the boot volume to load init");
    }

    af_thread_t *stub_thread = thread_create("usermode-stub", usermode_stub_thread,
                                             NULL, AF_KERNEL_STACK_SIZE,
                                             AF_PRIO_DEFAULT);
    if (stub_thread == NULL) {
        boot_failed(AF_ERR_NOMEM, "creating the ring-3 stub thread");
    }
    sched_admit(stub_thread);

    // Wait for the stub thread to actually exit, rather than sleeping a fixed
    // interval and hoping.
    //
    // A timed wait here is a race with a nasty shape. The first version slept
    // 20 ticks, and the stub — which enters ring 3, prints, and exits — happened
    // to still be mid-run when the boot thread woke: elf_exec then loaded init
    // into an address space the stub was still using, and the stub's exit
    // system call never arrived. The visible symptom was a missing AF_USER_OK.
    //
    // sched_collect_zombies is called here, not just yielded to, because reaping
    // is what removes the thread from the table. The idle thread normally does
    // it, but the idle thread only runs when nothing else can — and a thread
    // spinning in this loop is always runnable, so it would wait forever.
    af_tid_t stub_tid = stub_thread->tid;
    af_u64 spins = 0;
    while (thread_by_tid(stub_tid) != NULL) {
        sched_collect_zombies();
        sched_yield();

        // Bounded: a stub that never exits must fail loudly rather than hang the
        // boot with no output at all.
        if (++spins > 2000000) {
            af_panic("the ring-3 stub thread (tid %u) never exited", stub_tid);
        }
    }

    af_info("boot", "ring-3 stub thread finished; loading init");

    // Loads INIT.ELF off the volume, maps it, enters it at CPL 3. Never returns
    // to this point: init's exit system call terminates the boot thread.
    elf_exec(&init_volume, "/INIT.ELF");

    // -------------------------------------------------------------------------
    // Idle
    //
    // Only reachable if elf_exec is ever changed to return. The boot thread's
    // job is done; it stays runnable at default priority so the machine has
    // something to do, and yields rather than halting: the dedicated idle thread
    // is the one that halts the CPU.
    // -------------------------------------------------------------------------
    for (;;) {
        sched_collect_zombies();
        sched_yield();
    }
}

// -----------------------------------------------------------------------------
// The idle thread
//
// Runs only when nothing else can. Zombie thread structures are reaped HERE
// rather than in thread_exit(), because a thread cannot free the stack it is
// standing on — and this is the one context guaranteed not to belong to an
// exiting thread.
// -----------------------------------------------------------------------------
static void idle_thread_entry(void *arg)
{
    AF_UNUSED(arg);

    af_info("sched", "idle thread running (tid %u)", thread_current()->tid);

    for (;;) {
        sched_collect_zombies();
        af_halt();
    }
}

// -----------------------------------------------------------------------------
// The ring-3 stub thread
//
// usermode_selftest() ends in af_x86_enter_user_mode, which never returns: the
// stub's exit system call marks whichever thread entered ring 3 as a zombie. So
// it gets its own thread, and the boot thread keeps the machine to itself once
// the stub is gone.
// -----------------------------------------------------------------------------
static void usermode_stub_thread(void *arg)
{
    AF_UNUSED(arg);

    usermode_selftest();

    // Unreachable. Present so that a future change to usermode_selftest that
    // makes it return does not silently fall through into the idle loop with
    // the thread table in an unexplained state.
    af_panic("usermode_selftest returned");
}
