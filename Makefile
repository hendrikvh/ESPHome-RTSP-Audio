# Dockerised entry points for `esphome config`, `esphome compile`, and
# `pre-commit`. The only host dependency is Docker — no Python, no
# ESP-IDF toolchain. CI uses the same images and the same commands.

ESPHOME_IMAGE   ?= esphome/esphome:2026.5.0
# python:3.12 (not -slim) is the buildpack-deps variant, which ships git
# and the other dev tools pre-commit's hook envs expect.
PRECOMMIT_IMAGE ?= python:3.12
# Stock gcc image is fine for the host C++ unit tests; cmake + git are
# installed at run time (cached via the apt named volume below).
CPP_TEST_IMAGE  ?= gcc:13

TESTS_DIR := tests/components/rtsp_audio
YAMLS     := $(TESTS_DIR)/test.esp32-s2-idf.yaml $(TESTS_DIR)/test.esp32-s3-idf.yaml

# Single-board override: `make compile BOARD=s3-idf`.
BOARD ?=

DOCKER_RUN = docker run --rm -t \
	-v $(CURDIR):/config \
	-v esphome-rtsp-audio-platformio:/root/.platformio \
	-v esphome-rtsp-audio-piolibs:/root/.piolibdeps \
	-w /config \
	$(ESPHOME_IMAGE)

.DEFAULT_GOAL := help

.PHONY: help config compile compile-s2-idf compile-s3-idf lint test cpp-test clean

help: ## Show this help.
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z0-9_-]+:.*##/ {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo ""
	@echo "Variables:"
	@echo "  ESPHOME_IMAGE   = $(ESPHOME_IMAGE)"
	@echo "  PRECOMMIT_IMAGE = $(PRECOMMIT_IMAGE)"
	@echo "  CPP_TEST_IMAGE  = $(CPP_TEST_IMAGE)"
	@echo "  BOARD           = $(BOARD)  (override: make compile BOARD=s3-idf)"

config: ## Validate every test YAML with `esphome config`.
	@for f in $(YAMLS); do \
		echo "==> esphome config $$f"; \
		$(DOCKER_RUN) config $$f || exit $$?; \
	done

compile: ## Build firmware for every test YAML (or one, via BOARD=).
	@if [ -n "$(BOARD)" ]; then \
		f="$(TESTS_DIR)/test.esp32-$(BOARD).yaml"; \
		echo "==> esphome compile $$f"; \
		$(DOCKER_RUN) compile $$f; \
	else \
		for f in $(YAMLS); do \
			echo "==> esphome compile $$f"; \
			$(DOCKER_RUN) compile $$f || exit $$?; \
		done; \
	fi

compile-s2-idf: ## Build firmware for the ESP32-S2 + ESP-IDF test config.
	$(DOCKER_RUN) compile $(TESTS_DIR)/test.esp32-s2-idf.yaml

compile-s3-idf: ## Build firmware for the ESP32-S3 + ESP-IDF test config.
	$(DOCKER_RUN) compile $(TESTS_DIR)/test.esp32-s3-idf.yaml

cpp-test: ## Build and run host C++ unit tests in Docker (gtest, ctest).
	docker run --rm -t \
		-v $(CURDIR):/src \
		-v esphome-rtsp-audio-cpp-build:/src/tests/native/build \
		-v esphome-rtsp-audio-cpp-apt-archives:/var/cache/apt/archives \
		-v esphome-rtsp-audio-cpp-apt-lists:/var/lib/apt/lists \
		-w /src \
		$(CPP_TEST_IMAGE) \
		sh -c "rm -f /etc/apt/apt.conf.d/docker-clean && \
		       echo 'Binary::apt::APT::Keep-Downloaded-Packages \"true\";' > /etc/apt/apt.conf.d/keep-cache && \
		       apt-get update -qq && \
		       apt-get install -y --no-install-recommends cmake git ca-certificates >/dev/null && \
		       cmake -S tests/native -B tests/native/build -DCMAKE_BUILD_TYPE=Release && \
		       cmake --build tests/native/build --parallel && \
		       ctest --test-dir tests/native/build --output-on-failure"

test: config cpp-test ## Validate schemas + run C++ unit tests (full local test suite).

lint: ## Run pre-commit hooks against the whole repo.
	docker run --rm -t \
		-v $(CURDIR):/src \
		-v esphome-rtsp-audio-precommit:/root/.cache/pre-commit \
		-v esphome-rtsp-audio-pip:/root/.cache/pip \
		-w /src \
		-e PRE_COMMIT_HOME=/root/.cache/pre-commit \
		$(PRECOMMIT_IMAGE) \
		sh -c "pip install --quiet --root-user-action=ignore pre-commit && pre-commit run --all-files --show-diff-on-failure"

clean: ## Remove .esphome/ build artefacts.
	rm -rf .esphome
