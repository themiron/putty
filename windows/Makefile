TXTVER = $(shell git describe --tags 2>/dev/null)
BINVER = $(shell git describe --tags --abbrev=0 2>/dev/null | tr . ,),0,0
COMMIT = $(shell git rev-parse HEAD 2>/dev/null)

all: x64

x64: version
	TOOLPATH=x86_64-w64-mingw32- \
	$(MAKE) -f Makefile.mgw

x32: version
	TOOLPATH=i686-w64-mingw32- \
	$(MAKE) -f Makefile.mgw

clean:
	$(MAKE) -f Makefile.mgw clean

version:
	sed -E 's/(define\s+TEXTVER\s*).*/\1"PuTTY build $(TXTVER)"/' -i ../version.h
	sed -E 's/(define\s+SSHVER\s*).*/\1"-Build-$(TXTVER)"/' -i ../version.h
	sed -E 's/(define\s+BINARY_VERSION\s*).*/\1$(BINVER)/' -i ../version.h
	sed -E 's/(define\s+SOURCE_COMMIT\s*).*/\1"$(COMMIT)"/' -i ../version.h

.PHONY: version
