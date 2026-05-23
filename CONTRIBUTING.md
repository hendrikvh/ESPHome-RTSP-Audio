# Contributing

## Prerequisites

Only [Docker](https://docs.docker.com/get-docker/) is required. The
Makefile shells out to pinned ESPHome and Python images, so you do
not need a local Python, ESPHome, or ESP-IDF toolchain.

## Running tests locally

| Command | What it does |
|---------|--------------|
| `make help` | List every target. |
| `make config` | `esphome config` against each YAML in `tests/components/rtsp_audio/`. Fast. |
| `make compile` | `esphome compile` against each test YAML. Slow (full firmware build). |
| `make compile BOARD=s3-idf` | Build a single board. Values: `s2-idf`, `s3-idf`. |
| `make lint` | Run `pre-commit` (clang-format, ruff, file-hygiene hooks) against the whole repo. |
| `make clean` | Remove the `.esphome/` build cache. |

The image tags used by `make` are pinned at the top of the
[`Makefile`](Makefile) and match what CI runs.

## CI

GitHub Actions runs on every push and PR
([`.github/workflows/ci.yml`](.github/workflows/ci.yml)):

- `pre-commit` and `esphome config` always run.
- `esphome compile` runs the **ESP32-S3 + ESP-IDF** target on pull
  requests, and **both** S2-IDF and S3-IDF targets on pushes to
  `main`.

Run `make config` and `make compile` locally before opening a PR to
get the same answer CI will.

## Code style

`make lint` is authoritative. Hooks are defined in
[`.pre-commit-config.yaml`](.pre-commit-config.yaml): clang-format for
C++ (style matches [`.clang-format`](.clang-format), which mirrors
ESPHome's), ruff for Python, plus trailing-whitespace / EOL
hygiene.
