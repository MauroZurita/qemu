/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "stm32f2xx.h"
#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/devices.h"
#include "ui/console.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"


typedef struct {
    Stm32 *stm32; // FIXME: what is this used for? cruft?

    bool last_button_pressed;
    qemu_irq button_irq;
} Stm32P205;




static void led_irq_handler(void *opaque, int n, int level)
{
    /* There should only be one IRQ for the LED */
    assert(n == 0);

    /* Assume that the IRQ is only triggered if the LED has changed state.
     * If this is not correct, we may get multiple LED Offs or Ons in a row.
     */
    switch (level) {
        case 0:
            printf("LED Off\n");
            break;
        case 1:
            printf("LED On\n");
            break;
    }
}

static void stm32_p205_key_event(void *opaque, int keycode)
{
    Stm32P205 *s = (Stm32P205 *)opaque;
    bool make;
    int core_keycode;

    if((keycode & 0x80) == 0) {
        make = true;
        core_keycode = keycode;
    } else {
        make = false;
        core_keycode = keycode & 0x7f;
    }

    /* Responds when a "B" key press is received.
     * Inside the monitor, you can type "sendkey b"
     */
    if(core_keycode == 0x30) {
        if(make) {
            if(!s->last_button_pressed) {
                qemu_irq_raise(s->button_irq);
                s->last_button_pressed = true;
            }
        } else {
            if(s->last_button_pressed) {
                qemu_irq_lower(s->button_irq);
                s->last_button_pressed = false;
            }
        }
    }
    return;

}


static void stm32_p205_init(MachineState *machine) {
    
//    qemu_irq *led_irq;
    Stm32P205 *s;
    Stm32Gpio *stm32_gpio[STM32F2XX_GPIO_COUNT];
    Stm32Uart *stm32_uart[STM32_UART_COUNT];
    Stm32Timer *stm32_timer[STM32_TIM_COUNT];
    DeviceState *rtc_dev;
    ARMCPU *cpu;

    struct stm32f2xx stm;

    s = (Stm32P205 *)g_malloc0(sizeof(Stm32P205));

    stm32f2xx_init(/*flash_size*/ 1024,
               /*ram_size*/ 128,
               machine->kernel_filename,
               stm32_gpio,
               stm32_uart,
               stm32_timer,
               &rtc_dev,
               8000000,
               32768,
               &stm,
               &cpu);

//    /* Connect LED to GPIO C pin 12 */
//    led_irq = qemu_allocate_irqs(led_irq_handler, NULL, 1);
//    qdev_connect_gpio_out((DeviceState *)stm32_gpio[STM32_GPIOC_INDEX], 12, led_irq[0]);
//
//    /* Connect button to GPIO A pin 0 */
//    s->button_irq = qdev_get_gpio_in((DeviceState *)stm32_gpio[STM32_GPIOA_INDEX], 0);
//    qemu_add_kbd_event_handler(stm32_p205_key_event, s);

    /* Connect RS232 to UART */
//    stm32_uart_connect(
//            stm32_uart[STM32_UART2_INDEX],
//            serial_hds[0],
//            STM32_USART2_NO_REMAP);
 }

static QEMUMachine stm32_p205_machine = {
    .name = "stm32-p205",
    .desc = "Olimex STM32 p205 Dev Board",
    .init = stm32_p205_init
};


static void stm32_p205_machine_init(void)
{
    qemu_register_machine(&stm32_p205_machine);
}

machine_init(stm32_p205_machine_init);
