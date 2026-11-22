# HG8245V2 stock optical module profiles

Source in the NOR root filesystem: `etc/wap/HG8245V2/hw_ldsp_cfg.xml`, subtree `OPTIC`. Cfg1 supplies board-level configuration. Cfg2 through Cfg19 carry command 0x20005001.

| XML | Vendor identifier at A0h offset 96 | Part number at A0h offset 40 | Bias ADC offset in A2h |
|---|---|---|---|
| Cfg2 | PTB4J85-85654-SC/APC-KHL+ | — | 0x7c |
| Cfg3 | ITR-D3T-HW6 | — | 0x6c |
| Cfg4 | — | — | 0x64 |
| Cfg5 | — | HPSF212 | 0x64 |
| Cfg6 | PTB3J85-5638N-SC/APC-KNS+ | PTB3J85-5638N-S | 0x64 |
| Cfg7 | — | RTXM167-407 | 0x64 |
| Cfg8 | PTB3J85-5638N-SC/APC-KN+ | PTB3J85-5638N-S | 0x64 |
| Cfg9 | SFA-34-24T-HP-CDE-HW-3 | — | 0x64 |
| Cfg10 | SFU-34-24T-HP-CDE-HW-4 | SFU3424THPCDEHW | 0x64 |
| Cfg11 | — | RTXM167-407-C10 | 0x64 |
| Cfg12 | — | LTB34D3-CC+ | 0x64 |
| Cfg13 | SFU-34-GB-P2-CDFB-HW | SFU34GBP2CDFBHW | 0x64 |
| Cfg14 | — | RTXM169-504 | 0x64 |
| Cfg15 | — | LTB3468L-CHG+ | 0x64 |
| Cfg16 | — | LTB3468L-BHG+ | 0x64 |
| Cfg17 | — | LTB3468L-CCG+ | 0x64 |
| Cfg18 | — | LTB3468L-BCG+ | 0x64 |
| Cfg19 | default-optic | default-optic | 0x64 |

## I²C during initialization

Stock ARM module `hw_module_optic.ko`, SHA-256 `78d41b416afc984f0e0fdbca19f1acf601f72c7d535502751419eaea04dd70ba`. Text addresses are ELF section offsets.

The matching profile calls `hw_drv_optic_init` at 0x3a8c. At 0x387c and 0x38e4 the type check reads 32 bytes at 0x60 and 16 at 0x28 respectively from 7-bit address 0x50. The HPSF212 prefix adds six date bytes at 0x54. Calibration mode is read at 0x5c. Each register read selects its offset before reading one byte. Across all 16 named profiles the successful matching invocation wrote zero data bytes to either I²C address in ARM replay. The 55 or 49 reads exclude any earlier attempts to match other profiles and subsequent monitoring. The SoC-level init and LOS callbacks were intercepted in replay. Their GPON branches use the datapath SDK.

The only direct I²C write call in this ARM module is at 0x2dfc inside `hw_drv_optic_threshold_set`. A separate command writes four 16-bit thresholds MSB first, eight single-byte writes to 0x51 starting at the supplied offset. This is outside the matched initialization call. No 8051 payload or loader was identified in this module.

Replay with `python tools/replay_stock_gpon_rx.py` from the project directory with Unicorn and pyelftools installed. The EEPROM responses are modeled, including the part fields. A physical boot-time bus capture was unavailable.

## Проверка аппаратного I²C SD5113

На плате Linux зарегистрировал адаптер `SD5113 hardware I2C`. При отключённом волокне два чтения HPSF2120 дали одинаковые PN и RX: `raw=11`, −45,47 дБм. Затем через hwmon прочитаны температура, напряжение питания, ток и мощность. [Фрагмент UART](hg8245-gpon-rx.uart.txt)

Драйвер обращается к окну с базой `0x16151000` и проверяет идентификатор DesignWare `0x44570140` при инициализации. Успешные чтения на плате подтверждают обычные транзакции EEPROM. Обработка ошибок и тайм-аутов проверяется отдельно на модели контроллера. [Драйвер I²C](../openwrt-target/target/linux/sd5113/files-6.6/drivers/i2c/busses/i2c-sd5113.c) · [Тест контроллера](../tools/test_i2c_sd5113.py)
