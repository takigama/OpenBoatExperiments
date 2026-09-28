# Reproducible build+flash image for FishFinderProBluetoothCYD - same
# pattern as FishFinderProBluetooth/ESP32Seatalk's Dockerfiles.
#
# Build:
#   docker build -t fishfinderprobluetoothcyd-build .
#
# Compile only:
#   docker run --rm -v "$(pwd):/work" fishfinderprobluetoothcyd-build pio run
#
# Compile + flash over USB (CH340 USB-serial, enumerates as /dev/ttyUSB0
# once attached to the container/WSL2, unlike the C3 boards' native USB
# which show up as /dev/ttyACM0):
#   docker run --rm -v "$(pwd):/work" --device=/dev/ttyUSB0 fishfinderprobluetoothcyd-build \
#       pio run -t upload --upload-port /dev/ttyUSB0
#
# Serial monitor:
#   docker run --rm -it -v "$(pwd):/work" --device=/dev/ttyUSB0 fishfinderprobluetoothcyd-build \
#       pio device monitor -p /dev/ttyUSB0

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
