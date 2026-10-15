# wolfBoot 构建系统深度剖析（lesson_1）

> 本文基于对工作区 `wolfBoot/`（v2.10.0）源码的真实逐文件核对，所有结论都标注了源码文件与行号，可自行验证。
> 工作区根目录：`c:\Users\tangyp\Downloads\wolfboot-explorer-v2.10.0`
> 说明：wolfBoot 的 make 构建脚本大量使用 GNU make 语法与 Unix 工具（`sed`、`cat`、`dd`、`grep`、`sh`），官方 CI 都在 Linux 上运行。本文描述官方构建逻辑本身；在 Windows 上实际执行需 Git Bash / MSYS2 / WSL 之类提供 make + Unix 工具的环境（仓库也提供 CMake/MSBuild 替代构建，不在本文范围）。

---

## 1. 工作区顶层结构

```
wolfboot-explorer-v2.10.0/
├── wolfBoot/                      ← wolfBoot 工程本体（git 仓库）
├── lab/                           ← 教学配套文件（本教程的自定义移植）
│   ├── apply-to-wolfboot.sh
│   ├── app_stm32f4.c
│   └── stm32f407-explorer.config
└── wolfboot-explorer-tutorial.md  ← 教学总纲
```

`wolfBoot/` 下与构建相关的关键文件/目录：

| 文件/目录 | 作用 |
|---|---|
| `wolfBoot/Makefile` | **顶层构建入口 Makefile** |
| `wolfBoot/arch.mk` | 按 `ARCH`/`TARGET` 选择交叉编译器、编译标志、目标对象 |
| `wolfBoot/options.mk` | 按 `SIGN`/`HASH` 等选项推导密钥/签名工具参数与 `CFLAGS` |
| `wolfBoot/tools/config.mk` | 配置项默认值 + `CONFIG_VARS` 清单 |
| `wolfBoot/config/Makefile` | `make config` 交互式配置入口 |
| `wolfBoot/config/examples/` | 官方示例配置（含 `stm32f407-discovery.config`） |
| `wolfBoot/config/target.ld` | **构建时生成**的链接脚本（由 `hal/*.ld` 模板而来） |
| `wolfBoot/include/target.h` | **构建时生成**的目标头文件（由 `target.h.in` 模板而来） |
| `wolfBoot/tools/keytools/` | 密钥生成/签名工具（keygen、sign） |
| `wolfBoot/tools/bin-assemble/` | 按地址拼接多个 bin 的工具 |
| `wolfBoot/test-app/` | 测试应用（构建 `image.elf`/`image.bin`） |
| `wolfBoot/hal/stm32f4.c` / `hal/stm32f4.ld` | STM32F4 HAL 与链接脚本模板 |
| `wolfBoot/include/target.h.in` | `target.h` 的模板 |

---

## 2. 构建阶段执行哪个 Makefile？具体命令是什么？

### 2.1 一句话结论

**入口永远是根目录的 `wolfBoot/Makefile`**（即 `make` 直接读的那个）。它通过 `include` 把 `arch.mk`、`options.mk`、`tools/config.mk`、`tools/test.mk` 等“拉进来”成为同一个 make 进程的一部分，再通过 `$(MAKE) -C <子目录>` 递归调用若干**子 Makefile**去构建宿主工具和测试应用。

### 2.2 顶层 Makefile 的 include 链（源码 `wolfBoot/Makefile`）

```makefile
-include .config            # 第 7 行：读用户配置（即拷贝来的 config 文件）
include tools/config.mk     # 第 8 行：默认值 + CONFIG_VARS
include arch.mk             # 第 229 行：ARCH/TARGET → 编译器与标志
include options.mk          # 第 251 行：SIGN/HASH → 密钥/签名参数与 CFLAGS
include tools/test.mk       # 第 568 行：SIGN_TOOL/KEYGEN_TOOL/BINASSEMBLE + 测试目标
include tools/test-enc.mk   # 第 569 行：加密测试
include tools/test-delta.mk # 第 570 行：增量更新测试
include tools/test-renode.mk# 第 571 行：Renode 模拟器测试
include tools/sbom/build/sbom.mk  # 第 1094 行：SBOM 生成
```

其中真正影响“普通 build”的是前五个（`.config`、`config.mk`、`arch.mk`、`options.mk`、`test.mk`）。

默认目标：

```makefile
MAIN_TARGET=factory.bin     # 第 355 行（绝大多数目标，含 stm32f4）
...
all: $(SECONDARY_PRIVATE_KEY) $(MAIN_TARGET)   # 第 511 行
```

所以直接执行 `make`（等价于 `make all`）时，对 `stm32f407-discovery.config` 这个配置而言，最终构建的就是 `factory.bin`（`SIGN_SECONDARY` 未设置，`SECONDARY_PRIVATE_KEY` 为空，`all` 实际只依赖 `factory.bin`）。

### 2.3 递归调用的子 Makefile

顶层 Makefile 会以 `$(MAKE) -C <dir>` 的方式启动这些子构建：

| 子 Makefile | 触发点（顶层 Makefile 行号） | 产物 |
|---|---|---|
| `tools/keytools/Makefile` | `keytools` 目标（第 631-633 行） | `tools/keytools/sign`、`tools/keytools/keygen` |
| `tools/bin-assemble/Makefile` | `$(BINASSEMBLE)` 规则（`tools/test.mk` 第 32-33 行） | `tools/bin-assemble/bin-assemble` |
| `test-app/Makefile` | `test-app/image.elf` 目标（顶层第 708-709 行） | `test-app/image.elf`、`test-app/image.bin` |
| `config/Makefile` | `config` 目标（顶层第 906-907 行） | 交互式生成 `.config` |
| `tools/check_config/Makefile`、`tools/delta/Makefile` 等 | 对应工具目标 | 各自工具 |

### 2.4 编译与链接的实际命令（关键变量）

编译器/链接器等工具链变量在 `arch.mk` 第 2199-2208 行定义（当 `USE_GCC=1`，这是默认路径）：

```makefile
CC       = $(CROSS_COMPILE)gcc      # arch.mk 2201
LD       = $(CROSS_COMPILE)gcc      # arch.mk 2202
AS       = $(CROSS_COMPILE)gcc      # arch.mk 2203
AR       = $(CROSS_COMPILE)ar       # arch.mk 2204
OBJCOPY ?= $(CROSS_COMPILE)objcopy  # arch.mk 2205
SIZE     = $(CROSS_COMPILE)size     # arch.mk 2206
OUTPUT_FLAG ?= -o                   # arch.mk 2208
```

对 STM32F4，`arch.mk` 第 358-359 行设置了：

```makefile
ifeq ($(ARCH),ARM)
  CROSS_COMPILE?=arm-none-eabi-
```

所以实际工具是 `arm-none-eabi-gcc`、`arm-none-eabi-objcopy` 等。

**编译单个 .c 的规则**（`Makefile` 第 970-976 行）：

```makefile
%.o:%.c
	$(Q)$(CC) $(CFLAGS) -c $(OUTPUT_FLAG) $@ $^
%.o:%.S
	$(Q)$(CC) $(CFLAGS) -c $(OUTPUT_FLAG) $@ $^
```

即形如：

```bash
arm-none-eabi-gcc <CFLAGS...> -c -o src/image.o src/image.c
```

**链接 wolfboot.elf 的规则**（`Makefile` 第 753-758 行）：

```makefile
wolfboot.elf: include/target.h $(LSCRIPT) $(OBJS) $(BINASSEMBLE) $(WOLFBOOT_SIGN_KEY_DEP) FORCE
	$(Q)$(LD) $(LDFLAGS) $(LSCRIPT_FLAGS) $(SECURE_LDFLAGS) \
	    $(LD_START_GROUP) $(OBJS) $(LIBS) $(LD_END_GROUP) -o $@
```

展开后形如：

```bash
arm-none-eabi-gcc <LDFLAGS> -T config/target.ld \
  -Wl,--start-group <OBJS...> -Wl,--end-group -o wolfboot.elf
```

**转 bin**（`Makefile` 第 542-544 行）：

```makefile
wolfboot.bin: wolfboot.elf
	$(Q)$(OBJCOPY) $(OBJCOPY_FLAGS) -O binary $^ $@
```

即：

```bash
arm-none-eabi-objcopy --gap-fill 0xff -O binary wolfboot.elf wolfboot.bin
```

（`OBJCOPY_FLAGS+=--gap-fill $(FILL_BYTE)` 在 `Makefile` 第 291 行，`FILL_BYTE` 默认 `0xff`。）

### 2.5 宿主工具的三个关键变量定义在 `tools/test.mk`

`tools/test.mk` 第 5、11-21 行（在 `Makefile` 第 568 行被 include）：

```makefile
BINASSEMBLE=tools/bin-assemble/bin-assemble

ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/keygen.exe)","")
	KEYGEN_TOOL="$(WOLFBOOT_ROOT)/tools/keytools/keygen.exe"
else
	KEYGEN_TOOL="$(WOLFBOOT_ROOT)/tools/keytools/keygen"
endif

ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/sign.exe)","")
	SIGN_TOOL="$(WOLFBOOT_ROOT)/tools/keytools/sign.exe"
else
	SIGN_TOOL="$(WOLFBOOT_ROOT)/tools/keytools/sign"
endif
```

注意：它会检测 `.exe` 是否存在来决定用 Windows 版还是 POSIX 版可执行文件。


---

## 3. 生成配置的流程

wolfBoot 的“配置”不是 C 头文件，而是一个 `KEY ?= value` 格式的 **`.config` 文件**，再由 make 的 `sed` 把值灌进两个**构建期生成**的文件：

1. `include/target.h`（C 宏头文件，供 bootloader / test-app 编译时使用）
2. `config/target.ld`（链接脚本，供链接时确定 Flash/RAM 布局）

### 3.1 第一步：生成 `.config`

有两种方式：

**方式 A（推荐、最常用）：直接拷贝官方示例**

```bash
cp config/examples/stm32f407-discovery.config .config
```

这正是官方 CI（`wolfBoot/.github/workflows/footprint.yml` 第 30 行）和几乎所有 workflow 的做法：

```bash
cp config/examples/stm32f407-discovery.config .config && make include/target.h
```

**方式 B：交互式 `make config`**

顶层 `Makefile` 第 906-907 行：

```makefile
config: FORCE
	$(MAKE) -C config
```

进入 `config/` 目录执行 `config/Makefile`（全文只有 8 行）：

```makefile
-include ../.config
include ../tools/config.mk
config:
	rm -f ../config/target.ld
	rm -f ../.config
	rm -f ../include/target.h
	@$(foreach q, $(CONFIG_VARS), a=$($q); echo -n $q \? [$$a] :; a=$($q); read SH_$q; test -z $$SH_$q && SH_$q=$$a; echo "$q?=$$SH_$q" >> ../.config;)
```

它的行为：删除旧的 `config/target.ld`、`.config`、`include/target.h`，然后对 `CONFIG_VARS` 列表（定义在 `tools/config.mk` 第 108-150 行，包含 `ARCH TARGET SIGN HASH ...` 等上百个变量）**逐个提示**：

```
ARCH ? [ARM] :        ← 显示默认值 ARM，回车则用 ARM，输入新值则覆盖
TARGET ? [stm32f4] :
SIGN ? [ED25519] :
...
```

每次输入（或回车取默认）后，把 `KEY?=value` 追加写入 `../.config`。最终 `.config` 内容形如：

```
ARCH?=ARM
TARGET?=stm32f4
SIGN?=ED25519
...
```

### 3.2 默认值从哪来：`tools/config.mk`

`.config` 里没写的项，由 `tools/config.mk` 提供默认值。注意 `tools/config.mk` 第 2-101 行被包在 `ifeq ($(ARCH),)` 里——只有 **`ARCH` 为空**时才设置默认 `ARCH?=ARM TARGET?=stm32f4 SIGN?=ED25519 HASH?=SHA256 ...`。

`CONFIG_VARS` 变量（第 108 行起）正是 `make config` 要逐项询问的清单。

### 3.3 第二步：`.config` 值如何进入 make 变量

顶层 `Makefile` 第 7-8 行：

```makefile
-include .config
include tools/config.mk
```

`-include` 表示 `.config` 不存在也不报错。`.config` 里的 `KEY?=value` 是 GNU make 的“条件赋值”——只有变量尚未被设置时才生效，因此**命令行 `make XXX=yyy` 优先级高于 `.config`**。

### 3.4 第三步：生成 `include/target.h`

`Makefile` 第 359 行 `TARGET_H_TEMPLATE:=include/target.h.in`，第 870-888 行：

```makefile
include/target.h: $(TARGET_H_TEMPLATE) FORCE
	$(Q)cat $(TARGET_H_TEMPLATE) | \
	sed -e "s/@WOLFBOOT_PARTITION_SIZE@/$(WOLFBOOT_PARTITION_SIZE)/g" | \
	sed -e "s/@WOLFBOOT_SECTOR_SIZE@/$(WOLFBOOT_SECTOR_SIZE)/g" | \
	sed -e "s/@WOLFBOOT_PARTITION_BOOT_ADDRESS@/$(WOLFBOOT_PARTITION_BOOT_ADDRESS)/g" | \
	... （若干条 sed）...
	> $@
```

即：读取模板 `include/target.h.in`，用 `sed` 把里面的 `@WOLFBOOT_PARTITION_SIZE@`、`@WOLFBOOT_SECTOR_SIZE@`、`@WOLFBOOT_PARTITION_BOOT_ADDRESS@` 等占位符替换成 make 变量的实际值，输出为 `include/target.h`。

> 注意：`target.h.in` 第 69 行 `WOLFBOOT_PARTITION_BOOT_ADDRESS (ARCH_FLASH_OFFSET + @WOLFBOOT_PARTITION_BOOT_ADDRESS@)` 是给 simulator（`ARCH_SIM`）用的运行时偏移路径；STM32F4 走 `#else` 分支直接用数值（内部 Flash 映射在 `0x08000000`，配置里的地址就是绝对地址）。

### 3.5 第四步：生成 `config/target.ld`（链接脚本）

`Makefile` 第 35 行 `LSCRIPT:=config/target.ld`，第 41 行 `LSCRIPT_IN:=hal/$(TARGET).ld`（对 stm32f4 就是 `hal/stm32f4.ld`）。生成规则在第 760-797 行：

```makefile
$(LSCRIPT): $(LSCRIPT_IN) FORCE
	$(Q)cat $(LSCRIPT_IN) | \
	sed -e "s/@ARCH_FLASH_OFFSET@/$(ARCH_FLASH_OFFSET)/g" | \
	sed -e "s/@BOOTLOADER_PARTITION_SIZE@/$(BOOTLOADER_PARTITION_SIZE)/g" | \
	... （若干条 sed，含 @WOLFBOOT_PARTITION_BOOT_ADDRESS@ 等）...
	> $@
```

即：读取 `hal/stm32f4.ld` 模板，用 `sed` 替换 `@BOOTLOADER_PARTITION_SIZE@`、`@WOLFBOOT_PARTITION_BOOT_ADDRESS@` 等，输出为 `config/target.ld`。

`hal/stm32f4.ld` 模板的核心内容（第 1-5、56-59 行）：

```ld
MEMORY
{
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = @BOOTLOADER_PARTITION_SIZE@
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x0001C000
}
...
_wolfboot_partition_boot_address   = @WOLFBOOT_PARTITION_BOOT_ADDRESS@;
_wolfboot_partition_size           = @WOLFBOOT_PARTITION_SIZE@;
_wolfboot_partition_update_address = @WOLFBOOT_PARTITION_UPDATE_ADDRESS@;
_wolfboot_partition_swap_address   = @WOLFBOOT_PARTITION_SWAP_ADDRESS@;
```

### 3.6 配置如何影响“编译选项”：`options.mk`

`options.mk`（在 `Makefile` 第 251 行被 include）根据 `SIGN`、`HASH` 等推导：

- `KEYGEN_OPTIONS` / `SIGN_OPTIONS`：传给 `keygen` / `sign` 工具的命令行参数
- `SIGN_ALG`：用于 keystore 一致性校验
- `CFLAGS` 里的算法宏（如 `-DWOLFBOOT_SIGN_ED25519`、`-DWOLFBOOT_HASH_SHA256`）
- `WOLFCRYPT_OBJS`：需要链接的 wolfCrypt 源文件

例如 `SIGN=ED25519`（`options.mk` 第 365-378 行）：

```makefile
ifeq ($(SIGN),ED25519)
  KEYGEN_OPTIONS+=--ed25519
  SIGN_OPTIONS+=--ed25519
  WOLFCRYPT_OBJS+=$(ED25519_OBJS)
  CFLAGS+=-D"WOLFBOOT_SIGN_ED25519"
  STACK_USAGE?=5000
  ... IMAGE_HEADER_SIZE 至少 256 ...
endif
```

`HASH=SHA256`（`options.mk` 第 1488-1490 行）只加了编译宏（sign 工具默认就是 SHA256）：

```makefile
ifeq ($(HASH),SHA256)
  CFLAGS+=-D"WOLFBOOT_HASH_SHA256"
endif
```

`SIGN_ALG` 兜底（第 1636-1638 行）：

```makefile
ifeq ($(SIGN_ALG),)
  SIGN_ALG=$(SIGN)
endif
```

---

## 4. tools 文件夹的作用（逐个介绍子文件夹）

`wolfBoot/tools/` 是**宿主侧（Host）工具集**：在 PC 上编译运行，用来生成密钥、签名镜像、拼接镜像、解析 ELF/FDT、跑单元测试、烧录、跑模拟器、生成 SBOM 等。下面按实际目录逐个说明（目录列表来自 `wolfBoot/tools/` 的真实内容）。

### 4.1 密钥与镜像工具（构建流程的核心）

**`keytools/`** —— 最重要的一组，由顶层 `make keytools` 调用 `tools/keytools/Makefile` 构建出两个宿主可执行文件：

- `keygen.c` → `keygen`：生成非对称密钥对，输出私钥 `wolfboot_signing_private_key.der`，并导出公钥生成两个产物：`src/keystore.c`（编译进 bootloader 的 C 数组）和 `keystore.der`（DER 格式 keystore）。支持 `--ecc256/--ed25519/--rsa2048/--lms/--xmss/--ml_dsa` 等。
- `sign.c` → `sign`：给固件镜像加 wolfBoot 镜像头并签名，输出 `image_vN_signed.bin`。
- `user_settings.h`：工具侧 wolfSSL 配置。
- `Makefile`：`all: sign keygen`，用宿主机 `gcc` 链接 `lib/wolfssl` 的 wolfCrypt 源。
- `wolfBootKeyTools.sln` / `wolfBootKeygenTool.vcxproj` / `wolfBootSignTool.vcxproj`：Windows/MSVC 的替代构建工程。
- `otp/` 子目录：`otp-keystore-gen.c`、`otp-keystore-primer.c`、`startup.c`、`target.ld`，用于把公钥烧进 OTP/一次性可编程区的 keystore（`FLASH_OTP_KEYSTORE=1` 时使用）。

**`bin-assemble/`** —— `bin-assemble.c` + `Makefile`。宿主工具，按地址把多个 bin 片段拼接成一个镜像（`factory.bin`、`internal_flash.dd` 都靠它），间隙用 `0xFF` 填充（`bin-assemble.c` 第 42-43 行 `FILL_BYTE 0xff`）。

### 4.2 解析/格式工具

**`elf-parser/`** —— `elf-parser.c` + `Makefile` + `README.md`：解析 ELF 文件（配合 `ELF=1` 直接加载 ELF 的功能）。

**`fdt-parser/`** —— `fdt-parser.c` + `Makefile` + `nxp_t1024.dtb` 样例：解析 Flattened Device Tree（设备树），配合带 DTB 的目标。

**`squashelf/`** —— `squashelf.c` + `Makefile` + 几个 python 溢出测试：把 scatter 加载的 ELF 压平成单个载荷（`ELF_FLASH_SCATTER=1` 时用），`test-app/Makefile` 第 1670-1680 行调用它做 `--nosht` 处理。

### 4.3 增量更新与 delta

**`delta/`** —— `bmdiff.c` + `test-bmdiff.c` + `Makefile`：增量更新（delta update）的 diff 工具，生成新旧固件的差异包（`DELTA_UPDATES=1` 时用）。

### 4.4 签名算法扩展

**`lms/`** —— LMS 后量子签名补丁与 `lms_common.h`、`lms_siglen.sh`（计算 LMS 签名长度）。

**`xmss/`** —— XMSS 后量子签名补丁与 `xmss_common.h`、`xmss_siglen.sh`。

### 4.5 配置检查 / 版本 / 更新服务 / UART 烧录

**`check_config/`** —— `check_config.c` + `Makefile`：`make check_config`（顶层第 909-910 行）调用，校验 `.config` 的一致性。

**`test-expect-version/`** —— 版本断言小工具。

**`test-update-server/`** —— `server.c` + `Makefile` + `README.md`：测试用更新服务器。

**`uart-flash-server/`** —— `ufserver.c` + `test_overflow.c` + `Makefile`：通过 UART 把镜像发给目标板烧录的宿主服务端（`UART_FLASH=1` 时配合目标侧使用）。

### 4.6 TPM / 安全 / 工具链适配

**`tpm/`** —— `rot.c`（Root of Trust）、`policy_create.c`、`policy_sign.c` + `Makefile`：生成/签名 TPM 策略，配合 `WOLFTPM=1` 的度量启动/密封。

**`ci/`** —— CI 辅助：`fetch-mcuxsdk-headers.sh`（拉取 MCUX SDK 头文件）、`gen_mpfs_libero_stub.sh`（生成 Microchip MPFS Libero 配置桩）、`pic32cz-wolfhsm-stub/`（PIC32CZ wolfHSM 桩）。

**`armclang/`** —— ARM Compiler 6 适配：`armclang_stubs.c`（桩符号）、`objcopy.sh`。

### 4.7 平台/模拟器/EFI

**`renode/`** —— Renode 模拟器脚本：`stm32f4_discovery_wolfboot.resc`、`nrf52840_wolfboot.resc`、`sifive_fe310_wolfboot.resc`、`stm32f746_wolfboot.resc`、`Dockerfile`、`docker-test.sh`。

**`efi/`** —— UEFI/EFI 构建脚本：`compile_efi_linux.sh`、`run_efi.sh`、`prepare_efi_partition.sh`、`linux-efi.config`、Buildroot 相关 `Config.in`/`external.mk` 等。

**`x86_fsp/`** —— `decode_fsp_upd.py`：解码 Intel FSP UPD 结构（x86 FSP 启动路径用）。

**`openocd/`** —— `max32665.cfg`：OpenOCD 调试配置。

### 4.8 SBOM 与脚本

**`sbom/`** —— 软件物料清单（SBOM）生成：`gen-sbom`、`sbom-driver`、`sbom-driver.py`、`sbom.mk`、`sbom.cmake`、`validate_sbom.py`、`frontends/`（`iar_sbom.py`、`zephyr_sbom.py`、`compdb_sbom.py`）。顶层 `make sbom` / `make sbom-hal` 走这里。

**`scripts/`** —— 数量最多的杂项脚本，按平台分组在子目录（`cm4/`、`imx8qm/`、`lpc55s69/`、`nrf5340/`、`tc3xx/`、`tc4xx/`、`va416x0/`、`x86_fsp/`、`zcu102/`、`zynq7000/`、`ide-sbom/`），加上一堆顶层脚本：
- 升级准备：`prepare_update.sh`、`prepare_update_l5.sh`、`prepare_update_u5.sh` 等（从 `.config` 取参数、调用 `sign` 生成 `update.bin`）
- 模拟器测试：`sim-sunnyday-update.sh`、`sim-update-fallback.sh`、`sim-update-powerfail-resume.sh` 等
- 烧录：`stm32h7s_flash.sh`、`stm32n6_flash.sh`、`nxp-lpc54s0xx-flash.sh` 等
- 状态/信息：`boot_status.py`、`boot-time.py`、`print_flash_map.py`
- 打包：`tegra234-mkbl33.sh`、`versal_boot.bif`、`zynqmp_sd_boot.bif` 等
- 配置转换：`config2presets.py`（`.config` → CMake presets）

### 4.9 文件系统与单元测试

**`fs-test/`** —— `fs-test.c` + `mkfixtures.sh` + `runbench.sh` + `runtests.sh`：文件系统测试。

**`unit-tests/`** —— 最大的目录，约两百个 `unit-*.c`（flash 擦写、分区、delta、FDT、SDHCI、TPM、加密、gzip 等），配 `Makefile`、`README.md`、`target.h`、`keystore_gen.c`，以及 `cc26x2_ti_stub/`、`cmsis_stub/` 两个桩目录。顶层 `make utilsclean`/`make -C tools/unit-tests` 触发。

### 4.10 tools 根目录的 .mk 文件

| 文件 | 作用 |
|---|---|
| `tools/config.mk` | 配置默认值 + `CONFIG_VARS` |
| `tools/test.mk` | 定义 `SIGN_TOOL`/`KEYGEN_TOOL`/`BINASSEMBLE`（第 5、11-21 行）+ 各种 test-* 目标 |
| `tools/test-enc.mk` | 加密更新测试目标 |
| `tools/test-delta.mk` | 增量更新测试目标 |
| `tools/test-renode.mk` | Renode 模拟器测试目标 |
| `tools/wolfboot-rpi-devicetree.diff` | Raspberry Pi 设备树补丁 |


---

## 5. 完整 Demo：`stm32f407-discovery.config` 的构建步骤

### 5.1 配置文件逐项解读

`config/examples/stm32f407-discovery.config`（全文 16 行）：

```makefile
ARCH?=ARM                                   # 架构：ARM（Cortex-M）
TARGET?=stm32f4                             # 目标：STM32F4 系列
SIGN?=ED25519                               # 签名算法：Ed25519
HASH?=SHA256                                # 摘要算法：SHA-256
VTOR?=1                                     # 使用 VTOR 重定位向量表
SPMATH?=1                                   # 使用 wolfSSL 单精度数学库（sp_int）
WOLFBOOT_PARTITION_SIZE?=0x20000            # 每个应用分区 128 KB
WOLFBOOT_SECTOR_SIZE?=0x20000               # 交换扇区 128 KB
WOLFBOOT_PARTITION_BOOT_ADDRESS?=0x08020000 # BOOT 分区起始
WOLFBOOT_PARTITION_UPDATE_ADDRESS?=0x08040000 # UPDATE 分区起始
WOLFBOOT_PARTITION_SWAP_ADDRESS?=0x08060000   # SWAP 分区起始
CLOCK_SPEED?=160000000                      # 主频 160 MHz
STM32_PLLM?=8
STM32_PLLN?=336
STM32_PLLP?=2
STM32_PLLQ?=7
```

### 5.2 关键派生值（源码可验证）

| 变量 | 值 | 依据 |
|---|---|---|
| `CROSS_COMPILE` | `arm-none-eabi-` | `arch.mk` 358-359 |
| `ARCH_FLASH_OFFSET` | `0x08000000` | `arch.mk` 405-408 |
| `WOLFBOOT_ORIGIN` | `0x08000000`（= ARCH_FLASH_OFFSET） | `arch.mk` 3092-3093 |
| `BOOTLOADER_PARTITION_SIZE` | `0x20000`（128 KB）= `0x08020000 - 0x08000000` | `Makefile` 59 |
| `IMAGE_HEADER_SIZE` | `256`（ED25519 要求 ≥256） | `config.mk` 54、`options.mk` 375-377 |
| `SIGN_ALG` | `ED25519` | `options.mk` 1636-1638 |
| `KEYGEN_OPTIONS` | `--ed25519` | `options.mk` 366 |
| `SIGN_OPTIONS` | `--ed25519`（SHA256 是 sign 默认） | `options.mk` 367、1488-1490 |
| `MAIN_TARGET` | `factory.bin` | `Makefile` 355 |
| `BOOT_IMG` | `test-app/image.bin` | `arch.mk` 3017 |

### 5.3 分区布局图

```
STM32F407 内部 Flash（映射基址 0x08000000）
┌─────────────────────────────────────────────┐
│ wolfboot.bin  (bootloader)                  │ 0x08000000
│   BOOTLOADER_PARTITION_SIZE = 128 KB        │   └─ 到 0x0801FFFF
├─────────────────────────────────────────────┤
│ BOOT  分区  image_v1_signed.bin (128 KB)    │ 0x08020000
├─────────────────────────────────────────────┤
│ UPDATE 分区 (128 KB)                        │ 0x08040000
├─────────────────────────────────────────────┤
│ SWAP   分区 (128 KB)                        │ 0x08060000
└─────────────────────────────────────────────┘
```

> 注意：`factory.bin` 只包含前两段（bootloader + BOOT 分区的签名应用），UPDATE/SWAP 出厂为空，运行时由 wolfBoot 使用。


### 5.4 完整构建命令

```bash
cd wolfBoot

# 1) 选取配置
cp config/examples/stm32f407-discovery.config .config

# 2) 构建（默认目标 all → factory.bin）
make
# 等价于：make all
```

首次 `make` 会按依赖顺序自动执行下面这些（无需手动一步步敲）：

```bash
# a. 构建宿主工具 keygen / sign（keytools_check -> keytools）
make -C tools/keytools -j          # 产物：tools/keytools/keygen, tools/keytools/sign

# b. 构建 bin-assemble
make -C tools/bin-assemble         # 产物：tools/bin-assemble/bin-assemble

# c. 生成签名私钥 + 由公钥生成 src/keystore.c、keystore.der
#    （wolfboot_signing_private_key.der 目标，Makefile 599-608）
./tools/keytools/keygen --ed25519 -g wolfboot_signing_private_key.der

# d. 生成 include/target.h（Makefile 870-888）
cat include/target.h.in | sed -e 's/@WOLFBOOT_PARTITION_SIZE@/0x20000/g' ... > include/target.h

# e. 生成 config/target.ld（Makefile 760-797）
cat hal/stm32f4.ld | sed -e 's/@BOOTLOADER_PARTITION_SIZE@/0x20000/g' ... > config/target.ld

# f. 编译 bootloader 各对象并链接 wolfboot.elf（Makefile 970-976, 753-758）
arm-none-eabi-gcc <CFLAGS> -c -o src/image.o src/image.c
# ...（src/string.o src/image.o src/libwolfboot.o hal/hal.o hal/stm32f4.o
#      src/keystore.o src/loader.o + wolfCrypt 对象 ...）
arm-none-eabi-gcc <LDFLAGS> -T config/target.ld \
  -Wl,--start-group <OBJS> -Wl,--end-group -o wolfboot.elf

# g. 转 bin（Makefile 542-544）
arm-none-eabi-objcopy --gap-fill 0xff -O binary wolfboot.elf wolfboot.bin

# h. 构建测试应用（test-app/image.elf -> image.bin）
make -C test-app image.elf   # 链接 app_stm32f4.o led.o system.o timer.o libwolfboot.o ...
arm-none-eabi-objcopy -O binary test-app/image.elf test-app/image.bin

# i. 签名生成 test-app/image_v1_signed.bin（Makefile 689-706）
IMAGE_HEADER_SIZE=256 WOLFBOOT_PARTITION_SIZE=0x20000 WOLFBOOT_SECTOR_SIZE=0x20000 \
  ./tools/keytools/sign --ed25519 test-app/image.bin wolfboot_signing_private_key.der 1

# j. 拼接出厂镜像 factory.bin（Makefile 728-736）
tools/bin-assemble/bin-assemble factory.bin \
  0x08000000 wolfboot.bin \
  0x08020000 test-app/image_v1_signed.bin
```

### 5.5 每个关键产物的来源与依赖（源码定位）

| 产物 | 规则位置 | 说明 |
|---|---|---|
| `.config` | 手工 `cp` 或 `make config` | 配置源 |
| `include/target.h` | `Makefile` 870-888 | `target.h.in` + sed |
| `config/target.ld` | `Makefile` 760-797 | `hal/stm32f4.ld` + sed |
| `src/keystore.c` | `Makefile` 811-819 | `keygen` 从 `wolfboot_signing_private_key.der` 导出公钥 |
| `wolfboot_signing_private_key.der` | `Makefile` 599-608 | `keygen --ed25519 -g ...` |
| `wolfboot.elf` | `Makefile` 753-758 | `$(LD) ... -o wolfboot.elf` |
| `wolfboot.bin` | `Makefile` 542-544 | `objcopy -O binary` |
| `test-app/image.elf` | `test-app/Makefile` 1683-1685 | 链接 `$(APP_OBJS)` |
| `test-app/image.bin` | `test-app/Makefile` 1652-1654 | `objcopy -O binary` |
| `test-app/image_v1_signed.bin` | `Makefile` 689-706 | `sign --ed25519 ... 1` |
| `factory.bin` | `Makefile` 728-736 | `bin-assemble` 拼接 |

### 5.6 最终 `factory.bin` 的布局

`bin-assemble` 用 `0xFF` 填充空隙（`bin-assemble.c` FILL_BYTE=0xff）：

```
factory.bin（可整片烧到 0x08000000）
┌──────────────────────────────────────────┐
│ 0x08000000  wolfboot.bin（bootloader）    │
│            （不足 128KB 的部分 0xFF 填充） │
├──────────────────────────────────────────┤
│ 0x08020000  test-app/image_v1_signed.bin  │  ← BOOT 分区
└──────────────────────────────────────────┘
```

烧录（官方做法）：

```bash
# 用 OpenOCD + ST-LINK 烧到内部 Flash 基址 0x08000000
openocd -f config/openocd/openocd_stm32f4.cfg -c "program factory.bin 0x08000000 verify reset exit"
```

`config/openocd/openocd_stm32f4.cfg` 就是仓库自带的 STM32F4 OpenOCD 配置（`wolfBoot/config/openocd/` 下）。

### 5.7 其它常用 make 目标（对理解构建有帮助）

| 目标 | 顶层 Makefile 行号 | 作用 |
|---|---|---|
| `make keytools` | 631-633 | 只构建 keygen/sign |
| `make keys` | 827 | 只生成签名私钥 |
| `make include/target.h` | 870 | 只生成目标头文件 |
| `make wolfboot.bin` | 542 | 只构建 bootloader 二进制 |
| `make factory.bin` | 728 | 只拼出厂镜像 |
| `make config` | 906 | 交互式配置 |
| `make clean` / `keysclean` / `distclean` | 829-868 | 清理 |
| `make check_config` | 909 | 配置一致性检查 |


---

## 6. 一句话总结构建全链路

```
.config (用户配置)
   │  include tools/config.mk (默认值) + include arch.mk (ARCH/TARGET)
   ▼
options.mk 推导: SIGN/HASH → KEYGEN_OPTIONS/SIGN_OPTIONS/CFLAGS/WOLFCRYPT_OBJS
   │
   ├──► keytools(keygen/sign)   ──► 私钥 + src/keystore.c(公钥)
   ├──► include/target.h        ──► (target.h.in + sed)
   ├──► config/target.ld        ──► (hal/stm32f4.ld + sed)
   ├──► 编译 OBJS + 链接        ──► wolfboot.elf → wolfboot.bin
   ├──► test-app image.elf      ──► image.bin → sign → image_v1_signed.bin
   └──► bin-assemble            ──► factory.bin (bootloader + BOOT 分区)
```

---

## 附：本文涉及的关键文件与行号索引

| 内容 | 文件 | 行号 |
|---|---|---|
| 顶层 include 链 | `wolfBoot/Makefile` | 7-8, 229, 251, 568-571, 1094 |
| 默认目标 all / MAIN_TARGET | `wolfBoot/Makefile` | 355, 511 |
| 工具链 CC/LD/OBJCOPY | `wolfBoot/arch.mk` | 2199-2208 |
| STM32F4 ARCH_FLASH_OFFSET | `wolfBoot/arch.mk` | 405-408 |
| WOLFBOOT_ORIGIN 默认 | `wolfBoot/arch.mk` | 3092-3095 |
| 编译规则 %.o:%.c | `wolfBoot/Makefile` | 970-976 |
| 链接 wolfboot.elf | `wolfBoot/Makefile` | 753-758 |
| target.h 生成 | `wolfBoot/Makefile` | 870-888 |
| target.ld 生成 | `wolfBoot/Makefile` | 760-797 |
| 私钥生成 | `wolfBoot/Makefile` | 599-608 |
| keystore 生成 | `wolfBoot/Makefile` | 811-819 |
| factory.bin 规则 | `wolfBoot/Makefile` | 728-736 |
| 签名 test-app | `wolfBoot/Makefile` | 689-706 |
| keytools 目标 | `wolfBoot/Makefile` | 631-633 |
| config 目标 | `wolfBoot/Makefile` | 906-907 |
| 交互式配置 | `wolfBoot/config/Makefile` | 1-8 |
| 默认值与 CONFIG_VARS | `wolfBoot/tools/config.mk` | 2-101, 108-150 |
| SIGN=ED25519 分支 | `wolfBoot/options.mk` | 365-378 |
| HASH=SHA256 分支 | `wolfBoot/options.mk` | 1488-1490 |
| SIGN_ALG 兜底 | `wolfBoot/options.mk` | 1636-1638 |
| SIGN_TOOL/KEYGEN_TOOL/BINASSEMBLE | `wolfBoot/tools/test.mk` | 5, 11-21 |
| stm32f4 链接脚本模板 | `wolfBoot/hal/stm32f4.ld` | 1-61 |
| target.h 模板 | `wolfBoot/include/target.h.in` | 27-274 |
| 示例配置 | `wolfBoot/config/examples/stm32f407-discovery.config` | 1-16 |
| bin-assemble 填充字节 | `wolfBoot/tools/bin-assemble/bin-assemble.c` | 38-44 |

