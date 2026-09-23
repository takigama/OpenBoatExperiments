# Reproducible build+flash image for ESP32Seatalk. Mirrors the pattern used
# for the AVR/SeaTalk CODE-lock firmware (see
# RaymarineST50Code/firmware/Dockerfile) - a small, purpose-built image
# checked into the repo instead of relying on whatever toolchain happens to
# be installed on whichever machine is building.
#
# Build:
#   docker build -t esp32seatalk-build .
#
# Compile only:
#   docker run --rm -v "$(pwd):/work" esp32seatalk-build pio run
#
# Compile + flash (needs the board's serial device passed through - it
# enumerates as /dev/ttyACM0 on the C3's native USB JTAG/serial controller,
# not /dev/ttyUSB0 - see ESP32Seatalk's schematic notes):
#   docker run --rm -v "$(pwd):/work" --device=/dev/ttyACM0 esp32seatalk-build \
#       pio run -t upload --upload-port /dev/ttyACM0
#
# Serial monitor:
#   docker run --rm -it -v "$(pwd):/work" --device=/dev/ttyACM0 esp32seatalk-build \
#       pio device monitor -p /dev/ttyACM0

FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio

WORKDIR /work

# Pre-fetch the espressif32 platform + toolchain into the image itself at
# build time, not at first-flash time - keeps `docker run ... pio run` from
# needing network access to PlatformIO's package index, and pins exactly
# which toolchain version this image builds with regardless of what's
# current upstream later.
COPY platformio.ini .
RUN mkdir -p src && echo 'void setup(){} void loop(){}' > src/main.cpp \
    && pio run \
    && rm -rf src .pio/build

CMD ["pio", "run"]
