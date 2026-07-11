SHELL := /bin/sh

BUILD_DIR ?= build-runtime-werror
LINUX_BUILD_DIR ?= build-linux
GENERATOR ?= Ninja
VAGRANT_WORKDIR ?= /home/vagrant/minicam-hal
V4L2_DEVICE ?= /dev/video0
PREVIEW_V4L2_DEVICE ?= /dev/video0
STILL_V4L2_DEVICE ?= /dev/video4
WIDTH ?= 640
HEIGHT ?= 480
OUT ?= /tmp/minicam.ppm
PREVIEW_FRAMES ?= 5
ARTIFACTS_DIR ?= artifacts
PREVIEW_ARTIFACT_PPM ?= $(ARTIFACTS_DIR)/minicam-preview.ppm
PREVIEW_ARTIFACT_PNG ?= $(ARTIFACTS_DIR)/minicam-preview.png
PREVIEW_VM_OUT ?= /tmp/minicam-preview.ppm
CAPTURE_ARTIFACT_PPM ?= $(ARTIFACTS_DIR)/minicam-capture.ppm
CAPTURE_ARTIFACT_PNG ?= $(ARTIFACTS_DIR)/minicam-capture.png
CAPTURE_VM_OUT ?= /tmp/minicam-capture.ppm

.PHONY: all configure build test clean \
	vagrant-rsync vagrant-build vagrant-test \
	capture-mock capture-v4l2 vagrant-capture-mock vagrant-capture-v4l2 \
	vagrant-vivid-two-capture vagrant-capture-v4l2-multifd \
	vagrant-preview-v4l2 preview-v4l2-png capture-v4l2-png \
	vagrant-preview-v4l2-png vagrant-capture-v4l2-multifd-png

all: build

configure:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ] && \
		grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=' "$(BUILD_DIR)/CMakeCache.txt" && \
		! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$$(pwd)$$" "$(BUILD_DIR)/CMakeCache.txt"; then \
		echo "Removing stale CMake cache in $(BUILD_DIR)"; \
		rm -rf "$(BUILD_DIR)"; \
	fi
	cmake -S . -B $(BUILD_DIR) -G $(GENERATOR) -DMINICAM_ENABLE_WARNINGS_AS_ERRORS=ON

build: configure
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	cmake --build $(BUILD_DIR) --target clean

vagrant-rsync:
	vagrant rsync

vagrant-build: vagrant-rsync
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && cmake -S . -B $(LINUX_BUILD_DIR) -G Ninja -DMINICAM_ENABLE_WARNINGS_AS_ERRORS=ON && cmake --build $(LINUX_BUILD_DIR)'

vagrant-test: vagrant-build
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && ctest --test-dir $(LINUX_BUILD_DIR) --output-on-failure'

capture-mock: build
	printf 'capture $(OUT)\nquit\n' | ./$(BUILD_DIR)/minicam_cli --mock --width $(WIDTH) --height $(HEIGHT)

capture-v4l2: build
	printf 'capture $(OUT)\nquit\n' | ./$(BUILD_DIR)/minicam_cli --v4l2 $(V4L2_DEVICE) --width $(WIDTH) --height $(HEIGHT)

vagrant-capture-mock: vagrant-build
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && printf "capture $(OUT)\nquit\n" | ./$(LINUX_BUILD_DIR)/minicam_cli --mock --width $(WIDTH) --height $(HEIGHT) && file $(OUT)'

vagrant-capture-v4l2: vagrant-build
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && printf "capture $(OUT)\nquit\n" | ./$(LINUX_BUILD_DIR)/minicam_cli --v4l2 $(V4L2_DEVICE) --width $(WIDTH) --height $(HEIGHT) && file $(OUT)'

vagrant-vivid-two-capture:
	vagrant ssh -c 'sudo modprobe -r vivid || true; sudo modprobe vivid n_devs=2 && sudo chgrp video $(PREVIEW_V4L2_DEVICE) $(STILL_V4L2_DEVICE) && sudo chmod 660 $(PREVIEW_V4L2_DEVICE) $(STILL_V4L2_DEVICE) && v4l2-ctl -d $(PREVIEW_V4L2_DEVICE) --info >/dev/null && v4l2-ctl -d $(STILL_V4L2_DEVICE) --info >/dev/null'

vagrant-capture-v4l2-multifd: vagrant-build vagrant-vivid-two-capture
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && printf "capture $(OUT)\nquit\n" | ./$(LINUX_BUILD_DIR)/minicam_cli --preview-v4l2 $(PREVIEW_V4L2_DEVICE) --still-v4l2 $(STILL_V4L2_DEVICE) --width $(WIDTH) --height $(HEIGHT) && file $(OUT)'

vagrant-preview-v4l2: vagrant-build vagrant-vivid-two-capture
	vagrant ssh -c 'cd $(VAGRANT_WORKDIR) && ./$(LINUX_BUILD_DIR)/minicam_cli --preview-v4l2 $(PREVIEW_V4L2_DEVICE) --preview-out $(OUT) --preview-frames $(PREVIEW_FRAMES) --width $(WIDTH) --height $(HEIGHT) && file $(OUT)'

preview-v4l2-png:
	$(MAKE) vagrant-preview-v4l2 OUT=$(PREVIEW_VM_OUT) PREVIEW_FRAMES=$(PREVIEW_FRAMES) LINUX_BUILD_DIR=$(LINUX_BUILD_DIR)
	mkdir -p $(ARTIFACTS_DIR)
	vagrant ssh -c 'cat $(PREVIEW_VM_OUT)' > $(PREVIEW_ARTIFACT_PPM)
	sips -s format png $(PREVIEW_ARTIFACT_PPM) --out $(PREVIEW_ARTIFACT_PNG) >/dev/null
	file $(PREVIEW_ARTIFACT_PPM)
	file $(PREVIEW_ARTIFACT_PNG)

capture-v4l2-png:
	$(MAKE) vagrant-capture-v4l2-multifd OUT=$(CAPTURE_VM_OUT) LINUX_BUILD_DIR=$(LINUX_BUILD_DIR)
	mkdir -p $(ARTIFACTS_DIR)
	vagrant ssh -c 'cat $(CAPTURE_VM_OUT)' > $(CAPTURE_ARTIFACT_PPM)
	sips -s format png $(CAPTURE_ARTIFACT_PPM) --out $(CAPTURE_ARTIFACT_PNG) >/dev/null
	file $(CAPTURE_ARTIFACT_PPM)
	file $(CAPTURE_ARTIFACT_PNG)

vagrant-preview-v4l2-png: preview-v4l2-png

vagrant-capture-v4l2-multifd-png: capture-v4l2-png
