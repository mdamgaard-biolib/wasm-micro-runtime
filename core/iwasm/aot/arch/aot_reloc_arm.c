/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_reloc.h"

#define R_ARM_CALL      28  /* PC relative 24 bit (BL, BLX).  */
#define R_ARM_JMP24     29  /* PC relative 24 bit (B/BL<cond>).  */
#define R_ARM_ABS32     2   /* Direct 32 bit */

#ifndef BH_MB
#define BH_MB 1024 * 1024
#endif

void __divdi3();
void __udivdi3();
void __moddi3();
void __umoddi3();
void __divsi3();
void __udivsi3();
void __modsi3();
void __udivmoddi4();
void __clzsi2();
void __fixsfdi();
void __fixunssfdi();
void __fixdfdi();
void __fixunsdfdi();
void __floatdisf();
void __floatundisf();
void __floatdidf();
void __floatundidf();
void __aeabi_l2f();
void __aeabi_f2lz();
void __aeabi_ul2f();
void __aeabi_d2lz();
void __aeabi_l2d();
void __aeabi_f2ulz();
void __aeabi_ul2d();
void __aeabi_d2ulz();
void __aeabi_idiv();
void __aeabi_uidiv();
void __aeabi_idivmod();
void __aeabi_uidivmod();
void __aeabi_ldivmod();
void __aeabi_uldivmod();

static SymbolMap target_sym_map[] = {
    REG_COMMON_SYMBOLS,
    /* compiler-rt symbols that come from compiler(e.g. gcc) */
    REG_SYM(__divdi3),
    REG_SYM(__udivdi3),
    REG_SYM(__umoddi3),
    REG_SYM(__divsi3),
    REG_SYM(__udivsi3),
    REG_SYM(__modsi3),
    REG_SYM(__udivmoddi4),
    REG_SYM(__clzsi2),
    REG_SYM(__fixsfdi),
    REG_SYM(__fixunssfdi),
    REG_SYM(__fixdfdi),
    REG_SYM(__fixunsdfdi),
    REG_SYM(__floatdisf),
    REG_SYM(__floatundisf),
    REG_SYM(__floatdidf),
    REG_SYM(__floatundidf),
    REG_SYM(__aeabi_l2f),
    REG_SYM(__aeabi_f2lz),
    REG_SYM(__aeabi_ul2f),
    REG_SYM(__aeabi_d2lz),
    REG_SYM(__aeabi_l2d),
    REG_SYM(__aeabi_f2ulz),
    REG_SYM(__aeabi_ul2d),
    REG_SYM(__aeabi_d2ulz),
    REG_SYM(__aeabi_idiv),
    REG_SYM(__aeabi_uidiv),
    REG_SYM(__aeabi_idivmod),
    REG_SYM(__aeabi_uidivmod),
    REG_SYM(__aeabi_ldivmod),
    REG_SYM(__aeabi_uldivmod)
};

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL)
        snprintf(error_buf, error_buf_size, "%s", string);
}

SymbolMap *
get_target_symbol_map(uint32 *sym_num)
{
    *sym_num = sizeof(target_sym_map) / sizeof(SymbolMap);
    return target_sym_map;
}

void
get_current_target(char *target_buf, uint32 target_buf_size)
{
    char *build_target = BUILD_TARGET;
    char *p = target_buf, *p_end;
    snprintf(target_buf, target_buf_size, "%s", build_target);
    p_end = p + strlen(target_buf);
    while (p < p_end) {
        if (*p >= 'A' && *p <= 'Z')
            *p++ += 'a' - 'A';
        else
            p++;
    }
    if (!strcmp(target_buf, "arm"))
        snprintf(target_buf, target_buf_size, "armv4");
}

uint32
get_plt_item_size()
{
    /* 8 bytes instructions and 4 bytes symbol address */
    return 12;
}

uint32
get_plt_table_size()
{
    return get_plt_item_size() * (sizeof(target_sym_map) / sizeof(SymbolMap));
}

void
init_plt_table(uint8 *plt)
{
    uint32 i, num = sizeof(target_sym_map) / sizeof(SymbolMap);
    for (i = 0; i < num; i++) {
        uint32 *p = (uint32*)plt;
        /* ldr pc, [pc] */
        *p++ = 0xe59ff000;
        /* nop */
        *p++ = 0xe1a00000;
        /* symbol addr */
        *p++ = (uint32)(uintptr_t)target_sym_map[i].symbol_addr;;
        plt += get_plt_item_size();
    }
}

static bool
check_reloc_offset(uint32 target_section_size,
                   uint64 reloc_offset, uint32 reloc_data_size,
                   char *error_buf, uint32 error_buf_size)
{
    if (!(reloc_offset < (uint64)target_section_size
          && reloc_offset + reloc_data_size <= (uint64)target_section_size)) {
        set_error_buf(error_buf, error_buf_size,
                      "AOT module load failed: invalid relocation offset.");
        return false;
    }
    return true;
}

bool
apply_relocation(AOTModule *module,
                 uint8 *target_section_addr, uint32 target_section_size,
                 uint64 reloc_offset, uint64 reloc_addend,
                 uint32 reloc_type, void *symbol_addr, int32 symbol_index,
                 char *error_buf, uint32 error_buf_size)
{
    switch (reloc_type) {
        case R_ARM_CALL:
        case R_ARM_JMP24:
        {
            intptr_t result;
            int32 RESULT_MASK = 0x03FFFFFE;
            int32 insn = *(int32*)(target_section_addr + reloc_offset);
            /* Initial addend: sign_extend(insn[23:0] << 2) */
            int32 initial_addend = ((insn & 0xFFFFFF) << 2)
                                    | ((insn & 0x800000) ? 0xFC000000 : 0);

            CHECK_RELOC_OFFSET(sizeof(int32));

            if (symbol_index < 0) {
                /* Symbol address itself is an AOT function.
                 * Apply relocation with the symbol directly.
                 * Suppose the symbol address is in +-32MB relative
                 * to the relocation address.
                 */
                /* operation: ((S + A) | T) - P  where S is symbol address and T is 0 */
                result = (intptr_t)
                         ((uint8*)symbol_addr + reloc_addend
                          - (target_section_addr + reloc_offset));
            }
            else {
                if (reloc_addend > 0) {
                     set_error_buf(error_buf, error_buf_size,
                                   "AOT module load failed: relocate to plt table "
                                   "with reloc addend larger than 0 is unsupported.");
                     return false;
                }

                /* Symbol address is not an AOT function,
                 * but a function of runtime or native. Its address is
                 * beyond of the +-32MB space. Apply relocation with
                 * the PLT which branch to the target symbol address.
                 */
                /* operation: ((S + A) | T) - P  where S is PLT address and T is 0 */
                uint8 *plt = (uint8*)module->code + module->code_size - get_plt_table_size()
                             + get_plt_item_size() * symbol_index;
                result = (intptr_t)
                         (plt + reloc_addend
                          - (target_section_addr + reloc_offset));
            }

            result += initial_addend;

            /* Check overflow: +-32MB */
            if (result > (32 * BH_MB) || result < (-32 * BH_MB)) {
                set_error_buf(error_buf, error_buf_size,
                              "AOT module load failed: "
                              "target address out of range.");
                return false;
            }

            *(int32*)(target_section_addr + reloc_offset) =
                (int32)
                ((insn & 0xff000000)
                 | (((int32)result & RESULT_MASK) >> 2));
            break;
        }
        case R_ARM_ABS32:
        {
            intptr_t initial_addend;
            /* (S + A) | T where T is 0 */
            CHECK_RELOC_OFFSET(sizeof(void*));
            initial_addend = *(intptr_t*)(target_section_addr + (uint32)reloc_offset);
            *(uint8**)(target_section_addr + reloc_offset)
                = (uint8*)symbol_addr + initial_addend + reloc_addend;
            break;
        }

        default:
            if (error_buf != NULL)
                snprintf(error_buf, error_buf_size,
                         "Load relocation section failed: "
                         "invalid relocation type %d.",
                         reloc_type);
            return false;
    }

    return true;
}

