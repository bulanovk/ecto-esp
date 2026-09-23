# Прокси-порт над одним UART: два варианта

**Дата:** 2026-09-21 (вариант 2 добавлен 2026-09-22)
**Статус:** дизайн записан, реализация не начата
**Область:** `/wrk/esp-modbus/esp32-gw1.yaml` + новый компонент `components/baud_proxy/`

Оба варианта используют один и тот же прокси — `UARTComponent`-подкласс над реальным UART.
Различаются тем, **сколько хабов** и **откуда прокси берёт скорость**:

| | Вариант A (§1–§7) | Вариант 2 (§10) |
|---|---|---|
| Хабов | два, по одному на скорость | **один** |
| Прокси знает скорость | статически, свой экземпляр на группу | по **карте адрес→скорость** |
| Сериализация TX | лок с TTL (`hold_time`) | не нужна — хаб один |
| Арбитраж RX | обязателен (гейт `available`/`read_array`/`peek_byte`) | **не нужен** |
| Предусловие | нет | адреса slave разводимы |
| Гейт переустановки драйвера | — | сравнение текущей и целевой скорости (§10.2) |

Вариант 2 предпочтительнее, когда адреса разводимы (адрес реле ≠ адрес DTSU666): он снимает самую
хрупкую часть — арбитраж RX между двумя хабами.

---

## 1. Задача

Одна физическая пара проводов (`intPort`, GPIO17/16, DE на GPIO23, второй приёмопередатчик
невозможен) должна обслуживать устройства с **разными скоростями** — и при этом уметь в них писать.

| Группа | Устройства | Скорость | Адреса |
|---|---|---|---|
| A | DTSU666 | **9600** (жёстко) | `0x3` |
| B | 2 адаптера бойлера ectoControl + модуль реле 10 каналов | **19200** (у ectoControl фиксирована) | `0x1`, `0x2`, `0x3` |

Таймеры реле (`0x0020–0x0029`) в объём не входят.

**Решение:** два `modbus`-хаба, каждый на своём **прокси-порту** — `UARTComponent`-подклассе, который
знает свою скорость статически и владеет реальным UART по очереди. Скорость не вытаскивается из кадра.

---

## 2. Почему наивные подходы не работают

Всё проверено по установленному пакету ESPHome 2026.9.0 (`/tmp/espenv/.../esphome/`).

| Факт | Где | Следствие |
|---|---|---|
| `Modbus : public uart::UARTDevice` → `parent_` имеет тип `UARTComponent *` | `modbus/modbus.h:50`, `uart/uart.h:76` | хаб работает с **абстракцией** UART, а не с `IDFUARTComponent` |
| `UART_DEVICE_SCHEMA = {GenerateID(uart_id): use_id(UARTComponent)}` | `uart/__init__.py:380-384` | `modbus:` примет **любой** `UARTComponent` |
| `register_uart_device` → `var.set_uart_parent(parent)` | `uart/__init__.py:477-483` | прокси подставляется без правок в modbus |
| `uart:` — ключ компонента `uart`, всегда инстанцирует `IDFUARTComponent`; `ALLOW_EXTRA` нет | `uart/__init__.py:245-274` | **прокси нельзя объявить внутри `uart:`** — это невалидный ключ |
| Геттеры невиртуальны и читают **защищённые** поля | `uart/uart_component.h:143-167` | подкласс со своим `baud_rate_` отдаёт хабу **свою** скорость |
| `Modbus::setup()` считает `frame_delay_us_` один раз | `modbus/modbus.cpp:31-58` | межкадровая пауза берётся из скорости прокси |
| `last_send_tx_offset_ = f(parent_->get_baud_rate())` | `modbus/modbus.cpp:803` | таймер ответа тоже считается по прокси |
| `clear_rx_buffer_` чистит только **свой** `rx_buffer_`, UART не дренирует | `modbus/modbus.cpp:1229` | байты, прочитанные не тем хабом, для нужного **потеряны** |
| `Modbus::loop()` не виртуален | `modbus/modbus.cpp:61` | `disable_loop()` его не отключает |
| `waiting_for_response_ = true` ставится **после** `write_array()` | `modbus/modbus.cpp:845` | хаб не может «сдаться» до ухода кадра |
| `tx_buffer_` — `std::deque<ModbusDeviceCommand>`, по одной на хаб; `select_next_ready_` — WRITE → READ → CONTINUOUS, FIFO внутри класса | `modbus/modbus.h:311`, `modbus.cpp:922-938` | очередь команд **уже есть**, своя не нужна |
| `flow_control_pin` на `modbus:` — софтверный тумблер вокруг `write_array` + `flush` | `modbus/modbus.cpp:794-798` | DE, поднятый хабом, отпустится **до** отложенной записи |
| `flow_control_pin` на `uart:` → `UART_MODE_RS485_HALF_DUPLEX` | `uart/uart_component_esp_idf.cpp:232` | направление держит периферия, точно по времени |
| `load_settings` = `uart_driver_delete` + `install` + `param_config`; TX-кольцо 0 → запись блокирующая | `uart/uart_component_esp_idf.cpp:125-147` | смена скорости = новое RX-кольцо; слив не нужен |
| `uart_set_baudrate`/`uart_param_config` напрямую ESPHome не зовёт | grep по `components/uart/` | сменить скорость **без** пересоздания драйвера штатно нельзя |
| `send_frame_` не виртуален | `modbus/modbus.cpp:777` | перехватить отправку подклассом хаба **нельзя** |
| `Component::suspend/resume` в 2026.9.0 нет | `core/component.h` (только упоминание в комментарии) | гасить поллер только `start_poller`/`stop_poller` |

**Главный вывод:** два хаба на одной паре физически не могут сосуществовать, пока оба вызываются —
неактивный `loop()` выгребает чужие байты через `available()`/`read_array()`. Прокси обязан
**гейтить RX** и **сериализовать TX**.

---

## 3. Форма конфига

Прокси — **свой ключ верхнего уровня**, по образцу `weikai` (единственный компонент в апстриме,
отдающий наружу свои `UARTComponent`: `weikai/__init__.py:28`
`WeikaiChannel = weikai_ns.class_("WeikaiChannel", uart.UARTComponent)`).

```yaml
external_components:
  - source: {type: local, path: components}
    components: [baud_proxy]

uart:
  - id: intPort                     # реальная пара проводов
    tx_pin: 17
    rx_pin: 16
    flow_control_pin: 23            # ← переехал с modbus: сюда = аппаратный RS485
    baud_rate: 9600                 # стартовое значение, прокси переставит при первой передаче
    stop_bits: 1
    data_bits: 8
    parity: NONE
    rx_buffer_size: 512

baud_proxy:                          # ← вот здесь видно, что это прокси
  - id: intPortA
    uart_id: intPort                 # реальный UART, не другой прокси
    baud_rate: 9600
    hold_time: 150ms
  - id: intPortB
    uart_id: intPort
    baud_rate: 19200
    hold_time: 100ms

modbus:
  - {id: intBusA, uart_id: intPortA, role: client, turnaround_time: 50ms, send_wait_time: 1000ms}
  - {id: intBusB, uart_id: intPortB, role: client, turnaround_time: 50ms, send_wait_time: 1000ms}

modbus_controller:
  - {id: intController, modbus_id: intBusA, address: 0x3, update_interval: 2s}
  - {id: b1, modbus_id: intBusB, address: 0x1, update_interval: 2s}
  - {id: b2, modbus_id: intBusB, address: 0x2, update_interval: 2s}
  - {id: r1, modbus_id: intBusB, address: 0x3, update_interval: 2s}
```

`flow_control_pin` убран с обоих хабов — DE теперь у периферии.

---

## 4. Механизм: лок с TTL

Общее состояние на пару проводов — один объект `Bus`, его адрес делят оба прокси:

```cpp
struct Bus {
  const void *owner{nullptr};
  uint32_t hold_until{0};
  bool free(uint32_t now) const { return owner == nullptr || (int32_t)(now - hold_until) >= 0; }
};
```

```cpp
// components/baud_proxy/baud_proxy.h
namespace esphome::baud_proxy {

class BaudProxy : public uart::UARTComponent, public Component {
 public:
  void set_real(uart::UARTComponent *real) { this->real_ = real; }
  void set_bus(Bus *bus) { this->bus_ = bus; }
  void set_hold_ms(uint32_t ms) { this->hold_ms_ = ms; }

  // Лок берётся здесь; отпускается таймером. Кадр проигравшего ждёт в стеке
  // его собственного send_frame_ — буферизовать нечего.
  void write_array(const uint8_t *d, size_t n) override {
    while (!this->bus_->free(millis()))
      vTaskDelay(1);                      // отдать CPU задачам IDF, не крутить вхолостую

    if (this->real_->get_baud_rate() != this->baud_rate_) {
      this->real_->set_baud_rate(this->baud_rate_);
      this->real_->set_data_bits(this->data_bits_);
      this->real_->set_stop_bits(this->stop_bits_);
      this->real_->set_parity(this->parity_);
      this->real_->load_settings(false);  // новое RX-кольцо: мусор со старой скорости отброшен
    }
    this->bus_->owner = this;
    this->real_->write_array(d, n);
    this->bus_->hold_until = millis() + this->hold_ms_;   // TTL, НЕ продлевается
  }

  // RX отдаётся только владельцу: иначе неактивный хаб выгребет чужой ответ
  size_t available() override { return this->bus_->owner == this ? this->real_->available() : 0; }
  bool read_array(uint8_t *d, size_t n) override {
    return this->bus_->owner == this && this->real_->read_array(d, n);
  }
  bool peek_byte(uint8_t *d) override {
    return this->bus_->owner == this && this->real_->peek_byte(d);
  }

  UARTFlushResult flush() override { return this->real_->flush(); }
  void check_logger_conflict() override {}
  void load_settings(bool dump) override {          // зовётся только вручную
    this->real_->set_baud_rate(this->baud_rate_);
    this->real_->load_settings(dump);
  }

 protected:
  uart::UARTComponent *real_{nullptr};
  Bus *bus_{nullptr};
  uint32_t hold_ms_{150};
};

}  // namespace esphome::baud_proxy
```

### Почему лок, а не отложенный кадр

Рассматривался вариант «придержать кадр в `pending_[64]` и отдать в `loop()`». Отвергнут:

- хаб, отдав кадр прокси, сразу ставит `waiting_for_response_` и заводит таймер от **своего**
  `last_send_` (`modbus.cpp:800-845`). Отложенный кадр уйдёт позже, а если после `send_wait_time` —
  на шину уйдёт кадр, которого хаб уже не ждёт: устройство ответит на «отменённую» транзакцию;
- блокировка этого не допускает by construction: кадр уходит **до** того, как хаб узнал об отправке;
- при блокировке `last_send_` у хаба становится **точнее**, а не врёт на величину задержки.

### Почему TTL не продлевается по принятым байтам

Если продлевать аренду в `read_array`, получается клинч: B стоит в `write_array` → `App.loop()` не
идёт → A не читает ответ → аренда не продлевается → B ждёт вечно. Продление несовместимо с
блокировкой. Поэтому TTL фиксированный.

### Почему очередь не нужна

`tx_buffer_` хаба — это и есть очередь команд: `std::deque<ModbusDeviceCommand>`, приоритеты
WRITE → READ → CONTINUOUS, FIFO внутри класса, `requeue` при таймауте (`modbus.h:311`,
`modbus.cpp:922-938`). Байтовой очереди в ESPHome нет вообще — `send_frame_` пишет в UART сразу.
Третья, самодельная, была бы лишней.

### `ready_for_immediate_send()` — хук для пользовательских действий

`modbus.h:579`: `parent_->tx_buffer_empty() && !parent_->tx_blocked()`. Через `ModbusClientDevice`
доступно действие, которое ставит кадр в очередь **только если шина прямо сейчас свободна**.
Кандидат на второй вариант дизайна (без прокси, с явной арендой шины из YAML).

---

## 5. Подбор `hold_time`

Бюджет = время ответа. Ответ на 20 регистров = 45 байт (1 адрес + 1 FC + 1 счётчик + 40 данных + 2 CRC).

| Скорость | 10 бит/символ | 45 байт | TTL | Запас |
|---|---|---|---|---|
| 19200 | 521 мкс | **23.4 мс** | 100 мс | ×4.3 |
| 9600 | 1042 мкс | **46.9 мс** | 150 мс | ×3.2 |

**Жёсткое требование: `hold_time` ≪ `send_wait_time`.** Иначе победитель, вернувшись в `loop()`,
увидит `last_receive_check_ - last_send_ > send_wait_time` и спишет **живую** транзакцию в таймаут,
хотя ответ уже пришёл. При 100–150 мс против 1000 мс запас ×6.

Диагностика по логу: `Stop waiting for response from N` = ответ не успел → поднять TTL.

---

## 6. Что исчезает из нынешнего конфига

| Удаляется | Почему |
|---|---|
| `script: bus_window` | окна больше не нужны: сериализацию делает прокси |
| `apply_writes` (21 блок) | штатные платформы пишут сами, в своё окно, через свой прокси |
| глобалы `*_dirty` (18+) | ретрай по флагу не нужен: хаб ретраит сам (`requeue` при таймауте) |
| `template` `number`/`select`/`button` | заменяются штатными `modbus_controller`-платформами |
| `set_baud_rate` + `load_settings` + `flush` в скрипте | переехало в прокси, в момент передачи |
| `delay: 500ms` (слив) | TX-кольцо 0 → запись блокирующая, кадр уже в линии |
| `interval: 9s` | нет окон — нет периода |

Остаётся один глобал — маска реле.

**Запись:** штатные `modbus_controller` `number`/`select`/`switch` с `use_write_multiple: true` →
FC **0x10**, как требует вендор (FC 0x06 в документации ectoControl не существует; локальный
`docs/PROTOCOL.md` содержит 4 ошибочных упоминания — `:87`, `:98`, `:725`, `:728`).
Реле — `switch` с `write_lambda`, мутирующей `g_relay_mask` и возвращающей `{}`.

---

## 7. Риски и непроверенное

1. **`App.loop()` стоит до `hold_time`.** Пока проигравший ждёт, не идут другие компоненты и
   публикация в API. Задачи WiFi/API в IDF живут отдельно, связь не рвётся. Реально ~23–47 мс и
   только при совпадении транзакций. `vTaskDelay(1)` обязателен, а не `while` вхолостую.
2. **Драйвер пересоздаётся при каждой смене владельца** — `uart_driver_delete` + `install` +
   `param_config`. Миллисекунды, но это новое поведение на постоянной основе.
3. **Аппаратный RS485 (`UART_MODE_RS485_HALF_DUPLEX`) не проверен на железе.** Единственное, что
   нельзя подтвердить из кода. Если пойдёт эхо приёма во время передачи — в логе появятся
   `Clearing buffer ... parse failed` сразу после каждого кадра.
4. **Порядок `on_shutdown`** прокси и реального UART не определён; оба зовут `uart_driver_delete`,
   второй получит no-op (`uart_is_driver_installed` проверяется). Риск низкий.
5. **Прокси — единственный владелец `intPort`.** Всё, что захочет этот UART (logger, ещё один
   UART-девайс), обязано идти через прокси.
6. **Смена скорости не гейтится** «только если изменилась» — гейт есть в коде, но при возврате
   туда-обратно пересоздание происходит на каждой границе.
7. **Не проверено на железе:** порядок битов реле; склеится ли блок чтения `0x0010–0x0023` в один
   кадр; что `hold_time` подобран верно.
8. **Только ESP32** — `load_settings`/`rx_full_threshold` живут в `IDFUARTComponent`.

---

## 8. Отвергнутые альтернативы

| Вариант | Почему отвергнут |
|---|---|
| Оконный скрипт + отложенная запись (`apply_writes`) | Пользователь против `apply_writes` и против скрипта. Сериализацию делает прокси — скрипт не нужен |
| Один хаб + скорость из кадра (вариант A в раннем обсуждении) | Требует парсинга кадра; адрес `0x3` занят дважды (DTSU966 @9600 и реле @19200), скорость из кадра всё равно не выводится |
| Отложенный кадр `pending_[64]` + `loop()` | Кадр уходит после того, как хаб завёл таймер ответа; возможен уход после `send_wait_time` → рассинхрон |
| Два `interval:` без скрипта | Границы окон по часам; `send_wait_time` 1000 мс выносит конец окна за границу → группа уходит на чужой скорости |
| `disable_loop`/`enable_loop` на хабах | `disable_loop` убирает компонент из `looping_components_` (`application.cpp:386-393`) и действительно гасит `loop()`, но переключение — живое состояние, которое надо вести; при прокси не требуется |

---

## 9. Файлы

- `components/baud_proxy/baud_proxy.h` — класс (см. §4)
- `components/baud_proxy/baud_proxy.cpp` — если понадобится вынести из заголовка
- `components/baud_proxy/__init__.py` — схема: `id`, `uart_id` (реальный), `baud_rate`,
  `hold_time`; `MULTI_CONF = True`; `AUTO_LOAD = ["uart"]`; `cg.add(var.set_real(...))`
  из `cg.get_variable(config[CONF_UART_ID])`, `Bus` — `cg.add_global` один на конфиг
- `esp32-gw1.yaml` — конфиг из §3 + карта регистров группы B

---

## 10. Вариант 2: один хаб + прокси с картой адрес→скорость

**Дата:** 2026-09-22. Предпочтителен при разводимых адресах.

### 10.1 Предусловие и что оно снимает

Два хаба в варианте A нужны **только** из-за коллизии `0x3`: DTSU666 @9600 и модуль реле @19200.
Если адреса разводимы, хаб нужен один — и вместе с ним исчезает самое хрупкое в варианте A:

- **кража байтов из RX** — второй хаб в своём `loop()` вызовет `available()`/`read_array()` и съест
  ответ первого (`Modbus::loop()` не виртуален, `disable_loop` его не остановит);
- **клин `expire_waiting_`** — `modbus.cpp:90`: чужой байт в `rx_buffer_` не даёт истечь ожиданию,
  хаб виснет в `waiting_for_response_` навсегда.

Разведение адресов — **разовая провизия**, не работа гейтвея:

| Устройство | Как менять адрес |
|---|---|
| DTSU666 | меню прибора (кнопки/ПО) |
| ectoControl, реле | `PROG_WRITE` — FC **0x47** (`docs/PROTOCOL.md:90`); адрес также читается: `0x0002` младший байт = `ADDR` (`PROTOCOL.md:115`) |

Процедуры `0x47` нет ни в `docs/PROTOCOL.md`, ни в интеграции (grep по `custom_components/` пуст) —
делается один раз с ПК/панели. Если реле переадресовать нельзя, вариант 2 разваливается и остаётся A.

### 10.2 Механизм: гейт по скорости

```cpp
// baud_router — тот же UARTComponent-подкласс, что и в варианте A,
// но скорость берётся по первому байту кадра (адрес slave), а не статически.
void write_array(const uint8_t *data, size_t len) override {
  const uint32_t target = this->baud_for_address_(data[0]);
  // ОПТИМИЗАЦИЯ: переустановка драйвера — только если скорость реально другая.
  // Сравниваем с текущей скоростью реального порта, а не со своей копией.
  if (this->real_->get_baud_rate() != target) {
    this->real_->set_baud_rate(target);
    this->real_->set_data_bits(this->data_bits_);
    this->real_->set_stop_bits(this->stop_bits_);
    this->real_->set_parity(this->parity_);
    this->real_->load_settings(false);   // uart_driver_delete + install + param_config
  }
  this->real_->write_array(data, len);   // TX-кольцо 0 → блокирующая запись, кадр уже в линии
}
```

Гейт закрывает **два разных случая**, и оба важны:

1. **Соседние кадры на одной скорости** — штатный режим. Хаб выбирает по `select_next_ready_`
   (WRITE → READ → CONTINUOUS, FIFO внутри класса, `modbus.cpp:922-938`), поэтому опрос одного
   устройства идёт подряд: b1 (0x1) → b2 (0x2) → реле (0x4) — все 19200, переустановка **ни разу**.
   Без гейта — на каждом кадре.
2. **Повторная запись в ту же скорость после ухода и возврата** — тоже покрыт, потому что сравнение
   идёт с `real_->get_baud_rate()`, то есть с **фактическим** состоянием порта, а не с флагом
   «какая скорость была в прошлый раз». Флаг пришлось бы синхронизировать с реальностью (сброс при
   boot, после `on_shutdown`, после чужой переустановки драйвера); геттер такой возможности не даёт.

**Почему это безопасно** (доказано кодом, а не таймингом):

- `send_next_frame_` выходит, если `tx_blocked()` (`modbus.cpp:821`);
- `ModbusClientHub::tx_blocked()` (`:153`) = `waiting_for_response_ || Modbus::tx_blocked()`;
- `waiting_for_response_ = true` ставится на `:845` **после** отправки и снимается только когда ответ
  **дочитан и разобран** либо истёк `send_wait_time`.

Значит к моменту записи кадра N ответ на кадр N−1 всегда уже полностью принят **на своей скорости**.
Один кадр в полёте — гарантия кода. Плюс `load_settings` создаёт новое RX-кольцо, поэтому мусор со
старой скорости отбрасывается по построению, а слив не нужен.

Все методы — публичные: `get_baud_rate`/`set_baud_rate`/`set_data_bits`/`set_stop_bits`/`set_parity`
(`uart_component.h:143-167`), `load_settings(bool)` (`:181`), `write_array` (`:62`); блок `public:`
идёт с `:41` до `:200`. Гейт не требует ни `friend`, ни правок в modbus/uart.

### 10.3 Форма конфига

```yaml
uart:
  - id: realPort
    tx_pin: 17
    rx_pin: 16
    baud_rate: 9600          # рабочая скорость драйвера; хаб её НЕ видит
    flow_control_pin: 23     # ← перенести с modbus: (см. риск 3)

baud_router:
  id: intPort                # ← то, что видит хаб
  uart_id: realPort
  baud_rate: 9600            # МИНИМУМ парка: отсюда frame_delay_us_ (правило 1)
  default_baud: 19200        # адрес не из карты
  map:
    0x3: 9600                # DTSU666
    0x1: 19200               # b1
    0x2: 19200               # b2
    0x4: 19200               # реле, переадресовано с 0x3

modbus:
  - id: intBus               # ХАБ ОДИН
    uart_id: intPort
    role: client
    send_wait_time: 1000ms
    turnaround_time: 50ms

modbus_controller:
  - {id: dtsu, modbus_id: intBus, address: 0x3, update_interval: 2s}
  - {id: b1,   modbus_id: intBus, address: 0x1, update_interval: 2s}
  - {id: b2,   modbus_id: intBus, address: 0x2, update_interval: 2s}
  - {id: r1,   modbus_id: intBus, address: 0x4, update_interval: 2s}
```

Скорость выбирается по `data[0]` — первому байту кадра, то есть по адресу slave. Кадр целиком
парсить не надо: читается один байт. Это и есть «вариант Б» из раннего обсуждения, только теперь
он работает на одном хабе, потому что карта адресов однозначна.

### 10.4 Почему точка переключения — `write_array`, а не хук хаба

Проверен весь путь отправки. Виртуальны в хабе ровно четыре метода: `tx_blocked()`,
`tx_delay_remaining()`, `parse_modbus_frames()`, `process_modbus_server_frame()`. **Выбор кадра
(`select_next_ready_`) и запись кадра (`send_frame_`) — не виртуальны** (`modbus.h:288,70`).
Отсюда:

| Кандидат | Где | Почему не годится |
|---|---|---|
| `tx_blocked()` | `:821` в `send_next_frame_` | вызывается **до** выбора кадра → кадра ещё нет |
| `tx_delay_remaining()` | `:778` в `send_frame_` | после выбора, но **без кадра** в аргументах; зовётся ещё и на `:150` из `Modbus::tx_blocked()`, то есть до выбора — контекст не различить |
| `on_sent(pdu)` | `:834` | уже **после** `write_array` |

`write_array` — единственная точка, где «момент отправки» и «содержимое кадра» совпадают. Она
полная: и клиентский, и серверный путь (`send_raw_` → `send_frame_`, `modbus.cpp:1195`) сходятся туда
же, мимо неё кадр не уйдёт.

Формально «кастомизировать момент отправки» можно подклассом хаба: переопределить
`tx_delay_remaining()`, заглянуть в `select_next_ready_()` (`protected`, детерминирован) и
переключить скорость. Работает, но требует правки Python-биндинга — `modbus_controller/__init__.py:494`
жёстко берёт `hub = await cg.get_variable(config[CONF_MODBUS_ID])`, а `modbus:` объявляет
`cv.declare_id(ModbusClient)` с классом `ModbusClientHub` (`modbus/__init__.py:280,45`).
Прокси не требует правки modbus вообще.

### 10.5 Что исчезает против варианта A

| Было в A | Стало |
|---|---|
| Второй хаб, гейт RX (`available`/`read_array`/`peek_byte`), арбитраж владельца | один хаб, арбитраж не нужен |
| `hold_time`, TTL, лок в `write_array` | карта адрес→скорость |
| `Bus`-структура с `owner`/`hold_until` | не нужна |
| `ready_for_immediate_send()` как хук владения | не нужен |

Плюс общее для обоих вариантов: `script:`, `interval:`, `stop_poller`/`start_poller`, `apply_writes`,
18+ `*_dirty`, template-сущности — всё это не нужно, потому что штатные `modbus_controller` пишут
сами через очередь хаба.

### 10.6 Риски варианта 2

1. **Переустановка драйвера на каждой смене скорости.** С гейтом (§10.2) — только на границе
   групп, а не на каждом кадре. Сколько именно стоит `load_settings` — не измерял; главное, что
   надо померить на железе. Порядок обхода `select_next_ready_` группирует устройства одной
   скорости, поэтому на практике переустановок ≈ число переходов между группами за обход.
2. **`frame_delay_us_` кэшируется один раз** (`modbus.cpp:31-58`) — с прокси-скоростью 9600 это
   3646 мкс. На 19200 нужно 1823 → пауза вдвое длиннее нужной. Безопасная сторона, +1.8 мс на кадр.
   **Правило 1 остаётся в силе:** `baud_rate` прокси = минимум парка.
3. **`flow_control_pin` переносить на `uart:`.** Тогда направление держит периферия
   (`UART_MODE_RS485_HALF_DUPLEX`), а не софт-тоггл вокруг `write_array`. При софт-тоггле на
   `modbus:` DE будет поднят всё время переустановки драйвера.
4. **`send_wait_time` один на хаб** — брать по самому терпеливому (ectoControl), это таймаут ответа,
   а не пауза. Компромисс между быстрым DTSU666 и «терпеливым» ectoControl.
5. **Адреса — разовая провизия**, и `PROG_WRITE 0x47` в репозитории не документирован (§10.1).
6. **`default_baud` — страховка, а не рабочий режим.** Адрес не из карты = скорее всего ошибка
   конфига; лучше логировать `WARN` при попадании в `default_baud`, чтобы опечатка в адресе не
   выглядела как таймаут устройства.
7. **Не проверено на железе:** стоит ли гейт (§10.2) реально дешевле безусловной переустановки;
   порядок битов реле; склеится ли блок чтения `0x0010–0x0023` в один кадр.
