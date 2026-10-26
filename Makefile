# MHGU Recomp build
# Windows: MSYS2 UCRT64 (clang, lld, make; see README.md). Linux: clang (planned).
#
#   make gen GAME=<folder with exefs/ and romfs/>   recompile the game code from your own dump into generated/
#   make -j4                                        build build/mhgurecomp(.exe) and the game code library, with its
#                                                   DLLs and the game files (build/data): start build/mhgurecomp.exe
# build/ holds only the game folder (what a player copies); the compiler's files go to obj/.
UNAME := $(shell uname -s)
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else
PLATFORM := linux
endif

GAME     ?= ../mhgu/game
GEN      ?= generated
B        ?= build
O        ?= obj
CC       := clang
CXX      := clang++
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA (Haswell or newer)
GENFLAGS := -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type -Wno-unused-label \
            -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden -Iruntime $(GENARCH)

GEN_SRC  := $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c)
GEN_OBJ  := $(patsubst $(GEN)/%.c,$(O)/gen/%.o,$(GEN_SRC)) $(O)/gen/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) $(wildcard runtime/net/*.c) \
            runtime/nvn/objects.c runtime/nvn/cmdbuf.c runtime/nvn/nvn_args.c runtime/nvn/tegra.c runtime/nvn/formats.c \
            runtime/nvn/backend_null.c runtime/nvn/astc.c runtime/nvn/maxwell/translate.c $(wildcard runtime/nvn/vulkan/*.c) \
            runtime/platform/$(PLATFORM).c
# (simd_int.c / neon.c, helpers of the recompiled code, live in the executable: the game code library imports them,
# and runtime/interp.c uses them too)
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) $(wildcard runtime/nvn/*/*.h)

ifeq ($(PLATFORM),windows)
EXE      := $(B)/mhgurecomp.exe
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_RAND_S -D_POSIX_THREAD_SAFE_FUNCTIONS
GENFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 -lSDL3 -lavformat -lavcodec -lavutil -lvulkan-1 -lshaderc_shared -lwinmm -lversion -lole32 -lshell32 -luuid -lws2_32 \
            -Wl,--export-all-symbols -Wl,--out-implib,$(O)/mhgurecomp.dll.a
GAMELIB  := $(B)/mhgugame.dll
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(O)/mhgurecomp.dll.a
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

RT_OBJ   = $(patsubst runtime/%.c,$(O)/rt/%.o,$(RT_SRC)) $(RT_CXX) $(O)/rt/overlay.o $(IMGUI_OBJ)
# Dear ImGui with cimgui's C API (third_party/cimgui: Forge plugins' ImGui, runtime/forge_imgui.c, overlay.cpp)
IMGUI    := third_party/cimgui
IMGUI_SRC := $(IMGUI)/cimgui.cpp $(addprefix $(IMGUI)/imgui/,imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp)
IMGUI_SRC += $(if $(filter 1,$(NULL_GPU)),,$(IMGUI)/imgui/backends/imgui_impl_vulkan.cpp)
IMGUI_OBJ := $(patsubst $(IMGUI)/%.cpp,$(O)/imgui/%.o,$(IMGUI_SRC))
IMGUIFLAGS := -O2 -g -DCIMGUI_NO_EXPORT -I$(IMGUI) -I$(IMGUI)/imgui
# (vendored VulkanMemoryAllocator fallback: -I on a missing dir is ignored, so MSYS2 builds
# with the system vulkan-memory-allocator package are unaffected)
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Ithird_party/vk_mem_alloc -I$(IMGUI) $(OSFLAGS)
# FSR 3.1 temporal upscaler (runtime/nvn/vulkan/fsr.c): third_party/fsr-vulkan = AMD FidelityFX SDK 1.1.4's FSR 3.1.4
# upscaler (host scheduler, Vulkan backend, its GLSL) + a portable C wrapper, all MIT. No AMD binaries: the shaders are
# compiled with shaderc from the embedded GLSL when first used (src/ffx_fsr3_shaderblobs.cpp), linked into the executable.
FSR      := third_party/fsr-vulkan
FFX      := $(FSR)/upstream/ffx-1.1.4/sdk
FSR_SRC  := $(FSR)/src/ffx_vk_portable.c $(FSR)/src/ffx_vk_fsr3_portable.cpp $(FSR)/src/ffx_fsr3_shaderblobs.cpp \
            $(FSR)/src/ffx_fsr3_glsl.c \
            $(addprefix $(FFX)/src/,shared/ffx_assert.cpp shared/ffx_message.cpp shared/ffx_object_management.cpp \
              shared/ffx_breadcrumbs_list.cpp components/fsr3upscaler/ffx_fsr3upscaler.cpp backends/vk/ffx_vk.cpp \
              backends/shared/ffx_shader_blobs.cpp)
FSR_OBJ  := $(patsubst $(FSR)/%,$(O)/fsr/%.o,$(FSR_SRC))
FSRFLAGS := -O2 -w -DFFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1 -DFFX_FSR3UPSCALER=1 -DDISABLE_FFX_DEBUG_BREAK=1 $(OSFLAGS) \
            -I$(FSR)/include -I$(FFX)/include -I$(FFX)/src/shared -I$(FFX)/src/components -I$(FFX)/src/backends/shared \
            -I$(FFX)/src/backends/shared/blob_accessors
# the GLSL embedded by ffx_fsr3_glsl.c (assembler .incbin, paths relative to the directory make runs in)
FFX_GPU  := $(FFX)/include/FidelityFX/gpu
FFX_PASS := $(FFX)/src/backends/vk/shaders/fsr3upscaler
$(O)/fsr/src/ffx_fsr3_glsl.c.o: FSRFLAGS += -DFFX_GPU_DIR='"$(FFX_GPU)"' -DFFX_PASS_DIR='"$(FFX_PASS)"'
$(O)/fsr/src/ffx_fsr3_glsl.c.o: $(wildcard $(FFX_GPU)/*.h $(FFX_GPU)/*/*.h $(FFX_PASS)/*.glsl)
RTFLAGS  += -I$(FSR)/include -Ithird_party/ffx-api
# FSR4_DRIVER_PATCH=1 (opt-in): FSR 4 can use the AMD driver's own amdvkffx64.dll directly, which needs one in-memory patch of
# that DLL's GetModuleHandleW import (runtime/nvn/vulkan/fsr4.c, docs/FSR4-PORT.md). The default, FSR4_DRIVER_PATCH=0, leaves
# all of that code out: nothing of AMD's is touched, and FSR 4 is only used through a loader DLL the player supplies (else FSR 3.1).
FSR4_DRIVER_PATCH ?= 0
.DEFAULT_GOAL := all
FSR4_STAMP := $(O)/fsr4_patch_$(FSR4_DRIVER_PATCH).stamp
$(FSR4_STAMP):
	@mkdir -p $(O) && rm -f $(O)/fsr4_patch_*.stamp && touch $@
$(O)/rt/nvn/vulkan/fsr4.o: $(FSR4_STAMP)
$(O)/rt/nvn/vulkan/fsr4.o: RTFLAGS += -DFSR4_DRIVER_PATCH=$(FSR4_DRIVER_PATCH)
# NULL_GPU=1: no renderer (runtime/nvn/backend_null.c: the game runs, nothing is drawn)
ifeq ($(NULL_GPU),1)
RTFLAGS  += -DMHGU_NULL_BACKEND
RT_SRC   := $(filter-out runtime/nvn/vulkan/%,$(RT_SRC))
RT_CXX   :=
else
RT_CXX   := $(O)/rt/nvn/vulkan/vma.o $(FSR_OBJ)
endif


all: $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(if $(filter windows,$(PLATFORM)),bundle) data

$(O)/gen/%.o: $(GEN)/%.c runtime/cpu.h runtime/vfp.h runtime/neon_rt.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@


# the id as a file, rewritten only when it changes: both sides of the library interface rebuild together
$(O)/gamelib.id: FORCE
	@mkdir -p $(O); [ "$$(cat $@ 2>/dev/null)" = '$(GAMELIB_ID)' ] || echo '$(GAMELIB_ID)' > $@

$(O)/gen/tools/gamelib_entry.o: tools/gamelib_entry.c $(O)/gamelib.id
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(O)/rt/gamelib.o: $(O)/gamelib.id
$(O)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'

$(O)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

$(O)/rt/forge_imgui.o: runtime/forge_imgui_thunks.inc $(IMGUI)/cimgui.h
$(O)/rt/forge_imgui.o: RTFLAGS += -DCIMGUI_NO_EXPORT -Wno-unused-variable

$(O)/rt/overlay.o: runtime/overlay.cpp runtime/overlay.h runtime/forge.h
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) $(if $(filter 1,$(NULL_GPU)),-DMHGU_NULL_BACKEND) -Wall -Iruntime -c $< -o $@

$(O)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUIFLAGS) -w -c $< -o $@

$(O)/rt/nvn/vulkan/vma.o: runtime/nvn/vulkan/vma.cpp
	@mkdir -p $(dir $@)
	$(CXX) -O2 -w -Ithird_party/vk_mem_alloc -c $< -o $@

$(O)/fsr/%.c.o: $(FSR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(FSRFLAGS) -c $< -o $@

$(O)/fsr/%.cpp.o: $(FSR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 $(FSRFLAGS) -include $(FSR)/src/$(if $(filter windows,$(PLATFORM)),ffx_mingw_portability.hpp,ffx_1_1_4_portability.hpp) -c $< -o $@

$(EXE): $(RT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(GAMELIB): $(GEN_OBJ) | $(EXE)
	@mkdir -p $(O)
	$(CC) $(GAMELIB_LDFLAGS) -o $(O)/gamelib.tmp $^
	mv -f $(O)/gamelib.tmp $@

PY := $(firstword $(wildcard tools/venv/Scripts/python.exe) $(wildcard tools/venv/bin/python) python3)

# The build folder is the game folder: mhgurecomp.exe starts from it on its own (runtime/main.c), with
# - bundle (Windows): the MSYS2 DLLs it needs beside it (tools/bundle_dlls.py; not the GPU driver's vulkan-1.dll)
# - data: the game files from $(GAME) in $(B)/data/exefs and $(B)/data/romfs, once (hard links where the file system
#   allows them - no second copy on disk, and deleting them leaves $(GAME) alone - else copies).
# Nothing else: a player copies it as a whole into their game folder (or over it: their settings, caches, mods/ and
# log.txt are written only when the game runs). run-game.bat runs it with those in run/ (MHGU_HOME), not here.
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

clean:  # (keeps $(B)/data, and the settings, caches and saves wherever the game ran)
	rm -rf $(O) $(B)/*.dll $(B)/*.so $(EXE) $(B)/vkprobe.exe

.PHONY: all gen test-lift runtime-objs clean bundle data FORCE
