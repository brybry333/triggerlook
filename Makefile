# triggerlook — Linux x64 build: build/triggerlook.so (the mod), build/libtriggerlook.so (the LD_PRELOAD loader that
# loads it), build/geomtest (offline test). Windows: win/build.sh (same sources, clang-cl).
# The mod needs only the Source SDK headers (third_party/source-sdk-2013, see README) and the game's tier0.
SDK      := third_party/source-sdk-2013/src
LIBPUB   := $(SDK)/lib/public/linux64
BUILD    := build

CXX      := g++
CC       := gcc
DEFINES  := -DGNUC -DPOSIX -D_POSIX -DCOMPILER_GCC -D_DLL_EXT=.so -D_LINUX -DLINUX -DPLATFORM_64BITS \
            -DVPC -DSOURCESDK -DNDEBUG -DCLIENT_DLL -DNO_MALLOC_OVERRIDE -Usprintf -Ustrncpy -UPROTECTED_THINGS_ENABLE
WARN     := -Wall -Wno-multichar -Wno-write-strings -Wno-unused-variable -Wno-unused-function -Wno-sign-compare \
            -Wno-unused-but-set-variable -Wno-unused-local-typedefs -Wno-switch -Wno-parentheses -Wno-unknown-pragmas \
            -Wno-unused-parameter -Wno-comment -Wno-ignored-attributes -Werror=return-type
WARNCXX  := -Wno-overloaded-virtual -Wno-class-memaccess -Wno-invalid-offsetof -Wno-reorder -Wno-deprecated-declarations
OPT      := -O2 -g -fno-strict-aliasing -fno-omit-frame-pointer
COMMON   := -march=x86-64 -fPIC -pipe -fvisibility=hidden '-ffile-prefix-map=$(CURDIR)=.' '-ffile-prefix-map=$(PWD)=.' $(OPT) $(DEFINES) \
            -I$(SDK)/common -I$(SDK)/public -I$(SDK)/public/tier0 -I$(SDK)/public/tier1
CXXFLAGS := $(COMMON) $(WARN) $(WARNCXX) -std=gnu++17 -Isrc
CFLAGS   := -O2 -g -fPIC -fvisibility=hidden '-ffile-prefix-map=$(CURDIR)=.' '-ffile-prefix-map=$(PWD)=.' -Wall

MOD_SRC  := $(wildcard src/*.cpp)
MOD_OBJ  := $(MOD_SRC:src/%.cpp=$(BUILD)/obj/%.o) $(BUILD)/obj/LzmaDec.o

all: $(BUILD)/triggerlook.so $(BUILD)/libtriggerlook.so $(BUILD)/geomtest

$(BUILD)/obj/%.o: src/%.cpp src/*.h src/tl_files.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -c $< -o $@
# the game files the mod puts in place itself (tl_runtime.cpp InstallGameFiles)
src/tl_files.h: tools/embed.py $(shell find game -type f)
	python3 tools/embed.py game $@
$(BUILD)/obj/LzmaDec.o: $(SDK)/utils/lzma/C/LzmaDec.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -c $< -o $@

# loadable on the Steam runtime's glibc 2.31 (src/glibc_compat.cpp); exports only TriggerLook_Init, triggerlook_enable_set
$(BUILD)/triggerlook.so: $(MOD_OBJ) src/triggerlook.ver
	$(CXX) -shared -o $@ $(MOD_OBJ) -static-libstdc++ -static-libgcc -Wl,--build-id -Wl,-z,noexecstack \
	    -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,--version-script=src/triggerlook.ver -L$(LIBPUB) -ltier0 \
	    -Wl,--no-as-needed -l:libdl.so.2 -l:libpthread.so.0 -lm

# the LD_PRELOAD loader: no dependencies (it's in every process css-launch starts), loads triggerlook.so in the game
$(BUILD)/libtriggerlook.so: src/loader/triggerlook_loader.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -shared -o $@ $< -ldl

# offline test of the map reader + mesh builder: build/geomtest <cstrike dir> <maps/x.bsp> [out.obj]
$(BUILD)/geomtest: tools/geomtest.cpp $(BUILD)/obj/tl_map.o $(BUILD)/obj/tl_mesh.o $(BUILD)/obj/tl_look.o \
                   $(BUILD)/obj/tl_platform_linux.o $(BUILD)/obj/LzmaDec.o
	$(CXX) $(CXXFLAGS) $^ -L$(LIBPUB) -ltier0 -lm -ldl -lpthread -o $@

clean:
	rm -rf $(BUILD)

# the Steam runtime has glibc 2.31: nothing newer may be needed; the mod exports only its two functions
check-glibc: $(BUILD)/triggerlook.so $(BUILD)/libtriggerlook.so
	@for f in $^; do v=$$(objdump -T $$f | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1); echo "$$f: highest glibc symbol $$v"; \
	  case "$$v" in GLIBC_2.[0-9]|GLIBC_2.[0-9].*|GLIBC_2.[12][0-9]|GLIBC_2.[12][0-9].*|GLIBC_2.3[01]) ;; *) echo "too new"; exit 1 ;; esac; done
	@e=$$(nm -D --defined-only $(BUILD)/triggerlook.so | grep -v ' [AB] _\|_edata\|_end\|__bss_start' | awk '{print $$3}' | LC_ALL=C sort | tr '\n' ' '); \
	  echo "triggerlook.so exports: $$e"; [ "$$e" = "TriggerLook_Init triggerlook_enable_set " ]

.PHONY: all clean check-glibc
