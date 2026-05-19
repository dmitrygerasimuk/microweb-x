# MicroWeb-X notes

Подробные заметки по изменениям, которые были сделаны в ветке MicroWeb-X: оптимизации DOS real-mode сборки, поддержка новых форматов/кодировок, исправления UI, диагностика памяти и вспомогательная Windows-сборка.

## Цели

- Удержать браузер работоспособным на реальном DOS-железе, где conventional memory меньше и сильнее фрагментирована, чем в эмуляторах.
- Перенести тяжелые данные страницы и изображений в EMS/XMS/Swap там, где это возможно.
- Сохранить совместимость с низкими видеорежимами: CGA, Hercules, EGA/VGA/VESA.
- Добавить минимальный набор современных HTML-возможностей: PNG, GET forms, hidden inputs, корректное кодирование URL и русский текст.
- Дать удобную диагностику через командную строку, чтобы проблемные страницы можно было разбирать без пересборки.

## GIF decoder

Файлы:
- `src/Image/Gif.cpp`
- `src/Image/Gif.h`
- `src/Image/Decoder.cpp`
- `src/Image/Image.h`

Что сделано:
- LZW-коды читаются через bit buffer, без побитового цикла на каждый входной байт.
- GIF dictionary хранит `first` byte строки, чтобы не ходить по цепочке `prev` при добавлении новых кодов.
- Decode stack вынесен из локального стека функции в объект декодера, чтобы не давить tiny DOS stack.
- Цветовой dithering для 8bpp вынесен в предрасчитанный `colourDitherLUT[16][256]`.
- Для horizontal scaling строится `xScaleBuffer`, чтобы не пересчитывать DDA на каждой строке.
- Non-interlaced vertical scaling переведен с `long`-делений на аккумулятор.
- 8bpp output loop частично развернут по 4 пикселя.
- Добавлен `Image::hasTransparency`, чтобы blit-код мог пропускать проверки прозрачности для непрозрачных изображений.
- Добавлена защита вывода строк GIF: decoder больше не пишет за пределы output image lines, что закрывает падение с `invalid pointer type` на некоторых картинках.
- В `MEMLOG` добавлен trace места использования битого `MemBlockHandle`, поэтому падения вида `invalid-use file=..\..\src\Image\Gif.cpp line=909` теперь локализуются быстро.

Результат:
- GIF быстрее декодируются на DOS.
- Меньше давление на стек.
- Ошибочные/странные GIF не должны портить memory block handles при выходе за границу изображения.

## PNG decoder

Файлы:
- `src/Image/Png.cpp`
- `src/Image/Png.h`
- `src/Image/Decoder.cpp`
- `lib/inflate/inflate.c`
- `lib/inflate/inflate.h`
- `project/DOS/Makefile`
- `project/Windows/Windows.vcxproj`

Что сделано:
- Заглушка PNG с чтением только dimensions заменена начальной реальной реализацией отображения PNG.
- Добавлен DOS-friendly inflate из `lib/inflate`.
- PNG decoder подключен к общему image pipeline рядом с GIF/JPEG.
- DOS-сборка линкует `inflate.c`.
- Windows-сборка тоже теперь линкует `lib\inflate\inflate.c`, иначе `Png.obj` падал на `_inflate_zlib`.

Ограничения:
- Реализация ориентирована на практичные web PNG, а не на полный coverage всех экзотических вариантов PNG.
- Inflate на Windows собирается с предупреждениями MSVC про narrowing `uint16_t -> uint8_t`; они не блокируют сборку.

## CGA / 2bpp rendering

Файл:
- `src/Draw/Surf2bpp.cpp`

Что сделано:
- Для CGA 2bpp добавлены pack tables для упаковки 8bpp image pixels в CGA bytes.
- Fast path для непрозрачных изображений пропускает проверку каждого пикселя на `TRANSPARENT_COLOUR_VALUE`.
- Pack tables сохраняют CGA/composite phase behavior старого кода (`colour | (colour << 4)`), чтобы composite CGA цвета не ломались.
- Исправлялись зависания/ребуты при выборе CGA 320x200 4-colour mode.
- Отдельно проверялось, что VESA-режимы не ломаются от CGA-правок.
- `MemBlockHandle::Get()` для image lines оставлен внутри построчного цикла, так как handle access может менять активный backing storage.

Результат:
- CGA-режим стал запускаться стабильнее.
- Скроллбар и packed rendering стали ближе к исходному поведению, хотя CGA остается самым чувствительным режимом.

## CGA mono / 1bpp rendering

Файл:
- `src/Draw/Surf1bpp.cpp`

Что сделано:
- Добавлен `BlitMonoBits()` с fast paths для совпадающего bit alignment и byte-aligned source.
- Центральные aligned chunks копируются байтами/словами вместо побитового цикла.
- Для DOS добавлены Watcom inline asm helpers:
  - `CopyMonoBytes`: `rep movsw` + хвостовой `rep movsb`.
  - `CopyMonoBytesInvert`: `lodsw/not ax/stosw` + хвостовой byte.
- Для `HLine`, `FillRect`, `InvertRect` полные байты теперь обрабатываются при `count >= 8`, а не только `count > 8`.
- `MemBlockHandle::Get()` намеренно оставлен внутри row loop.

Результат:
- Ускорен монохромный blit.
- Меньше лишней работы при отрисовке 1bpp изображений и UI.

## Progressive image drawing

Файл:
- `src/Nodes/ImgNode.cpp`

Что сделано:
- Исправлен progressive redraw: после создания `croppedContext` blit теперь вызывается именно через него, а не через исходный `context`.

Результат:
- Прогрессивная дорисовка картинок не рисует за пределы clip area.

## EGA / VGA / VESA video handling

Файлы:
- `src/VidModes.cpp`
- `src/DOS/Platform.cpp`
- `src/DOS/SurfVESA.cpp`
- `src/Draw/Surf8bpp.cpp`
- `src/Interface.cpp`

Что сделано:
- EGA 640x350 mono mode больше не использует linear `Format_1BPP` framebuffer path; он переведен на planar EGA renderer.
- Автодетект 64K EGA теперь предлагает 640x200 EGA, а не 640x350 16-colour mode.
- Добавлялся и проверялся VESA 640x400/800x600 path.
- Для VESA/high-color режимов исправлена потеря текста при выделении адресной строки: selection теперь рисуется явными foreground/background цветами, а не только инвертированным прямоугольником.
- Отдельно проверялось, что исправления CGA не регрессируют VESA/VGA.

Результат:
- VESA-режимы снова стабильно открываются.
- Выделенный текст адресной строки читается, а не превращается в сплошной темный блок.

## Data pack assets

Файл:
- `src/DataPack.cpp`

Что сделано:
- Встроенные bitmap assets помечаются как потенциально transparent, чтобы быстрые opaque blit paths не ломали иконки и UI assets.

## Memory diagnostics

Файлы:
- `src/App.cpp`
- `src/App.h`
- `src/Memory/Memory.cpp`
- `src/Memory/MemoryLog.h`
- `src/Memory/MemBlock.cpp`
- `src/Memory/MemBlock.h`
- `src/Memory/LinAlloc.h`

Что сделано:
- Отладка памяти переведена с compile-time define на ключ командной строки.
- Добавлены ключи:
  - `-memlog`
  - `-debugmem`
  - `-debug`
- При включении debug memory log создается `C:\MEMLOG.TXT`, а если не получилось, то `MEMLOG.TXT` в текущей директории.
- Логируются инициализация/сброс/shutdown `MEMBLOCK`, EMS, XMS и `LINALLOC`.
- `MemBlockHandle` теперь проверяет тип handle перед доступом.
- При invalid handle пишется raw dump первых байт handle, тип, conventional pointer, swap position, EMS/XMS поля.
- Добавлен `GetPtrDebug(file, line)`, чтобы видеть место, где впервые пойман неправильный handle.
- Fatal error по `Invalid pointer type` теперь сопровождается диагностикой в `MEMLOG`.

Полезные строки лога:

```text
MEMLOG enabled
MEMBLOCK init useSwap=0 useEMS=1 useXMS=1
EMS init ok pages=...
XMS init ok handle=...
LINALLOC init first=... chunk=...
MEMBLOCK invalid-handle op=get ...
MEMBLOCK invalid-use file=... line=...
LINALLOC fail-new-chunk size=... chunks=... used=...
```

## LinearAllocator chunk size

Файлы:
- `src/Memory/LinAlloc.h`
- `src/App.cpp`
- `src/App.h`

Проблема:
- На эмуляторе страница могла открываться даже с `-noems -noxms`, а на реальном железе падала с `out of memory loading page`.
- Лог показывал, что EMS/XMS почти свободны, а реально падал conventional/far heap:

```text
LINALLOC fail-new-chunk size=56 chunks=4 used=65420
```

Что сделано:
- `LINALLOC` переведен с fixed-size struct chunk на динамический chunk.
- По умолчанию сохранено старое поведение: chunk около 16K.
- Добавлен runtime-ключ для экспериментов на железе:

```bat
MICROWEB -linalloc=8k
MICROWEB -linalloc=8192
MICROWEB -linalloc 8
```

Семантика:
- Числа `1..64` считаются килобайтами.
- Значения ограничены диапазоном `1K..16K`.
- `-linalloc=8k` реально просит у DOS блок около 8K, а не выделяет 16K под капотом.
- В `MEMLOG` видно выбранный размер:

```text
LINALLOC config chunk=8192
LINALLOC init first=... chunk=8192
```

Зачем это нужно:
- На реальном DOS часто трудно найти непрерывный 16K conventional block после загрузки драйверов, packet driver, mouse driver, EMS/XMS manager и сетевого стека.
- 8K/4K chunks дают шанс той же странице пройти дальше без переработки DOM allocator.

Риск:
- Чем меньше chunk, тем больше overhead на служебные заголовки и больше отдельных heap allocation.
- Очень большая одиночная allocation больше выбранного chunk data size не пройдет. Для обычных DOM/node allocations это нормально.

Следующий шаг, если 8K не хватит:
- Переносить часть строк/атрибутов DOM из `pageAllocator` в `MemBlock`, чтобы они уходили в EMS/XMS/Swap.

## Forms and URLs

Файлы:
- `src/Nodes/Form.cpp`
- `src/Nodes/Form.h`
- `src/FormEncoding.h`
- `src/Tags.cpp`
- `src/Parser.cpp`
- `src/URL.h`

Что сделано:
- Реализованы GET forms.
- Поля формы теперь кодируются как query string.
- Добавлены hidden inputs.
- `&amp;` в URL декодируется, чтобы ссылки вида `index.php?a=1&amp;b=2` открывались как `index.php?a=1&b=2`.
- Добавлено percent-encoding для form values:
  - пробел кодируется как `+`;
  - unsafe ASCII и high bytes кодируются как `%XX`;
  - CP1251/8-bit ввод не разваливается при отправке формы.
- Код URL-encoding вынесен в `src/FormEncoding.h`, чтобы его можно было тестировать на хосте без подтягивания всего UI.

Результат:
- Поисковые формы и простые GET-формы начали работать.
- Hidden поля участвуют в submit.
- Ссылки с HTML entity ampersand больше не теряют параметры.

## Cyrillic text and input

Файлы:
- `src/Parser.cpp`
- `src/Parser.h`
- `src/Tags.cpp`
- `src/Font.cpp`
- `assets/Fonts/*`
- `tools/FontGen.cpp`

Что сделано:
- Добавлена начальная поддержка кириллицы.
- Сначала был добавлен режим транслитерации для CP1251/UTF-8 русского текста.
- Транслитерация не удалена и оставлена как fallback/экспериментальный режим:

```bat
MICROWEB -translit
MICROWEB -transliterate
```

- Затем добавлено нормальное отображение кириллицы через CP1251-шрифты.
- Parser умеет распознавать charset:
  - `utf-8` / `utf8`
  - `windows-1251` / `cp1251` / `cp-1251`
  - `cp866` / `ibm866` / `866`
  - latin fallback charsets
- Для DOS-ввода учтен будущий remap CP866 -> page encoding, потому что клавиатурный ввод под DOS обычно приходит в CP866.

Результат:
- Русские страницы стали читабельными при наличии CP1251-шрифтов.
- Можно сравнивать нормальную кириллицу и транслитерацию без пересборки.

## Keyboard/UI navigation

Файлы:
- `src/Interface.cpp`
- `src/KeyCodes.h`
- `src/DOS/DOSInput.cpp`

Что сделано:
- Space scrolls page down.
- Shift+Space scrolls page up.
- Поведение добавлено как browser-like navigation, чтобы не тянуться к scrollbar на старом железе.

Команды:

```text
Space       page down
Shift+Space page up
```

## Dropdown/select mouse handling

Файлы:
- `src/Interface.cpp`
- `src/Nodes/Select.cpp`
- `src/Nodes/Select.h`

Проблема:
- Выпадающий список можно было выбирать стрелками, но клик по тексту option часто проходил сквозь popup в страницу.
- Если под popup была ссылка, клик срабатывал по ссылке, а не по option.

Что сделано:
- `SelectNode` теперь хранит активный dropdown handler.
- `AppInterface::PickNode()` сначала проверяет активный dropdown.
- Пока dropdown открыт, клики внутри popup направляются в него.
- Клик вне popup закрывает dropdown и не пробивает underlying page link.

Результат:
- Клик по тексту option выбирает option.
- Фон страницы больше не имеет приоритет над раскрытым списком.

## Text selection rendering

Файлы:
- `src/Interface.cpp`
- video surface draw paths

Проблема:
- В VESA/high-resolution режимах выделение адресной строки иногда рисовало только инвертированный блок, а текст исчезал.

Что сделано:
- Selection text рисуется явными цветами.
- Инверсия используется как visual background, но текст поверх нее больше не теряется.

Результат:
- URL в адресной строке читается даже при выделении.

## Load/download status

Файлы:
- `src/App.cpp`
- `src/HTTP.cpp`
- `src/Nodes/Status.cpp`

Что сделано:
- Улучшена диагностика статусов загрузки.
- Разделены случаи обычной загрузки страницы и download flow, чтобы статус `Download Complete` не появлялся там, где пользователь просто открыл страницу.

## Windows build

Файлы:
- `project/Windows/Windows.vcxproj`
- `src/App.h`
- `src/Windows/Platform.cpp`
- `src/Windows/WinNet.h`
- `src/Windows/WinNet.cpp`
- `src/Windows/WinVid.h`
- `src/Windows/WinInput.h`

Что сделано:
- Windows-сборка починена после добавления PNG/inflate.
- `lib\inflate\inflate.c` добавлен в `Windows.vcxproj`.
- Исправлен конфликт `winsock.h` и `WinSock2.h`:
  - перед `windows.h` задается `WIN32_LEAN_AND_MEAN`;
  - в `src/Windows/Platform.cpp` `WinSock2.h` подключается до `windows.h`.
- Windows Debug Win32 сборка проверена через MSBuild.

Команда проверки:

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\MSBuild.exe" project\Windows\Windows.vcxproj /p:Configuration=Debug /p:Platform=Win32 /m
```

Текущий статус совместимости:
- Windows 10/11: ожидаемо работает.
- Windows 7/8/8.1: вероятно работает при наличии нужного MSVC runtime.
- Windows XP: текущая `v142`/modern SDK сборка, скорее всего, не запустится.
- Windows 98: практически точно нет без отдельной старой toolchain/ANSI WinAPI конфигурации.

## Host tests

Файлы:
- `tests/TestMain.cpp`
- `tests/build.bat`
- `tests/run.bat`
- `src/FormEncoding.h`
- `.gitignore`

Что сделано:
- Добавлен маленький host-side test runner под Open Watcom NT target.
- Тесты можно гонять на хосте без DOSBox и без запуска браузера.
- В `.gitignore` добавлены `tests/*.exe`.

Команда:

```bat
tests\run.bat
```

Покрытые сценарии:
- URL cleanup декодирует `&amp;`.
- Relative URL декодирует `&amp;`.
- Relative path cleanup для `..\file.html`.
- Form URL encoding для пробелов и ASCII-symbols.
- Form URL encoding для CP1251/high bytes.

Текущий успешный вывод:

```text
PASS url clean decodes amp
PASS relative url decodes amp
PASS relative url path cleanup
PASS form url encoding
PASS form url encoding cp1251
All tests passed
```

## DOS build verification

Основная DOS-сборка проверялась через Watcom 1.9:

```bat
cd project\DOS
wmake.exe -a
```

Окружение:

```bat
set PATH=c:\watcom19\binnt;%PATH%
set INCLUDE=c:\watcom19\h
set WATCOM=c:\watcom19
set EDPATH=c:\watcom19\eddat
set WIPFC=c:\watcom19\wipfc
set LIBDOS=c:\watcom19\lib286\dos;c:\watcom19\lib286
```

Типичные предупреждения, которые уже встречались и не блокировали сборку:
- `Parser.cpp`: unreachable code.
- `Interface.cpp`: unreachable code.
- `DOSNet.cpp`: no reference to `__static_assert`.
- `Surf8bpp.cpp` / `SurfVESA.cpp`: unused `edge`.
- `Surf1512.cpp`: unused `SetBorderColour`.

## Useful runtime flags

```bat
MICROWEB -noimages
MICROWEB -dumppage
MICROWEB -invert
MICROWEB -useswap
MICROWEB -noems
MICROWEB -noxms
MICROWEB -memlog
MICROWEB -debugmem
MICROWEB -debug
MICROWEB -translit
MICROWEB -transliterate
MICROWEB -linalloc=8k
MICROWEB -linalloc 8
```

Практичные комбинации для железа:

```bat
MICROWEB -memlog -linalloc=8k
MICROWEB -memlog -linalloc=4k
MICROWEB -memlog -noems -noxms
MICROWEB -memlog -useswap
```

## Known follow-ups

- Перенести больше DOM strings/attributes из `pageAllocator` в `MemBlock`, если реальное железо все еще упирается в `LINALLOC fail-new-chunk`.
- Добавить UI-настройку кодировки/транслитерации вместо только command-line flags.
- Доделать CP866 input remap для DOS-ввода.
- Расширить host tests: URL parser, HTML entities, charset detection, form submit, simple layout invariants.
- Добавить минимальные image decoder tests на маленьких GIF/PNG fixtures.
- Отдельно протестировать CGA 320x200 4-colour на реальном железе и нескольких эмуляторах.
- При желании сделать отдельную legacy Windows-конфигурацию для XP, но текущая Windows-сборка на это не рассчитана.
