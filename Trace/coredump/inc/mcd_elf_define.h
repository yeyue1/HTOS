/*
 * Copyright (c) 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-08-16     Rbb666       first version
 */

#ifndef __MCD_ELF_DEFINE_H__
#define __MCD_ELF_DEFINE_H__

/* Fields in e_ident[].  */
#define EI_MAG0 0    /* File identification byte 0 index */
#define ELFMAG0 0x7F /* Magic number byte 0 */

#define EI_MAG1 1   /* File identification byte 1 index */
#define ELFMAG1 'E' /* Magic number byte 1 */

#define EI_MAG2 2   /* File identification byte 2 index */
#define ELFMAG2 'L' /* Magic number byte 2 */

#define EI_MAG3 3   /* File identification byte 3 index */
#define ELFMAG3 'F' /* Magic number byte 3 */

#define EI_CLASS 4     /* File class */
#define ELFCLASSNONE 0 /* Invalid class */
#define ELFCLASS32 1   /* 32-bit objects */
#define ELFCLASS64 2   /* 64-bit objects */

#define EI_DATA 5     /* Data encoding */
#define ELFDATANONE 0 /* Invalid data encoding */
#define ELFDATA2LSB 1 /* 2's complement, little endian */
#define ELFDATA2MSB 2 /* 2's complement, big endian */

#define EI_VERSION 6 /* File version */

#define EI_OSABI 7           /* Operating System/ABI indication */
#define ELFOSABI_NONE 0      /* UNIX System V ABI */
#define ELFOSABI_HPUX 1      /* HP-UX operating system */
#define ELFOSABI_NETBSD 2    /* NetBSD */
#define ELFOSABI_GNU 3       /* GNU */
#define ELFOSABI_LINUX 3     /* Alias for ELFOSABI_GNU */
#define ELFOSABI_SOLARIS 6   /* Solaris */
#define ELFOSABI_AIX 7       /* AIX */
#define ELFOSABI_IRIX 8      /* IRIX */
#define ELFOSABI_FREEBSD 9   /* FreeBSD */
#define ELFOSABI_TRU64 10    /* TRU64 UNIX */
#define ELFOSABI_MODESTO 11  /* Novell Modesto */
#define ELFOSABI_OPENBSD 12  /* OpenBSD */
#define ELFOSABI_OPENVMS 13  /* OpenVMS */
#define ELFOSABI_NSK 14      /* Hewlett-Packard Non-Stop Kernel */
#define ELFOSABI_AROS 15     /* AROS */
#define ELFOSABI_FENIXOS 16  /* FenixOS */
#define ELFOSABI_CLOUDABI 17 /* Nuxi CloudABI */
#define ELFOSABI_OPENVOS 18  /* Stratus Technologies OpenVOS */

#define ELFOSABI_C6000_ELFABI 64 /* Bare-metal TMS320C6000 */
#define ELFOSABI_C6000_LINUX 65  /* Linux TMS320C6000 */
#define ELFOSABI_ARM_FDPIC 65    /* ARM FDPIC */
#define ELFOSABI_ARM 97          /* ARM */
#define ELFOSABI_STANDALONE 255  /* Standalone (embedded) application */

#define EI_ABIVERSION 8 /* ABI version */

#define EI_PAD 9 /* Start of padding bytes */

#define EI_NIDENT 16

/* Values for e_type, which identifies the object file type.  */

#define ET_NONE 0 /* No file type */
#define ET_REL 1  /* Relocatable file */
#define ET_EXEC 2 /* Position-dependent executable file */
#define ET_DYN                                                                 \
  3                      /* Position-independent executable or                 \
                    shared object file */
#define ET_CORE 4        /* Core file */
#define ET_LOOS 0xFE00   /* Operating system-specific */
#define ET_HIOS 0xFEFF   /* Operating system-specific */
#define ET_LOPROC 0xFF00 /* Processor-specific */
#define ET_HIPROC 0xFFFF /* Processor-specific */

/* Values for e_machine, which identifies the architecture.  These numbers
   are officially assigned by registry@sco.com.  See below for a list of
   ad-hoc numbers used during initial development.  */

#define EM_NONE 0  /* No machine */
#define EM_M32 1   /* AT&T WE 32100 */
#define EM_SPARC 2 /* SUN SPARC */
#define EM_386 3   /* Intel 80386 */
#define EM_68K 4   /* Motorola m68k family */
#define EM_88K 5   /* Motorola m88k family */
#define EM_IAMCU 6 /* Intel MCU */
#define EM_860 7   /* Intel 80860 */
#define EM_MIPS 8  /* MIPS R3000 (officially, big-endian only) */
#define EM_S370 9  /* IBM System/370 */
#define EM_MIPS_RS3_LE                                                         \
  10 /* MIPS R3000 little-endian (Oct 4 1999 Draft).  Deprecated.  */
#define EM_OLD_SPARCV9                                                         \
  11 /* Old version of Sparc v9, from before the ABI.  Deprecated.  */
#define EM_res011 11      /* Reserved */
#define EM_res012 12      /* Reserved */
#define EM_res013 13      /* Reserved */
#define EM_res014 14      /* Reserved */
#define EM_PARISC 15      /* HPPA */
#define EM_res016 16      /* Reserved */
#define EM_PPC_OLD 17     /* Old version of PowerPC.  Deprecated.  */
#define EM_VPP550 17      /* Fujitsu VPP500 */
#define EM_SPARC32PLUS 18 /* Sun's "v8plus" */
#define EM_960 19         /* Intel 80960 */
#define EM_PPC 20         /* PowerPC */
#define EM_PPC64 21       /* 64-bit PowerPC */
#define EM_S390 22        /* IBM S/390 */
#define EM_SPU 23         /* Sony/Toshiba/IBM SPU */
#define EM_res024 24      /* Reserved */
#define EM_res025 25      /* Reserved */
#define EM_res026 26      /* Reserved */
#define EM_res027 27      /* Reserved */
#define EM_res028 28      /* Reserved */
#define EM_res029 29      /* Reserved */
#define EM_res030 30      /* Reserved */
#define EM_res031 31      /* Reserved */
#define EM_res032 32      /* Reserved */
#define EM_res033 33      /* Reserved */
#define EM_res034 34      /* Reserved */
#define EM_res035 35      /* Reserved */
#define EM_V800 36        /* NEC V800 series */
#define EM_FR20 37        /* Fujitsu FR20 */
#define EM_RH32 38        /* TRW RH32 */
#define EM_MCORE                                                               \
  39 /* Motorola M*Core */ /* May also be taken by Fujitsu MMA                 \
                            */
#define EM_RCE 39          /* Old name for MCore */
#define EM_ARM 40          /* ARM */
#define EM_OLD_ALPHA 41    /* Digital Alpha */
#define EM_SH 42           /* Renesas (formerly Hitachi) / SuperH SH */
#define EM_SPARCV9 43      /* SPARC v9 64-bit */
#define EM_TRICORE 44      /* Siemens Tricore embedded processor */
#define EM_ARC 45          /* ARC Cores */
#define EM_H8_300 46       /* Renesas (formerly Hitachi) H8/300 */
#define EM_H8_300H 47      /* Renesas (formerly Hitachi) H8/300H */
#define EM_H8S 48          /* Renesas (formerly Hitachi) H8S */
#define EM_H8_500 49       /* Renesas (formerly Hitachi) H8/500 */
#define EM_IA_64 50        /* Intel IA-64 Processor */
#define EM_MIPS_X 51       /* Stanford MIPS-X */
#define EM_COLDFIRE 52     /* Motorola Coldfire */
#define EM_68HC12 53       /* Motorola M68HC12 */
#define EM_MMA 54          /* Fujitsu Multimedia Accelerator */
#define EM_PCP 55          /* Siemens PCP */
#define EM_NCPU 56         /* Sony nCPU embedded RISC processor */
#define EM_NDR1 57         /* Denso NDR1 microprocessor */
#define EM_STARCORE 58     /* Motorola Star*Core processor */
#define EM_ME16 59         /* Toyota ME16 processor */
#define EM_ST100 60        /* STMicroelectronics ST100 processor */
#define EM_TINYJ 61        /* Advanced Logic Corp. TinyJ embedded processor */
#define EM_X86_64 62       /* Advanced Micro Devices X86-64 processor */
#define EM_PDSP 63         /* Sony DSP Processor */
#define EM_PDP10 64        /* Digital Equipment Corp. PDP-10 */
#define EM_PDP11 65        /* Digital Equipment Corp. PDP-11 */
#define EM_FX66 66         /* Siemens FX66 microcontroller */
#define EM_ST9PLUS 67     /* STMicroelectronics ST9+ 8/16 bit microcontroller */
#define EM_ST7 68         /* STMicroelectronics ST7 8-bit microcontroller */
#define EM_68HC16 69      /* Motorola MC68HC16 Microcontroller */
#define EM_68HC11 70      /* Motorola MC68HC11 Microcontroller */
#define EM_68HC08 71      /* Motorola MC68HC08 Microcontroller */
#define EM_68HC05 72      /* Motorola MC68HC05 Microcontroller */
#define EM_SVX 73         /* Silicon Graphics SVx */
#define EM_ST19 74        /* STMicroelectronics ST19 8-bit cpu */
#define EM_VAX 75         /* Digital VAX */
#define EM_CRIS 76        /* Axis Communications 32-bit embedded processor */
#define EM_JAVELIN 77     /* Infineon Technologies 32-bit embedded cpu */
#define EM_FIREPATH 78    /* Element 14 64-bit DSP processor */
#define EM_ZSP 79         /* LSI Logic's 16-bit DSP processor */
#define EM_MMIX 80        /* Donald Knuth's educational 64-bit processor */
#define EM_HUANY 81       /* Harvard's machine-independent format */
#define EM_PRISM 82       /* SiTera Prism */
#define EM_AVR 83         /* Atmel AVR 8-bit microcontroller */
#define EM_FR30 84        /* Fujitsu FR30 */
#define EM_D10V 85        /* Mitsubishi D10V */
#define EM_D30V 86        /* Mitsubishi D30V */
#define EM_V850 87        /* Renesas V850 (formerly NEC V850) */
#define EM_M32R 88        /* Renesas M32R (formerly Mitsubishi M32R) */
#define EM_MN10300 89     /* Matsushita MN10300 */
#define EM_MN10200 90     /* Matsushita MN10200 */
#define EM_PJ 91          /* picoJava */
#define EM_OR1K 92        /* OpenRISC 1000 32-bit embedded processor */
#define EM_ARC_COMPACT 93 /* ARC International ARCompact processor */
#define EM_XTENSA 94      /* Tensilica Xtensa Architecture */
#define EM_SCORE_OLD                                                           \
  95 /* Old Sunplus S+core7 backend magic number. Written in the absence of    \
        an ABI.  */
#define EM_VIDEOCORE 95 /* Alphamosaic VideoCore processor */
#define EM_TMM_GPP 96   /* Thompson Multimedia General Purpose Processor */
#define EM_NS32K 97     /* National Semiconductor 32000 series */
#define EM_TPC 98       /* Tenor Network TPC processor */
#define EM_PJ_OLD 99    /* Old value for picoJava.  Deprecated.  */
#define EM_SNP1K 99     /* Trebia SNP 1000 processor */
#define EM_ST200 100    /* STMicroelectronics ST200 microcontroller */
#define EM_IP2K 101     /* Ubicom IP2022 micro controller */
#define EM_MAX 102      /* MAX Processor */
#define EM_CR 103       /* National Semiconductor CompactRISC */
#define EM_F2MC16 104   /* Fujitsu F2MC16 */
#define EM_MSP430 105   /* TI msp430 micro controller */
#define EM_BLACKFIN 106 /* ADI Blackfin */
#define EM_SE_C33 107   /* S1C33 Family of Seiko Epson processors */
#define EM_SEP 108      /* Sharp embedded microprocessor */
#define EM_ARCA 109     /* Arca RISC Microprocessor */
#define EM_UNICORE                                                             \
  110 /* Microprocessor series from PKU-Unity Ltd. and MPRC of Peking          \
         University */
#define EM_EXCESS 111 /* eXcess: 16/32/64-bit configurable embedded CPU */
#define EM_DXP 112    /* Icera Semiconductor Inc. Deep Execution Processor */
#define EM_ALTERA_NIOS2 113 /* Altera Nios II soft-core processor */
#define EM_CRX 114          /* National Semiconductor CRX */
#define EM_CR16_OLD                                                            \
  115 /* Old, value for National Semiconductor CompactRISC.  Deprecated.  */
#define EM_XGATE 115 /* Motorola XGATE embedded processor */
#define EM_C166 116  /* Infineon C16x/XC16x processor */
#define EM_M16C 117  /* Renesas M16C series microprocessors */
#define EM_DSPIC30F                                                            \
  118             /* Microchip Technology dsPIC30F Digital Signal Controller */
#define EM_CE 119 /* Freescale Communication Engine RISC core */
#define EM_M32C 120       /* Renesas M32C series microprocessors */
#define EM_res121 121     /* Reserved */
#define EM_res122 122     /* Reserved */
#define EM_res123 123     /* Reserved */
#define EM_res124 124     /* Reserved */
#define EM_res125 125     /* Reserved */
#define EM_res126 126     /* Reserved */
#define EM_res127 127     /* Reserved */
#define EM_res128 128     /* Reserved */
#define EM_res129 129     /* Reserved */
#define EM_res130 130     /* Reserved */
#define EM_TSK3000 131    /* Altium TSK3000 core */
#define EM_RS08 132       /* Freescale RS08 embedded processor */
#define EM_res133 133     /* Reserved */
#define EM_ECOG2 134      /* Cyan Technology eCOG2 microprocessor */
#define EM_SCORE 135      /* Sunplus Score */
#define EM_SCORE7 135     /* Sunplus S+core7 RISC processor */
#define EM_DSP24 136      /* New Japan Radio (NJR) 24-bit DSP Processor */
#define EM_VIDEOCORE3 137 /* Broadcom VideoCore III processor */
#define EM_LATTICEMICO32                                                       \
  138                   /* RISC processor for Lattice FPGA architecture        \
                         */
#define EM_SE_C17 139   /* Seiko Epson C17 family */
#define EM_TI_C6000 140 /* Texas Instruments TMS320C6000 DSP family */
#define EM_TI_C2000 141 /* Texas Instruments TMS320C2000 DSP family */
#define EM_TI_C5500 142 /* Texas Instruments TMS320C55x DSP family */
#define EM_res143 143   /* Reserved */
#define EM_TI_PRU 144   /* Texas Instruments Programmable Realtime Unit */
#define EM_res145 145   /* Reserved */
#define EM_res146 146   /* Reserved */
#define EM_res147 147   /* Reserved */
#define EM_res148 148   /* Reserved */
#define EM_res149 149   /* Reserved */
#define EM_res150 150   /* Reserved */
#define EM_res151 151   /* Reserved */
#define EM_res152 152   /* Reserved */
#define EM_res153 153   /* Reserved */
#define EM_res154 154   /* Reserved */
#define EM_res155 155   /* Reserved */
#define EM_res156 156   /* Reserved */
#define EM_res157 157   /* Reserved */
#define EM_res158 158   /* Reserved */
#define EM_res159 159   /* Reserved */
#define EM_MMDSP_PLUS                                                          \
  160 /* STMicroelectronics 64bit VLIW Data Signal Processor */
#define EM_CYPRESS_M8C 161 /* Cypress M8C microprocessor */
#define EM_R32C 162        /* Renesas R32C series microprocessors */
#define EM_TRIMEDIA 163    /* NXP Semiconductors TriMedia architecture family */
#define EM_QDSP6 164       /* QUALCOMM DSP6 Processor */
#define EM_8051 165        /* Intel 8051 and variants */
#define EM_STXP7X 166      /* STMicroelectronics STxP7x family */
#define EM_NDS32                                                               \
  167 /* Andes Technology compact code size embedded RISC processor family */
#define EM_ECOG1 168  /* Cyan Technology eCOG1X family */
#define EM_ECOG1X 168 /* Cyan Technology eCOG1X family */
#define EM_MAXQ30                                                              \
  169                  /* Dallas Semiconductor MAXQ30 Core Micro-controllers   \
                        */
#define EM_XIMO16 170  /* New Japan Radio (NJR) 16-bit DSP Processor */
#define EM_MANIK 171   /* M2000 Reconfigurable RISC Microprocessor */
#define EM_CRAYNV2 172 /* Cray Inc. NV2 vector architecture */
#define EM_RX 173      /* Renesas RX family */
#define EM_METAG                                                               \
  174 /* Imagination Technologies Meta processor architecture                  \
       */
#define EM_MCST_ELBRUS                                                         \
  175                 /* MCST Elbrus general purpose hardware architecture */
#define EM_ECOG16 176 /* Cyan Technology eCOG16 family */
#define EM_CR16 177   /* National Semiconductor CompactRISC 16-bit processor */
#define EM_ETPU 178   /* Freescale Extended Time Processing Unit */
#define EM_SLE9X 179  /* Infineon Technologies SLE9X core */
#define EM_L1OM 180   /* Intel L1OM */
#define EM_K1OM 181   /* Intel K1OM */
#define EM_INTEL182 182 /* Reserved by Intel */
#define EM_AARCH64 183  /* ARM 64-bit architecture */
#define EM_ARM184 184   /* Reserved by ARM */
#define EM_AVR32 185    /* Atmel Corporation 32-bit microprocessor family */
#define EM_STM8 186     /* STMicroeletronics STM8 8-bit microcontroller */
#define EM_TILE64 187   /* Tilera TILE64 multicore architecture family */
#define EM_TILEPRO 188  /* Tilera TILEPro multicore architecture family */
#define EM_MICROBLAZE                                                          \
  189                 /* Xilinx MicroBlaze 32-bit RISC soft processor core */
#define EM_CUDA 190   /* NVIDIA CUDA architecture */
#define EM_TILEGX 191 /* Tilera TILE-Gx multicore architecture family */
#define EM_CLOUDSHIELD 192 /* CloudShield architecture family */
#define EM_COREA_1ST                                                           \
  193 /* KIPO-KAIST Core-A 1st generation processor family                     \
       */
#define EM_COREA_2ND                                                           \
  194 /* KIPO-KAIST Core-A 2nd generation processor family                     \
       */
#define EM_ARC_COMPACT2 195 /* Synopsys ARCompact V2 */
#define EM_OPEN8 196        /* Open8 8-bit RISC soft processor core */
#define EM_RL78 197         /* Renesas RL78 family.  */
#define EM_VIDEOCORE5 198   /* Broadcom VideoCore V processor */
#define EM_78K0R 199        /* Renesas 78K0R.  */
#define EM_56800EX                                                             \
  200                   /* Freescale 56800EX Digital Signal Controller (DSC)   \
                         */
#define EM_BA1 201      /* Beyond BA1 CPU architecture */
#define EM_BA2 202      /* Beyond BA2 CPU architecture */
#define EM_XCORE 203    /* XMOS xCORE processor family */
#define EM_MCHP_PIC 204 /* Microchip 8-bit PIC(r) family */
#define EM_INTEL205 205 /* Reserved by Intel */
#define EM_INTEL206 206 /* Reserved by Intel */
#define EM_INTEL207 207 /* Reserved by Intel */
#define EM_INTEL208 208 /* Reserved by Intel */
#define EM_INTEL209 209 /* Reserved by Intel */
#define EM_KM32 210     /* KM211 KM32 32-bit processor */
#define EM_KMX32 211    /* KM211 KMX32 32-bit processor */
#define EM_KMX16 212    /* KM211 KMX16 16-bit processor */
#define EM_KMX8 213     /* KM211 KMX8 8-bit processor */
#define EM_KVARC 214    /* KM211 KVARC processor */
#define EM_CDP 215      /* Paneve CDP architecture family */
#define EM_COGE 216     /* Cognitive Smart Memory Processor */
#define EM_COOL 217     /* Bluechip Systems CoolEngine */
#define EM_NORC 218     /* Nanoradio Optimized RISC */
#define EM_CSR_KALIMBA 219 /* CSR Kalimba architecture family */
#define EM_Z80 220         /* Zilog Z80 */
#define EM_VISIUM 221      /* Controls and Data Services VISIUMcore processor */
#define EM_FT32                                                                \
  222 /* FTDI Chip FT32 high performance 32-bit RISC architecture */
#define EM_MOXIE 223  /* Moxie processor family */
#define EM_AMDGPU 224 /* AMD GPU architecture */
#define EM_RISCV 243  /* RISC-V */
#define EM_LANAI 244  /* Lanai 32-bit processor.  */
#define EM_BPF 247    /* Linux BPF – in-kernel virtual machine.  */
#define EM_NFP 250    /* Netronome Flow Processor.  */
#define EM_CSKY 252   /* C-SKY processor family.  */

/* Values of note segment descriptor types for core files.  */

#define NT_PRSTATUS    1        /* Contains copy of prstatus struct */
#define NT_FPREGSET    2        /* Contains copy of fpregset struct */
#define NT_PRPSINFO    3        /* Contains copy of prpsinfo struct */
#define NT_TASKSTRUCT    4        /* Contains copy of task struct */
#define NT_AUXV        6        /* Contains copy of Elfxx_auxv_t */
#define NT_PRXFPREG    0x46e62b7f    /* Contains a user_xfpregs_struct; */
/*   note name must be "LINUX".  */
#define NT_PPC_VMX    0x100        /* PowerPC Altivec/VMX registers */
/*   note name must be "LINUX".  */
#define NT_PPC_VSX    0x102        /* PowerPC VSX registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TAR    0x103        /* PowerPC Target Address Register */
/*   note name must be "LINUX".  */
#define NT_PPC_PPR    0x104        /* PowerPC Program Priority Register */
/*   note name must be "LINUX".  */
#define NT_PPC_DSCR    0x105        /* PowerPC Data Stream Control Register */
/*   note name must be "LINUX".  */
#define NT_PPC_EBB    0x106        /* PowerPC Event Based Branch Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_PMU    0x107        /* PowerPC Performance Monitor Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CGPR    0x108        /* PowerPC TM checkpointed GPR Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CFPR    0x109        /* PowerPC TM checkpointed FPR Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CVMX    0x10a        /* PowerPC TM checkpointed VMX Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CVSX    0x10b        /* PowerPC TM checkpointed VSX Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_SPR    0x10c        /* PowerPC TM Special Purpose Registers */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CTAR    0x10d        /* PowerPC TM checkpointed TAR */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CPPR    0x10e        /* PowerPC TM checkpointed PPR */
/*   note name must be "LINUX".  */
#define NT_PPC_TM_CDSCR    0x10f        /* PowerPC TM checkpointed Data SCR */
/*   note name must be "LINUX".  */
#define NT_386_TLS    0x200        /* x86 TLS information */
/*   note name must be "LINUX".  */
#define NT_386_IOPERM    0x201        /* x86 io permissions */
/*   note name must be "LINUX".  */
#define NT_X86_XSTATE    0x202        /* x86 XSAVE extended state */
/*   note name must be "LINUX".  */
#define NT_X86_CET    0x203        /* x86 CET state.  */
/*   note name must be "LINUX".  */
#define NT_S390_HIGH_GPRS 0x300        /* S/390 upper halves of GPRs  */
/*   note name must be "LINUX".  */
#define NT_S390_TIMER    0x301        /* S390 timer */
/*   note name must be "LINUX".  */
#define NT_S390_TODCMP    0x302        /* S390 TOD clock comparator */
/*   note name must be "LINUX".  */
#define NT_S390_TODPREG    0x303        /* S390 TOD programmable register */
/*   note name must be "LINUX".  */
#define NT_S390_CTRS    0x304        /* S390 control registers */
/*   note name must be "LINUX".  */
#define NT_S390_PREFIX    0x305        /* S390 prefix register */
/*   note name must be "LINUX".  */
#define NT_S390_LAST_BREAK      0x306   /* S390 breaking event address */
/*   note name must be "LINUX".  */
#define NT_S390_SYSTEM_CALL     0x307   /* S390 system call restart data */
/*   note name must be "LINUX".  */
#define NT_S390_TDB    0x308        /* S390 transaction diagnostic block */
/*   note name must be "LINUX".  */
#define NT_S390_VXRS_LOW    0x309    /* S390 vector registers 0-15 upper half */
/*   note name must be "LINUX".  */
#define NT_S390_VXRS_HIGH    0x30a    /* S390 vector registers 16-31 */
/*   note name must be "LINUX".  */
#define NT_S390_GS_CB    0x30b        /* s390 guarded storage registers */
/*   note name must be "LINUX".  */
#define NT_S390_GS_BC    0x30c        /* s390 guarded storage broadcast control block */
/*   note name must be "LINUX".  */
#define NT_ARM_VFP    0x400        /* ARM VFP registers */
/* The following definitions should really use NT_AARCH_..., but defined
   this way for compatibility with Linux.  */
#define NT_ARM_TLS    0x401        /* AArch TLS registers */
/*   note name must be "LINUX".  */
#define NT_ARM_HW_BREAK    0x402        /* AArch hardware breakpoint registers */
/*   note name must be "LINUX".  */
#define NT_ARM_HW_WATCH    0x403        /* AArch hardware watchpoint registers */
/*   note name must be "LINUX".  */
#define NT_ARM_SVE    0x405        /* AArch SVE registers.  */
/*   note name must be "LINUX".  */
#define NT_ARM_PAC_MASK    0x406        /* AArch pointer authentication code masks */
/*   note name must be "LINUX".  */
#define NT_ARC_V2    0x600        /* ARC HS accumulator/extra registers.  */
/*   note name must be "LINUX".  */
#define NT_SIGINFO    0x53494749    /* Fields of siginfo_t.  */
#define NT_FILE        0x46494c45    /* Description of mapped files.  */

/* Values for program header, p_type field.  */

#define PT_NULL        0        /* Program header table entry unused */
#define PT_LOAD        1        /* Loadable program segment */
#define PT_DYNAMIC    2        /* Dynamic linking information */
#define PT_INTERP    3        /* Program interpreter */
#define PT_NOTE        4        /* Auxiliary information */
#define PT_SHLIB    5        /* Reserved, unspecified semantics */
#define PT_PHDR        6        /* Entry for header table itself */
#define PT_TLS        7        /* Thread local storage segment */
#define PT_LOOS        0x60000000    /* OS-specific */
#define PT_HIOS        0x6fffffff    /* OS-specific */
#define PT_LOPROC    0x70000000    /* Processor-specific */
#define PT_HIPROC    0x7FFFFFFF    /* Processor-specific */

/* Program segment permissions, in program header p_flags field.  */

#define PF_X        (1 << 0)    /* Segment is executable */
#define PF_W        (1 << 1)    /* Segment is writable */
#define PF_R        (1 << 2)    /* Segment is readable */
/* #define PF_MASKOS    0x0F000000    *//* OS-specific reserved bits */
#define PF_MASKOS    0x0FF00000    /* New value, Oct 4, 1999 Draft */
#define PF_MASKPROC    0xF0000000    /* Processor-specific reserved bits */

#define CORE_MEM_LINE_MASK  0xFFFFFFC0

#endif /* __MCD_ELF_DEFINE_H__ */
