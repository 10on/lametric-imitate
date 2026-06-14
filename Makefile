PIO := $(HOME)/.platformio/penv/bin/pio

.PHONY: build upload flash monitor clean

all: upload

build:
	$(PIO) run -e ota

upload: build
	$(PIO) run -e ota -t upload

flash: build
	$(PIO) run -e usb -t upload

monitor:
	$(PIO) device monitor --filter esp32_exception_decoder

clean:
	$(PIO) run -t clean
