# Аудит кода AppAlchemist: качество, стабильность, production-readiness

Дата: 2026-06-09. Объём: backend (`src/`, `include/`), GTK-frontend, сборка/CI/packaging.
Особый фокус: почему RPM-пакеты конвертируются хуже DEB (отсутствие иконки,
приложение не запускается после конвертации).

---

## 1. Корневые причины деградации RPM относительно DEB

Пайплайн для DEB и RPM формально общий (`PackageToAppImagePipeline`), но парсеры
`DebParser` и `RpmParser` — это две независимые копии одного кода, разошедшиеся
в деталях. Все найденные ниже расхождения — конкретные баги, каждый
самостоятельно воспроизводит симптомы «нет иконки» / «не запускается».

### 1.1. `RpmParser::searchInDirectory` игнорирует `executableOnly=false` → иконки не находятся (КРИТИЧНО)

`src/rpmparser.cpp:673`:

```cpp
QFileInfoList entries = dirObj.entryInfoList({pattern}, QDir::Files | QDir::Executable, QDir::Name);
```

Фильтр `QDir::Executable` применяется **всегда**, даже когда вызывающий код просит
все файлы (`executableOnly=false`). `RpmParser::findIcon()` ищет `*.png/*.svg/...`
именно через эту функцию — но иконки в RPM почти никогда не имеют бита
исполняемости (cpio сохраняет права 0644). Итог: `metadata.iconPath` для RPM
практически всегда пустой, и сборщик AppDir вынужден полагаться на эвристический
fallback-поиск, который может выбрать не ту иконку или не найти её вовсе
(тогда создаётся placeholder 1×1).

В `DebParser::searchInDirectory` (`src/debparser.cpp:671`) условие написано
правильно: `QDir::Files | (executableOnly ? QDir::Executable : QDir::Files)`.

**Фикс:** повторить условную логику DEB-версии (или объединить парсеры, см. п. 2).

### 1.2. `RpmParser::parseDesktopFile` читает `Icon=`, но выбрасывает значение (КРИТИЧНО)

`src/rpmparser.cpp:627-631`: значение `Icon=` сохраняется в локальную переменную
`iconName` и больше нигде не используется. `DebParser::parseDesktopFile`
(`src/debparser.cpp:599-620`) после парсинга резолвит имя иконки в реальный файл
(`usr/share/pixmaps`, `usr/share/icons/...`, с подбором расширений) и заполняет
`metadata.iconPath`. Для RPM этот шаг полностью отсутствует.

**Фикс:** перенести резолвинг `Icon=` из DebParser (после объединения парсеров
получится автоматически).

### 1.3. Сломанное вычисление `dataDir` в `RpmParser::parseDesktopFile` → `Exec=` никогда не резолвится (КРИТИЧНО, «не запускается»)

`src/rpmparser.cpp:640`:

```cpp
QString dataDir = QString("%1/data").arg(QFileInfo(desktopPath).absolutePath().section('/', 0, -3));
```

Для desktop-файла `<extract>/data/usr/share/applications/app.desktop` это даёт
`<extract>/data/usr` + `/data` = `<extract>/data/usr/data` — несуществующий путь.
Поэтому абсолютный путь из `Exec=` (например `/usr/bin/app`) **никогда** не
находится, `metadata.mainExecutable` из desktop-файла не устанавливается, и выбор
главного бинарника падает на эвристики (см. 1.4). DEB-версия делает это корректно
через `indexOf("/data/")` (`src/debparser.cpp:520-526`).

Дополнительно RpmParser, в отличие от DebParser:
- не обрабатывает **относительные** `Exec=` (DEB ищет в `usr/bin/<name>`, `bin/<name>` и т. д.);
- не обрабатывает Java/`-jar` команды в `Exec=`.

### 1.4. Эвристика выбора главного бинарника работает с пустым именем пакета (КРИТИЧНО, «не запускается» / «запускается не то»)

Порядок операций в `PackageExtractor::extractRpmMetadata` (`src/package_extractor.cpp:114-202`):
сначала вызывается `m_rpmParser->parseMetadata()` (внутри которого `metadata.package`
**пустой** — RPM-заголовок ещё не прочитан), и только потом результат `rpm -qp
--queryformat %{NAME}` сливается в metadata. К этому моменту `mainExecutable` уже
выбран.

При пустом `packageName` условие в `rpmparser.cpp:288-290`:

```cpp
if (baseName == packageName || baseName.contains(packageName) || packageName.contains(baseName))
```

`baseName.contains("")` == `true` для любой строки → «совпадает» **первый
попавшийся** исполняемый файл из отфильтрованного списка. Для пакета с несколькими
бинарниками/скриптами это регулярно выбирает не то (хелпер, утилиту), и AppRun
запускает не приложение. У DEB control-файл парсится до эвристик, поэтому имя
пакета там известно.

**Фикс:** (а) получать NAME/VERSION через `rpm -qp` (с fallback на имя файла) **до**
вызова `parseMetadata` и передавать имя внутрь; (б) в обоих парсерах защитить
сравнение: `if (!packageName.isEmpty() && (...))`.

### 1.5. Зависимости RPM не извлекаются вообще

Для DEB `Depends:` парсится из control-файла и передаётся в `DependencyResolver`.
Для RPM никто не вызывает `rpm -qpR`; `RpmParser::parseSpecFile` — мёртвый код
(в бинарных RPM нет .spec). В итоге `metadata.depends` пуст, и весь механизм
докачивания недостающих библиотек (`analyzeDependencies`,
`resolveAppDirDependencies` по списку depends) для RPM не работает. Это вторая
типовая причина «AppImage из RPM не стартует»: недостающие библиотеки не
бандлятся.

**Фикс:** добавить `rpm -qpR <pkg>` (и `rpm -qp --queryformat '%{ARCH}'`) в
`extractRpmMetadata`, нормализовать имена (отрезать версии, `rpmlib(...)`,
`/bin/sh` и пр.), плюс маппинг RPM-имён библиотек на DEB-имена для резолвера,
если резолвер ориентирован на Debian-репозитории.

### 1.6. Извлечение RPM через `sh -c` с интерполяцией путей

`src/rpmparser.cpp:89-92`:

```cpp
SubprocessWrapper::execute("sh", {"-c",
    QString("cd '%1' && rpm2cpio '%2' | cpio -idmv 2>&1").arg(extractPath).arg(rpmPath)});
```

Путь с одинарной кавычкой или спецсимволами ломает команду (и формально это
shell-injection через имя файла). Надёжнее связать два `QProcess` через
`setStandardOutputProcess(rpm2cpio → cpio)` без shell. Заодно: магика `drpm`
(`rpmparser.cpp:49`) — это deltarpm, который `rpm2cpio` распаковать не может;
принимать его в `validateRpmFile` некорректно.

### 1.7. Сводка: что чинит каждый фикс

| Симптом | Причина | Файл:строка |
|---|---|---|
| Нет иконки / placeholder | фильтр `QDir::Executable` всегда включён | `rpmparser.cpp:673` |
| Не та иконка | `Icon=` из desktop-файла выбрасывается | `rpmparser.cpp:627` |
| Не запускается | `Exec=` не резолвится (битый `dataDir`) | `rpmparser.cpp:640` |
| Запускается не тот бинарник | эвристика с пустым именем пакета | `rpmparser.cpp:288`, `package_extractor.cpp:181` |
| Падает на старте (нет библиотек) | `Requires` не читаются | `package_extractor.cpp` (отсутствует `rpm -qpR`) |

---

## 2. Архитектура: дублирование DebParser/RpmParser — источник всего класса RPM-багов

~80 % кода `debparser.cpp` и `rpmparser.cpp` — копипаста (`parseMetadata`,
`findExecutables`, фильтры хелперов, `searchInDirectory`, `isElfExecutable`,
`parseDesktopFile`). Все баги из п. 1 — это расхождения копий. Пока существует две
копии, любое улучшение DEB-пути будет молча «забываться» для RPM.

**Рекомендация:** выделить общий `PackageParserBase` (или свободные функции в
`package_parsing.cpp`): поиск executables, выбор mainExecutable, парсинг/резолвинг
desktop-файла, поиск иконки — формат-специфичными остаются только extract+чтение
метаданных (control vs rpm-заголовок). `TarballParser` тоже частично дублирует это.

Зеркальный баг подтверждает проблему: в `DebParser::searchInDirectory`
(`debparser.cpp:682-685`) рекурсия по подкаталогам находится **внутри** цикла по
паттернам → при 4 паттернах поддеревья обходятся 4 раза и результаты дублируются.
В RpmParser рекурсия вынесена правильно. Две копии — два разных бага.

---

## 3. Стабильность и надёжность (общее)

1. **Нет ни одного теста.** В репозитории есть `docs/regression-corpus.json`
   (список реальных пакетов с ожидаемым результатом), но нет ни unit-тестов, ни
   запуска корпуса. Для конвертера, целиком построенного на эвристиках, это
   главный риск: любой фикс может молча сломать другой пакет.
   - Минимум: Qt Test/ctest для чистых функций (`extractDesktopExecBinary`,
     `resolveExecutableFromCommand`, `sanitizeDesktopCategories`,
     `extractIconScore`, выбор mainExecutable, парсинг control/desktop).
   - Интеграционно: в CI собирать крошечные fixture-пакеты (`dpkg-deb -b`,
     `rpmbuild`/`fpm`) и прогонять конвертацию end-to-end, проверяя: AppRun
     существует и исполняем, desktop-файл валиден (`desktop-file-validate`),
     иконка ≠ placeholder, `AppRun --appimage-extract-and-run`-smoke.
   - Прогон `docs/regression-corpus.json` как nightly job.

2. **CI не собирает проект.** Единственный workflow — `build-arm64.yml`, и тот
   только по тегам/вручную. Нужен PR-workflow: сборка x86_64 + arm64, `-Wall
   -Wextra`, ctest, `clang-tidy`/`clang-format --dry-run`.

3. **Жёстко зашитый `/tmp`** (`packagetoappimagepipeline.cpp:121`) и оптимизация
   «hardlink, если источник в /tmp» (`appdirbuilder.cpp:918`). На системах с
   маленьким tmpfs конвертация большого пакета (Chrome ~300 МБ → AppDir ~1 ГБ)
   упадёт по ENOSPC без внятной ошибки. Использовать `QStandardPaths::TempLocation`
   /`$TMPDIR`, перед началом проверять свободное место (`QStorageInfo`) и давать
   пользователю понятную ошибку.

4. **`catch (...)` вокруг не бросающего Qt-кода** (`appdirbuilder.cpp:735, 1422,
   1644`) — маскирует логические ошибки и создаёт ложное ощущение защиты. Qt не
   использует исключения; убрать, заменив проверками.

5. **Таймауты процессов.** `SubprocessWrapper::execute` по умолчанию убивает
   процесс по таймауту — но распаковка/squashfs больших пакетов легко превышает
   дефолт. Проверить дефолт в `utils.h` и выставить адекватные значения для
   extract/appimagetool (для appimagetool уже 300 с — ок), плюс пробрасывать
   причину «timed out» в UI.

6. **`verifyAppDirReadiness` с `const_cast` для `emit log`**
   (`packagetoappimagepipeline.cpp:421`) — сделать метод неконстантным или
   возвращать список сообщений.

7. **Сырые `new/delete` в пайплайне** (`packagetoappimagepipeline.cpp:17-49`) —
   перейти на `std::unique_ptr`; сейчас при исключении в конструкторе утечка, и
   легко получить double-delete при рефакторинге.

8. **Гигантские методы-эвристики.** `AppDirBuilder::createAppRun` (~750 строк) и
   `buildAppDir` (~470 строк) с ветками на Chrome/Electron/Python/Java/Steam и
   хардкодом имён (`chrome-sandbox`, `codium`, `krita`, `steam`,
   `yandex-music`...). Уже есть правильный механизм — `compatibility_rules.json` +
   `CompatibilityRuleEngine`. Рекомендация: переносить app-специфику в JSON-правила,
   AppRun собирать из шаблонов; каждая ветка должна быть покрыта тестом.

9. **Логирование.** Смесь `qDebug()` (уходит в консоль) и `emit log(...)` (уходит в
   UI): половина диагностики недоступна пользователю при разборе «почему не
   сконвертировалось». Ввести `QLoggingCategory` + единый канал, добавить запись
   лога конвертации в файл рядом с результатом (это же поможет в issue-репортах —
   шаблон `conversion_failure.yml` уже просит логи).

---

## 4. Безопасность

1. **Скачивание appimagetool без верификации** (`appimagebuilder.cpp:161-222`):
   качается `continuous`-релиз по HTTPS и сразу исполняется. Пинить версию,
   проверять SHA-256, не качать молча — спросить пользователя. Также
   `QEventLoop`-блокировка — выполнять не в GUI-потоке.
2. **`executeWithSudo` с паролем из GUI** (`utils.cpp:457`): пароль живёт в
   `QString` (не затирается), передаётся в `sudo -S`. Предпочтительно polkit
   (`pkexec`) или вообще отказаться от привилегированных операций в конвертере.
3. **Распаковка недоверенных архивов**: `cpio -idmv` без явной защиты от
   `..`-путей; `ar`/`tar` аналогично. Современные tar/cpio по умолчанию защищают,
   но стоит добавить пост-проверку, что все распакованные пути остались внутри
   each extractDir (символические ссылки наружу — тоже).
4. Команды строятся аргумент-списком (хорошо), кроме `sh -c` в `rpmparser.cpp:89`
   — см. п. 1.6.

---

## 5. Качество кода / сборка

- Включить `-Wall -Wextra -Wpedantic` (сейчас флагов нет вообще) и чинить
  предупреждения; добавить `clang-format` + `clang-tidy` конфиги.
- `RpmParser::parseMetadata` содержит мёртвый/вводящий в заблуждение код:
  `QString rpmPath = m_tempDir; // We need to store the RPM path` (`rpmparser.cpp:196`).
- `DebParser`/`RpmParser::findScripts` для RPM ищет несуществующие пути
  (`var/lib/rpm-state`) — скриптлеты RPM извлекаются только через
  `rpm -qp --scripts`; сейчас результат не используется осмысленно.
- `detectPackageType` — только по расширению; magic-проверка есть в validate,
  но `.rpm`-файл с неверным расширением даст «Unknown package type» вместо
  внятного сообщения.
- Версия в `CMakeLists.txt` (1.5.0) и changelog/spec в `packaging/` должны
  синхронизироваться из одного источника.

---

## 6. План работ до production-ready (по приоритетам)

**P0 — паритет RPM с DEB (точечные фиксы, малые диффы):**
1. `rpmparser.cpp:673` — честный `executableOnly`.
2. Резолвинг `Icon=` в `RpmParser::parseDesktopFile` (как в DEB).
3. `rpmparser.cpp:640` — `dataDir` через `indexOf("/data/")`; поддержка
   относительных `Exec=` и Java.
4. `rpm -qp` (NAME/VERSION/SUMMARY) **до** эвристик + guard на пустой
   `packageName` в обоих парсерах.
5. `rpm -qpR` → `metadata.depends`.
6. Извлечение rpm2cpio|cpio без `sh -c`.

**P1 — защита от регрессий:**
7. Unit-тесты чистых функций + интеграционные fixture-пакеты в CI (x86_64 PR-сборка).
8. Прогон regression-corpus как nightly.

**P2 — архитектурная консолидация:**
9. Объединить DebParser/RpmParser/TarballParser над общей базой.
10. Перенос app-специфичных эвристик в `compatibility_rules.json`.
11. unique_ptr, убрать `catch (...)`, `QLoggingCategory`, лог-файл конвертации.

**P3 — безопасность и UX:**
12. Checksum для appimagetool, скачивание вне GUI-потока, явное согласие.
13. polkit вместо sudo-пароля.
14. Проверка свободного места + переносимый temp-каталог.
