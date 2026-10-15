# Android area: text for the coordinator to merge (2026-10-14)

## CHANGELOG.md — append to the existing section `## [Unreleased] — Android` (Russian, like the file)

```markdown
### Android: первый запуск на AVD и сборка arm64
- Проверено на эмуляторе (AVD API 35 x86_64, Vulkan 1.3 RTX 3080 через gfxstream): Showcase 300 кадров с
  валидацией, 0 ошибок Vulkan, `exit code 0`; LLDB (`tools/android/debug.ps1 -Batch`) останавливается в
  `ox::Engine::tick` и печатает стек до `android_main`. Собран APK для телефонов (arm64-v8a, 133 МБ); на устройстве
  ещё не запускался.
- Скрипты (Lua): `require` работает в собранной игре (`--pak`). Раньше модули искались только на диске, и в
  Showcase из пака (Windows и Android) не загружался `Scripts/game.lua` (`module 'lib.flow' not found`).
  `ScriptVM::setModuleFileReader`: если файла модуля нет на диске, VM спрашивает читателя; модуль `script` движка
  читает такие пути через VFS (`project://…`). Игре только из пака корень по умолчанию — `project://scripts`;
  Showcase добавляет `project://Assets/Scripts`.
- vcpkg: overlay-порт `sol2` с патчем для clang 19 (NDK r28): `&upvalue_this_member_variable<…>::call<…>` с
  `noexcept(std::is_nothrow_copy_assignable_v<T>)` не приводился к `lua_CFunction`. Порт-версия 3.5.0#2, на Windows
  поведение не меняется.
- runtime: разбор чисел с плавающей точкой в аргументах плеера без `std::from_chars` для `double` (его нет в libc++
  NDK r28).
- physics: если у Jolt нет конфигурации Debug (release-only триплеты Android), Debug-сборка линкуется с release
  `libJolt.a` и получает его определения (`NDEBUG`), иначе раскладка классов Jolt не совпала бы с библиотекой.
- Android-плеер: `TRACY_NO_INVARIANT_CHECK=1` до загрузки библиотеки (клиент Tracy на эмуляторе без invariant TSC
  вызывал `exit(1)` в статическом конструкторе, процесс перезапускался по кругу).
- Gradle/скрипты: debug-APK по умолчанию с урезанными библиотеками (неурезанная `liboxwald_player.so` весит ~760 МБ;
  символы для LLDB/Studio берутся из дерева сборки, `-PoxKeepSymbols=true` возвращает старое поведение);
  `build.ps1` удаляет прошлый APK перед упаковкой (инкрементальная упаковка AGP раздувала APK до 800+ МБ);
  `-Jobs` (по умолчанию 4); kotlin-bom 1.8.22 (дубликаты классов kotlin-stdlib-jdk7/jdk8); `JAVA_HOME` из
  пользовательских переменных (машинный JDK ломал `jlink` в AGP); у каждого Android-триплета свой корень установки
  vcpkg (`vcpkg_installed_android/<triplet>`): установка `arm64-android` в общий корень удаляла `x64-android`.
- Смоук-тест: `Android.DeviceSmoke` пропускается (код 77), когда устройства нет — раньше он падал в Windows
  PowerShell 5.1, потому что stderr adb («no devices/emulators found») превращался в исключение. Скриншот экрана
  снимается во время игры, а не после выхода; логи сохраняются в UTF-8; системное окно «Viewing full screen»
  подавляется. `debug.ps1`: `lldb.cmd` вместо `lldb.exe` (встроенный Python), без `--log-channels ""` (lldb-server
  обрывал первое подключение), `pkill -x` (`pkill -f` убивал собственный `sh`), выбор потока на точке останова в
  `-Batch`, выключенный JIT-загрузчик lldb.
```

## README.md / docs/guide/README.md

No changes needed (chapter 37 is already listed).

## docs/dev/perf.md

Nothing to add: the emulator numbers (validation on, gfxstream) are not performance data; they are recorded in
guide chapter 37, section «Проверено на AVD».

## Update 2026-10-15 (verification rerun) — CHANGELOG addition (Russian), same `[Unreleased] — Android` section

```markdown
- Скриншот плеера (`--screenshot`) больше не теряется при рендере в отдельном потоке: `RuntimeRenderer` забирает
  запросы один раз за кадр (раньше запрос между серединой и концом кадра оставался без текстуры захвата, на AVD это
  дало `exit code 3`); запрос, после которого не нарисовано ни одного кадра, при остановке рендерера завершается
  ошибкой в логе. AVD: четыре смоук-прогона по 300 кадров подряд с кодом 0.
- Скрипты: `debug.ps1 -Batch` выбирает поток на точке останова через `SBProcess` цели и не падает при выводе в файл
  (`adb push` пишет прогресс в stderr); `smoke-test.ps1` берёт устройство только из строки `[rhi] Device:`.
```
