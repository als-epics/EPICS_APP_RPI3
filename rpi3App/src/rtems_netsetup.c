/*
 * rtems_netsetup.c — override of rtems_dhcpcd_start(): static IP by default,
 * or libbsd's DHCP client, selected at boot time from cmdline.txt.
 *
 * Linked into the IOC only on RTEMS targets (see src/Makefile ifeq OS_CLASS).
 * We can't simply redefine the strong libbsd symbol (the archive member is
 * still pulled in via other references → multiple definition). Instead we
 * use GNU ld's --wrap: at link time every reference to rtems_dhcpcd_start
 * is redirected to __wrap_rtems_dhcpcd_start (this function), and the real
 * libbsd implementation remains callable as __real_rtems_dhcpcd_start.
 *
 * Static mode (default): configure the first non-loopback network interface
 * we find, add a default route, wait for link UP, sync the clock from the
 * NTP server, set hostname, stash the NTP server IP into the EPICS global
 * so the one-shot NTP sync in rtems_init.c can use it, and signal the
 * dhcpDone event so the 10-minute wait in rtems_init.c exits promptly.
 *
 * DHCP mode: call the real rtems_dhcpcd_start(); Base's dhcpcd hook in
 * modules/libcom/RTEMS/posix/rtems_init.c then takes the hostname (DHCP
 * option 12) and the NTP server (option 42) from the lease and signals
 * dhcpDone when the lease is BOUND. Base's own one-shot NTP sync follows.
 *
 * Mode selection: an optional "--net=" argument in cmdline.txt on the SD
 * card (the firmware passes the line to the kernel, the BSP exposes it
 * through rpi_cmdline_get_arg(), the same mechanism as "--console=").
 * Switching needs no rebuild, only an edit of cmdline.txt:
 *
 *   --net=dhcp[,ntp=A.B.C.D][,host=NAME]
 *       DHCP; ntp= is the fallback when the server sends no option 42,
 *       host= is replaced by option 12 when the server sends one.
 *   --net=A.B.C.D/N[,gw=A.B.C.D][,ntp=A.B.C.D][,host=NAME]
 *   --net=A.B.C.D[,mask=A.B.C.D][,gw=...][,ntp=...][,host=...]
 *       static address; fields left out keep the RPI3_* defaults below.
 *   --net=static
 *       the RPI3_* defaults (same as no --net argument at all).
 *   ...,mac=A:B:C:D:E:F | mac=random
 *       MAC address of the interface (any mode). Default: the board MAC
 *       the firmware uses (b8:27:eb + the low three bytes of the board
 *       serial), see "MAC address" below. mac=random keeps libbsd's
 *       random address.
 *
 * Example: --console=/dev/ttyS0 --net=dhcp,ntp=172.233.177.198
 *
 * MAC address: the LAN7515 on the Pi has no EEPROM. The boot ROM and
 * start.elf program it with b8:27:eb:xx:yy:zz derived from the board
 * serial (also used by the ROM's network boot DHCP), but libbsd's muge
 * driver finds no address in the chip and makes up a random locally
 * administered one at every boot. Before configuring the interface we
 * therefore set the firmware MAC ourselves: from the device tree property
 * local-mac-address of the node behind the alias "ethernet0" (the firmware
 * fills it at boot), or, if that is missing, computed from the serial via
 * the mailbox. The DHCP server then sees one MAC for network boot and for
 * the IOC, and a MAC reservation works (patches/dnsmasq-shared-rpi-ioc.conf).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_media.h>

#include <rtems.h>
#include <rtems/dhcpcd.h>
#include <bsp.h>                 /* rpi_cmdline_get_arg() */
#include <bsp/vc.h>              /* bcm2835_mailbox_get_board_serial() */
#include <bsp/fdt.h>             /* bsp_fdt_get() */
#include <libfdt.h>
#include <machine/rtems-bsd-commands.h>

/*
 * Pull libbsd nexus-device references into this translation unit so the
 * linker retains dwcotg / uhub / muge / smsc / ukphy from libbsd.a.
 * Must be included exactly once in the application.
 */
#include <bsp/nexus-devices.h>

#include "epicsEvent.h"

/* --- User-editable static network configuration ---------------------- */
#define RPI3_IP_ADDRESS   "192.168.2.21" //"131.243.92.221"
//#define RPI3_IP_ADDRESS   "131.243.92.221"
#define RPI3_NETMASK      "255.255.255.0"
/* 2026-10-14: the gateway is the laptop (192.168.2.10) which shares its
 * WiFi internet connection (NetworkManager "shared" mode) so the IOC can
 * reach the NTP server.  There is no router at 192.168.2.1. */
#define RPI3_GATEWAY      "192.168.2.10" //"192.168.2.1" //"131.243.92.1"
//#define RPI3_GATEWAY      "131.243.92.1"
#define RPI3_NTP_SERVER   "172.233.177.198" 
#define RPI3_HOSTNAME     "rtems-pi3"
/* -------------------------------------------------------------------- */

/* Globals defined (non-static) in modules/libcom/RTEMS/posix/rtems_init.c */
extern epicsEventId dhcpDone;
extern char rtemsInit_NTP_server_ip[16];

/* Seconds between NTP epoch (1900-01-01) and UNIX epoch (1970-01-01) */
#define NTP_TIMESTAMP_DELTA 2208988800UL

/*
 * Minimal NTP client: query the server, set CLOCK_REALTIME on success.
 * Called BEFORE dhcpDone so the clock is correct before any EPICS init.
 * This prevents the 12-year clock_settime() jump that destabilises RTEMS
 * POSIX timers when osdNTPGet() runs later during NTPTimeSyncNow.
 */
static int
early_ntp_sync(const char *server_ip, int max_attempts, int timeout_sec)
{
    struct sockaddr_in serv_addr;
    unsigned char pkt[48];
    int attempt;
    rtems_interval tps = rtems_clock_get_ticks_per_second();

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(server_ip);
    serv_addr.sin_port        = htons(123);

    for (attempt = 0; attempt < max_attempts; ++attempt) {
        if (attempt > 0)
            rtems_task_wake_after(tps);      /* 1 s between retries */

        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) continue;

        struct timeval tv = { timeout_sec, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&serv_addr,
                    sizeof(serv_addr)) < 0) {
            close(fd); continue;
        }

        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x1b;                      /* LI=0, VN=3, Mode=3 (client) */

        if (write(fd, pkt, sizeof(pkt)) < 0) {
            close(fd); continue;
        }

        int n = read(fd, pkt, sizeof(pkt));
        close(fd);

        if (n >= (int)sizeof(pkt)) {
            uint32_t txSec = ((uint32_t)pkt[40] << 24) |
                             ((uint32_t)pkt[41] << 16) |
                             ((uint32_t)pkt[42] <<  8) |
                             ((uint32_t)pkt[43]);
            struct timespec ts;
            ts.tv_sec  = (time_t)(txSec - NTP_TIMESTAMP_DELTA);
            ts.tv_nsec = 0;
            clock_settime(CLOCK_REALTIME, &ts);
            return 0;
        }
        printf(".");
        fflush(stdout);
    }
    return -1;
}

/* The real libbsd implementation, reachable through --wrap. */
extern rtems_status_code __real_rtems_dhcpcd_start(const rtems_dhcpcd_config *config);

/* Network configuration for this boot: the RPI3_* defaults, possibly
 * changed by the "--net=" argument in cmdline.txt (see the file header). */
struct net_cfg {
    int  dhcp;                 /* 1: libbsd dhcpcd, 0: static address */
    char ip[16];
    char mask[16];
    char gw[16];
    char ntp[16];              /* static: NTP server; dhcp: fallback for option 42 */
    char host[32];
    int  mac_random;           /* 1: keep libbsd's random MAC (mac=random) */
    uint8_t mac[6];            /* mac=...: forced MAC, valid when mac_set */
    int  mac_set;
};

static void
copy_bounded(char *dst, size_t dstsz, const char *src, size_t len)
{
    if (len >= dstsz)
        len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int
valid_ipv4(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) != 0;
}

static void
prefix_to_mask(int prefix, char *out, size_t outsz)
{
    uint32_t m;
    if (prefix <= 0)
        m = 0;
    else if (prefix >= 32)
        m = 0xffffffffu;
    else
        m = ~((1u << (32 - prefix)) - 1u);
    snprintf(out, outsz, "%u.%u.%u.%u",
             (unsigned)(m >> 24), (unsigned)((m >> 16) & 0xff),
             (unsigned)((m >> 8) & 0xff), (unsigned)(m & 0xff));
}

/* Fill *cfg from the defaults and the "--net=" argument of cmdline.txt.
 * Copies the raw argument (without "--net=") to *shown for the boot log,
 * empty when the argument is absent. Bad values are reported and ignored. */
static void
net_cfg_from_cmdline(struct net_cfg *cfg, char *shown, size_t shownsz)
{
    char buf[128];
    char *tok, *save;
    const char *arg;
    size_t n;

    memset(cfg, 0, sizeof(*cfg));
    copy_bounded(cfg->ip,   sizeof cfg->ip,   RPI3_IP_ADDRESS, strlen(RPI3_IP_ADDRESS));
    copy_bounded(cfg->mask, sizeof cfg->mask, RPI3_NETMASK,    strlen(RPI3_NETMASK));
    copy_bounded(cfg->gw,   sizeof cfg->gw,   RPI3_GATEWAY,    strlen(RPI3_GATEWAY));
    copy_bounded(cfg->ntp,  sizeof cfg->ntp,  RPI3_NTP_SERVER, strlen(RPI3_NTP_SERVER));
    copy_bounded(cfg->host, sizeof cfg->host, RPI3_HOSTNAME,   strlen(RPI3_HOSTNAME));
    shown[0] = '\0';

    arg = rpi_cmdline_get_arg("--net=");
    if (arg == NULL)
        return;
    n = strcspn(arg, " \t\r\n");          /* the argument ends at white space */
    copy_bounded(buf, sizeof buf, arg, n);
    copy_bounded(shown, shownsz, buf, strlen(buf));

    tok = strtok_r(buf, ",", &save);
    if (tok == NULL || *tok == '\0') {
        printf("cmdline.txt: empty --net= argument, using the compiled in static address\n");
        return;
    }
    if (strcmp(tok, "dhcp") == 0) {
        cfg->dhcp = 1;
    } else if (strcmp(tok, "static") == 0) {
        /* defaults */
    } else {
        char *slash = strchr(tok, '/');
        if (slash != NULL) {
            *slash = '\0';
            prefix_to_mask(atoi(slash + 1), cfg->mask, sizeof cfg->mask);
        }
        if (valid_ipv4(tok))
            copy_bounded(cfg->ip, sizeof cfg->ip, tok, strlen(tok));
        else
            printf("cmdline.txt: --net=%s is not an IPv4 address, keeping %s\n",
                   tok, cfg->ip);
    }

    while ((tok = strtok_r(NULL, ",", &save)) != NULL) {
        char *eq = strchr(tok, '=');
        const char *val;
        if (eq == NULL) {
            printf("cmdline.txt: --net: ignoring '%s' (expected key=value)\n", tok);
            continue;
        }
        *eq = '\0';
        val = eq + 1;
        if (strcmp(tok, "host") == 0) {
            copy_bounded(cfg->host, sizeof cfg->host, val, strlen(val));
            continue;
        }
        if (strcmp(tok, "mac") == 0) {
            unsigned m[6];
            char tail;
            if (strcmp(val, "random") == 0) {
                cfg->mac_random = 1;
            } else if (sscanf(val, "%x:%x:%x:%x:%x:%x%c", &m[0], &m[1],
                              &m[2], &m[3], &m[4], &m[5], &tail) == 6 &&
                       (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) <= 0xff) {
                int i;
                for (i = 0; i < 6; ++i)
                    cfg->mac[i] = (uint8_t)m[i];
                cfg->mac_set = 1;
            } else {
                printf("cmdline.txt: --net: mac=%s is neither a MAC address "
                       "nor 'random', ignored\n", val);
            }
            continue;
        }
        if (!valid_ipv4(val)) {
            printf("cmdline.txt: --net: %s=%s is not an IPv4 address, ignored\n", tok, val);
            continue;
        }
        if (strcmp(tok, "gw") == 0)
            copy_bounded(cfg->gw, sizeof cfg->gw, val, strlen(val));
        else if (strcmp(tok, "mask") == 0)
            copy_bounded(cfg->mask, sizeof cfg->mask, val, strlen(val));
        else if (strcmp(tok, "ntp") == 0)
            copy_bounded(cfg->ntp, sizeof cfg->ntp, val, strlen(val));
        else
            printf("cmdline.txt: --net: unknown key '%s', ignored\n", tok);
    }
}

/* --- Board MAC address -------------------------------------------------- */

/* The firmware fills "local-mac-address" of the node behind the alias
 * "ethernet0" (the LAN7515) in the device tree it hands to the kernel. */
static int
board_mac_from_fdt(uint8_t mac[6])
{
    const void *fdt = bsp_fdt_get();
    const char *path;
    const uint8_t *prop;
    int node, len, i, nonzero = 0;

    if (fdt == NULL || fdt_check_header(fdt) != 0)
        return -1;
    path = fdt_get_alias(fdt, "ethernet0");
    if (path == NULL)
        return -1;
    node = fdt_path_offset(fdt, path);
    if (node < 0)
        return -1;
    prop = fdt_getprop(fdt, node, "local-mac-address", &len);
    if (prop == NULL || len != 6)
        return -1;
    for (i = 0; i < 6; ++i)
        nonzero |= prop[i];
    if (!nonzero)
        return -1;
    memcpy(mac, prop, 6);
    return 0;
}

/* Board serial number. First choice: the "serial-number" string the
 * firmware puts into the root node of the device tree. Fallback: the
 * mailbox; but bcm2835_mailbox_get_board_serial() in the RTEMS 6 BSP
 * (bsps/arm/raspberrypi/start/vc.c) flushes the cache for
 * sizeof(&buffer) = 4 bytes instead of sizeof(buffer), so the answer
 * usually reads as 0 (seen 2026-10-16). Returns 0 when nothing worked. */
static uint64_t
board_serial(void)
{
    const void *fdt = bsp_fdt_get();
    bcm2835_get_board_serial_entries e;

    if (fdt != NULL && fdt_check_header(fdt) == 0) {
        int len;
        const char *p = fdt_getprop(fdt, 0, "serial-number", &len);
        if (p != NULL && len > 1) {
            uint64_t v = strtoull(p, NULL, 16);
            if (v != 0)
                return v;
        }
    }
    memset(&e, 0, sizeof e);
    if (bcm2835_mailbox_get_board_serial(&e) == 0)
        return e.board_serial;
    return 0;
}

/* The Raspberry Pi firmware MAC: the fixed OUI b8:27:eb followed by the low
 * 24 bits of the board serial number. */
static int
board_mac_from_serial(uint8_t mac[6], uint64_t *serial)
{
    bcm2835_get_board_serial_entries e;

    e.board_serial = board_serial();
    if (e.board_serial == 0)
        return -1;
    *serial = e.board_serial;
    mac[0] = 0xb8;
    mac[1] = 0x27;
    mac[2] = 0xeb;
    mac[3] = (uint8_t)(e.board_serial >> 16);
    mac[4] = (uint8_t)(e.board_serial >> 8);
    mac[5] = (uint8_t)(e.board_serial);
    return 0;
}

/* Give the interface the firmware's MAC (or the one from cmdline.txt)
 * before it is configured. libbsd's muge driver writes the interface link
 * address into the chip when the interface comes up (muge_init), so the
 * hardware filter follows. Must run while the interface is still down. */
static void
set_interface_mac(const char *nic, const struct net_cfg *cfg)
{
    uint8_t mac[6];
    uint64_t serial = 0;
    const char *source;
    char macstr[18];
    int rc;

    if (cfg->mac_random) {
        printf("MAC: keeping the random address chosen by libbsd (mac=random)\n");
        return;
    }
    if (cfg->mac_set) {
        memcpy(mac, cfg->mac, 6);
        source = "cmdline.txt";
    } else if (board_mac_from_fdt(mac) == 0) {
        source = "device tree local-mac-address";
    } else if (board_mac_from_serial(mac, &serial) == 0) {
        source = "board serial via mailbox";
    } else {
        printf("MAC: no device tree address and no board serial; keeping "
               "the random address chosen by libbsd\n");
        return;
    }
    if (serial == 0)
        serial = board_serial();
    snprintf(macstr, sizeof macstr, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    {
        char *argv[] = { "ifconfig", (char *)nic, "ether", macstr, NULL };
        rc = rtems_bsd_command_ifconfig(4, argv);
    }
    printf("Board serial %08lx%08lx, MAC %s (%s) -> ifconfig %s ether: %d\n",
           (unsigned long)(serial >> 32), (unsigned long)(serial & 0xffffffffu),
           macstr, source, nic, rc);
}

/* EPICS Base's rtems_init.c (default_network_dhcpcd) means to write
 * /etc/dhcpcd.conf, but its test `ENOENT == stat(...)` can never be true
 * (stat() returns -1, not ENOENT), so the file is never written, and the
 * option names it would write ("ntp-servers") are not the ones dhcpcd 6
 * knows ("ntp_servers"). dhcpcd therefore runs with its defaults and asks
 * for neither the NTP server (option 42) nor the hostname (option 12);
 * seen in the 2026-10-16 capture (dnsmasq: requested options 1,33,3,28,
 * 51,58,59; no client id; IPv6 router solicitations). Write the file
 * ourselves before dhcpcd starts. No "clientid" line: the lease is then
 * keyed on the MAC set above, which is what the server's reservation uses.
 * Base's hook picks up new_ntp_servers and new_host_name from the lease. */
static void
write_dhcpcd_conf(void)
{
    static const char conf[] =
        "# written by the IOC (rtems_netsetup.c) before dhcpcd starts\n"
        "nodhcp6\n"
        "ipv4only\n"
        "noipv4ll\n"                /* no 169.254.x.y detour while the server is away */
        "option ntp_servers\n"
        "option host_name\n";
    FILE *f;

    mkdir("/etc", 0777);                     /* normally exists already */
    f = fopen("/etc/dhcpcd.conf", "w");
    if (f == NULL) {
        printf("dhcpcd: cannot write /etc/dhcpcd.conf, dhcpcd runs with "
               "its defaults (no NTP server, no hostname from the lease)\n");
        return;
    }
    fputs(conf, f);
    fclose(f);
    printf("/etc/dhcpcd.conf written: IPv4 only, no IPv4LL, requesting "
           "ntp_servers and host_name\n");
}

/* Second dhcpcd hook, next to the one in EPICS Base's rtems_init.c.
 * Base's hook wakes EPICS (dhcpDone) only when dhcpcd reports the reason
 * "BOUND". A lease that replaces an earlier address arrives as "REBIND"
 * (seen 2026-10-17: the DHCP server came up a minute after the IOC,
 * dhcpcd had taken an IPv4LL address meanwhile, the real lease was
 * reported as REBIND, Base ignored it and EPICS sat in its 10-minute
 * timeout, then started with the fallback NTP server and hostname).
 * "RENEW" and "REBOOT" carry an address as well. This hook accepts all of
 * them, takes the NTP server and hostname from the lease the way Base's
 * hook does for BOUND, and signals dhcpDone once. It leaves BOUND to
 * Base's hook so the event is never signalled twice, and never signals
 * after the first time because Base destroys the event once its wait has
 * returned. */
static void
netsetup_dhcpcd_hook(rtems_dhcpcd_hook *hook, char *const *env)
{
    static int done;
    const char *reason = NULL, *ntp = NULL, *host = NULL;
    char *const *e;

    (void)hook;
    for (e = env; *e != NULL; ++e) {
        if (strncmp(*e, "reason=", 7) == 0)
            reason = *e + 7;
        else if (strncmp(*e, "new_ntp_servers=", 16) == 0)
            ntp = *e + 16;
        else if (strncmp(*e, "new_host_name=", 14) == 0)
            host = *e + 14;
    }
    if (reason == NULL || done)
        return;
    if (strcmp(reason, "BOUND") == 0)
        return;                                  /* Base's hook handles it */
    if (strcmp(reason, "RENEW") != 0 && strcmp(reason, "REBIND") != 0 &&
        strcmp(reason, "REBOOT") != 0)
        return;

    if (ntp != NULL && *ntp != '\0') {
        size_t n = strcspn(ntp, " ");            /* first server of the list */
        copy_bounded(rtemsInit_NTP_server_ip, sizeof(rtemsInit_NTP_server_ip),
                     ntp, n);
    }
    if (host != NULL && *host != '\0')
        sethostname(host, (int)strlen(host));
    printf("netsetup: lease reported as %s: NTP server %s, hostname %s; "
           "EPICS continues\n", reason,
           rtemsInit_NTP_server_ip[0] ? rtemsInit_NTP_server_ip : "(none)",
           host != NULL && *host ? host : "(unchanged)");
    done = 1;
    if (dhcpDone)
        epicsEventSignal(dhcpDone);
}

static rtems_dhcpcd_hook netsetup_hook = {
    .name = "netsetup",
    .handler = netsetup_dhcpcd_hook
};

static const char *
find_ethernet_iface(char *buf, size_t bufsz)
{
    struct if_nameindex *ifs = if_nameindex();
    if (!ifs) return NULL;
    buf[0] = '\0';
    for (struct if_nameindex *p = ifs; p->if_index != 0; ++p) {
        if (strcmp(p->if_name, "lo0") == 0) continue;
        strncpy(buf, p->if_name, bufsz - 1);
        buf[bufsz - 1] = '\0';
        break;
    }
    if_freenameindex(ifs);
    return buf[0] ? buf : NULL;
}

/* Define to 1 to skip all network configuration for hang debugging.
 * If the system runs indefinitely with this set, the DWC OTG USB
 * controller / muge Ethernet driver is causing the hang. */
#define SKIP_NETWORK_FOR_DEBUG  0

extern void heartbeat_start(void);

/* Override of libbsd's rtems_dhcpcd_start via linker --wrap. */
rtems_status_code
__wrap_rtems_dhcpcd_start(const rtems_dhcpcd_config *config)
{
    struct net_cfg cfg;
    char shown[128];
    char iface[16];
    int rc;
    int s;

    /* Arm the hardware watchdog keeper (heartbeat.c). */
    heartbeat_start();

#if SKIP_NETWORK_FOR_DEBUG
    (void)config;
    printf("\n***** NETWORK DISABLED FOR DEBUGGING *****\n");
    if (dhcpDone)
        epicsEventSignal(dhcpDone);
    return RTEMS_SUCCESSFUL;
#endif

    net_cfg_from_cmdline(&cfg, shown, sizeof shown);
    if (shown[0] != '\0')
        printf("\n***** cmdline.txt: --net=%s *****\n", shown);
    if (cfg.dhcp)
        printf("\n***** DHCP via libbsd dhcpcd (address, route, NTP server and "
               "hostname from the lease) *****\n");
    else
        printf("\n***** Static IP override: %s/%s gw %s *****\n",
               cfg.ip, cfg.mask, cfg.gw);

    /* USB enumeration of the LAN7515 (muge) runs asynchronously; wait
     * up to 30 s for a non-loopback interface to appear before giving up. */
    const char *nic = NULL;
    const rtems_interval tps = rtems_clock_get_ticks_per_second();
    for (s = 0; s < 30; ++s) {
        nic = find_ethernet_iface(iface, sizeof iface);
        if (nic) break;
        if (s == 0) printf("Waiting for USB Ethernet to enumerate");
        printf("."); fflush(stdout);
        rtems_task_wake_after(tps);
    }
    if (s > 0) printf("\n");

    if (nic)
        set_interface_mac(nic, &cfg);

    if (cfg.dhcp) {
        rtems_status_code sc;

        if (!nic)
            printf("No non-loopback interface found after 30 s; dhcpcd will "
                   "pick it up when it appears\n");
        else
            printf("Using interface: %s\n", nic);

        /* Fallbacks for a server that sends no option 12 / option 42;
         * Base's hook overwrites both from the lease when present. */
        sethostname(cfg.host, (int)strlen(cfg.host));
        copy_bounded(rtemsInit_NTP_server_ip, sizeof(rtemsInit_NTP_server_ip),
                     cfg.ntp, strlen(cfg.ntp));
        if (cfg.ntp[0] != '\0')
            printf("NTP server if the lease has no option 42: %s\n", cfg.ntp);

        write_dhcpcd_conf();
        rtems_dhcpcd_add_hook(&netsetup_hook);
        sc = __real_rtems_dhcpcd_start(config);
        printf("dhcpcd started -> %d; EPICS continues when the lease is BOUND "
               "(10 min limit)\n", (int)sc);
        /* dhcpDone is signalled by Base's dhcpcd hook on BOUND. */
        return sc;
    }

    if (!nic) {
        printf("No non-loopback interface found after 30 s — network not configured\n");
    } else {
        printf("Using interface: %s\n", nic);
        char *ifcfg_argv[] = {
            "ifconfig", (char*)nic,
            "inet", cfg.ip,
            "netmask", cfg.mask,
            NULL
        };
        rc = rtems_bsd_command_ifconfig(6, ifcfg_argv);
        printf("ifconfig %s inet %s netmask %s -> %d\n",
               nic, cfg.ip, cfg.mask, rc);

        char *route_argv[] = {
            "route", "add", "default", cfg.gw, NULL
        };
        rc = rtems_bsd_command_route(4, route_argv);
        printf("route add default %s -> %d\n", cfg.gw, rc);
        /* Wait for the physical link to come UP before proceeding.
         * The LAN7515 PHY negotiation typically takes 3-5 s after
         * ifconfig.  Without this, the one-shot NTP sync in
         * rtems_init.c fails ("Network is unreachable") and the clock
         * stays at 2014.  When the NTPTimeSync thread finally syncs
         * 60 s later, the resulting 12-year clock_settime() jump
         * destabilises RTEMS timers and causes the IOC to hang. */
        {
            struct ifmediareq ifmr;
            int link_up = 0;
            int w;
            printf("Waiting for %s link UP", nic);
            fflush(stdout);
            for (w = 0; w < 15; ++w) {
                memset(&ifmr, 0, sizeof(ifmr));
                copy_bounded(ifmr.ifm_name, sizeof(ifmr.ifm_name), nic, strlen(nic));
                int sd = socket(AF_INET, SOCK_DGRAM, 0);
                if (sd >= 0) {
                    if (ioctl(sd, SIOCGIFMEDIA, &ifmr) == 0 &&
                        (ifmr.ifm_status & IFM_ACTIVE)) {
                        link_up = 1;
                    }
                    close(sd);
                }
                if (link_up) break;
                printf(".");
                fflush(stdout);
                rtems_task_wake_after(tps);
            }
            if (link_up)
                printf(" UP\n");
            else
                printf(" timeout (continuing anyway)\n");
        }

        /* Sync the clock NOW, before dhcpDone wakes EPICS.
         * 5 attempts with 3-second timeout each — allows time for ARP
         * resolution on the first packet.  Without this, the clock stays
         * at 2014 and osdNTPGet() later does a 12-year clock_settime()
         * jump that hangs RTEMS. */
        printf("Early NTP sync with %s", cfg.ntp);
        fflush(stdout);
        if (early_ntp_sync(cfg.ntp, 5, 3) == 0)
            printf(" OK\n");
        else
            printf(" FAILED (will retry later)\n");
    }

    sethostname(cfg.host, (int)strlen(cfg.host));
    copy_bounded(rtemsInit_NTP_server_ip, sizeof(rtemsInit_NTP_server_ip),
                 cfg.ntp, strlen(cfg.ntp));

    /* Wake EPICS's wait-for-network loop in posix/rtems_init.c. */
    if (dhcpDone)
        epicsEventSignal(dhcpDone);

    return RTEMS_SUCCESSFUL;
}
