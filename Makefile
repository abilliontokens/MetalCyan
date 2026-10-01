# Builds MetalCyan.kext with clang and cctools-port ld64 (Linux, or macOS with LD64/STRIP pointed at Xcode's).
#
#   make                       # Debug build
#   make CONFIG=Release        # optimised, stripped
#   make zip
#
# Toolchain overrides: CLANG, LD64, STRIP, LLVM_TOOLS.

PRODUCT_NAME   := MetalCyan
MODULE_NAME    := com.amethyst8118.MetalCyan
MODULE_VERSION := 1.0.0
CONFIG         ?= Debug

CLANG  ?= $(firstword $(foreach c,clang-21 clang-20 clang-19 clang,$(shell command -v $(c) 2>/dev/null)))
CCTOOLS_PREFIX ?= /opt/cctools
LD64   ?= $(CCTOOLS_PREFIX)/bin/x86_64-apple-darwin-ld
STRIP  ?= $(CCTOOLS_PREFIX)/bin/x86_64-apple-darwin-strip
OTOOL  ?= $(CCTOOLS_PREFIX)/bin/x86_64-apple-darwin-otool
NM     ?= $(CCTOOLS_PREFIX)/bin/x86_64-apple-darwin-nm

# C++ runtime hooks the kernel does not export. A kext importing any of these links fine here but is refused by
# macOS at load time (it is silently not loaded). Xcode's kext mode never emits them.
KERNEL_MISSING_IMPORTS := ___cxa_atexit ___cxa_thread_atexit _atexit ___cxa_guard_acquire ___cxa_guard_release

SRC_DIR    := $(PRODUCT_NAME)
SDK_DIR    := MacKernelSDK
LILU_DIR   := Lilu/Lilu
BUILD_DIR  := build-linux/$(CONFIG)
OBJ_DIR    := $(BUILD_DIR)/obj
KEXT_DIR   := $(BUILD_DIR)/$(PRODUCT_NAME).kext

SOURCES := $(shell find $(SRC_DIR) -name '*.cpp' | sort) $(LILU_DIR)/Library/plugin_start.cpp
OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES)) $(OBJ_DIR)/$(PRODUCT_NAME)_info.o

TARGET := -target x86_64-apple-macos10.14

COMMON_FLAGS := $(TARGET) -mkernel -nostdinc -nostdlib -fno-builtin \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-non-call-exceptions -ftree-vectorize \
	-mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse \
	-fvisibility-inlines-hidden \
	-isystem $(SDK_DIR)/Headers \
	-isystem $(LILU_DIR) -I$(SRC_DIR) \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-DPRODUCT_NAME=$(PRODUCT_NAME) -DMODULE_VERSION=$(MODULE_VERSION) \
	-Wall -Wextra -Wno-c23-extensions -Wno-unknown-warning-option \
	-Werror=incompatible-pointer-types -Werror=return-type

ifeq ($(CONFIG),Debug)
COMMON_FLAGS += -O0 -DDEBUG=1 -DAPPLE_KEXT_ASSERTIONS=1 -DMACH_ASSERT=1
else ifeq ($(CONFIG),ResearchRelease)
COMMON_FLAGS += -O3 -DDEBUG=1 -DAPPLE_KEXT_ASSERTIONS=1 -DMACH_ASSERT=1 -fvisibility=hidden
else ifeq ($(CONFIG),Release)
COMMON_FLAGS += -O3 -fvisibility=hidden
else
$(error CONFIG must be Debug, ResearchRelease or Release)
endif

# -fno-use-cxa-atexit / -disable-atexit-based-global-dtor-lowering: put static destructors in __mod_term_func
# like Xcode's kext mode. Upstream LLVM otherwise lowers them to ___cxa_atexit, which the kernel does not export,
# and macOS refuses to load the kext. -fno-sized-deallocation: only the unsized operator delete is guaranteed.
CXXFLAGS := $(COMMON_FLAGS) -std=c++23 -fapple-kext -fno-rtti -fno-exceptions \
	-fno-use-cxa-atexit -fno-sized-deallocation \
	-mllvm -disable-atexit-based-global-dtor-lowering
CFLAGS   := $(COMMON_FLAGS) -std=c2x

LDFLAGS := -arch x86_64 -kext -static -Z -platform_version macos 10.14 11.0 \
	-L$(SDK_DIR)/Library/x86_64 -lkmod

.PHONY: all clean zip check-toolchain

all: check-toolchain $(KEXT_DIR)

check-toolchain:
	@test -n "$(CLANG)" || { echo "error: clang >= 19 not found (needed for C23 #embed)"; exit 1; }
	@$(CLANG) --version | head -1 | grep -Eq 'version (19|[2-9][0-9])\.' || \
		{ echo "error: $(CLANG) is older than clang 19 (needed for #embed)"; exit 1; }
	@test -x "$(LD64)" || { echo "error: ld64 not found at $(LD64); run scripts/setup-linux-toolchain.sh"; exit 1; }
	@test -f $(SDK_DIR)/Library/x86_64/libkmod.a || { echo "error: run 'git submodule update --init'"; exit 1; }
	@test -f $(LILU_DIR)/Library/plugin_start.cpp || { echo "error: run 'git submodule update --init'"; exit 1; }

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(CLANG) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Equivalent of the <PRODUCT>_info.c file Xcode generates for kernel extensions.
$(OBJ_DIR)/$(PRODUCT_NAME)_info.c:
	@mkdir -p $(dir $@)
	@printf '%s\n' \
		'#include <mach/mach_types.h>' \
		'extern kern_return_t _start(kmod_info_t *ki, void *data);' \
		'extern kern_return_t _stop(kmod_info_t *ki, void *data);' \
		'extern kern_return_t $(PRODUCT_NAME)_kern_start(kmod_info_t *ki, void *data);' \
		'extern kern_return_t $(PRODUCT_NAME)_kern_stop(kmod_info_t *ki, void *data);' \
		'__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($(MODULE_NAME), "$(MODULE_VERSION)", _start, _stop)' \
		'__private_extern__ kmod_start_func_t *_realmain = $(PRODUCT_NAME)_kern_start;' \
		'__private_extern__ kmod_stop_func_t *_antimain = $(PRODUCT_NAME)_kern_stop;' \
		'__private_extern__ int _kext_apple_cc = __APPLE_CC__;' > $@

$(OBJ_DIR)/$(PRODUCT_NAME)_info.o: $(OBJ_DIR)/$(PRODUCT_NAME)_info.c
	@echo "  CC   $<"
	@$(CLANG) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PRODUCT_NAME): $(OBJECTS)
	@echo "  LD   $@"
	@$(LD64) $(LDFLAGS) -o $@ $(OBJECTS)
	@bad="$$($(NM) -u $@ | grep -xF $(foreach s,$(KERNEL_MISSING_IMPORTS),-e $(s)))"; \
		if [ -n "$$bad" ]; then echo "error: kext imports symbols the kernel does not export:"; echo "$$bad"; \
		rm -f $@; exit 1; fi
ifneq ($(CONFIG),Debug)
	@$(STRIP) -x $@
endif

$(BUILD_DIR)/Info.plist: $(SRC_DIR)/Info.plist
	@mkdir -p $(dir $@)
	@sed -e 's/$$(EXECUTABLE_NAME)/$(PRODUCT_NAME)/g' \
	     -e 's/$$(PRODUCT_BUNDLE_IDENTIFIER)/$(MODULE_NAME)/g' \
	     -e 's/$$(PRODUCT_NAME:rfc1034identifier)/$(PRODUCT_NAME)/g' \
	     -e 's/$$(PRODUCT_NAME)/$(PRODUCT_NAME)/g' \
	     -e 's/$$(MODULE_VERSION)/$(MODULE_VERSION)/g' $< > $@
	@! grep -n '\$$(' $@ || { echo "error: unexpanded variable in Info.plist"; exit 1; }

$(KEXT_DIR): $(BUILD_DIR)/$(PRODUCT_NAME) $(BUILD_DIR)/Info.plist
	@rm -rf $@
	@mkdir -p $@/Contents/MacOS
	@cp $(BUILD_DIR)/$(PRODUCT_NAME) $@/Contents/MacOS/$(PRODUCT_NAME)
	@cp $(BUILD_DIR)/Info.plist $@/Contents/Info.plist
	@echo "  KEXT $@"

zip: all
	@cp LICENSE README.md $(BUILD_DIR)/
	@cd $(BUILD_DIR) && rm -f *.zip && \
		zip -qry $(PRODUCT_NAME)-$(MODULE_VERSION)-$(shell echo $(CONFIG) | tr a-z A-Z).zip \
		$(PRODUCT_NAME).kext LICENSE README.md
	@ls $(BUILD_DIR)/*.zip

clean:
	rm -rf build-linux

-include $(OBJECTS:.o=.d)
