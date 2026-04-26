// Phase 2a — TinyUSB glue replacing ST USB Device Library.
// Strategy: preserve ODrive's interface_usb.cpp + communication.cpp unchanged.
// Shim the functions they call (CDC_Transmit_FS, USBD_CDC_ReceivePacket,
// MX_USB_DEVICE_Init) with TinyUSB-backed implementations.
// ODrive's osMessagePut event protocol stays identical (events 1=connect,
// 2=disconnect, 3=CDC TX done, 5=CDC RX done).

#include <stdint.h>
#include <string.h>
#include <cmsis_os.h>
#include "stm32f4xx_hal.h"
#include "tusb.h"

// ODrive types/defines (from preserved ST USB library headers)
extern "C" {
#include "usbd_def.h"
#include "usbd_cdc.h"
}

// ---- Globals expected by interface_usb.cpp / usbd_cdc_if.c replacements ----

// ODrive code takes the address of `usb_dev_handle` and passes it to USBD_CDC_
// functions. Defined in Board/v3/board.cpp — we just reference it.
extern "C" USBD_HandleTypeDef usb_dev_handle;

// Event queue defined in MotorControl/main.cpp — we post to it.
extern "C" osMessageQId usb_event_queue;

// Declared in interface_usb.cpp — ODrive's RX dispatch.
extern "C" void usb_rx_process_packet(uint8_t *buf, uint32_t len, uint8_t endpoint_pair);

// ---- Pending-read state (shim for ST's buffer-hand-off RX model) -----------
// ODrive's USBD_CDC_ReceivePacket registers a user-provided buffer that the
// hardware fills via DMA. TinyUSB maintains its own FIFO and we must copy from
// there. We store (buf, size) pairs and drain them in our task loop.
struct PendingRead {
    uint8_t*  buf  = nullptr;
    uint16_t  size = 0;
    uint8_t   endpoint_num = 0;
    volatile bool active = false;
};

static PendingRead pending_cdc_rx{};

// ---- TinyUSB task (owns tud_task + RX fanout) -----------------------------

static void usb_tusb_task(void* /*ctx*/) {
    for (;;) {
        tud_task();

        // Service pending CDC read — if ODrive armed a buffer via
        // USBD_CDC_ReceivePacket and TinyUSB has data, copy and dispatch.
        if (pending_cdc_rx.active && tud_cdc_available() > 0) {
            uint32_t to_read = tud_cdc_available();
            if (to_read > pending_cdc_rx.size) to_read = pending_cdc_rx.size;

            uint32_t n = tud_cdc_read(pending_cdc_rx.buf, to_read);
            if (n > 0) {
                pending_cdc_rx.active = false; // one-shot per arm
                usb_rx_process_packet(pending_cdc_rx.buf, n,
                                      pending_cdc_rx.endpoint_num);
            }
        }


        // Yield — tud_task is non-blocking; without a short delay we spin at
        // 100% CPU. 1 tick (1 ms) is plenty for ASCII throughput.
        osDelay(1);
    }
}

// ---- Shims for ST USB Device Library API ----------------------------------

// Bypass do pipeline ODrive — usado pelo cmdparser do OpenFFBoard pra
// escrever replies direto na FIFO do TinyUSB sem passar pelo BufferedStreamSink
// + Stm32UsbTxStream do ODrive (que entra em estado quebrado se o Configurator
// floodar muitos comandos juntos). Mesma estratégia da usb_task em Firmware-Merged.
// Loop com osDelay igual ao CDC_Transmit_FS abaixo, mas sem mexer em estados
// do ODrive — totalmente isolado do pipeline ASCII.
extern "C" int cmdparser_cdc_write(const uint8_t *buf, uint32_t len) {
    if (!tud_cdc_connected()) return 0;
    uint32_t remaining = len;
    const uint8_t *p = buf;
    uint32_t spin = 0;
    while (remaining > 0) {
        if (!tud_cdc_connected()) return (int)(p - buf);
        uint32_t n = tud_cdc_write(p, remaining);
        if (n > 0) {
            p += n;
            remaining -= n;
            spin = 0;
            tud_cdc_write_flush();
        } else {
            tud_cdc_write_flush();
            if (++spin > 100) return (int)(p - buf); // 100ms timeout
            osDelay(1);
        }
    }
    return (int)len;
}

extern "C" uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len, uint8_t endpoint_pair) {
    if (endpoint_pair == CDC_IN_EP) {
        if (!tud_cdc_connected()) return USBD_FAIL;

        // CRÍTICO: tud_cdc_write pode escrever menos que Len quando o FIFO
        // interno do TinyUSB (CFG_TUD_CDC_TX_BUFSIZE, default 64) está cheio
        // por uma transferência IN ainda não drenada pelo host. Retornar
        // USBD_FAIL nesse caso faz interface_usb.cpp marcar o stream com
        // kStreamError e parar de drenar o BufferedStreamSink — sintoma:
        // serial trava no meio de uma resposta longa do ASCII protocol.
        //
        // Aqui a gente faz o que a ST USBD_CDC_Transmit faria: aguardar
        // espaço, copiar tudo, e só retornar USBD_OK quando todos os bytes
        // estão na fila (o callback tud_cdc_tx_complete_cb depois posta
        // o evento 3 quando o host efetivamente drena, fechando o ciclo).
        uint32_t remaining = Len;
        const uint8_t* p = Buf;
        uint32_t spin = 0;

        while (remaining > 0) {
            if (!tud_cdc_connected()) return USBD_FAIL;

            uint32_t n = tud_cdc_write(p, remaining);
            if (n > 0) {
                p += n;
                remaining -= n;
                spin = 0;
                // Flush imediato pra empurrar pra IN endpoint e abrir
                // espaço no FIFO interno na próxima iteração.
                tud_cdc_write_flush();
            } else {
                // FIFO cheio — força flush, espera 1 tick e reavalia.
                tud_cdc_write_flush();
                if (++spin > 100) return USBD_FAIL; // ~100 ms timeout
                osDelay(1);
            }
        }
        return USBD_OK;
    }

    // Fibre native endpoint (ODRIVE_IN_EP 0x83) not exposed in CDC-only build.
    // ODrive's fibre_over_usb still tries writes to it — pretend ok + post
    // TX-done event so its state machine doesn't get stuck.
    if (endpoint_pair == ODRIVE_IN_EP) {
        osMessagePut(usb_event_queue, 4, 0);
    }
    return USBD_OK;
}

// ODrive calls this to arm the next receive. We capture the buffer and drain
// TinyUSB FIFO into it from our task loop. Returns immediately.
extern "C" uint8_t USBD_CDC_ReceivePacket(USBD_HandleTypeDef* pdev,
                                          uint8_t* buf, uint16_t size,
                                          uint8_t endpoint_num) {
    (void)pdev;

    if (endpoint_num == CDC_OUT_EP) {
        pending_cdc_rx.buf  = buf;
        pending_cdc_rx.size = (uint16_t)size;
        pending_cdc_rx.endpoint_num = endpoint_num;
        __DMB();
        pending_cdc_rx.active = true;
        return USBD_OK;
    }
    // Fibre native endpoint not exposed — no-op arm.
    return USBD_OK;
}

// ---- MX_USB_DEVICE_Init — replaces the ST-generated init ------------------

extern "C" void MX_USB_DEVICE_Init(void) {
    // Hardware setup that used to live in HAL_PCD_MspInit (usbd_conf.c) PLUS
    // the PHY tweaks that Firmware-Merged established as required on MKS Mini.
    // Without these, USB fails to enumerate silently (no pull-up on D+).

    // Ensure GPIOA clock is on before we touch PA11/PA12 AF.
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio{};
    gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF10_OTG_FS;
    HAL_GPIO_Init(GPIOA, &gpio);

    // Peripheral clock
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    // PHY tweaks — crítico em MKS Mini: PA9 (VBUS) não está conectado ao VBUS
    // do USB. Sem NOVBUSSENS, o PHY espera sinal VBUS eterno e nunca assina
    // presença no barramento.
    {
        USB_OTG_GlobalTypeDef *USBx =
            (USB_OTG_GlobalTypeDef *)USB_OTG_FS_PERIPH_BASE;
        USBx->GCCFG |=  USB_OTG_GCCFG_PWRDWN;        // acorda o PHY
        USBx->GCCFG |=  USB_OTG_GCCFG_NOVBUSSENS;    // <- faltava
        USBx->GCCFG &= ~(USB_OTG_GCCFG_VBUSASEN | USB_OTG_GCCFG_VBUSBSEN);
        // Destrava o clock do PHY (PCGCCTL=0) caso bootloader DFU tenha deixado
        // algum bit setado.
        *(volatile uint32_t *)(USB_OTG_FS_PERIPH_BASE + USB_OTG_PCGCCTL_BASE) = 0;
    }

    HAL_Delay(50); // tempo pro PHY estabilizar

    // Priority deve ser numericamente >= configLIBRARY_MAX_SYSCALL_INTERRUPT_
    // PRIORITY (5) pra TinyUSB poder chamar xQueueSendFromISR no FreeRTOS
    // sem trigger em assert.
    HAL_NVIC_SetPriority(OTG_FS_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);

    // Init TinyUSB on rhport 0 (OTG_FS).
    tusb_init();

    // Spin up our task that runs tud_task and fans out RX bytes.
    osThreadDef(tusbTask, usb_tusb_task, osPriorityNormal, 0,
                2048 / sizeof(StackType_t));
    osThreadCreate(osThread(tusbTask), NULL);
}

// ---- OTG_FS ISR vector ----------------------------------------------------

extern "C" void OTG_FS_IRQHandler(void) {
    tud_int_handler(0);
}

// ---- TinyUSB device callbacks — translate to ODrive events ---------------

extern "C" void tud_mount_cb(void) {
    osMessagePut(usb_event_queue, 1, 0);  // connected
}

extern "C" void tud_umount_cb(void) {
    osMessagePut(usb_event_queue, 2, 0);  // disconnected
    // Failsafe FFB: zera torque imediato pra evitar volante puxando sozinho
    // se game travou ou USB foi removido durante FFB ativo.
    extern void ffb_on_usb_unmount(void);
    ffb_on_usb_unmount();
}

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    osMessagePut(usb_event_queue, 2, 0);
}

extern "C" void tud_resume_cb(void) {
    osMessagePut(usb_event_queue, 1, 0);
}

// Fires when host drains the CDC IN FIFO — ODrive's "CDC TX done" event.
extern "C" void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    osMessagePut(usb_event_queue, 3, 0);
}

