# Every directory under platforms/ is a build target: it must contain
# build.env (GOOS/GOARCH/GOARM for that device) and profile.json (screen,
# touch). Adding a device = adding a directory, no Makefile edits.
#
# VERSION holds the release number; it's stamped into the binary so the
# updater knows what it's running. Bump it before cutting a release.

IMAGE     := signalkpaperdisplay-build
DOCKER    := docker run --rm -v "$(CURDIR):/work" -v signalkpaperdisplay-gomod:/go/pkg/mod -w /work $(IMAGE)
PLATFORMS := $(patsubst platforms/%/,%,$(wildcard platforms/*/))
VERSION   := $(shell cat VERSION)
LDFLAGS   := -s -w -X main.version=$(VERSION)

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
	$(DOCKER) go build -trimpath -ldflags="$(LDFLAGS)" -o dist/host/paperdisplay ./cmd/paperdisplay

$(PLATFORMS):
	$(DOCKER) sh -c 'set -a; . platforms/$@/build.env; go build -trimpath -ldflags="$(LDFLAGS)" -o dist/$@/paperdisplay ./cmd/paperdisplay'

all: host $(PLATFORMS)

list:
	@echo $(PLATFORMS)

clean:
	rm -rf dist
