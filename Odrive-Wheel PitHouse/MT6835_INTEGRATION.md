# Intégration MT6835 — SPI absolu 21 bits

Référence d'intégration pour ajouter l'encodeur **MagnTek MT6835** (21 bits, lecture SPI absolue) au firmware Odrive-Wheel et à une interface de configuration.

- **Cible matérielle de référence** : ODESC V4.2 (clone ODrive v3.6, STM32F405). Compatible MKS XDrive Mini.
- **Firmware** : branche `encoder`. Mode `SPI_ABS_MT6835 = 261`.
- **Statut** : validé sur banc (lecture, CRC, calibration d'offset, FFB) sur ODESC V4.2.
- **Datasheet** : `MT6835_Rev.1.3.pdf`, §7.6 (protocole SPI), §7.6.9 (burst read), §7.6.8 (layout angle + CRC).

---

## 1. Le mode encodeur

| | |
|---|---|
| Nom | `SPI_ABS_MT6835` |
| Valeur | **261** (`0x105`) |
| Famille | absolu mono-tour, 21 bits |
| Entrée dropdown (JS) | `261: 'SPI_ABS_MT6835'` |

Placé dans l'enum `ODrive.Encoder.Mode` après `SPI_ABS_MA732 = 260` :

```
INCREMENTAL      = 0
HALL             = 1
SINCOS           = 2
SPI_ABS_CUI      = 256
SPI_ABS_AMS      = 257   (AS5047)
SPI_ABS_AEAT     = 258
SPI_ABS_RLS      = 259
SPI_ABS_MA732    = 260
SPI_ABS_MT6835   = 261   <-- nouveau
```

---

## 2. Propriétés de configuration

Protocole ASCII ODrive : écriture `w <path> <val>`, lecture `r <path>`.

| Propriété | Valeur | Note |
|---|---|---|
| `axis0.encoder.config.mode` | `261` | active le MT6835 |
| `axis0.encoder.config.cpr` | `2097152` | 2²¹ — **obligatoire** |
| `axis0.encoder.config.abs_spi_cs_gpio_pin` | `6` (ODESC V4.2) · `7` (MKS) | GPIO du CS |
| `axis0.encoder.config.use_index` | `0` | absolu, pas de pulse Z |
| `axis0.encoder.config.direction` | `±1` | déterminé par DIR_FIND |
| `axis0.encoder.config.bandwidth` | `100`–`500` | bande passante PLL (Hz) |
| `axis0.encoder.config.pre_calibrated` | `1` | après calibration réussie |
| `axis0.motor.config.pre_calibrated` | `1` | moteur déjà calibré |
| `axis0.config.startup_closed_loop_control` | `1` | auto-arme le FFB au boot |
| `axis0.config.startup_encoder_offset_calibration` | `0` | inutile (absolu pré-calibré) |
| `axis0.config.startup_motor_calibration` | `0` | inutile (pré-calibré) |

---

## 3. Câblage — port SPI ODESC V4.2

Connecteur SPI (le plus à droite), ordre des broches : `GND · MOSI · MISO · SCK · CS · 3.3V`

| ODESC | STM32 | MT6835 (TSSOP-16) |
|---|---|---|
| SCK | PC10 | SCK (pin 7) |
| MISO | PC11 | MISO (pin 5) |
| MOSI | PC12 | MOSI (pin 6) |
| CS | GPIO6 / PB2 | CSN (pin 8) |
| 3.3V | — | VDD |
| GND | — | GND |

> Le bus SPI3 (PC10/11/12) est partagé avec le DRV8301 et l'AS5047 embarqué (sur MKS). Chaque esclave a son propre CS et met MISO en haute impédance hors sélection. L'ODESC V4.2 n'a pas d'encodeur embarqué : le bus et le CS sont libres.

---

## 4. Séquence de mise en service

```text
1) Config encodeur
   w axis0.encoder.config.mode 261
   w axis0.encoder.config.cpr 2097152
   w axis0.encoder.config.abs_spi_cs_gpio_pin 6
   w axis0.encoder.config.use_index 0
   sys.save!                  -> reboot

2) Vérifier la lecture
   sys.encraw!                -> last != 0xFFFF et pos varie en tournant l'axe

3) Calibration d'offset (moteur sous alim DC)
   w axis0.requested_state 7  -> le moteur tourne ~1/2 tour lentement
   r axis0.error              -> 0
   r axis0.encoder.error      -> 0

4) Verrouiller
   w axis0.encoder.config.pre_calibrated 1
   w axis0.motor.config.pre_calibrated 1
   w axis0.config.startup_closed_loop_control 1
   sys.save!                  -> reboot

5) Centrer le volant (encodeur absolu)
   (volant au centre physique)
   axis.zeroenc!
   sys.save!
```

États : `1` = IDLE, `7` = ENCODER_OFFSET_CALIBRATION, `8` = CLOSED_LOOP_CONTROL.

---

## 5. Commandes de diagnostic / lecture live

| Commande | Retour / rôle |
|---|---|
| `sys.encraw!` | `ok=N pty=N ef=N xfr=N last=0xNNNN pos=NNNN` |
| `axis.curpos!` | position courante |
| `axis.curspd!` / `axis.curaccel!` | vitesse / accélération |
| `axis.zeroenc!` | capture la position courante comme centre (EXEC) |
| `axis.zeroofs` | offset de zéro persistant en degrés (GET/SET) |
| `r axis0.encoder.calib_scan_response` | counts mesurés pendant la calibration |
| `r axis0.error` / `r axis0.encoder.error` | codes d'erreur |
| `sys.errors!` / `sys.errorsclr!` | liste / efface les erreurs |
| `sys.save!` / `sys.reboot!` | persiste / redémarre |

### Décodage de `sys.encraw!` (spécifique MT6835)

| Champ | Signification |
|---|---|
| `ok` | lectures avec CRC valide (doit monter = bus sain) |
| `pty` | échecs CRC (réutilise le compteur de parité) |
| `ef` | inutilisé en MT6835 |
| `xfr` | échecs de transfert SPI (problème de bus) |
| `last` | 1er mot de données `[reg0x003][reg0x004]` — `0xFFFF` = aucune réponse |
| `pos` | angle absolu 21 bits (0 … 2 097 151) |

**Grille de dépannage** :

| Observation | Cause probable |
|---|---|
| `last=0xFFFF`, `pos` figé | aucune réponse : CS faux, MISO/MOSI inversés, pas de 3.3V |
| `xfr` qui grimpe | bus instable : court-circuit / continuité SCK ou CS |
| `pty` grimpe, `ok=0`, mais `last` varie | données reçues mais CRC rejette (init/poly CRC) |
| `ok` grimpe vite | sain |
| `CPR_POLEPAIRS_MISMATCH` à la calib | lectures qui décrochent quand le moteur tourne, ou `pole_pairs` faux |

---

## 6. Détails du protocole SPI (pour portage / firmware)

- **Mode SPI 3** : CPOL=1, CPHA=1, MSB first.
- **Burst Read Angle** (datasheet §7.6.9) : commande `C3..C0 = 1010` (0xA) + adresse `0x003` → mot 16 bits `0xA003`. Le chip émet ensuite en continu les registres `0x003 → 0x006`.
- **Transaction** : 3 frames de 16 bits, CS maintenu bas tout du long.
  - word0 (TX `0xA003`) : commande ; MISO en Hi-Z → RX0 ignoré.
  - word1 (TX `0x0000`) : RX1 = `[reg0x003][reg0x004]`.
  - word2 (TX `0x0000`) : RX2 = `[reg0x005][reg0x006]`.

### Layout des registres (datasheet §7.6.8)

| Reg | Bits |
|---|---|
| 0x003 | `ANGLE[20:13]` |
| 0x004 | `ANGLE[12:5]` |
| 0x005 | `ANGLE[4:0]` (bits 7:3) + `STATUS[2:0]` (bits 2:0) |
| 0x006 | `CRC[7:0]` |

### Décodage

```c
uint8_t  b003 = rx1 >> 8;          // ANGLE[20:13]
uint8_t  b004 = rx1 & 0xFF;        // ANGLE[12:5]
uint8_t  b005 = rx2 >> 8;          // ANGLE[4:0]<<3 | STATUS[2:0]
uint8_t  crc  = rx2 & 0xFF;        // CRC[7:0]

uint32_t angle  = ((uint32_t)b003 << 13) | ((uint32_t)b004 << 5) | (b005 >> 3); // 0..2097151
uint8_t  status = b005 & 0x07;
```

- **STATUS** : bit0 = survitesse, bit1 = champ magnétique faible, bit2 = sous-tension.
- **CRC8** = CRC-8/SMBUS : polynôme `0x07` (X⁸+X²+X+1), init `0x00`, sans réflexion, sans XOR final, calculé sur `b003, b004, b005`, comparé à `b006`. (Confirmé correct sur hardware.)
- **CPR** = 2 097 152.

---

## 7. Comportement (notes UX)

- **Absolu** : position connue dès le boot, aucune recherche d'index Z, prêt immédiatement.
- **Centre** : la position au boot = angle absolu de l'aimant (arbitraire physiquement). Il faut **capturer le zéro une fois** (`axis.zeroenc!` + `sys.save!`). L'offset (`zeroOffset_`, en degrés) est persisté en flash et **revient correctement à chaque reboot** car l'aimant rend le même angle à la même position physique.
- **Pipeline position** : `degrees = (shadow_count / cpr) * 360 − zeroOffset` → `pos_f = degrees / (rangeDegrees/2)` (borné ±1) → `pos_scaled_16b = pos_f * 32767` (sortie HID).
- **Mono-tour** : le MT6835 est absolu sur **un tour**. En direct-drive avec une plage > 360°, la position au boot n'est connue qu'à un tour près ; le multi-tour s'accumule ensuite correctement en fonctionnement.

---

## 8. ⚠️ Dépendance firmware critique — garde de ré-entrance SPI

Le MT6835 est en **mode 3** alors que le DRV8301 (et l'AS5047) sont en **mode 1**. L'arbitre SPI réinitialise donc le périphérique (`HAL_SPI_DeInit/Init`) à chaque alternance encodeur↔DRV. La lecture encodeur tourne dans `sampling_cb()`, une **ISR haute priorité** : sans protection, elle préempte un transfert DRV en cours et réinitialise le SPI au milieu → le `HAL_SPI_TransmitReceive` polled attend un flag qui ne vient jamais (le SysTick ne peut pas préempter cette ISR, donc le timeout ne se déclenche jamais) → **freeze total, moteur calé, reboot obligatoire**.

**Correctif** : un flag de ré-entrance `in_transfer_` dans `Stm32SpiArbiter::transfer()` (`stm32_spi_arbiter.cpp` / `.hpp`). Si un transfert est déjà en cours, l'appelant qui préempte **saute son tour** au lieu de toucher le périphérique (un saut de lecture encodeur est inoffensif, la PLL interpole). La priorité d'interruption stricte garantit que le transfert haute priorité se termine entièrement avant que le bas-priorité ne reprenne — pas de chevauchement possible.

> Indispensable pour tout encodeur en mode 3 (MT6835, mais aussi AEAT / MA732) dès que le moteur est armé.

---

## 9. Fichiers modifiés (firmware)

| Fichier | Modification |
|---|---|
| `ODrive-fw-v0.5.6/.../Firmware/odrive-interface.yaml` | enum `SPI_ABS_MT6835: 0x105` |
| `ODrive-fw-v0.5.6/.../Firmware/autogen/interfaces.hpp` | `MODE_SPI_ABS_MT6835 = 261` |
| `ODrive-fw-v0.5.6/.../Firmware/MotorControl/encoder.cpp` | config SPI mode 3, trame burst, décodage 21 bits, CRC8, switches de mode |
| `ODrive-fw-v0.5.6/.../Firmware/MotorControl/encoder.hpp` | buffers `[3]`, champ `mt6835_status_` |
| `ODrive-fw-v0.5.6/.../Firmware/Drivers/STM32/stm32_spi_arbiter.cpp` + `.hpp` | garde de ré-entrance `in_transfer_` |
| `Odrive-Wheel/tools/odrive-wheel.html` | dropdown mode 261, hints CS=6 (ODESC) / CPR |
