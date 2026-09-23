# Reproducible build+flash image for FishFinderProBluetooth - same pattern
# as ESP32Seatalk's Dockerfile.
#
# Build:
#   docker build -t fishfinderprobluetooth-build .
#
# Compile only:
#   docker run --rm -v "$(pwd):/work" fishfinderprobluetooth-build pio run
#
# Compile + flash (needed once, over USB, to bootstrap OTA - the C3's
# native USB-Serial/JTAG controller enumerates as /dev/ttyACM0, not
# /dev/ttyUSB0, same as ESP32Seatalk):
#   docker run --rm -v "$(pwd):/work" --device=/dev/ttyACM0 fishfinderprobluetooth-build \
#       pio run -t upload --upload-port /dev/ttyACM0
#
# Serial monitor:
#   docker run --rm -it -v "$(pwd):/work" --device=/dev/ttyACM0 fishfinderprobluetooth-build \
#       pio device monitor -p /dev/ttyACM0

FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio

WORKDIR /work

COPY platformio.ini .
RUN mkdir -p src && echo 'void setup(){} void loop(){}' > src/main.cpp \
    && pio run \
    && rm -rf src .pio/build

CMD ["pio", "run"]
