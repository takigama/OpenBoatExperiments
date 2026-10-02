# Every directory under platforms/ is a build target: it must contain
# build.env (GOOS/GOARCH/GOARM for that device) and profile.json (screen,
# touch). Adding a device = adding a directory, no Makefile edits.

IMAGE     := signalkpaperdisplay-build
DOCKER    := docker run --rm -v "$(CURDIR):/work" -v signalkpaperdisplay-gomod:/go/pkg/mod -w /work $(IMAGE)
PLATFORMS := $(patsubst platforms/%/,%,$(wildcard platforms/*/))

.PHONY: image tidy test host all clean list $(PLATFORMS)

image:
	docker build -t $(IMAGE) .

tidy:
	$(DOCKER) go mod tidy

test:
	$(DOCKER) go test ./...

# Native (linux/amd64) build - for running the PNG preview against a real
# or simulated SignalK server without touching a device.
host:
	$(DOCKER) go build -trimpath -o dist/host/paperdisplay ./cmd/paperdisplay

$(PLATFORMS):
	$(DOCKER) sh -c 'set -a; . platforms/$@/build.env; go build -trimpath -ldflags="-s -w" -o dist/$@/paperdisplay ./cmd/paperdisplay'

all: host $(PLATFORMS)

list:
	@echo $(PLATFORMS)

clean:
	rm -rf dist
