/*************************************************************************\
* Copyright (c) 2002 The University of Saskatchewan
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/
/*
 * RTEMS configuration for EPICS — APP-LEVEL OVERRIDE.
 *
 * Verbatim copy of base-7.0.10/modules/libcom/RTEMS/posix/rtems_config.c
 * with CONFIGURE_MICROSECONDS_PER_TICK pinned to 500 us (= 2 kHz tick).
 * Placed in the app so it shadows the one in librtemsCom.a at link time:
 * our object is resolved before the archive, so the archive member is
 * never pulled in and there's no multiple-definition conflict.
 *
 * Why OP_SYS_CFLAGS += -DCONFIGURE_MICROSECONDS_PER_TICK=500 in
 * configure/CONFIG_SITE.local was NOT enough: that CFLAG only affects
 * app-side compilation; base's rtems_config.o was already compiled with
 * the 10000 us default when base was installed. Without this override
 * epicsThreadSleepQuantum() = 1/100 s = 10 ms, and initPeriodic then
 * rejects the ".01 second" scan rate.
 */

/* 500 us = 2 kHz tick for 0.01 s EPICS scan support on RPi 3B+. */
#define CONFIGURE_MICROSECONDS_PER_TICK 500

#include <rtems.h>

/*
 ***********************************************************************
 *                         RTEMS CONFIGURATION                         *
 ***********************************************************************
 */

extern void *POSIX_Init(void *argument);

#define CONFIGURE_POSIX_INIT_THREAD_TABLE
#define CONFIGURE_POSIX_INIT_THREAD_ENTRY_POINT POSIX_Init
#define CONFIGURE_POSIX_INIT_THREAD_STACK_SIZE  (64*1024)

#define CONFIGURE_MAXIMUM_PERIODS 	5
/*
 * Override in configure/CONFIG_SITE.local with:
 *
 * OP_SYS_CFLAGS += -DCONFIGURE_MICROSECONDS_PER_TICK=1000
 */
#ifndef CONFIGURE_MICROSECONDS_PER_TICK
#define CONFIGURE_MICROSECONDS_PER_TICK 10000
#endif
#define CONFIGURE_MALLOC_STATISTICS     1
/* MINIMUM_STACK_SIZE == 8K */
#define CONFIGURE_EXTRA_TASK_STACKS         (4000 * RTEMS_MINIMUM_STACK_SIZE)

/*
 * ARM start.S carves FIQ/ABT/UND stacks from the TOP of the ISR stack area.
 * Default ISR stack = CPU_STACK_MINIMUM_SIZE = 4096.  With bsp_stack_fiq_size
 * = 4096 and bsp_stack_abt_size = 1024, the carved total (5120) exceeds the
 * default, pushing the SVC boot stack into BSS → crash during bss_clear.
 * Size the ISR area to hold all exception stacks plus usable IRQ space.
 */
#define CONFIGURE_INTERRUPT_STACK_SIZE  16384

#if __RTEMS_MAJOR__ > 4
#define CONFIGURE_FILESYSTEM_DEVFS
#define CONFIGURE_FILESYSTEM_TFTPFS
#endif
#define CONFIGURE_FILESYSTEM_NFS
#define CONFIGURE_FILESYSTEM_IMFS

#ifndef RTEMS_LEGACY_STACK
#define CONFIGURE_USE_IMFS_AS_BASE_FILESYSTEM
/*
 * Configure LibBSD.
 */
//#define RTEMS_BSD_CONFIG_NET_PF_UNIX
//#define RTEMS_BSD_CONFIG_NET_IF_BRIDGE
//#define RTEMS_BSD_CONFIG_NET_IF_LAGG
//#define RTEMS_BSD_CONFIG_NET_IF_VLAN
#define RTEMS_BSD_CONFIG_BSP_CONFIG
#define RTEMS_BSD_CONFIG_INIT
#include <machine/rtems-bsd-config.h>
#endif // not LEGACY_STACK

/*
 * Configure RTEMS.
 */
#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_STUB_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_ZERO_DRIVER

/* Note: The select() system call can only be used with the first FD_SETSIZE
 *       File Descriptors (newlib default is 64).  Beginning RTEMS 5.1, FDs are
 *       allocated sequentially.  So changing this CONFIGURE parameter such
 *       that CONFIGURE_MAXIMUM_FILE_DESCRIPTORS >= FD_SETSIZE will likely
 *       cause applications making select() calls to fault at some point.
 *
 *       IOC core components (libca and RSRV) do not make select() calls.
 *
 *       Applications and driver code using poll() or other socket
 *       multiplexers do not share this limitation.
 *
 *       cf. https://github.com/epics-base/epics-base/issues/300
 */
#define CONFIGURE_MAXIMUM_FILE_DESCRIPTORS 64
#define CONFIGURE_IMFS_ENABLE_MKFIFO    2

#define CONFIGURE_MAXIMUM_NFS_MOUNTS 		3
#define CONFIGURE_MAXIMUM_USER_EXTENSIONS 	5

#define CONFIGURE_UNLIMITED_ALLOCATION_SIZE 32
#define CONFIGURE_UNLIMITED_OBJECTS
#define CONFIGURE_UNIFIED_WORK_AREAS

#define CONFIGURE_STACK_CHECKER_ENABLED

#define CONFIGURE_APPLICATION_NEEDS_LIBBLOCK
#define CONFIGURE_BDBUF_BUFFER_MAX_SIZE (64 * 1024)
#define CONFIGURE_BDBUF_MAX_READ_AHEAD_BLOCKS 4
#define CONFIGURE_BDBUF_CACHE_MEMORY_SIZE (1 * 1024 * 1024)

/* we are using POSIX_INIT needed by V4
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE (32 * 1024)
#define CONFIGURE_INIT_TASK_INITIAL_MODES RTEMS_DEFAULT_MODES
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT
#endif
*/

//#define RTEMS_PCI_CONFIG_LIB
//#define CONFIGURE_PCI_LIB PCI_LIB_AUTO

#if __RTEMS_MAJOR__ > 4
#define CONFIGURE_SHELL_COMMANDS_INIT

#include <bsp/irq-info.h>

#ifndef RTEMS_LEGACY_STACK
#include <rtems/netcmds-config.h>

#define CONFIGURE_SHELL_USER_COMMANDS \
  &bsp_interrupt_shell_command, \
  &rtems_shell_HOSTNAME_Command, \
  &rtems_shell_PING_Command, \
  &rtems_shell_ROUTE_Command, \
  &rtems_shell_NETSTAT_Command, \
  &rtems_shell_IFCONFIG_Command, \
  &rtems_shell_TCPDUMP_Command, \
  &rtems_shell_PFCTL_Command, \
  &rtems_shell_SYSCTL_Command
#else // LEGACY_STACK:
#define CONFIGURE_SHELL_USER_COMMANDS \
  &bsp_interrupt_shell_command
#endif // not LEGACY_STACK

#define CONFIGURE_SHELL_COMMAND_CPUUSE
#define CONFIGURE_SHELL_COMMAND_PERIODUSE
#define CONFIGURE_SHELL_COMMAND_STACKUSE
#define CONFIGURE_SHELL_COMMAND_PROFREPORT
#define CONFIGURE_SHELL_COMMAND_TOP
#define CONFIGURE_SHELL_COMMAND_RTEMS

#define CONFIGURE_SHELL_COMMANDS_ALL_NETWORKING

#define CONFIGURE_SHELL_COMMAND_CP
#define CONFIGURE_SHELL_COMMAND_PWD
#define CONFIGURE_SHELL_COMMAND_LS
#define CONFIGURE_SHELL_COMMAND_LN
#define CONFIGURE_SHELL_COMMAND_LSOF
#define CONFIGURE_SHELL_COMMAND_CHDIR
#define CONFIGURE_SHELL_COMMAND_CD
#define CONFIGURE_SHELL_COMMAND_MKDIR
#define CONFIGURE_SHELL_COMMAND_RMDIR
#define CONFIGURE_SHELL_COMMAND_CAT
#define CONFIGURE_SHELL_COMMAND_MV
#define CONFIGURE_SHELL_COMMAND_RM
#define CONFIGURE_SHELL_COMMAND_MALLOC_INFO
#define CONFIGURE_SHELL_COMMAND_SHUTDOWN
/* memory inspection from iocsh: rt mdump 0x30000000 0x100 (bytes), wdump,
 * ldump (16/32-bit), medit addr v1 v2..., mfill addr len value, mmove */
#define CONFIGURE_SHELL_COMMAND_MDUMP
#define CONFIGURE_SHELL_COMMAND_WDUMP
#define CONFIGURE_SHELL_COMMAND_LDUMP
#define CONFIGURE_SHELL_COMMAND_MEDIT
#define CONFIGURE_SHELL_COMMAND_MFILL
#define CONFIGURE_SHELL_COMMAND_MMOVE

#include <rtems/shellconfig.h>

#define RTEMS_BSD_CONFIG_BSP_CONFIG
#define RTEMS_BSD_CONFIG_SERVICE_TELNETD
#define RTEMS_BSD_CONFIG_TELNETD_STACK_SIZE (16 * 1024)
#define RTEMS_BSD_CONFIG_SERVICE_FTPD
#define RTEMS_BSD_CONFIG_FIREWALL_PF
#else // __RTEMS_MAJOR__ > 4
#include <rtems/shellconfig.h>
#endif // __RTEMS_MAJOR__ > 4

#if __RTEMS_MAJOR__ < 5 // still needed in Version 4?
#define CONFIGURE_MAXIMUM_TASKS             rtems_resource_unlimited(30)
#endif

#define CONFIGURE_MAXIMUM_DRIVERS 40

/*
 * This should be made BSP dependent, not CPU dependent but I know of no
 * appropriate conditionals to use.
 * The new general time support makes including the RTC driver less important.
 */
#if !defined(mpc604) && !defined(__mc68040__) && !defined(__mcf5200__) && \
    !defined(mpc7455) && !defined(__arm__)  && !defined(__nios2__)
    /* don't have RTC code */
#define CONFIGURE_APPLICATION_NEEDS_RTC_DRIVER
#endif

#if defined(BSP_pc386) || defined(BSP_pc686)
#define RTEMS_BSD_CONFIG_DOMAIN_PAGE_MBUFS_SIZE (64 * 1024 * 1024)
#elif defined(BSP_qoriq_e500)
#define RTEMS_BSD_CONFIG_DOMAIN_PAGE_MBUFS_SIZE (32 * 1024 * 1024)
#endif

#define CONFIGURE_INIT

#include <rtems/confdefs.h>
