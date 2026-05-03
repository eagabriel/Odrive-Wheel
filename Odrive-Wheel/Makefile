# =============================================================================
# Odrive-Wheel — ODrive v0.5.6 + OpenFFBoard FFB stack para MKS XDrive Mini
# Espelha Tupfile.lua de ODrive-fw-v0.5.6 + adiciona FFB (HidFFB +
# EffectsCalculator). CDC USB aceita ASCII (enable_ascii_protocol_on_usb
# default true) + parser OpenFFBoard.
# =============================================================================

TARGET    = odrive-wheel
BUILD_DIR = build

# --- Toolchain ---------------------------------------------------------------
PREFIX = arm-none-eabi-
CC  = $(PREFIX)gcc
CXX = $(PREFIX)g++
AS  = $(PREFIX)gcc -x assembler-with-cpp
CP  = $(PREFIX)objcopy
SZ  = $(PREFIX)size

HEX = $(CP) -O ihex
# --gap-fill=0xFF é CRÍTICO. objcopy default preenche gaps com 0x00, e como
# nosso linker tem buracos entre setores reservados (S1/S2 da FFB EE entre
# vetores em S0 e .text em S3), gravar via dfu-util com 0x00 nesses gaps
# CORROMPE a EE — flash só vai 1->0 sem erase, então 0x00 stuck até erase
# manual. Com 0xFF (= flash erased), gravar sobre setor erased é no-op.
BIN = $(CP) -O binary -S --gap-fill=0xFF

# --- Root paths --------------------------------------------------------------
ODRIVE_ROOT = ../ODrive-fw-v0.5.6/ODrive-fw-v0.5.6/Firmware
FFB_ROOT    = ../OpenFFBoard-master/OpenFFBoard-master/Firmware

HAL_DIR     = $(ODRIVE_ROOT)/ThirdParty/STM32F4xx_HAL_Driver
CMSIS_DIR   = $(ODRIVE_ROOT)/ThirdParty/CMSIS
USBLIB_DIR  = $(ODRIVE_ROOT)/ThirdParty/STM32_USB_Device_Library
TUSB_DIR    = $(FFB_ROOT)/FFBoard/USB
RTOS_DIR    = $(ODRIVE_ROOT)/ThirdParty/FreeRTOS/Source
BOARD_DIR   = $(ODRIVE_ROOT)/Board/v3
MC_DIR      = $(ODRIVE_ROOT)/MotorControl
FIBRE_DIR   = $(ODRIVE_ROOT)/fibre-cpp
AUTOGEN_DIR = $(ODRIVE_ROOT)/autogen
DRV_DIR     = $(ODRIVE_ROOT)/Drivers
COMM_DIR    = $(ODRIVE_ROOT)/communication

# --- MCU flags ---------------------------------------------------------------
CPU       = -mcpu=cortex-m4
FPU       = -mfpu=fpv4-sp-d16
FLOAT_ABI = -mfloat-abi=hard
MCU       = $(CPU) -mthumb $(FPU) $(FLOAT_ABI)

# --- Defines -----------------------------------------------------------------
# Build MKS XDrive Mini style: v3.6-56V (mesmo que MKS v0.5.1 oficial usa).
# - HW_VERSION_VOLTAGE=56: OVP trip = 59.92V (seguro pra 24V real).
# - VBUS divider ja hardcoded como 19:1 em low_level.cpp (nosso patch previo).
# - USB_PROTOCOL_NATIVE: CDC endpoint aceita ASCII via enable_ascii_protocol_on_usb.
# Phase 2a: USB transport is TinyUSB. CDC endpoint carries ASCII (ODrive's
# enable_ascii_protocol_on_usb default true). Fibre "native" endpoint not
# exposed in our descriptor — `#if defined(USB_PROTOCOL_NATIVE)` blocks stay
# defined so interface_usb.cpp stays identical to stock (fibre_over_cdc
# instances sit idle, harmless).
#
# Phase 4.x — FFB EEPROM emulada movida de S10/S11 (0x080C0000) para
# S1/S2 (0x08004000). Motivo: ODrive 0.5.6 stm32_nvm.c hardcoda S10/S11
# para STM32F405 (Drivers/STM32/stm32_nvm.c linhas 44-49), o que causava
# colisão direta — ambos subsistemas escrevendo no mesmo bloco físico,
# com layouts incompatíveis. S1+S2 (16K cada) ficavam livres no linker
# (.text começa em S3). 16K dá ~4095 entries antes de page swap; erase
# de 16K = ~250ms (vs ~1.5s do setor 128K).
DEFS = -DSTM32F405xx \
       -DUSE_HAL_DRIVER \
       -DHSE_VALUE=8000000 \
       -DARM_MATH_CM4 \
       -DHW_VERSION_MAJOR=3 \
       -DHW_VERSION_MINOR=6 \
       -DHW_VERSION_VOLTAGE=56 \
       -DUSB_PROTOCOL_NATIVE \
       -DUART_PROTOCOL_ASCII \
       -DUSE_EEPROM_EMULATION \
       -DEEPROM_START_ADDRESS=0x08004000 \
       -DPAGE_SIZE=0x4000 \
       -DPAGE0_ID=FLASH_SECTOR_1 \
       -DPAGE1_ID=FLASH_SECTOR_2 \
       -DFIBRE_ENABLE_SERVER=1 \
       -DFIBRE_ENABLE_CLIENT=0 \
       -DFIBRE_ENABLE_EVENT_LOOP=0 \
       -DFIBRE_ALLOW_HEAP=0 \
       -DFIBRE_MAX_LOG_VERBOSITY=0 \
       -DFIBRE_DEFAULT_LOG_VERBOSITY=0 \
       -DFIBRE_ENABLE_LIBUSB_BACKEND=0 \
       -DFIBRE_ENABLE_TCP_SERVER_BACKEND=0 \
       -DFIBRE_ENABLE_TCP_CLIENT_BACKEND=0 \
       -D__weak=__attribute__\(\(weak\)\) \
       -D__packed=__attribute__\(\(__packed__\)\)

# --- Sources: locais (patches MKS XDrive Mini) -------------------------------
# Usamos o board.cpp upstream direto — motors[1] ficam em memoria valida
# pois AXIS_COUNT=2 (default stock). MKS main.cpp patch opcional esta em
# motor setup (axes[0] only) — mas como motor[1] nao tem hardware, setup
# falha graciosamente com timeout SPI.
C_SOURCES_LOCAL = \
  src/instrumentation_stubs.c \
  src/usb_descriptors.c \
  src/usb_hid_2ffb_desc.c \
  src/eeprom.c \
  src/eeprom_addresses.c \
  src/cmdparser.c

CPP_SOURCES_LOCAL = \
  src/usb_tinyusb_glue.cpp \
  src/board_v3.cpp \
  src/odrive_bridge.cpp \
  src/ffb_task.cpp \
  src/cmd_table.cpp \
  src/HidFFB.cpp \
  src/EffectsCalculator.cpp \
  src/Filters.cpp \
  src/UsbHidHandler.cpp \
  src/PersistentStorage.cpp \
  src/gpio_inputs.cpp \
  src/cthread.cpp \
  src/cmutex.cpp \
  src/csemaphore.cpp \
  src/cqueue.cpp \
  src/ccondition_variable.cpp

ASM_SOURCES_LOCAL =

# --- Sources: HAL ------------------------------------------------------------
C_SOURCES_HAL = \
  $(HAL_DIR)/Src/stm32f4xx_hal.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_adc.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_adc_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_can.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_cortex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_dma.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_dma_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_flash.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_flash_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_flash_ramfunc.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_gpio.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_i2c.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_i2c_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_pcd.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_pcd_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_pwr.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_pwr_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_rcc.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_rcc_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_spi.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_tim.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_tim_ex.c \
  $(HAL_DIR)/Src/stm32f4xx_hal_uart.c \
  $(HAL_DIR)/Src/stm32f4xx_ll_usb.c

# --- Sources: TinyUSB (replacing STM32 USB Device Library) -------------------
# Phase 2a: TinyUSB CDC-only. Our src/usb_tinyusb_glue.cpp shims the ST USB
# lib APIs that interface_usb.cpp and communication.cpp call.
C_SOURCES_USBLIB = \
  $(TUSB_DIR)/tusb.c \
  $(TUSB_DIR)/common/tusb_fifo.c \
  $(TUSB_DIR)/device/usbd.c \
  $(TUSB_DIR)/device/usbd_control.c \
  $(TUSB_DIR)/class/cdc/cdc_device.c \
  $(TUSB_DIR)/class/hid/hid_device.c \
  $(TUSB_DIR)/portable/synopsys/dwc2/dcd_dwc2.c \
  $(TUSB_DIR)/portable/synopsys/dwc2/dwc2_common.c

# --- Sources: FreeRTOS 9 -----------------------------------------------------
C_SOURCES_RTOS = \
  $(RTOS_DIR)/croutine.c \
  $(RTOS_DIR)/event_groups.c \
  $(RTOS_DIR)/list.c \
  $(RTOS_DIR)/queue.c \
  $(RTOS_DIR)/stream_buffer.c \
  $(RTOS_DIR)/tasks.c \
  $(RTOS_DIR)/timers.c \
  $(RTOS_DIR)/CMSIS_RTOS/cmsis_os.c \
  $(RTOS_DIR)/portable/MemMang/heap_4.c \
  $(RTOS_DIR)/portable/GCC/ARM_CM4F/port.c

# --- Sources: Board/v3 -------------------------------------------------------
# Phase 2a: usb_device.c, usbd_conf.c, usbd_cdc_if.c, usbd_desc.c REMOVED —
# replaced by src/usb_tinyusb_glue.cpp + src/usb_descriptors.c.
C_SOURCES_BOARD = \
  $(BOARD_DIR)/Src/gpio.c \
  $(BOARD_DIR)/Src/spi.c \
  $(BOARD_DIR)/Src/tim.c \
  $(BOARD_DIR)/Src/adc.c \
  $(BOARD_DIR)/Src/can.c \
  $(BOARD_DIR)/Src/dma.c \
  $(BOARD_DIR)/Src/i2c.c \
  $(BOARD_DIR)/Src/usart.c \
  $(BOARD_DIR)/Src/stm32f4xx_hal_msp.c \
  $(BOARD_DIR)/Src/stm32f4xx_hal_timebase_TIM.c \
  $(BOARD_DIR)/Src/stm32f4xx_it.c \
  $(BOARD_DIR)/Src/system_stm32f4xx.c \
  $(BOARD_DIR)/Src/freertos.c \
  $(BOARD_DIR)/Src/main.c

# --- Sources: autogen --------------------------------------------------------
C_SOURCES_AUTOGEN = \
  $(AUTOGEN_DIR)/version.c

# --- Sources: MotorControl ---------------------------------------------------
CPP_SOURCES_MC = \
  $(MC_DIR)/utils.cpp \
  $(MC_DIR)/low_level.cpp \
  $(MC_DIR)/axis.cpp \
  $(MC_DIR)/motor.cpp \
  $(MC_DIR)/thermistor.cpp \
  $(MC_DIR)/encoder.cpp \
  $(MC_DIR)/endstop.cpp \
  $(MC_DIR)/acim_estimator.cpp \
  $(MC_DIR)/mechanical_brake.cpp \
  $(MC_DIR)/controller.cpp \
  $(MC_DIR)/foc.cpp \
  $(MC_DIR)/open_loop_controller.cpp \
  $(MC_DIR)/oscilloscope.cpp \
  $(MC_DIR)/sensorless_estimator.cpp \
  $(MC_DIR)/trapTraj.cpp \
  $(MC_DIR)/pwm_input.cpp \
  $(MC_DIR)/main.cpp

C_SOURCES_MC = \
  $(MC_DIR)/arm_sin_f32.c \
  $(MC_DIR)/arm_cos_f32.c

# --- Sources: Board/v3 startup ASM ------------------------------------------
ASM_SOURCES_BOARD = \
  $(BOARD_DIR)/startup_stm32f405xx.s

# --- Sources: Drivers (STM32 + DRV8301) + board.cpp + comm + fibre -----------
CPP_SOURCES_DRV = \
  $(DRV_DIR)/STM32/stm32_system.cpp \
  $(DRV_DIR)/STM32/stm32_gpio.cpp \
  $(DRV_DIR)/STM32/stm32_spi_arbiter.cpp \
  $(DRV_DIR)/DRV8301/drv8301.cpp \
  $(COMM_DIR)/communication.cpp \
  $(COMM_DIR)/ascii_protocol.cpp \
  $(COMM_DIR)/interface_usb.cpp \
  $(COMM_DIR)/interface_uart.cpp \
  $(COMM_DIR)/interface_i2c.cpp \
  $(COMM_DIR)/can/can_simple.cpp \
  $(COMM_DIR)/can/odrive_can.cpp

C_SOURCES_DRV = \
  $(DRV_DIR)/STM32/stm32_nvm.c \
  $(ODRIVE_ROOT)/syscalls.c \
  $(ODRIVE_ROOT)/FreeRTOS-openocd.c

CPP_SOURCES_FIBRE = \
  $(FIBRE_DIR)/fibre.cpp \
  $(FIBRE_DIR)/channel_discoverer.cpp \
  $(FIBRE_DIR)/legacy_protocol.cpp

# --- All sources -------------------------------------------------------------
C_SOURCES   = $(C_SOURCES_LOCAL) $(C_SOURCES_HAL) $(C_SOURCES_USBLIB) $(C_SOURCES_RTOS) \
              $(C_SOURCES_BOARD) $(C_SOURCES_AUTOGEN) $(C_SOURCES_MC) $(C_SOURCES_DRV)
CPP_SOURCES = $(CPP_SOURCES_LOCAL) $(CPP_SOURCES_MC) $(CPP_SOURCES_DRV) $(CPP_SOURCES_FIBRE)
ASM_SOURCES = $(ASM_SOURCES_LOCAL) $(ASM_SOURCES_BOARD)

# --- Include dirs ------------------------------------------------------------
INCLUDES = \
  -Iinc \
  -I$(ODRIVE_ROOT) \
  -I$(MC_DIR) \
  -I$(BOARD_DIR)/Inc \
  -I$(HAL_DIR)/Inc \
  -I$(HAL_DIR)/Inc/Legacy \
  -I$(CMSIS_DIR)/Include \
  -I$(CMSIS_DIR)/Device/ST/STM32F4xx/Include \
  -I$(USBLIB_DIR)/Core/Inc \
  -I$(USBLIB_DIR)/Class/CDC/Inc \
  -I$(TUSB_DIR) \
  -I$(TUSB_DIR)/common \
  -I$(TUSB_DIR)/device \
  -I$(TUSB_DIR)/osal \
  -I$(TUSB_DIR)/class/cdc \
  -I$(TUSB_DIR)/class/hid \
  -I$(TUSB_DIR)/portable/synopsys/dwc2 \
  -I$(RTOS_DIR)/include \
  -I$(RTOS_DIR)/CMSIS_RTOS \
  -I$(RTOS_DIR)/portable/GCC/ARM_CM4F \
  -I$(FIBRE_DIR)/include

# --- Optimization ------------------------------------------------------------
OPT ?= -O2

# --- Flags -------------------------------------------------------------------
COMMON_FLAGS = $(MCU) $(DEFS) $(INCLUDES) $(OPT) \
               -Wall -fdata-sections -ffunction-sections -g -gdwarf-2 \
               -ffast-math -fno-finite-math-only

CFLAGS   = $(COMMON_FLAGS) -std=gnu11 -MMD -MP -MF"$(@:%.o=%.d)"
CXXFLAGS = $(COMMON_FLAGS) -std=gnu++17 -Wno-register -fno-use-cxa-atexit -MMD -MP -MF"$(@:%.o=%.d)"
ASFLAGS  = $(MCU) -Wall -fdata-sections -ffunction-sections

LDSCRIPT = linker/STM32F405RGTx_FLASH.ld
LDFLAGS  = $(MCU) -specs=nosys.specs -specs=nano.specs \
           -u _printf_float -u _scanf_float \
           -T$(LDSCRIPT) \
           -L$(CMSIS_DIR)/Lib/GCC \
           -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
           -Wl,--gc-sections \
           -Wl,--undefined=uxTopUsedPriority \
           -Wl,--start-group -lc -lm -larm_cortexM4lf_math -Wl,--end-group

# --- Objects -----------------------------------------------------------------
C_OBJECTS   = $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o))
CPP_OBJECTS = $(addprefix $(BUILD_DIR)/,$(CPP_SOURCES:.cpp=.o))
ASM_OBJECTS = $(addprefix $(BUILD_DIR)/,$(ASM_SOURCES:.s=.o))
OBJECTS     = $(C_OBJECTS) $(CPP_OBJECTS) $(ASM_OBJECTS)

# --- Default -----------------------------------------------------------------
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

# --- Compile rules -----------------------------------------------------------
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(BIN) $< $@

# --- Flash (DFU) -------------------------------------------------------------
flash-dfu: $(BUILD_DIR)/$(TARGET).bin
	dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D $<

# --- Clean -------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/**/*.d)

.PHONY: all clean flash-dfu
