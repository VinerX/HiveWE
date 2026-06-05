# План: CLI/агентская обёртка над HiveWE

Статус: черновик плана (ветка `worktree-cli-wrapper-plan`). Реализация ещё не начата.

Цель: AI-агент правит карту **через проверенные функции записи** (SLK shadow_data, бинарные modification-таблицы, штатные load/save), а не лезет в бинарник напрямую. Пример: «усиль всех гоблинов — подними хп и цену».

Среда: MSVC 17.14.37314 на месте, vcpkg-деки закэшированы. **Сборку проверяю сам** через Bash (`cmake --preset Release` → `cmake --build --preset Release`). Рантайм/UI-поведение проверяет пользователь.

---

## Фазировка (согласована с пользователем)

- **Фаза 0** — Qt-free **ядро** (`HiveWE_core`), на которое можно повесить CLI. Покрытие: **данные объектов, триггеры, кастомный код, общие параметры карты**. Ландшафт — НЕ приоритет.
- **Фаза 1** — собственно **CLI** поверх ядра + **хоткей/кнопка reload** в редакторе.
- **Фаза 1.5** — если выйдет: чтобы CLI сам инициировал reload открытого редактора (через слежение за файлами).
- **Фаза 2** — полноценная живая интеграция (встроенный сервер, правки без перезагрузки).

---

## 0. Проверенные факты о коде

- `src/file_formats/slk.ixx` — чистая key-value модель (`base_data` игра + `shadow_data` карта). `set_shadow_data`, `copy_row`, `data<T>`. **Qt нет.**
- `src/utilities/modification_tables.ixx` — чтение/запись `.w3u/.w3a/.w3t/.w3h/.w3q/.w3d/.w3b`. **Qt нет.**
- `src/base/triggers/triggers.ixx` — `class Triggers`: `categories`, `variables`, `triggers` (GUI-дерево ECA), плюс per-trigger `custom_text` и `global_jass` (кастомный код). `load()`/`save()`, `load_scripts()`/`save_scripts()`. **Qt нет.** Генерация финального скрипта — `triggers/map_script.cpp`.
- `src/base/map_info.ixx` — `MapInfo`: игроки, силы (forces), имя/описание, флаги, доступность апгрейдов/техов, random unit tables. **Qt нет.**
- `src/base/hierarchy.ixx` — **единственный Qt-узел** в data-цепочке: `QSettings` в конструкторе и `open_casc()`. `#include <QSettings>`.
- `src/base/map/map.ixx` — `class Map : public QObject` (`Q_OBJECT`, `QMessageBox`). Держит всё в памяти; рендер-путь.
- `src/models/table_model.ixx` — `TableModel` поверх SLK, шлёт `dataChanged` → вьюхи обновляются.
- `CMakeLists.txt:39` — `HiveWE_lib STATIC`, но PUBLIC-линкует весь Qt-стек (`CMakeLists.txt:65-87`). Сам по себе не Qt-free.
- **`data/tools/pjass.exe`** — валидатор JASS (+ `clijasshelper.exe`, `jasshelper.conf`). Ключ к «проверенным инструментам»: валидировать код агента перед записью.

Вывод: все четыре куска нулевого scope — Qt-free модули. Единственный физический блокер — `QSettings` в Hierarchy.

---

## Фаза 0 — ядро `HiveWE_core` (Qt-free)

### 0.1 Снять Qt-узел в Hierarchy ✅ СДЕЛАНО (сборка проверена)
Вынести `local_files/ptr/hd/teen/warcraft_directory` в поля, заполняемые снаружи. Заменить оба `QSettings`. GUI заполняет их в `main` из QSettings, CLI — из аргументов/env. После этого SLK/ModTables/Triggers/MapInfo собираются без Qt.
- `hierarchy.ixx`: убран `#include <QSettings>` и `module;`-фрагмент; ctor = `default`; `open_casc` больше не читает настройки.
- `main.cpp`: перед запуском CASC-потока выставляет `hierarchy.ptr/hd/teen` из QSettings и `local_files` из реестра Blizzard.
- `hivewe.cpp::switch_warcraft`: обновляет флаги перед повторным `open_casc`.
- Проверено: `cmake --build --preset Release` → `HiveWE.exe` + `HiveWE_tests.exe` собрались, 0 ошибок.

### 0.2 Headless-контейнер карты `CoreMap`
POD-контейнер без QObject/рендера:
- object-SLK (units/items/abilities/buffs/upgrades/doodads/destructables) + их meta-SLK;
- `Triggers` (GUI-дерево + custom_text + global_jass);
- `MapInfo` (игроки, силы, общие параметры).

Свободные функции `core::load(CoreMap&, dir)` / `core::save(CoreMap&, dir)`, переиспользующие существующие `load/save_modification_table`, `Triggers::load/save`, `MapInfo::load/save`.

**Реальный объём (изучено):** загрузка object-data в `Map::load` ([map.ixx:140-340+](src/base/map/map.ixx:140)) — большой блок: для каждого типа строится base-SLK из игровых `*.slk` + meta-SLK (`build_meta_map`), мержатся десятки `*Func.txt`/`*Strings.txt`/`*Skin.txt` INI и доп. SLK, добавляются колонки, правится shadow-data meta, делается `substitute(world_edit_strings)`. Поверх накладываются modification-таблицы карты (`.w3u/.w3a/...`). Это не «три вызова» — нужно либо **вынести этот блок в переиспользуемую функцию** (предпочтительно: один источник истины, GUI и CLI зовут одно и то же), либо аккуратно воспроизвести. План: вынести «object-data load» из `Map::load` в свободную функцию в `map.ixx`/новом модуле, которую зовут и `Map::load`, и `core::load` — **без** конструирования `RenderManager`/террейна.

### 0.3 Слой резолва «человеческое → ID/код» (критично для use-case)
- Имя объекта → id: индекс name→id из отображаемых имён (`WorldEditStrings.txt` + SLK `Name/Tip`), поиск по подстроке. Locale Hierarchy уже автодетектит.
- Имя поля → код: таблица синонимов («хп→hithpoints», «цена→goldcost») + прямой доступ по коду.
- Неоднозначности возвращать списком, не угадывать.

### 0.4 CMake — РЕШЕНО: прагматичный путь (CLI линкует HiveWE_lib)
**Находка:** чистый Qt-free `HiveWE_core` блокируется транзитивными зависимостями:
- `Utilities` тянет `<QSettings>` (только `find_warcraft_directory()` — легко вынести в GUI).
- `Triggers` импортирует `Terrain/Units/Doodads/Regions/...`, а `Terrain` тянет `<QObject>`, `<brush.h>`, `glad`, шейдеры. Причина — генерация map-скрипта. Сериализация данных триггеров (.wtg/.wct) Qt не требует, но импорты класса притаскивают рендерер на этапе компиляции.

**Решение (согласовано):** для v0 НЕ карвим Qt-free ядро. Новый таргет `HiveWE_cli` (executable) **линкуется к существующему `HiveWE_lib`** — Qt тянется как зависимость линковки, но окно не открывается. Это даёт полный функционал (object-data + map-info + триггеры) быстро. Чистый core и расщепление Triggers — отложенный рефактор (фаза «позже»).

**Важно (рантайм):** даже линкуя `HiveWE_lib`, CLI **НЕ зовёт полный `Map::load`** — тот строит `RenderManager`/GL-ресурсы террейна и требует GL-контекст. Вместо этого headless-загрузка (0.2) грузит только нужное: object-SLK + mod-tables + `MapInfo` + данные `Triggers` (`load`/`load_scripts`), без террейна/рендера. Сохранение триггеров = `.wtg/.wct` (`Triggers::save`/`save_scripts`), БЕЗ регенерации `war3map.j` (она требует размещённых объектов/террейна).

### Риски фазы 0
- Headless-загрузка должна обходить GL-путь `Map`. Конструируем нужные объекты (SLK, Triggers, MapInfo) напрямую и зовём их `load`, не трогая `RenderManager`. Проверяю сборку+рантайм итеративно.
- `find_warcraft_directory()` в Utilities — мелкий Qt-узел; для CLI путь к WC3 берём из аргумента, функцию не зовём. Чистка Utilities — опциональна.

---

## Инструменты агента (то, что CLI экспонирует)

Основной интерфейс — **JSON-режим** (один запрос = массив операций, один ответ = результаты/диффы/ошибки). Плюс человекочитаемые подкоманды. Сгруппировано по назначению.

### Чтение / разведка («посмотри что как»)
| Инструмент | Назначение |
|---|---|
| `list-types` | какие категории объектов есть |
| `search-objects --type unit --name "гоблин"` | id + отображаемые имена (резолв имён) |
| `get-object <id>` | все поля (base+shadow), с человеческими именами |
| `list-fields --type unit` | коды полей + имена + meta (тип, min/max) |
| `resolve-field "хп" --type unit` | код(ы) поля по синониму |
| `list-triggers` / `get-trigger <name>` | дерево триггеров; чтение custom-text/JASS |
| `get-custom-script` | глобальный кастомный код карты |
| `get-map-info` | игроки, силы, имя, флаги |

### Запись данных объектов
| Инструмент | Назначение |
|---|---|
| `set-field <id> <field> <value>` | одиночная правка (`set_shadow_data`) |
| `batch-edit <json>` | много объектов/полей разом — это «усиль всех гоблинов» |
| `copy-object <src> <newid>` | создать кастомный объект (`copy_row`) |
| `reset-field <id> <field>` | снять shadow-переопределение |

### Триггеры / код
| Инструмент | Назначение |
|---|---|
| `set-custom-script <text>` | заменить глобальный кастомный код |
| `set-trigger-text <name> <text>` | тело custom-text триггера |
| GUI-триггеры (ECA-деревья) | **v0: только чтение.** Программная сборка ECA — отдельный большой surface, не в нулевой scope |

### Общие параметры карты
| Инструмент | Назначение |
|---|---|
| `set-map-info <key> <value>` | имя, описание, флаги |
| `set-player` / `set-force` | игроки и силы |

### Сборка / валидация / запуск карты (новое требование)
Сценарий: агент уже переписал карту (напр. в Lua, папка `C:\Games\23 Race\LUA\map_lua`), CLI должен её собрать/проверить/запустить. Здесь скрипт уже написан — **регенерация из триггеров не нужна** (тот путь render-bound через terrain/units). Изучено в коде:
- упаковка папки → `.w3x`: чистый **StormLib** (`SFileCreateArchive`/`SFileAddFileEx`/`SFileCompactArchive`) — см. `export_mpq` ([hivewe.cpp:418](src/main_window/hivewe.cpp:418)), **Qt не нужен**;
- запуск: `Warcraft III.exe -launch -loadfile <папка|.w3x>` — см. `play_test` ([hivewe.cpp:454](src/main_window/hivewe.cpp:454)); Reforged грузит прямо из папки;
- JASS-компиляция: `data/tools/clijasshelper.exe`; Lua — `war3map.lua` кладётся как есть ([map_script.cpp:852](src/base/triggers/map_script.cpp:852)).

| Инструмент | Назначение |
|---|---|
| `build-map <dir> [--out map.w3x]` | упаковать папку карты в `.w3x` (StormLib). Без `--out` — только проверить, что папка валидна для запуска |
| `run-map <dir\|.w3x> [--args ...]` | запустить WC3 с `-loadfile`; вернуть статус старта |
| `validate-script <dir>` | JASS → `pjass.exe`/`clijasshelper --scriptonly`; **Lua → см. оговорку ниже** |
| `compile-script <dir>` (низкий приоритет) | регенерация `war3map.j`/`.lua` из триггеров — render-bound, для v0 не нужно |

**Lua-валидация — РЕШЕНО:** v0 — положить `luac.exe` (Lua 5.x, как в WC3) в `data/tools`, валидировать через `luac -p war3map.lua`. **В будущем — встроить Lua как зависимость** (vcpkg) и парсить в процессе CLI, убрав внешний exe. `pjass` остаётся для JASS-карт.

### Безопасность / «проверенность» (ядро ценности)
| Инструмент | Назначение |
|---|---|
| `validate` | прогнать **pjass.exe** по сгенерированному скрипту перед сохранением; отказ при ошибке кода |
| `--dry-run` / `diff` | показать, что изменится, без записи |
| авто-бэкап | копия map-файлов перед записью (откат при сбое) |
| транзакция | стейджинг правок → один `commit` (один save, один диф) |

### Жизненный цикл
`open <map>` · `save` / `save-as` · `status`.

---

## Фаза 1 — CLI + reload-хоткей

### 1.1 Фронтенд CLI
Таргет `HiveWE_cli` (executable, линкуется к `HiveWE_core`). Парсер аргументов + JSON-режим (`nlohmann-json` уже в деках). Реализация инструментов выше поверх ядра. pjass-валидация и авто-бэкап включены по умолчанию.

### 1.2 Хоткей/кнопка «Reload object data» в редакторе
Маленькое GUI-добавление: команда, перечитывающая object-данные (и при желании триггеры/map-info) текущей карты с диска в открытый `Map`, с обновлением моделей (`beginResetModel`/`dataChanged`). Это мост: агент правит через CLI (фаза 1) → юзер жмёт reload → видит новые значения. Без потоков и сети.

---

## Фаза 1.5 — CLI инициирует reload (если выйдет)
`QFileSystemWatcher` на каталог карты в запущенном HiveWE: при изменении map-файлов извне — предложить «файлы изменены на диске, перезагрузить?». Тогда правка из CLI автоматически подсвечивает reload в открытом редакторе. Чисто GUI-сторона, сети нет. Делается, только если дёшево ляжет поверх 1.2.

---

## Фаза 2 — полноценная живая интеграция
Встроенный локальный JSON-RPC сервер (`QLocalServer`/loopback) внутри HiveWE за фичефлагом `--enable-agent-server`. Правки идут через `TableModel` + `WorldUndoManager` (refresh + Ctrl+Z бесплатно). Главная сложность — **маршалинг в GUI-поток** (`QMetaObject::invokeMethod`, `Qt::QueuedConnection`); прямые мутации `Map` из сетевого потока запрещены. Переиспользует общий data+resolve слой из фазы 0. Объём кода умеренный (~600–1000 строк), но петля отладки потоков длинная.

---

## Порядок исполнения
1. **0.1** (снять QSettings) ✅ — сделано, сборка проверена.
2. **1.0 build/validate/run** — самостоятельный срез: `build-map` (StormLib→.w3x), `run-map` (-loadfile), `validate-script`. **Не зависит от 0.2** и закрывает конкретный сценарий пользователя (агент переписал карту в Lua → собрать/запустить). Рекомендуется делать ПЕРВЫМ после скелета CLI-таргета.
3. **0.2 + 0.3** — headless load/save object-data + резолв имён (тяжёлая экстракция из `Map::load`).
4. **1.1** — остальные инструменты CLI (edit/search/triggers) + pjass-валидация + бэкап.
5. **1.2** — reload-хоткей.
6. **1.5** — file-watcher reload (опционально).
7. **2** — живой сервер (по результатам практики).

Скелет `HiveWE_cli` (executable, линкует `HiveWE_lib`, парсит аргументы + JSON) — общий префикс для шагов 2–4.

Фазы 0.1–1.1 начинаю, как допишу/согласую план. Все фазы делят один data+resolve слой — поэтому ядро вперёд выгодно всегда.

## Решения (согласованы)
- **Охват object-типов в v0:** всё сразу — units, items, abilities, buffs, upgrades, doodads, destructables.
- **Резолв имён:** рус + англ (по автодетектед locale игры). Синонимы полей — на двух языках.
- **Триггеры в v0:** read всех триггеров + write кастом-кода (custom-text + глобальный JASS/Lua) с pjass-валидацией. GUI-триггеры (ECA) — только чтение.
- **Интерфейс:** оптимизирован под LLM-агента — структурный JSON-протокол как приоритет (само-описывающие ответы, явные ошибки, dry-run, schema через list-fields). Человеческие подкоманды — вторичны, по мере надобности для отладки.
