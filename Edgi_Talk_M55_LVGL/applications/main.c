#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#define LED_PIN_G               GET_PIN(16, 6)

/* lv_user_gui_init() is now provided by ui/ui_main.c */
extern int lvgl_thread_init(void);   /* lv_rt_thread_port.c (MSH_CMD_EXPORT) */

int main(void)
{
    rt_kprintf("=== M55 BOOT OK ===\n");
    rt_pin_mode(LED_PIN_G, PIN_MODE_OUTPUT);

    lvgl_thread_init();

    while (1)
    {
        rt_pin_write(LED_PIN_G, PIN_LOW);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN_G, PIN_HIGH);
        rt_thread_mdelay(500);
    }
    return 0;
}
