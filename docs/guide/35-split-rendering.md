# 35. Split rendering: второй GPU для лёгких пассов

> Модули `rhi` (`<oxwald/rhi/multi_device.hpp>`, `<oxwald/rhi/split_scheduler.hpp>`) и `render`
> (`<oxwald/render/split_rendering.hpp>`, входит в зонтичный `<oxwald/render/render.hpp>`). По умолчанию
> **выключено** (`r.SplitRendering 0`). Глава честно объясняет, когда это даёт выигрыш, а когда нет, как сделать
> пасс переносимым, как работает механизм и как включить встроенную графику процессора в BIOS.

## Зачем

В большинстве игровых ПК стоят два GPU: дискретная видеокарта и встроенная графика процессора (iGPU), которая
простаивает. Split rendering отдаёт iGPU часть кадра: «лёгкие» пассы render graph выполняются на втором устройстве,
остальное — на основном. Вывод на экран всегда идёт с основной видеокарты: второму GPU не нужны ни монитор, ни
swapchain, ни surface. Он работает как безголовый (headless) вычислитель.

Прежде чем включать, посмотрите на цифры. Они решают всё.

| | RTX 3080 (основной) | Intel UHD 630 (i9-9900K) |
| --- | --- | --- |
| FP32 | ~30 TFLOPS | ~0.46 TFLOPS (24 EU × 1.2 ГГц) |
| Память | 760 ГБ/с GDDR6X | общая с CPU DDR4, ~40 ГБ/с |
| Связь между ними | PCIe 3.0 x16, на практике ~11–12 ГБ/с | |

iGPU здесь медленнее в 40–70 раз, а кадр 1080p RGBA8 весит 8.3 МБ. Чтобы отдать FXAA на iGPU, нужно отправить
картинку туда и вернуть обратно: 16.6 МБ по PCIe, это ~1.5 мс копий на основной видеокарте. Сам FXAA на RTX 3080
занимает **0.04 мс** (замер ниже). Перенос стоит в 35 раз дороже, чем экономит, а iGPU считал бы этот FXAA
~2–4 мс. Поэтому:

- **Экранные пассы каждого кадра (хвост постобработки: тонмаппинг, bloom, FXAA, композиция UI) на связке
  «мощная дискретная + слабая встроенная» не окупаются никогда.** Планировщик в режиме `Auto` это видит и оставляет
  их на основном GPU.
- Окупается работа, у которой **маленькие входы и выходы, много вычислений и терпимость к задержке**: обновление
  проб и IBL, генерация процедурного шума (облака), симуляции, мипы для стриминга. Или конфигурации, где iGPU не
  намного слабее дискретной (ноутбук с современной Xe/RDNA-графикой и начальной дискретной картой).

Режим `Auto` измеряет обе стороны во время работы и включает перенос, только если он выгоден. На машине из таблицы
он ничего не переносит, и это правильный ответ.

## Почему это сложно в Vulkan

- **Нет device groups между вендорами.** NVIDIA и Intel — два независимых `VkDevice` с разными драйверами. Общей
  памяти, общих семафоров и общих очередей нет.
- **Opaque-хэндлы памяти и семафоров** (`VK_KHR_external_memory_win32`/`_fd`, `VK_KHR_external_semaphore_*`)
  действительны только между устройствами с одинаковыми `deviceUUID` и `driverUUID`, то есть на одном и том же
  физическом GPU. Между дискретной и встроенной видеокартой они не работают.
- **Работает `VK_EXT_external_memory_host`.** Обычную память процесса (выровненный `malloc`) можно импортировать в оба
  устройства как буфер. Дискретная карта пишет в неё по PCIe, iGPU читает её напрямую: для встроенной графики
  системная память и есть видеопамять. Копировать на CPU ничего не нужно. Расширение есть у NVIDIA, AMD и Intel.
- **Запасной путь — staging.** У каждого устройства свой host-visible буфер, а CPU копирует между ними `memcpy`.
- **Синхронизация идёт через CPU.** Рабочий поток ждёт timeline-семафор основного устройства
  (`vkWaitSemaphores`) и только потом отправляет работу на второе. Чтобы ожидания не тормозили кадр, результат
  используется с задержкой в `r.SplitRendering.Latency` кадров (по умолчанию 2).

## Ключевые понятия

| Понятие | Что это |
| --- | --- |
| Основное устройство | `rhi::Device` — дискретная видеокарта, на ней весь кадр и вывод на экран |
| Второе устройство | `rhi::SecondaryDevice` — ещё один `VkDevice` на другом адаптере (предпочтительно `INTEGRATED`), только compute-очередь, свой bindless-набор той же раскладки, свой рабочий поток |
| Переносимый пасс | Compute-пасс, у которого push constants содержат только bindless-индексы текстур и обычные числа (без адресов буферов и `VIEW`). Один и тот же SPIR-V работает на обоих устройствах |
| Сегмент | Один или несколько переносимых пассов подряд (`PortableSegmentDesc`). Планировщик переносит сегмент целиком |
| Канал | `rhi::CrossDeviceChannel`: набор текстур сегмента на втором устройстве и буферы передачи в обе стороны, по слоту на кадр в полёте |
| Путь передачи | `ExternalMemory` (одна общая аллокация, только один физический GPU), `HostPointer` (`VK_EXT_external_memory_host`), `Staging` (копии через CPU) |
| Задержка | Результат кадра N используется в кадре N + `r.SplitRendering.Latency` |
| Калибровка | При создании второго устройства одно и то же ядро, похожее на постобработку (9 выборок + ALU, 1080p), выполняется на обоих GPU. Отношение времён — стартовая оценка скорости второго GPU |

## Шаг 1. Включение и cvar'ы

```
r.SplitRendering Auto        // или On, Off
```

| CVar | По умолчанию | Что делает |
| --- | --- | --- |
| `r.SplitRendering` | `Off` | `Off` — второе устройство не создаётся. `On` — переносить каждый переносимый сегмент, даже невыгодный (для экспериментов). `Auto` — переносить только то, что по модели стоимости экономит время основного GPU |
| `r.SplitRendering.Adapter` | `-1` | Индекс адаптера в порядке перечисления Vulkan. `-1` — лучший GPU кроме основного, встроенный в приоритете |
| `r.SplitRendering.AllowSameAdapter` | `false` | Разрешить второе устройство на том же GPU. Нужно для тестов на машине с одной видеокартой. Ускорения не даёт. То же самое включает переменная окружения `OX_SPLIT_RENDERING_ALLOW_SAME_ADAPTER=1` |
| `r.SplitRendering.Transfer` | `Auto` | `Auto`, `ExternalMemory`, `HostPointer`, `Staging`. Недоступный путь заменяется лучшим доступным, причина пишется в лог и статистику |
| `r.SplitRendering.Latency` | `2` | 1–3 кадра. При 1 CPU ждёт предыдущий кадр GPU, и параллельность CPU/GPU теряется |
| `r.SplitRendering.MinGainMs` | `0.1` | `Auto`: минимальная экономия основного GPU (мс), ради которой стоит что-то переносить |
| `r.SplitRendering.Calibrate` | `true` | Калибровать скорость при создании второго устройства (~5–20 мс один раз) |

`r.SplitRendering`, `.Adapter`, `.Transfer` и `.Latency` помечены `Persist` и сохраняются в настройках игрока, как
остальные графические cvar'ы. Их видно в окне cvar'ов отладочного оверлея и в консоли
([глава 29](29-ui.md)). В группы качества Low–Ultra они не входят: выигрыш зависит от пары видеокарт, а не от
уровня качества. Переключение работает на лету: при `Off` второе устройство и все его ресурсы освобождаются в
начале следующего кадра.

Чтобы общая память (`ExternalMemory`, `HostPointer`) была доступна, основное устройство нужно создавать с
дополнительными расширениями. `RuntimeRenderer` делает это сам. Если создаёте устройство вручную:

```cpp
#include <oxwald/render/split_rendering.hpp>

ox::rhi::DeviceDesc desc;
ox::render::appendUpscalerVulkanExtensions(desc);      // DLSS, как обычно
ox::render::appendSplitRenderingVulkanExtensions(desc); // external memory win32/fd + external memory host
auto device = ox::rhi::Device::create(desc);
```

Без этих расширений split rendering тоже работает, но только через `Staging`.

## Шаг 2. Как сделать пасс переносимым

Встроенный пример — `LdrPost` (FXAA, LUT, зерно плёнки; `features/postprocess/ldr_post.cpp`). Вместо
`graph().addPass(...)` фича описывает пасс данными и вызывает `addPortableSegment`:

```cpp
#include <oxwald/render/split_rendering.hpp>

struct LdrPush {               // раскладка push constants шейдера (scalar)
    u32 src, dst, fxaaSteps, lut;
    f32 lutSize, lutIntensity;
    u32 lutSrgb;
    f32 grain;
    u32 frame, pad;
    glm::vec2 size;
};

ox::render::PortablePassDesc pass;
pass.name = "FXAA";
pass.pipeline = m_pipeline;                 // обычный compute-пайплайн основного устройства
pass.setPush(LdrPush{0, 0, steps, ...});    // индексы текстур пока нули
pass.textures = {
    {src, u32(offsetof(LdrPush, src)), false},  // чтение (sampled)
    {out, u32(offsetof(LdrPush, dst)), true},   // запись (storage), перезаписывает каждый тексель
};
pass.threads = {size.width, size.height, 1};    // как dispatchThreads

ox::render::PortableSegmentDesc segment;
segment.name = "LdrPost";                   // стабильное имя: по нему копятся замеры
segment.passes = {pass};
segment.portable = lutIndex == kInvalidIndex;   // LUT — текстура вне графа: тогда только основной GPU
ox::render::addPortableSegment(ctx, segment);
R.setTexture(res::kSceneColorLDR, out);
```

Если split rendering выключен, невыгоден или невозможен, `addPortableSegment` объявляет **ровно тот же**
compute-пасс, что фича объявила бы сама. Индексы `sampledIndex`/`storageIndex` подставляются в указанные смещения
push constants. Поэтому перевод фичи на переносимые сегменты ничего не меняет, пока перенос не включён.

Правила переносимого пасса:

- только compute; шейдер подключает `common/bindless.glsl` и использует набор 0 (как все шейдеры движка);
- в push constants нет адресов буферов (`ViewBuffer`, `SceneBuffer`, `OX_BUFFER`) — память основного GPU второму
  недоступна;
- все текстуры перечислены в `textures`: 2D, один мип, один слой, цветовой формат. Для записываемых текстур
  формат должен поддерживать storage на обоих GPU (например, `RGBA8_UNORM`, но не `RGBA8_SRGB`);
- записываемые текстуры перезаписываются полностью;
- текстуры, прочитанные до записи внутри сегмента, становятся его **входами**, а текстуры, последнее обращение к
  которым — запись, становятся **результатами**. Промежуточные текстуры живут только на втором устройстве;
- семплеры — только `DefaultSampler` (индексы 0–5 одинаковы на обоих устройствах).

## Шаг 3. Что происходит в кадре

Когда планировщик отдаёт сегмент второму GPU, в графе кадра N появляются два transfer-пасса вместо исходных:

```
кадр N:      ... Tonemap → Split.Export.LdrPost (SceneColorLDR → общий буфер, слот N % 3)
                         → Split.Import.LdrPost (результат кадра N-2 → LdrPost) → Overlay → Final
рабочий поток: ждёт, пока кадр N отработает на основном GPU (timeline семафор)
               → [Staging: memcpy] → второй GPU: буфер → текстура, FXAA, текстура → буфер
               → [Staging: memcpy обратно] → слот N готов
кадр N+2:    Split.Import берёт слот N
```

- **Прогрев.** Первые `Latency` кадров после включения результатов ещё нет, и сегмент считается на основном GPU,
  хотя экспорт уже идёт.
- **Задержка.** Картинка постобработки отстаёт на `Latency` кадров. UI и оверлеи редактора рисуются после сегмента и
  остаются актуальными.
- **Сбой.** Если задание на втором GPU не выполнилось (потеря устройства, таймаут 2 с), Import копирует вход в выход:
  один кадр без FXAA. В начале следующего кадра второе устройство освобождается, всё снова идёт на основном GPU, а
  статистика показывает причину. Повторная попытка будет после переключения `r.SplitRendering` в `Off` и обратно.
- **Смена разрешения** или `Latency` пересоздаёт канал. Старые задания сначала дожидаются завершения.
- **Нет второго GPU.** Устройство не создаётся, сегменты считаются на основном GPU, в статистике — причина.

## Шаг 4. Планировщик (`Auto`)

Каждые 30 кадров планировщик (`rhi::chooseSplit`, только CPU) оценивает каждый сегмент:

```
копии на основном   = (вход + выход) / пропускная_способность_основного + 2 × накладные
экономия основного  = время_на_основном − копии_на_основном
нагрузка второго    = время_на_втором + (вход + выход) / пропускная_способность_второго + memcpy (Staging)
```

Выбирается набор сегментов с наибольшей экономией при условии, что второй GPU успевает: суммарная нагрузка ≤ 90 %
нового времени кадра основного GPU (иначе узким местом станет он). Если лучшая экономия меньше
`r.SplitRendering.MinGainMs`, ничего не переносится.

Откуда берутся числа:

- **время на основном** — замер timestamp'ов пассов сегмента, пока он идёт на основном GPU; до первого замера —
  `costHintMs` из описания сегмента;
- **время на втором** — замер timestamp'ов задания на втором GPU; до него — время на основном × коэффициент скорости
  (калибровка, затем отношение реальных замеров);
- **пропускная способность** — стандартные оценки пути (PCIe ~11 ГБ/с для host memory). Когда сегмент уже
  переносился, их заменяют измеренные времена `Split.*` пассов.

Решение меняется только после трёх подряд одинаковых оценок (`rhi::SplitController`), чтобы перенос не
«дёргался» туда-обратно из-за шума замеров.

## Шаг 5. Статистика

`Renderer::stats().split` (`SplitRenderingStats`) и `ox::render::splitRenderingStatus(renderer)`:

| Поле | Что |
| --- | --- |
| `mode`, `active`, `adapter`, `sameAdapter`, `transfer` | режим, есть ли второе устройство, его имя, выбранный путь |
| `segments`, `offloaded`, `imported`, `fallbacks` | переносимые сегменты в кадре, сколько из них на втором GPU, сколько результатов импортировано, сколько импортов не удалось |
| `secondaryGpuMs`, `secondaryComputeMs` | время заданий на втором GPU (копии + вычисления) и только вычислений |
| `transferMs`, `cpuCopyMs` | копии: пассы `Split.*` на основном + копии на втором + `memcpy` |
| `waitMs` | сколько поток рендера ждал результатов второго GPU |
| `predictedSavingMs`, `speedRatio`, `status` | оценка экономии, отношение скоростей, причина решения или неактивности |

`RenderStats::toString()` выводит строку `split rendering: ...`, а окно Stats оверлея (F1) — раздел
**Split rendering**. Пассы `Split.Export.*` и `Split.Import.*` видны в списке GPU-пассов.

## Шаг 6. Проверка на машине с одной видеокартой

Механизм целиком проверяется и без второго GPU: `r.SplitRendering.AllowSameAdapter 1` создаёт второй `VkDevice` на
том же физическом GPU. Тогда доступны все три пути передачи (`ExternalMemory` — одна общая аллокация
opaque-хэндлом), работают рабочий поток, задержка, сбои и переключение на лету. Ускорения нет: оба устройства делят
один GPU, и драйвер переключается между их контекстами.

Тесты (`ctest -L gpu`):

- `ox_rhi_gpu_tests`, `MultiDeviceTest.*`: перечисление адаптеров, отказ без второго GPU, калибровка, круговой
  проход текстуры через каждый путь (4 задания на 2 слотах), цепочка с промежуточной текстурой, потеря устройства,
  отказ на неподдерживаемых форматах;
- `ox_render_gpu_tests`, `SplitRenderTest.*` и `Paths/SplitTransferTest.*`: картинка со split rendering совпадает с
  обычной (FXAA на сцене с тонкой геометрией, golden `split_rendering_fxaa.png` один на все пути), откат без второго
  GPU, переключение `On`/`Off`/`Latency` без утечек и ошибок validation, потеря устройства посреди кадра, `Auto`
  оставляет лёгкий пасс на основном GPU, замер накладных расходов в 1080p;
- `ox_rhi_tests`, `SplitTransferPath.*`, `SplitScheduler.*`, `SplitController.*`: выбор пути по возможностям
  устройств и выбор переноса по модели стоимости (только CPU).

Накладные расходы на RTX 3080, когда оба устройства на ней (`SplitRenderTest.PerfReportSameAdapter1080p`, 1080p,
FXAA, RelWithDebInfo с validation, остальные агенты нагружали машину — порядок величин, а не точные цифры):

| Конфигурация | Кадр CPU, мс | GPU основного, мс | FXAA, мс | `Split.*` на основном, мс | Задание на втором, мс |
| --- | --- | --- | --- | --- | --- |
| Один GPU | 0.70 | 0.47 | 0.04 | — | — |
| Split, ExternalMemory | 2.09 | 1.78 | — | 0.57 | 1.02 |
| Split, HostPointer | 3.90 | 3.48 | — | 2.98 | 2.82 |
| Split, Staging | 5.28 | 2.67 | — | 1.40 | 1.75 |

Перенос пасса за 0.04 мс стоит 0.6–3 мс копий на основном GPU. На одном физическом GPU выигрыша нет и быть не
может, а на паре «RTX 3080 + UHD 630» его не будет для экранных пассов (см. «Зачем»).

## Шаг 7. Включить встроенную графику по-настоящему (BIOS)

Когда в слот PCIe вставлена видеокарта, большинство плат **отключают iGPU**, и Windows с Vulkan его не видят.
Сейчас на этой машине (i9-9900K + RTX 3080) Vulkan перечисляет только RTX 3080.

**MSI MEG Z390 ACE.** У этой платы нет видеовыходов на задней панели, но iGPU процессора всё равно можно включить
как безголовый вычислитель:

1. Войдите в BIOS (`Del` при старте), включите расширенный режим (`F7`).
2. **Settings → Advanced → Integrated Graphics Configuration**:
   - **Initiate Graphic Adapter = PEG** — экран по-прежнему инициализируется на видеокарте;
   - **IGD Multi-Monitor = Enabled** — iGPU остаётся включённым рядом с дискретной картой. Если такого пункта в
     вашей версии BIOS нет, обновите BIOS: без него плата выключает iGPU, как только находит видеокарту в PEG-слоте.
   - **IGD Memory** (DVMT pre-allocated) можно оставить по умолчанию (64 МБ): остальную память драйвер берёт сам.
3. Сохраните (`F10`), загрузите Windows, установите драйвер **Intel UHD Graphics 630** с сайта Intel (Intel
   Graphics Driver / Driver & Support Assistant). С драйвером ставится Vulkan ICD Intel.
4. Монитор к iGPU подключать не нужно. Вывод остаётся на RTX 3080, а iGPU работает как вычислитель. Движку от
   второго GPU не нужны ни выходы, ни swapchain.

Другие платы: ASUS — **Advanced → System Agent (SA) Configuration → Graphics Configuration → iGPU Multi-Monitor =
Enabled**; Gigabyte — **Settings → IO Ports → Internal Graphics = Enabled**; ASRock — **Advanced → Chipset
Configuration → IGPU Multi-Monitor = Enabled**. На процессорах с суффиксом F (i9-9900KF и т. п.) встроенной графики
нет.

Проверка:

```
vulkaninfo --summary
```

Должны появиться два устройства: `NVIDIA GeForce RTX 3080` (`PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`) и `Intel(R) UHD
Graphics 630` (`PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`). В логе движка при `r.SplitRendering Auto` появятся строки
вида (числа второго GPU здесь условные: на этой машине iGPU пока не включён и не измерялся):

```
[rhi] split: secondary device 'Intel(R) UHD Graphics 630' (integrated), queue family 0, transfer HostPointer (...)
[rhi] split: calibration (1080p post kernel): primary 0.12 ms, secondary ... ms -> ratio ...
```

Основным устройством остаётся дискретная карта (`DeviceDesc::preferDiscreteGpu`). Настройка Windows «Параметры
графики → предпочтение GPU» на выбор устройства в Vulkan не влияет.

## Типичные ошибки и подводные камни

- **Ждать ускорения от постобработки на слабом iGPU.** Копии по PCIe стоят дороже самого пасса (шаг 6). Оставьте
  `Auto`: он не перенесёт невыгодное.
- **Адрес буфера в push constants переносимого пасса.** Второй GPU не видит память основного: шейдер прочитает
  мусор или вызовет потерю устройства. В переносимых пассах — только текстуры через `textures` и числа.
- **sRGB-текстура как результат.** У `*_SRGB` форматов обычно нет storage. Канал не создастся, сегмент останется на
  основном GPU, а причина попадёт в лог.
- **Текстура вне графа.** Если пасс читает постоянную текстуру по её индексу (LUT, шум), на втором GPU этого индекса
  нет. Ставьте `portable = false`, как `LdrPost` с LUT.
- **`r.SplitRendering On` в релизе.** `On` — режим для экспериментов: он переносит всё, даже невыгодное.
- **Задержка в тестах.** Сравнивая картинки, рендерьте не меньше `Latency + 1` кадров, иначе последний кадр ещё
  посчитан на основном GPU (`stats().split.imported == 0`).
- **Ускорение на одном GPU.** `AllowSameAdapter` проверяет механизм, а не скорость.

## API

| Заголовок | Содержимое |
| --- | --- |
| [`render/split_rendering.hpp`](../../engine/render/include/oxwald/render/split_rendering.hpp) | `addPortableSegment`, `PortableSegmentDesc`, `PortablePassDesc`, `PortableTextureBinding`, `SplitRenderingMode`, `appendSplitRenderingVulkanExtensions`, `splitRenderingStatus`, `splitRenderingSimulateDeviceLost` |
| [`render/render_stats.hpp`](../../engine/render/include/oxwald/render/render_stats.hpp) | `SplitRenderingStats` (`RenderStats::split`) |
| [`rhi/multi_device.hpp`](../../engine/rhi/include/oxwald/rhi/multi_device.hpp) | `enumerateAdapters`, `AdapterInfo`, `appendMultiDeviceExtensions`, `SecondaryDevice`, `CrossDeviceChannel`, `PortableDispatch`, `CrossDeviceJobTiming` |
| [`rhi/split_scheduler.hpp`](../../engine/rhi/include/oxwald/rhi/split_scheduler.hpp) | `CrossDeviceTransfer`, `CrossDeviceCaps`, `selectTransferPath`, `SplitSegmentCost`, `SplitCostModel`, `chooseSplit`, `SplitController`, `TimingFilter` |

Детали для разработчиков: [`docs/dev/modules/rhi.md`](../dev/modules/rhi.md) («Multi-device») и
[`docs/dev/modules/render.md`](../dev/modules/render.md) §14.

## Что дальше

- [17. RHI и Vulkan](17-rhi-vulkan.md) — устройство, очереди, timeline-семафоры, render graph.
- [18. Устройство рендерера](18-rendering-overview.md) — фичи, точки внедрения, свой compute-пасс.
- [25. Апскейлеры и постобработка](25-upscalers-postprocess.md) — что делает `LdrPost`.
- [26. Качество графики](26-quality-settings.md) — остальные графические cvar'ы и настройки игрока.
- [27. GPU-driven рендеринг и производительность](27-gpu-driven-performance.md) — замеры и async compute на одном GPU.
- [Оглавление](README.md)
