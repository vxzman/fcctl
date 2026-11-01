CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
SRCS := src/main.cpp src/json.cpp src/net.cpp
BIN := build/fcctl

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
GENERATORDIR ?= $(SYSCONFDIR)/systemd/system-generators

.PHONY: all test static install clean

all: $(BIN)

$(BIN): $(SRCS) src/json.hpp src/net.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN) self-test
	tests/check.sh $(BIN)

# Alpine's g++ targets musl. -static produces a musl static binary.
static:
	mkdir -p build
	docker run --rm -v "$(CURDIR)":/src -w /src alpine:3.22 \
		sh -c 'apk add --no-cache g++ linux-headers && g++ -std=c++17 -O2 -static -s -Wall -Wextra -Wpedantic -o build/fcctl src/main.cpp src/json.cpp src/net.cpp'
	file build/fcctl

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/fcctl
	install -d $(DESTDIR)$(SYSCONFDIR)/fcctl
	install -d $(DESTDIR)$(GENERATORDIR)
	ln -sf $(BINDIR)/fcctl $(DESTDIR)$(GENERATORDIR)/fcctl-generator
	@if [ ! -e $(DESTDIR)$(SYSCONFDIR)/fcctl/config.json ]; then \
		install -m 644 examples/config.json $(DESTDIR)$(SYSCONFDIR)/fcctl/config.json; \
		echo "Installed default config to $(SYSCONFDIR)/fcctl/config.json"; \
	else \
		echo "Kept existing $(SYSCONFDIR)/fcctl/config.json"; \
	fi

clean:
	rm -rf build
