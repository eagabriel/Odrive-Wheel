// Phase 2a — TinyUSB CDC-only USB descriptors pra Firmware-V56-Stock.
// Device se apresenta como "ODrive MKS-Mini CDC" com 1 interface CDC.
// HID FFB virá na Fase 2b (bcdDevice/PID bumps pra invalidar cache do host).

#include "tusb.h"

// Phase 2d: VID/PID/bcdDevice/Strings TODOS alinhados com Firmware-Merged.
// Configurator do OpenFFBoard inspeciona descritores USB durante enumeração
// e marca como "supported" antes mesmo do handshake — entao precisa bater
// byte-a-byte com Firmware-Merged que comprovadamente ele reconhece.
//   VID=0x1209 PID=0x0D40 bcdDevice=0x0D00
//   Manufacturer="OpenFFBoard" Product="ODrive-Wheel" Serial="0001"
#define USB_VID   0x1209
#define USB_PID   0x0D40
#define USB_BCD   0x0D00

// ---------- Device Descriptor ----------
// CDC puro com IAD não é necessário (1 função só), mas manteremos CLASS_MISC/IAD
// pra facilitar migração futura pra composite (Phase 2b).
// Same pattern as Firmware-Merged (proven to enumerate on same MKS Mini hw):
// bDeviceClass=0x00 (per-interface classes declared via IAD in config desc).
// Device subclass/protocol still say "IAD" for compat com hosts antigos.
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// ---------- Configuration Descriptor ----------
// CDC (control+data, IAD combined) + Vendor-specific (Fibre native bulk).
// Endpoint numbers match ODrive stock (CDC_*_EP / ODRIVE_*_EP) pra que o
// shim em usb_tinyusb_glue.cpp possa rotear pelo `endpoint_pair` passado
// pelo interface_usb.cpp sem tradução.
// HID FFB PID 2-axis descriptor (copiado do Firmware-Merged proven-working)
#include "class/hid/hid_device.h"
#include "usb_hid_ffb_desc.h"
extern const uint8_t hid_2ffb_desc[USB_HID_2FFB_REPORT_DESC_SIZE];

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_2ffb_desc;
}

// Ordem das interfaces igual Firmware-Merged: HID primeiro, CDC depois.
// FFBeast (que funciona FFB no mesmo hw) segue essa ordem. Windows pode ser
// sensível à ordem pra binding de FFB.
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_CDC_CTRL,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

// Endpoint numbers match ODrive stock CDC macros (CDC_*_EP) pra que o shim
// em usb_tinyusb_glue.cpp possa rotear pelo endpoint_pair sem tradução.
// HID usa EP 0x83 IN / 0x02 OUT (espaço livre, não conflita com CDC).
#define EPNUM_CDC_NOTIF   0x82
#define EPNUM_CDC_OUT     0x01
#define EPNUM_CDC_IN      0x81
#define EPNUM_HID_IN      0x83
#define EPNUM_HID_OUT     0x02

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP | TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 5 /* iInterface */, HID_ITF_PROTOCOL_NONE,
        USB_HID_2FFB_REPORT_DESC_SIZE, EPNUM_HID_IN, EPNUM_HID_OUT,
        CFG_TUD_HID_EP_BUFSIZE, 1 /* bInterval ms */),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_CTRL, 4 /* iInterface */, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---------- String Descriptors ----------
// Serial fixo "0001" igual Firmware-Merged. UID-derived gerava serial diferente
// por placa, e Configurator pode usar o serial pra cache de "device supported".
static const char* string_desc_arr[] = {
    (const char[]){0x09, 0x04},        // 0: language = en-US
    "OpenFFBoard",                     // 1: Manufacturer
    "ODrive-Wheel",                    // 2: Product
    "0001",                            // 3: Serial (estatico, igual Merged)
    "ODrive-Wheel CDC",                // 4: CDC interface name
    "ODrive-Wheel HID"                 // 5: HID interface name
};

static uint16_t desc_str_buf[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    if (index == 0) {
        desc_str_buf[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        desc_str_buf[1] = 0x0409;
        return desc_str_buf;
    }

    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
        return NULL;
    }

    const char* str = string_desc_arr[index];
    size_t chars = strlen(str);
    if (chars > 31) chars = 31;

    desc_str_buf[0] = (TUSB_DESC_STRING << 8) | (2 * chars + 2);
    for (size_t i = 0; i < chars; i++) {
        desc_str_buf[1 + i] = (uint16_t)str[i];
    }

    return desc_str_buf;
}

// ---------- HID get/set report callbacks — Fase 2c: bridge pra HidFFB ------
// ffb_task.cpp define as bridges (usbhid_hidGet_bridge / usbhid_hidOut_bridge)
// que encaminham pro globalHidHandler. Se ffb_task_init() ainda não rodou,
// bridges retornam 0/no-op e fallback preenche zeros (evita STALL).
extern uint16_t usbhid_hidGet_bridge(uint8_t report_id, hid_report_type_t type,
                                     uint8_t *buf, uint16_t reqlen);
extern void usbhid_hidOut_bridge(uint8_t report_id, hid_report_type_t type,
                                 const uint8_t *buf, uint16_t size);

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    uint16_t n = usbhid_hidGet_bridge(report_id, report_type, buffer, reqlen);
    if (n == 0) {
        // Fallback: preenche com zeros pra não STALL (Windows interpreta STALL = erro 10)
        uint16_t len = reqlen > 8 ? 8 : (reqlen > 0 ? reqlen : 1);
        for (uint16_t i = 0; i < len; i++) buffer[i] = 0;
        return len;
    }
    return n;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    usbhid_hidOut_bridge(report_id, report_type, buffer, bufsize);
}
