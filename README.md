# Пошукова система з паралельною індексацією та інтеграцією LLM

Курсова робота з дисципліни «Паралельні обчислення» (НН ІПСА КПІ ім. Ігоря Сікорського, 2026–2027).
Тема: «Дослідження та реалізація паралельних обчислень на прикладі побудови пошукової системи з інтеграцією LLM».

Сервер на C++20 будує інвертований індекс текстових файлів у власному пулі потоків, на власній
потокобезпечній хеш-таблиці, і виконує пошук із розширенням запиту через Ollama.
Доступні REST API, WebSocket зі стрімінгом етапів пошуку, CLI-клієнт і Web UI.

## Вимоги до середовища

Для запуску через Docker (основний спосіб):

- Docker з Docker Compose v2 (`docker compose`);
- близько 1 ГБ вільного місця для моделі `qwen2.5:1.5b` (том `ollama_data`) і доступ до інтернету під час першого запуску;
- Python 3 для скрипта завантаження датасету (лише стандартна бібліотека).

Для нативної збірки додатково: компілятор з підтримкою C++20 (GCC ≥ 11 або Clang ≥ 14), CMake ≥ 3.20 і доступ до інтернету
(залежності `cpp-httplib` v0.67.0 та `nlohmann/json` v3.12.0 завантажує CMake через `FetchContent`).
На macOS: `brew install cmake`. Для скриптів бенчмарків: `pip install -r scripts/requirements.txt`.

## Датасет

Використовується піднабір PG-19 (англійські книги): файли `train/<id>.txt` з `id` від 10 до 30000
(10 746 файлів, близько 4,16 ГБ). Датасет не зберігається в репозиторії.

```bash
# повний набір
python3 scripts/prepare_dataset.py --out ./dataset

# малий набір для розробки (перші 200 файлів за порядком id) в окрему теку
python3 scripts/prepare_dataset.py --out ./dataset_small --limit 200
```

Скрипт завантажує файли паралельно і дозавантажує лише ті, що відсутні або мають неправильний розмір.
Сервер рекурсивно шукає `*.txt` у теці `DATASET_DIR`. Якщо тека порожня або відсутня, сервер стартує штатно,
а індексація повертає `documents_indexed: 0`.

## Запуск через Docker Compose

```bash
docker compose up
```

Команда збирає образ `search-server`, піднімає `ollama` та `search-server` і запускає одноразовий `ollama-init`,
який завантажує модель (`ollama pull`) і прогріває її. Перший запуск довший через завантаження моделі (~1 ГБ).

За замовчуванням датасет береться з `./dataset`. Щоб вказати іншу теку:

```bash
DATASET_PATH=/шлях/до/dataset docker compose up
```

Перевірка стану (відповідь 200 приходить одразу після старту `search-server`; `ollama` стає `true`,
коли модель завантажена):

```bash
curl http://localhost:8080/api/health
```

Якщо `ollama pull` не вдався (наприклад, не було мережі), `ollama-init` завершується з помилкою.
Це не заважає `search-server`: `/api/health` повертає 200 з `"ollama": false`, а пошук працює без розширення
запиту (використовуються лише терміни запиту). Щоб повторити завантаження моделі:

```bash
docker compose up ollama-init
```

Зупинка: `docker compose down` (том `ollama_data` зберігається; `docker compose down -v` видаляє і його).

## REST API

Усі відповіді мають `Content-Type: application/json`, включно з помилками (поле `error`).
Сервер слухає порт `8080`. Приклади нижче виконано на корпусі з двох невеликих файлів.

### `GET /api/health`

```bash
curl http://localhost:8080/api/health
```

```json
{"ollama":false,"status":"ok"}
```

Відповідь завжди 200. `ollama` — чи відповідає Ollama і чи є в ній потрібна модель.

### `POST /api/index`

Будує індекс із файлів датасету у пулі з `threads` потоків (1–256). Запит синхронний: відповідь приходить
після завершення побудови.

```bash
curl -X POST http://localhost:8080/api/index \
  -H 'Content-Type: application/json' \
  -d '{"threads": 4}'
```

```json
{"documents_indexed":2,"status":"ok","time_ms":8}
```

Паралельний `POST /api/index` під час побудови повертає 409 `{"error": "Indexing already in progress"}`.
Некоректний `threads` (відсутній, не ціле, поза діапазоном) — 400.

### `POST /api/search`

```bash
curl -X POST http://localhost:8080/api/search \
  -H 'Content-Type: application/json' \
  -d '{"query": "ghost", "mode": "keyword"}'
```

```json
{"results":[{"doc_id":"1.txt","score":1.0,"snippet":"The ghost walked the ocean voyage at night. ..."}],"time_ms":0}
```

Запит розширюється через Ollama, потім терміни шукаються в індексі. Якщо Ollama недоступна або не вклалась у
`LLM_TIMEOUT_MS`, пошук виконується за термінами запиту. Дозволений лише `"mode": "keyword"`.
Порожній `query` або інший `mode` дають 400, наприклад:

```json
{"error":"Field 'query' must be a non-empty string"}
```

Запит українською також допустимий: LLM перекладає його в англійські ключові слова.

### `GET /api/stats`

```bash
curl http://localhost:8080/api/stats
```

```json
{"documents":2,"index_build_time_ms":8,"threads_used":4}
```

До першої індексації: `{"documents":0,"index_build_time_ms":0,"threads_used":0}`.

### Коди помилок

| Код | Ситуація |
| :--- | :--- |
| 400 | некоректний JSON або поля `threads` / `query` / `mode` |
| 404 | невідомий маршрут |
| 409 | індексація вже виконується |
| 413 | тіло запиту більше 1 МБ |
| 500 | внутрішня помилка |

## WebSocket `/ws/search`

Клієнт надсилає `{"query": "..."}`, сервер надсилає по одному повідомленню на етап:
`query_expansion` → `keyword_search` → `done`. Після `done` з'єднання залишається відкритим.
У разі помилки (невалідний JSON або порожній `query`) приходить `{"stage": "error", ...}`
і сервер закриває з'єднання.

Приклад на Python (бібліотека `websockets` входить до `scripts/requirements.txt`):

```bash
pip install websockets
```

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://localhost:8080/ws/search") as ws:
        await ws.send(json.dumps({"query": "ghost"}))
        for _ in range(3):
            print(await ws.recv())

asyncio.run(main())
```

Вивід:

```json
{"data":{"expanded_terms":["ghost"],"original":"ghost"},"stage":"query_expansion"}
{"data":{"results":[{"doc_id":"1.txt","score":1.0,"snippet":"The ghost walked the ocean voyage at night. ..."}],"time_ms":0},"stage":"keyword_search"}
{"data":{"total_time_ms":0},"stage":"done"}
```

Із працюючою Ollama `expanded_terms` містить додаткові терміни від LLM після термінів запиту.

## CLI

Клієнт `search` працює через REST API.

Нативна збірка (разом із сервером):

```bash
# macOS: brew install cmake
cmake -S . -B build
cmake --build build --target search_server search
```

Використання:

```bash
./build/search "ghost"                 # пошук
./build/search --top 5 "ocean voyage"  # не більше 5 результатів
./build/search --index 8               # індексація з 8 потоками
./build/search --stats                 # статистика
./build/search --health                # стан сервера та Ollama
./build/search --host http://localhost:8080 "ghost"
```

Адресу сервера можна задати прапорцем `--host` або змінною `SEARCH_HOST` (за замовчуванням `http://localhost:8080`).
Вивід пошуку — таблиця (ранг, `doc_id`, score, snippet) і рядок «N results in X ms»:

```text
Rank  doc_id           Score    Snippet
1     1.txt            1.0000   The ghost walked the ocean voyage at night. ...
1 results in 0 ms
```

Коди виходу: 0 — успіх, 1 — помилка сервера або мережі, 2 — некоректні аргументи.

Усередині контейнера CLI вже доступний у `PATH`:

```bash
docker compose exec search-server search --index 8
docker compose exec search-server search "ghost"
docker compose exec search-server search --stats
```

## Web UI

Сервер віддає Web UI на `http://localhost:8080/`. Інтерфейс містить індикатор стану сервера та Ollama,
статистику індексу, панель індексації (кількість потоків і кнопка), форму пошуку та візуалізацію етапів
`query_expansion → keyword_search → done` з часом кожного етапу, розширеними термінами і результатами.
Пошук іде через WebSocket.

## Нативна збірка і запуск сервера

```bash
# macOS: brew install cmake
cmake -S . -B build
cmake --build build
```

Запуск (потрібна Ollama на `http://localhost:11434`, наприклад `docker compose up ollama ollama-init`;
без неї сервер працює з fallback):

```bash
DATASET_DIR=./dataset_small ./build/search_server
```

За замовчуванням у нативному режимі: `DATASET_DIR=./dataset`, `WEB_DIR=./web`, `OLLAMA_HOST=http://localhost:11434`.
Далі користуйтеся тими самими `curl`, CLI і Web UI на `http://localhost:8080`.
