/* SPDX-License-Identifier: GPL-2.0-only
 * herolte stage-0 probe: print the S-Boot handoff on the firmware
 * framebuffer, then return so probe.S can request TWRP.
 */
#include <stdint.h>
#include "../fdt.h"
#include "fbtext.h"

#define RECOVERY_DELAY_S 5U
#define PROBE_LOG_BASE   0x8ffff000UL

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r" (value));
    return value;
}

void hero_probe_main(uintptr_t fdt, uint64_t el, uint64_t sctlr, uint64_t image)
{
    uint64_t midr, mpidr, mmfr0, pfr0, isar0, freq, start = 0, end = 0;
    uint32_t trig_before = hero_fb_enable();

    __asm__ volatile("mrs %0, midr_el1" : "=r" (midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r" (mmfr0));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r" (pfr0));
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r" (isar0));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));

    unsigned row = 0;
    hero_fb_text(row++, "HEROLTE XNU PROBE 1");
    hero_fb_hex(row++, "EL", el >> 2);
    hero_fb_hex(row++, "SCTLR", sctlr);
    hero_fb_hex(row++, "FDT", fdt);
    hero_fb_hex(row++, "IMAGE", image);
    hero_fb_hex(row++, "MIDR", midr);
    hero_fb_hex(row++, "MPIDR", mpidr);
    hero_fb_hex(row++, "MMFR0", mmfr0);
    hero_fb_hex(row++, "PFR0", pfr0);
    hero_fb_hex(row++, "ISAR0", isar0);
    hero_fb_hex(row++, "CNTFRQ", freq);
    hero_fb_hex(row++, "TRIG", trig_before);
    if (fdt && !(fdt & 7) && fdt_chosen_u64(fdt, "linux,initrd-start", &start) &&
        fdt_chosen_u64(fdt, "linux,initrd-end", &end)) {
        hero_fb_hex(row++, "INITRD", start);
        hero_fb_hex(row++, "INITRD END", end);
    } else {
        hero_fb_text(row++, "INITRD ?");
    }
    /* MMFR0[23:20] TGran16: 0 means no 16 KiB pages (needs the 4 KiB XNU). */
    hero_fb_text(row++, ((mmfr0 >> 20) & 15U) ? "16K PAGES: YES" : "16K PAGES: NO");
    uint32_t usbpmu = *(volatile uint32_t *)0x105c0704UL;
    uint32_t fsys0 = *(volatile uint32_t *)0x156e0800UL;
    /* Switch MAX77854 MUIC (0x25 on HSI2C_1 0x13650000, GPD1-2/3) from OPEN to USB_AP (reg 0x0c = 0x49). */
    volatile uint32_t *gpd1 = (volatile uint32_t *)0x136d0040UL;
    volatile uint32_t *i2c1 = (volatile uint32_t *)0x13650000UL;
    gpd1[8 / 4] &= ~0xf0U;
    gpd1[0 / 4] = (gpd1[0 / 4] & ~0xff00U) | 0x2200U;
    i2c1[0x00 / 4] = 0x80000009U;
    i2c1[0x00 / 4] = 0x00000009U;
    i2c1[0x48 / 4] = 0;
    i2c1[0x60 / 4] = 0x78787800U;
    i2c1[0x64 / 4] = 0x3c007878U;
    i2c1[0x68 / 4] = 0x000100f0U;
    i2c1[0x6c / 4] = 0x3cU;
    i2c1[0x08 / 4] = 0xfU;
    i2c1[0x40 / 4] = 0x80000000U;
    i2c1[0x20 / 4] = 0;
    i2c1[0x24 / 4] = i2c1[0x24 / 4];
    i2c1[0x04 / 4] = 3U | (1U << 16);
    i2c1[0x70 / 4] = 0x25U << 10;
    i2c1[0x00 / 4] = 0x89U;
    i2c1[0x20 / 4] = 0xf81U;
    i2c1[0x34 / 4] = 0x0cU;
    i2c1[0x34 / 4] = 0x49U;
    i2c1[0x44 / 4] = 2U | 0x20000U | 0x80000000U;
    for (volatile unsigned i = 0; i < 200000 && (i2c1[0x50 / 4] & (1U << 17)); ++i)
        ;
    *(volatile uint32_t *)0x105c0704UL = 1U;
    __asm__ volatile("dsb sy" : : : "memory");
    volatile uint32_t *phy = (volatile uint32_t *)0x15500000UL;
    volatile uint32_t *dwc = (volatile uint32_t *)0x15400000UL;
    /* Exynos 8890 USBDRD PHY init (26 MHz refclk, UTMI + VBUS/BVALID force). */
    phy[0x14 / 4] = 0;
    phy[0x34 / 4] = (1U << 9);
    phy[0x04 / 4] = (1U << 27) | (0x20U << 1) | (1U << 8) | (1U << 7);
    phy[0x1c / 4] = 0x26f987e0U;
    phy[0x08 / 4] = (1U << 6);
    phy[0x28 / 4] &= ~((1U << 3) | (1U << 2));
    uint32_t clkrst = (1U << 31) | (1U << 20) | (1U << 19) | (0x60U << 11) |
                      (5U << 5) | (1U << 4) | (2U << 2) | (1U << 1) | 1U;
    phy[0x10 / 4] = clkrst;
    __asm__ volatile("dsb sy" : : : "memory");
    for (volatile unsigned i = 0; i < 10000; ++i)
        ;
    phy[0x10 / 4] = clkrst & ~(1U << 1);
    __asm__ volatile("dsb sy" : : : "memory");
    /* Soft-reset DWC3 device block and assert DCTL_RUN_STOP to test host USB reset event. */
    dwc[0xc704 / 4] = (1U << 30);
    for (volatile unsigned i = 0; i < 100000 && (dwc[0xc704 / 4] & (1U << 30)); ++i)
        ;
    dwc[0xc400 / 4] = 0x8fffe000U;
    dwc[0xc404 / 4] = 0;
    dwc[0xc408 / 4] = (1U << 31) | 0x1000U;
    dwc[0xc40c / 4] = dwc[0xc40c / 4] & 0xffffU;
    dwc[0xc700 / 4] = dwc[0xc700 / 4] & ~(7U | (0x7fU << 3));
    dwc[0xc708 / 4] = 7U;
    /* Endpoint 0 OUT/IN config before DCTL_RUN_STOP, as in 0073-sweet-usb-device-ep0.patch. */
    static const struct { uint32_t ep, cmd, p0, p1; } ep0_init[] = {
        { 0, 9U | (1U << 10), 0, 0 },
        { 0, 1U | (1U << 10), (512U << 3), (1U << 8) | (1U << 10) },
        { 0, 2U | (1U << 10), 1U, 0 },
        { 1, 1U | (1U << 10), (512U << 3), (1U << 8) | (1U << 10) | (1U << 25) },
        { 1, 2U | (1U << 10), 1U, 0 },
    };
    for (unsigned k = 0; k < 5; ++k) {
        uint32_t base = (0xc800U + 0x10U * ep0_init[k].ep) / 4U;
        dwc[base + 0] = 0;
        dwc[base + 1] = ep0_init[k].p1;
        dwc[base + 2] = ep0_init[k].p0;
        dwc[base + 3] = ep0_init[k].cmd;
        for (volatile unsigned i = 0; i < 100000 && (dwc[base + 3] & (1U << 10)); ++i)
            ;
    }
    dwc[0xc720 / 4] = 3U; /* DALEPENA */
    dwc[0xc704 / 4] |= (1U << 31);
    __asm__ volatile("dsb sy" : : : "memory");
    if (!freq)
        freq = 26000000U;
    uint64_t t_usb = counter();
    while ((counter() - t_usb) / freq < 2U)
        ;
    hero_fb_hex(row++, "USBPMU", usbpmu);
    hero_fb_hex(row++, "FSYS0", fsys0);
    hero_fb_hex(row++, "SNPSID", dwc[0xc120 / 4]);
    hero_fb_hex(row++, "GCTL", dwc[0xc110 / 4]);
    hero_fb_hex(row++, "DCFG", dwc[0xc700 / 4]);
    hero_fb_hex(row++, "DCTL", dwc[0xc704 / 4]);
    hero_fb_hex(row++, "DSTS", dwc[0xc70c / 4]);
    hero_fb_hex(row++, "EVTCNT", dwc[0xc40c / 4]);

    volatile uint64_t *log = (volatile uint64_t *)PROBE_LOG_BASE;
    log[0] = 0x31504f5245483248ULL; /* "H2HEROP1" */
    log[1] = el >> 2;
    log[2] = sctlr;
    log[3] = fdt;
    log[4] = image;
    log[5] = midr;
    log[6] = mpidr;
    log[7] = mmfr0;
    log[8] = pfr0;
    log[9] = isar0;
    log[10] = freq;
    log[11] = trig_before;
    log[12] = start;
    log[13] = end;
    for (uintptr_t p = PROBE_LOG_BASE; p < PROBE_LOG_BASE + 256; p += 64)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");

    if (!freq)
        freq = 26000000U;   /* exynos8890.dtsi timer clock-frequency */
    uint64_t t0 = counter();
    unsigned shown = ~0U;
    for (;;) {
        uint64_t elapsed = (counter() - t0) / freq;
        if (elapsed >= RECOVERY_DELAY_S)
            return;
        unsigned left = RECOVERY_DELAY_S - (unsigned)elapsed;
        if (left != shown) {
            char line[] = "TWRP EN 00 S";
            line[8] = (char)('0' + left / 10);
            line[9] = (char)('0' + left % 10);
            hero_fb_text(row + 1, line);
            shown = left;
        }
    }
}
