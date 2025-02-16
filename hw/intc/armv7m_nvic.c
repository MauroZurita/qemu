/*
 * ARM Nested Vectored Interrupt Controller
 *
 * Copyright (c) 2006-2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 *
 * The ARMv7M System controller is fairly tightly tied in with the
 * NVIC.  Much of that is also implemented here.
 */

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "hw/arm/arm.h"
#include "exec/address-spaces.h"
#include "gic_internal.h"
#include "sysemu/sysemu.h"

typedef struct {
    GICState gic;
    struct {
        uint32_t control;
        uint32_t reload;
        int64_t tick;
        QEMUTimer *timer;
    } systick;
    uint16_t aircr_reg;     /* low word of Application Interrupt and Reset Control Register */
    MemoryRegion sysregmem;
    MemoryRegion gic_iomem_alias;
    MemoryRegion container;
    uint32_t  num_irq;
    uint32_t  scr_reg;      /* contents of SCR register */

    /* set true if we executed a WFI instruction with the SLEEPDEEP bit set in the SCR */
    bool      in_deep_sleep;

    // Set true if we execute a WFI instruction with both SLEEPDEEP bit set in the SCR
    // and PDDS (Power Down Deep Sleep) bit is set in the PWR_CR register
    bool      in_standby;

    // Properties
    void *stm32_pwr_prop;

    // output IRQs
    qemu_irq cpu_wakeup_out;
    qemu_irq power_out;

} nvic_state;

#define TYPE_NVIC "armv7m_nvic"
/**
 * NVICClass:
 * @parent_reset: the parent class' reset handler.
 *
 * A model of the v7M NVIC and System Controller
 */
typedef struct NVICClass {
    /*< private >*/
    ARMGICClass parent_class;
    /*< public >*/
    DeviceRealize parent_realize;
    void (*parent_reset)(DeviceState *dev);
} NVICClass;

#define NVIC_CLASS(klass) \
    OBJECT_CLASS_CHECK(NVICClass, (klass), TYPE_NVIC)
#define NVIC_GET_CLASS(obj) \
    OBJECT_GET_CLASS(NVICClass, (obj), TYPE_NVIC)
#define NVIC(obj) \
    OBJECT_CHECK(nvic_state, (obj), TYPE_NVIC)

static const uint8_t nvic_id[] = {
    0x00, 0xb0, 0x1b, 0x00, 0x0d, 0xe0, 0x05, 0xb1
};

/* SCR register definitions */
#define SCR_REG_SLEEPDEEP   0x00000004

/* qemu timers run at 1GHz.   We want something closer to 1MHz.  */
#define SYSTICK_SCALE 1000ULL

#define SYSTICK_ENABLE    (1 << 0)
#define SYSTICK_TICKINT   (1 << 1)
#define SYSTICK_CLKSOURCE (1 << 2)
#define SYSTICK_COUNTFLAG (1 << 16)

int system_clock_scale;
int external_ref_clock_scale = 1000;

/* Conversion factor from qemu timer to SysTick frequencies.  */
static inline int64_t systick_scale(nvic_state *s)
{
    if (s->systick.control & SYSTICK_CLKSOURCE)
        return system_clock_scale;
    else
        return external_ref_clock_scale;
}

static void systick_reload(nvic_state *s, int reset)
{
    if (reset)
        s->systick.tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->systick.tick += (s->systick.reload + 1) * systick_scale(s);
    timer_mod(s->systick.timer, s->systick.tick);
}

static void systick_timer_tick(void * opaque)
{
    nvic_state *s = (nvic_state *)opaque;
    s->systick.control |= SYSTICK_COUNTFLAG;
    if (s->systick.control & SYSTICK_TICKINT) {
        if (!s->in_deep_sleep) {
            /* NOTE: In deep sleep mode, all peripherals are off (no clocks), so
             * no IRQs should be made pending. Eventually, we should gate all 
             * peripherals according to deep sleep mode, but SysTick is a good
             * important start. */
            armv7m_nvic_set_pending(s, ARMV7M_EXCP_SYSTICK);
        } 
    }
    if (s->systick.reload == 0) {
        s->systick.control &= ~SYSTICK_ENABLE;
    } else {
        systick_reload(s, 0);
    }
}

static void systick_reset(nvic_state *s)
{
    s->systick.control = 0;
    s->systick.reload = 0;
    s->systick.tick = 0;
    timer_del(s->systick.timer);
}

/* Set the base priority */
void armv7m_nvic_set_base_priority(void *opaque, unsigned int priority)
{
    nvic_state *s = (nvic_state *)opaque;
    if (priority == 0) {
        s->gic.priority_mask[0] = 0x100;
    } else {
        /* Pay only attention to the priority group when masking interrupts */
        uint32_t group_setting = (s->aircr_reg >> 8) & 0x03;
        uint32_t group_mask = (0x0FF << (group_setting + 1)) & 0x0FF;
        s->gic.priority_mask[0] = priority & group_mask;    /* TODO: Make this more general */
    }
    gic_update(&s->gic);
}

/* The external routines use the hardware vector numbering, ie. the first
   IRQ is #16.  The internal GIC routines use #32 as the first IRQ.  */
void armv7m_nvic_set_pending(void *opaque, int irq)
{
    nvic_state *s = (nvic_state *)opaque;
    if (irq >= 16)
        irq += 16;
    gic_set_pending_private(&s->gic, 0, irq);
}

/* Make pending IRQ active.  */
int armv7m_nvic_acknowledge_irq(void *opaque)
{
    nvic_state *s = (nvic_state *)opaque;
    uint32_t irq;

    /* We can't be in deep sleep mode anymore because we received an interrupt
     * Actually, the correct way to do this to match the hardware exactly would be to fall 
     * out of deep sleep even if an interrupt is pending - regardless if it is active or 
     * not or masked due to BASEPRI. This would involved moving this reset of deep sleep
     * mode higher up the call chain, perhaps in arm_gic.c, where we get notification of
     * interrupts that change to pending state.  */
    s->in_deep_sleep = false;
    if (s->in_standby) {
        qemu_set_irq(s->power_out, true);
        s->in_standby = false;
    }

    irq = gic_acknowledge_irq(&s->gic, 0);
    if (irq == 1023)
        //hw_error("Interrupt but no vector\n");
        return 1023;
    if (irq >= 32)
        irq -= 16;
    return irq;
}

void armv7m_nvic_complete_irq(void *opaque, int irq)
{
    nvic_state *s = (nvic_state *)opaque;
    if (irq >= 16)
        irq += 16;
    gic_complete_irq(&s->gic, 0, irq);
}

void armv7m_nvic_cpu_executed_wfi(void *opaque)
{
    nvic_state *s = (nvic_state *)opaque;
    if ((s->scr_reg & SCR_REG_SLEEPDEEP) != 0) {
        s->in_deep_sleep = true;
        if (s->stm32_pwr_prop && f2xx_pwr_powerdown_deepsleep(s->stm32_pwr_prop)) {
            s->in_standby = true;
            // For now, this is an easy way to disable nearly all interrupts from waking up
            // the CPU. Technically, this is not correct and we should allow specific ones
            // through (some RTC interrupts, etc.). 
            armv7m_nvic_set_base_priority(opaque, 0x01);

            // Inform peripherals that the power is off
            qemu_set_irq(s->power_out, false);
        }
    }
}


// -----------------------------------------------------------------------------
// Called when the WKUP pin changes state (GPIO A0)
static void nvic_wakeup_in_cb(void *opaque, int n, int level)
{
    nvic_state *s = (nvic_state *)opaque;

    // If we are in standby mode, wake up the CPU
    if (level && s->in_standby) {
        s->in_standby = false;
        s->in_deep_sleep = false;
        qemu_set_irq(s->power_out, true);
        qemu_set_irq(s->cpu_wakeup_out, level);
    } else if (!level) {
        qemu_set_irq(s->cpu_wakeup_out, level);
    }
}

static uint32_t nvic_readl(nvic_state *s, uint32_t offset)
{
    ARMCPU *cpu;
    uint32_t val;
    int irq;

    switch (offset) {
    case 4: /* Interrupt Control Type.  */
        return (s->num_irq / 32) - 1;
    case 0x10: /* SysTick Control and Status.  */
        val = s->systick.control;
        s->systick.control &= ~SYSTICK_COUNTFLAG;
        return val;
    case 0x14: /* SysTick Reload Value.  */
        return s->systick.reload;
    case 0x18: /* SysTick Current Value.  */
        {
            int64_t t;
            if ((s->systick.control & SYSTICK_ENABLE) == 0)
                return 0;
            t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (t >= s->systick.tick)
                return 0;
            val = ((s->systick.tick - (t + 1)) / systick_scale(s)) + 1;
            /* The interrupt in triggered when the timer reaches zero.
               However the counter is not reloaded until the next clock
               tick.  This is a hack to return zero during the first tick.  */
            if (val > s->systick.reload)
                val = 0;
            return val;
        }
    case 0x1c: /* SysTick Calibration Value.  */
        return 10000;
    case 0xd00: /* CPUID Base.  */
        cpu = ARM_CPU(current_cpu);
        return cpu->midr;
    case 0xd04: /* Interrupt Control State.  */
        /* VECTACTIVE */
        val = s->gic.running_irq[0];
        if (val == 1023) {
            val = 0;
        } else if (val >= 32) {
            val -= 16;
        }
        /* RETTOBASE */
        if (s->gic.running_irq[0] == 1023
                || s->gic.last_active[s->gic.running_irq[0]][0] == 1023) {
            val |= (1 << 11);
        }
        /* VECTPENDING */
        if (s->gic.current_pending[0] != 1023)
            val |= (s->gic.current_pending[0] << 12);
        /* ISRPENDING */
        for (irq = 32; irq < s->num_irq; irq++) {
            if (s->gic.irq_state[irq].pending) {
                val |= (1 << 22);
                break;
            }
        }
        /* PENDSTSET */
        if (s->gic.irq_state[ARMV7M_EXCP_SYSTICK].pending)
            val |= (1 << 26);
        /* PENDSVSET */
        if (s->gic.irq_state[ARMV7M_EXCP_PENDSV].pending)
            val |= (1 << 28);
        /* NMIPENDSET */
        if (s->gic.irq_state[ARMV7M_EXCP_NMI].pending)
            val |= (1 << 31);
        return val;
    case 0xd08: /* Vector Table Offset.  */
        cpu = ARM_CPU(current_cpu);
        return cpu->env.v7m.vecbase;
    case 0xd0c: /* Application Interrupt/Reset Control.  */
        return 0xfa050000 | s->aircr_reg;
    case 0xd10: /* System Control.  */
        return s->scr_reg;
        break;
    case 0xd14: /* Configuration Control.  */
        /* TODO: Implement Configuration Control bits.  */
        return 0;
    case 0xd24: /* System Handler Status.  */
        val = 0;
        if (s->gic.irq_state[ARMV7M_EXCP_MEM].active) val |= (1 << 0);
        if (s->gic.irq_state[ARMV7M_EXCP_BUS].active) val |= (1 << 1);
        if (s->gic.irq_state[ARMV7M_EXCP_USAGE].active) val |= (1 << 3);
        if (s->gic.irq_state[ARMV7M_EXCP_SVC].active) val |= (1 << 7);
        if (s->gic.irq_state[ARMV7M_EXCP_DEBUG].active) val |= (1 << 8);
        if (s->gic.irq_state[ARMV7M_EXCP_PENDSV].active) val |= (1 << 10);
        if (s->gic.irq_state[ARMV7M_EXCP_SYSTICK].active) val |= (1 << 11);
        if (s->gic.irq_state[ARMV7M_EXCP_USAGE].pending) val |= (1 << 12);
        if (s->gic.irq_state[ARMV7M_EXCP_MEM].pending) val |= (1 << 13);
        if (s->gic.irq_state[ARMV7M_EXCP_BUS].pending) val |= (1 << 14);
        if (s->gic.irq_state[ARMV7M_EXCP_SVC].pending) val |= (1 << 15);
        if (s->gic.irq_state[ARMV7M_EXCP_MEM].enabled) val |= (1 << 16);
        if (s->gic.irq_state[ARMV7M_EXCP_BUS].enabled) val |= (1 << 17);
        if (s->gic.irq_state[ARMV7M_EXCP_USAGE].enabled) val |= (1 << 18);
        return val;
    case 0xd28: /* Configurable Fault Status.  */
        /* TODO: Implement Fault Status.  */
        qemu_log_mask(LOG_UNIMP, "Configurable Fault Status unimplemented\n");
        return 0;
    case 0xd2c: /* Hard Fault Status.  */
    case 0xd30: /* Debug Fault Status.  */
    case 0xd34: /* Mem Manage Address.  */
    case 0xd38: /* Bus Fault Address.  */
    case 0xd3c: /* Aux Fault Status.  */
        /* TODO: Implement fault status registers.  */
        qemu_log_mask(LOG_UNIMP, "Fault status registers unimplemented\n");
        return 0;
    case 0xd40: /* PFR0.  */
        return 0x00000030;
    case 0xd44: /* PRF1.  */
        return 0x00000200;
    case 0xd48: /* DFR0.  */
        return 0x00100000;
    case 0xd4c: /* AFR0.  */
        return 0x00000000;
    case 0xd50: /* MMFR0.  */
        return 0x00000030;
    case 0xd54: /* MMFR1.  */
        return 0x00000000;
    case 0xd58: /* MMFR2.  */
        return 0x00000000;
    case 0xd5c: /* MMFR3.  */
        return 0x00000000;
    case 0xd60: /* ISAR0.  */
        return 0x01141110;
    case 0xd64: /* ISAR1.  */
        return 0x02111000;
    case 0xd68: /* ISAR2.  */
        return 0x21112231;
    case 0xd6c: /* ISAR3.  */
        return 0x01111110;
    case 0xd70: /* ISAR4.  */
        return 0x01310102;
    /* TODO: Implement debug registers.  */
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "NVIC: Bad read offset 0x%x\n", offset);
        return 0;
    }
}

static void nvic_writel(nvic_state *s, uint32_t offset, uint32_t value)
{
    ARMCPU *cpu;
    uint32_t oldval;
    switch (offset) {
    case 0x10: /* SysTick Control and Status.  */
        oldval = s->systick.control;
        s->systick.control &= 0xfffffff8;
        s->systick.control |= value & 7;
        if ((oldval ^ value) & SYSTICK_ENABLE) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if (value & SYSTICK_ENABLE) {
                if (s->systick.tick) {
                    s->systick.tick += now;
                    timer_mod(s->systick.timer, s->systick.tick);
                } else {
                    systick_reload(s, 1);
                }
            } else {
                timer_del(s->systick.timer);
                s->systick.tick -= now;
                if (s->systick.tick < 0)
                  s->systick.tick = 0;
            }
        } else if ((oldval ^ value) & SYSTICK_CLKSOURCE) {
            /* This is a hack. Force the timer to be reloaded
               when the reference clock is changed.  */
            systick_reload(s, 1);
        }
        break;
    case 0x14: /* SysTick Reload Value.  */
        s->systick.reload = value;
        break;
    case 0x18: /* SysTick Current Value.  Writes reload the timer.  */
        systick_reload(s, 1);
        s->systick.control &= ~SYSTICK_COUNTFLAG;
        break;
    case 0xd04: /* Interrupt Control State.  */
        if (value & (1 << 31)) {
            armv7m_nvic_set_pending(s, ARMV7M_EXCP_NMI);
        }
        if (value & (1 << 28)) {
            armv7m_nvic_set_pending(s, ARMV7M_EXCP_PENDSV);
        } else if (value & (1 << 27)) {
            s->gic.irq_state[ARMV7M_EXCP_PENDSV].pending = 0;
            gic_update(&s->gic);
        }
        if (value & (1 << 26)) {
            armv7m_nvic_set_pending(s, ARMV7M_EXCP_SYSTICK);
        } else if (value & (1 << 25)) {
            s->gic.irq_state[ARMV7M_EXCP_SYSTICK].pending = 0;
            gic_update(&s->gic);
        }
        break;
    case 0xd08: /* Vector Table Offset.  */
        cpu = ARM_CPU(current_cpu);
        cpu->env.v7m.vecbase = value & 0xffffff80;
        break;
    case 0xd0c: /* Application Interrupt/Reset Control.  */
        if ((value >> 16) == 0x05fa) {
            if (value & 2) {
                qemu_log_mask(LOG_UNIMP, "VECTCLRACTIVE unimplemented\n");
            }
            if (value & 5) {
                qemu_system_reset_request();
            }
            s->aircr_reg = value & 0x00700;    /* keep only the bits we suport */
        }
        break;
    case 0xd10: /* System Control.  */
        s->scr_reg = value;
        break;
    case 0xd14: /* Configuration Control.  */
        /* TODO: Implement control registers.  */
        qemu_log_mask(LOG_UNIMP, "NVIC: CCR unimplemented\n");
        break;
    case 0xd24: /* System Handler Control.  */
        /* TODO: Real hardware allows you to set/clear the active bits
           under some circumstances.  We don't implement this.  */
        s->gic.irq_state[ARMV7M_EXCP_MEM].enabled = (value & (1 << 16)) != 0;
        s->gic.irq_state[ARMV7M_EXCP_BUS].enabled = (value & (1 << 17)) != 0;
        s->gic.irq_state[ARMV7M_EXCP_USAGE].enabled = (value & (1 << 18)) != 0;
        break;
    case 0xd28: /* Configurable Fault Status.  */
    case 0xd2c: /* Hard Fault Status.  */
    case 0xd30: /* Debug Fault Status.  */
    case 0xd34: /* Mem Manage Address.  */
    case 0xd38: /* Bus Fault Address.  */
    case 0xd3c: /* Aux Fault Status.  */
        qemu_log_mask(LOG_UNIMP,
                      "NVIC: fault status registers unimplemented\n");
        break;
    case 0xd9c:
    case 0xda0:
    case 0xda4:
    case 0xda8:
    case 0xdac:
    case 0xdb0:
        /* XXX memory protection - just ignore it. */
        break;
    case 0xf00: /* Software Triggered Interrupt Register */
        if ((value & 0x1ff) < s->num_irq) {
            gic_set_pending_private(&s->gic, 0, value & 0x1ff);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "NVIC: Bad write offset 0x%x\n", offset);
    }
}

static uint64_t nvic_sysreg_read(void *opaque, hwaddr addr,
                                 unsigned size)
{
    nvic_state *s = (nvic_state *)opaque;
    uint32_t offset = addr;
    int i;
    uint32_t val;

    switch (offset) {
    case 0xd18 ... 0xd23: /* System Handler Priority.  */
        val = 0;
        for (i = 0; i < size; i++) {
            val |= s->gic.priority1[(offset - 0xd14) + i][0] << (i * 8);
        }
        return val;
    case 0xfe0 ... 0xfff: /* ID.  */
        if (offset & 3) {
            return 0;
        }
        return nvic_id[(offset - 0xfe0) >> 2];
    }
    if (size == 4) {
        return nvic_readl(s, offset);
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "NVIC: Bad read of size %d at offset 0x%x\n", size, offset);
    return 0;
}

static void nvic_sysreg_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    nvic_state *s = (nvic_state *)opaque;
    uint32_t offset = addr;
    int i;

    switch (offset) {
    case 0xd18 ... 0xd23: /* System Handler Priority.  */
        for (i = 0; i < size; i++) {
            s->gic.priority1[(offset - 0xd14) + i][0] =
                (value >> (i * 8)) & 0xff;
        }
        gic_update(&s->gic);
        return;
    }
    if (size == 4) {
        nvic_writel(s, offset, value);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "NVIC: Bad write of size %d at offset 0x%x\n", size, offset);
}

static const MemoryRegionOps nvic_sysreg_ops = {
    .read = nvic_sysreg_read,
    .write = nvic_sysreg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const VMStateDescription vmstate_nvic = {
    .name = "armv7m_nvic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(systick.control, nvic_state),
        VMSTATE_UINT32(systick.reload, nvic_state),
        VMSTATE_INT64(systick.tick, nvic_state),
        VMSTATE_TIMER(systick.timer, nvic_state),
        VMSTATE_END_OF_LIST()
    }
};

static void armv7m_nvic_reset(DeviceState *dev)
{
    nvic_state *s = NVIC(dev);
    NVICClass *nc = NVIC_GET_CLASS(s);

    nc->parent_reset(dev);
    /* Common GIC reset resets to disabled; the NVIC doesn't have
     * per-CPU interfaces so mark our non-existent CPU interface
     * as enabled by default, with a priority mask which allows
     * all interrupts through, and reset all priorities. 
     */
    s->gic.cpu_enabled[0] = true;
    s->gic.priority_mask[0] = 0x100;
    int i;
    for (i=0; i<GIC_INTERNAL; i++) {
        s->gic.priority1[i][0] = 0;
    }
    memset(s->gic.priority2, 0, sizeof(s->gic.priority2));

    /* The NVIC as a whole is always enabled. */
    s->gic.enabled = true;
    systick_reset(s);

    s->scr_reg = 0;
    s->in_deep_sleep = false;
    s->in_standby = false;
    qemu_set_irq(s->power_out, true);
}

static void armv7m_nvic_realize(DeviceState *dev, Error **errp)
{
    nvic_state *s = NVIC(dev);
    NVICClass *nc = NVIC_GET_CLASS(s);
    Error *local_err = NULL;

    /* The NVIC always has only one CPU */
    s->gic.num_cpu = 1;
    /* Tell the common code we're an NVIC */
    s->gic.revision = 0xffffffff;
    s->num_irq = s->gic.num_irq;
    nc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    gic_init_irqs_and_distributor(&s->gic, s->num_irq);
    /* The NVIC and system controller register area looks like this:
     *  0..0xff : system control registers, including systick
     *  0x100..0xcff : GIC-like registers
     *  0xd00..0xfff : system control registers
     * We use overlaying to put the GIC like registers
     * over the top of the system control register region.
     */
    memory_region_init(&s->container, OBJECT(s), "nvic", 0x1000);
    /* The system register region goes at the bottom of the priority
     * stack as it covers the whole page.
     */
    memory_region_init_io(&s->sysregmem, OBJECT(s), &nvic_sysreg_ops, s,
                          "nvic_sysregs", 0x1000);
    memory_region_add_subregion(&s->container, 0, &s->sysregmem);
    /* Alias the GIC region so we can get only the section of it
     * we need, and layer it on top of the system register region.
     */
    memory_region_init_alias(&s->gic_iomem_alias, OBJECT(s),
                             "nvic-gic", &s->gic.iomem,
                             0x100, 0xc00);
    memory_region_add_subregion_overlap(&s->container, 0x100,
                                        &s->gic_iomem_alias, 1);
    /* Map the whole thing into system memory at the location required
     * by the v7M architecture.
     */
    memory_region_add_subregion(get_system_memory(), 0xe000e000, &s->container);
    s->systick.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, systick_timer_tick, s);

    // Create the input handler to be notified when the WKUP pin gets asserted
    qdev_init_gpio_in_named(dev, nvic_wakeup_in_cb, "wakeup_in", 1);

    // This is the handler that will wakeup the CPU
    qdev_init_gpio_out_named(dev, &s->cpu_wakeup_out, "wakeup_out", 1);

    // This is the handler that informs peripherals that the power is on/off
    qdev_init_gpio_out_named(dev, &s->power_out, "power_out", 1);

}

static void armv7m_nvic_instance_init(Object *obj)
{
    /* We have a different default value for the num-irq property
     * than our superclass. This function runs after qdev init
     * has set the defaults from the Property array and before
     * any user-specified property setting, so just modify the
     * value in the GICState struct.
     */
    GICState *s = ARM_GIC_COMMON(obj);
    /* The ARM v7m may have anything from 0 to 496 external interrupt
     * IRQ lines. We default to 64. Other boards may differ and should
     * set the num-irq property appropriately.
     */
    s->num_irq = 64;
}

static Property armv7m_nvic_properties[] = {
    DEFINE_PROP_PTR("stm32_pwr", nvic_state, stm32_pwr_prop),
    DEFINE_PROP_END_OF_LIST(),
};

static void armv7m_nvic_class_init(ObjectClass *klass, void *data)
{
    NVICClass *nc = NVIC_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    nc->parent_reset = dc->reset;
    nc->parent_realize = dc->realize;
    dc->vmsd  = &vmstate_nvic;
    dc->reset = armv7m_nvic_reset;
    dc->realize = armv7m_nvic_realize;
    dc->props = armv7m_nvic_properties;
}

static const TypeInfo armv7m_nvic_info = {
    .name          = TYPE_NVIC,
    .parent        = TYPE_ARM_GIC_COMMON,
    .instance_init = armv7m_nvic_instance_init,
    .instance_size = sizeof(nvic_state),
    .class_init    = armv7m_nvic_class_init,
    .class_size    = sizeof(NVICClass),
};

static void armv7m_nvic_register_types(void)
{
    type_register_static(&armv7m_nvic_info);
}

type_init(armv7m_nvic_register_types)
