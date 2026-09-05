
#include "stm32_spi_arbiter.hpp"
#include "stm32_system.h"
#include "utils.hpp"
#include <cmsis_os.h>

bool equals(const SPI_InitTypeDef& lhs, const SPI_InitTypeDef& rhs) {
  return (lhs.Mode == rhs.Mode)
      && (lhs.Direction == rhs.Direction)
      && (lhs.DataSize == rhs.DataSize)
      && (lhs.CLKPolarity == rhs.CLKPolarity)
      && (lhs.CLKPhase == rhs.CLKPhase)
      && (lhs.NSS == rhs.NSS)
      && (lhs.BaudRatePrescaler == rhs.BaudRatePrescaler)
      && (lhs.FirstBit == rhs.FirstBit)
      && (lhs.TIMode == rhs.TIMode)
      && (lhs.CRCCalculation == rhs.CRCCalculation)
      && (lhs.CRCPolynomial == rhs.CRCPolynomial);
}

bool Stm32SpiArbiter::acquire_task(SpiTask* task) {
    return !__atomic_exchange_n(&task->is_in_use, true, __ATOMIC_SEQ_CST);
}

void Stm32SpiArbiter::release_task(SpiTask* task) {
    task->is_in_use = false;
}

bool Stm32SpiArbiter::start() {
    if (!task_list_) {
        return false;
    }

    SpiTask& task = *task_list_;
    if (!equals(task.config, hspi_->Init)) {
        HAL_SPI_DeInit(hspi_);
        hspi_->Init = task.config;
        HAL_SPI_Init(hspi_);
        __HAL_SPI_ENABLE(hspi_);
    }
    task.ncs_gpio.write(false);
    
    HAL_StatusTypeDef status = HAL_ERROR;

    if (hspi_->hdmatx->State != HAL_DMA_STATE_READY || hspi_->hdmarx->State != HAL_DMA_STATE_READY) {
        // This can happen if the DMA or interrupt priorities are not configured properly.
        status = HAL_BUSY;
    } else if (task.tx_buf && task.rx_buf) {
        status = HAL_SPI_TransmitReceive_DMA(hspi_, (uint8_t*)task.tx_buf, task.rx_buf, task.length);
    } else if (task.tx_buf) {
        status = HAL_SPI_Transmit_DMA(hspi_, (uint8_t*)task.tx_buf, task.length);
    } else if (task.rx_buf) {
        status = HAL_SPI_Receive_DMA(hspi_, task.rx_buf, task.length);
    }

    if (status != HAL_OK) {
        task.ncs_gpio.write(true);
    }

    return status == HAL_OK;
}

void Stm32SpiArbiter::transfer_async(SpiTask* task) {
    task->next = nullptr;
    
    // Append new task to task list.
    // We could try to do this lock free but we could also use our time for useful things.
    SpiTask** ptr = &task_list_;
    CRITICAL_SECTION() {
        while (*ptr)
            ptr = &(*ptr)->next;
        *ptr = task;
    }

    // If the list was empty before, kick off the SPI arbiter now
    if (ptr == &task_list_) {
        if (!start()) {
            if (task->on_complete) {
                (*task->on_complete)(task->on_complete_ctx, false);
            }
        }
    }
}

// Transfer POLLED (sem DMA, sem arbiter async). Usa HAL_SPI_TransmitReceive
// blocking, igual ao firmware v0.5.1 que comprovadamente funciona na MKS Mini.
// O DMA async do v0.5.6 travava: DMA ISR nao disparava no nosso HW (conflito
// com DMA streams do USB OTG? priorities IRQ? MSP init?). Confirmado via
// odrv.testspi! (HAL_SPI polled direto funciona, arbiter+DMA nao).
bool Stm32SpiArbiter::transfer(SPI_InitTypeDef config, Stm32Gpio ncs_gpio, const uint8_t* tx_buf, uint8_t* rx_buf, size_t length, uint32_t timeout_ms) {
    if (!hspi_) return false;

    // Re-entrancy guard. A higher-priority caller (encoder read in sampling_cb
    // or the MT6835 encoder thread) can preempt a lower-priority transfer
    // (DRV8301 fault check). If it does, skip rather than touch the peripheral
    // mid-transfer — a re-init over an in-flight transfer corrupts the SPI and
    // hangs the bus. Critical for mode-3 encoders (MT6835) whose config differs
    // from the DRV (mode 1), forcing a re-config on every switch. A skipped
    // encoder read is harmless (counts as a COM miss; the PLL interpolates).
    // Atomic exchange (not test-then-set): with several *threads* sharing this
    // path (encoder thread + axis thread) a context switch between the check
    // and the store could let both proceed — same idiom as acquire_task().
    if (__atomic_exchange_n(&in_transfer_, true, __ATOMIC_ACQUIRE)) return false;

    // Reconfiguração LEVE do SPI (só CR1/CR2), NÃO HAL_SPI_DeInit/Init.
    // O MT6835 (SPI mode 3) e o DRV8301 (mode 1) diferem só na polaridade do
    // clock, então a config muda a cada switch encoder<->DRV. Um DeInit/Init
    // completo refaz MspInit (GPIO + clock + DMA) e, chamado a 8 kHz no ISR de
    // alta prioridade, sufoca a CPU e derruba o loop USB/FFB — o HID OUT caía
    // de ~670 Hz pra 2-45 Hz. GPIO/clock/DMA já foram inicializados no boot
    // (MX_SPI3_Init); só precisamos reescrever os registros de protocolo,
    // exatamente como HAL_SPI_Init faz, mas sem o MspInit. Custa alguns
    // registradores (<1 µs) em vez de dezenas de µs.
    if (!equals(config, hspi_->Init)) {
        __HAL_SPI_DISABLE(hspi_);
        hspi_->Init = config;
        WRITE_REG(hspi_->Instance->CR1,
                  (config.Mode | config.Direction | config.DataSize |
                   config.CLKPolarity | config.CLKPhase | (config.NSS & SPI_CR1_SSM) |
                   config.BaudRatePrescaler | config.FirstBit | config.CRCCalculation));
        WRITE_REG(hspi_->Instance->CR2,
                  (((config.NSS >> 16U) & SPI_CR2_SSOE) | config.TIMode));
        __HAL_SPI_ENABLE(hspi_);
    }

    // Assert CS
    ncs_gpio.write(false);

    // Polled transfer
    HAL_StatusTypeDef st = HAL_ERROR;
    if (tx_buf && rx_buf) {
        st = HAL_SPI_TransmitReceive(hspi_, (uint8_t*)tx_buf, rx_buf, length, timeout_ms);
    } else if (tx_buf) {
        st = HAL_SPI_Transmit(hspi_, (uint8_t*)tx_buf, length, timeout_ms);
    } else if (rx_buf) {
        st = HAL_SPI_Receive(hspi_, rx_buf, length, timeout_ms);
    }

    // Release CS
    ncs_gpio.write(true);

    __atomic_store_n(&in_transfer_, false, __ATOMIC_RELEASE);
    return st == HAL_OK;
}

void Stm32SpiArbiter::on_complete() {
    if (!task_list_) {
        return; // this should not happen
    }

    // Wrap up transfer
    task_list_->ncs_gpio.write(true);
    if (task_list_->on_complete) {
        (*task_list_->on_complete)(task_list_->on_complete_ctx, true);
    }

    // Start next task if any
    SpiTask* next = nullptr;
    CRITICAL_SECTION() {
        next = task_list_ = task_list_->next;
    }
    if (next) {
        start();
    }
}
