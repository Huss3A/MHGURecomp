# MHGU Recomp build
# Windows: MSYS2 UCRT64 (clang, lld, make; see docs/BUILD-windows.md). Linux: clang (planned).
#
#   make gen GAME=<folder with exefs/ and romfs/>   recompile the game code from your own dump into generated/
#   make -j4                                        build build/mhgurecomp(.exe) and the game code library, with its
#                                                   DLLs and the game files (build/data): start build/mhgurecomp.exe
UNAME := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

GAME     ?= ../mhgu/game
GEN      ?= generated
B        ?= build
CC       := clang
CXX      := clang++
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA (Haswell or newer)
GENFLAGS := -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type -Wno-unused-label \
            -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden -Iruntime $(GENARCH)

GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(B)/obj/%.o,$(GEN_SRC)) $(B)/obj/tools/gamelib_entry.o $(B)/obj/simd_int.o \
            $(B)/obj/neon.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/net/*.c) \
            runtime/nvn/objects.c runtime/nvn/cmdbuf.c runtime/nvn/nvn_args.c runtime/nvn/tegra.c runtime/nvn/formats.c \
            runtime/nvn/backend_null.c runtime/nvn/astc.c runtime/nvn/maxwell/translate.c $(wildcard runtime/nvn/vulkan/*.c) \
            runtime/platform/$(PLATFORM).c
RT_SRC   := $(filter-out runtime/simd_int.c runtime/neon.c,$(RT_SRC))
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) $(wildcard runtime/nvn/*/*.h)

ifeq ($(PLATFORM),windows)
EXE      := $(B)/mhgurecomp.exe
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS
GENFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 -lSDL3 -lavformat -lavcodec -lavutil -lvulkan-1 -lshaderc_shared -lwinmm -lole32 -lshell32 -luuid -lws2_32 \
            -Wl,--export-all-symbols -Wl,--out-implib,$(B)/mhgurecomp.dll.a
GAMELIB  := $(B)/mhgugame.dll
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/mhgurecomp.dll.a
else
EXE      := $(B)/mhgurecomp
OSFLAGS  :=
GENFLAGS += -fPIC
LDFLAGS  := -fuse-ld=lld -rdynamic -lSDL3 -lavformat -lavcodec -lavutil -lvulkan -lshaderc_shared -lpthread -ldl -lm
GAMELIB  := $(B)/mhgugame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld
endif

# GAMELIB_ID: what the game code library is compiled against. A library with another id is refused at startup.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c \
                runtime/hle/hle_symbols.txt runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | sha256sum | cut -c1-16)

RT_OBJ   = $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC)) $(RT_CXX)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime $(OSFLAGS)
# NULL_GPU=1: no renderer (runtime/nvn/backend_null.c: the game runs, nothing is drawn)
ifeq ($(NULL_GPU),1)
RTFLAGS  += -DMHGU_NULL_BACKEND
RT_SRC   := $(filter-out runtime/nvn/vulkan/%,$(RT_SRC))
RT_CXX   :=
else
RT_CXX   := $(B)/rt/nvn/vulkan/vma.o
endif

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(if $(filter windows,$(PLATFORM)),bundle) data

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/vfp.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/simd_int.o: runtime/simd_int.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@
$(B)/obj/neon.o: runtime/neon.c runtime/cpu.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

# the id as a file, rewritten only when it changes: both sides of the library interface rebuild together
$(B)/gamelib.id: FORCE
	@mkdir -p $(B); [ "$$(cat $@ 2>/dev/null)" = '$(GAMELIB_ID)' ] || echo '$(GAMELIB_ID)' > $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(B)/gamelib.id
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(B)/gamelib.id
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(B)/rt/nvn/vulkan/vma.o: runtime/nvn/vulkan/vma.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w -c $< -o $@

$(EXE): $(RT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@

PY := $(firstword $(wildcard tools/venv/Scripts/python.exe) $(wildcard tools/venv/bin/python) python3)

# The build folder is the game folder: mhgurecomp.exe starts from it on its own (runtime/main.c), with
# - bundle (Windows): the MSYS2 DLLs it needs beside it (tools/bundle_dlls.py; not the GPU driver's vulkan-1.dll)
# - data: the game files from $(GAME) in $(B)/data/exefs and $(B)/data/romfs, once (hard links where the file system
#   allows them - no second copy on disk, and deleting them leaves $(GAME) alone - else copies). Saves, caches,
#   mods/, mhgurecomp.ini and log.txt go there too, so `clean` removes only what the build made.
bundle: $(EXE) $(if $(GEN_SRC),$(GAMELIB))
	@$(PY) tools/bundle_dlls.py "$$(cygpath -m "$$(dirname "$$(command -v $(CC))")")" $(B) $^

data: $(B)/data/.complete
$(B)/data/.complete:
	@if [ ! -f "$(GAME)/exefs/main" ] || [ ! -d "$(GAME)/romfs" ]; then \
	    echo "data: no game files in $(GAME) (make GAME=<folder with exefs/ and romfs/>): $(B)/data left empty"; \
	else \
	    mkdir -p $(B)/data && for d in exefs romfs; do \
	        rm -rf "$(B)/data/$$d.tmp"; \
	        cp -rl "$(GAME)/$$d" "$(B)/data/$$d.tmp" 2>/dev/null || { rm -rf "$(B)/data/$$d.tmp"; \
	            echo "data: no hard links here, copying $$d"; cp -r "$(GAME)/$$d" "$(B)/data/$$d.tmp"; } || exit 1; \
	        rm -rf "$(B)/data/$$d" && mv "$(B)/data/$$d.tmp" "$(B)/data/$$d" || exit 1; \
	    done && touch $@ && echo "data: game files in $(B)/data"; \
	fi

gen:
	cd tools && ../$(PY) gen.py sdk ../$(GAME)/exefs/sdk 0x02400000 ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt
	cd tools && ../$(PY) gen.py main ../$(GAME)/exefs/main 0x00200000 ../$(GEN)/main

# differential test of the recompiler against Unicorn (tools/test_lift.py)
test-lift:
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/main 0x00200000 --tests 6000
	cd tools && ../$(PY) test_lift.py ../$(GAME)/exefs/sdk 0x02400000 --tests 6000

runtime-objs: $(RT_OBJ)

clean:  # (keeps $(B)/data, saves, caches, settings: see above)
	rm -rf $(B)/obj $(B)/rt $(B)/*.dll $(B)/*.dll.a $(B)/gamelib.id $(EXE) $(B)/vkprobe.exe

.PHONY: all gen test-lift runtime-objs clean bundle data FORCE
