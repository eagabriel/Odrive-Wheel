#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// STM32F4 com OTG_FS (core synopsys DWC2)
#define CFG_TUSB_MCU              OPT_MCU_STM32F4
#define BOARD_DEVICE_RHPORT_NUM   0
#define BOARD_DEVICE_RHPORT_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)

// FreeRTOS integration for tud_task scheduling
#define CFG_TUSB_OS               OPT_OS_FREERTOS

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))

// Endpoint 0 size
#define CFG_TUD_ENDPOINT0_SIZE    64

// Phase 2b — CDC + HID FFB composite (padrão Firmware-Merged proven-working).
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_HID               1
#define CFG_TUD_VENDOR            0
#define CFG_TUD_AUDIO             0
#define CFG_TUD_DFU_RT            0

// CDC FIFO — 512 bytes RX pra não perder dados em bursts
#define CFG_TUD_CDC_RX_BUFSIZE    512
#define CFG_TUD_CDC_TX_BUFSIZE    512

// HID buffer — OpenFFBoard usa 64 bytes (FFB reports cabem)
#define CFG_TUD_HID_EP_BUFSIZE    64

#ifdef __cplusplus
}
#endif

#endif
