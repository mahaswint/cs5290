	.text
	.file	"wram_budget_probe.c"
	.section	.text.descriptor_bytes,"ax",@progbits
	.globl	descriptor_bytes                // -- Begin function descriptor_bytes
	.type	descriptor_bytes,@function
descriptor_bytes:                       // @descriptor_bytes
.Lfunc_begin0:
	.file	1 "/home/mahaswint/cs5290" "pim-reproduction/research/wram_budget_probe.c"
	.loc	1 21 0                          // pim-reproduction/research/wram_budget_probe.c:21:0
	.cfi_sections .debug_frame
	.cfi_startproc
// %bb.0:
	.cfi_def_cfa_offset 0
	move r0, 32
.Ltmp0:
	.loc	1 21 35 prologue_end            // pim-reproduction/research/wram_budget_probe.c:21:35
	jump r23
.Ltmp1:
.Lfunc_end0:
	.size	descriptor_bytes, .Lfunc_end0-descriptor_bytes
	.cfi_endproc
	.section	.stack_sizes,"o",@progbits,.text.descriptor_bytes
	.long	.Lfunc_begin0
	.byte	0
	.section	.text.descriptor_bytes,"ax",@progbits
                                        // -- End function
	.section	.text.one_slot_16_tasklets_bytes,"ax",@progbits
	.globl	one_slot_16_tasklets_bytes      // -- Begin function one_slot_16_tasklets_bytes
	.type	one_slot_16_tasklets_bytes,@function
one_slot_16_tasklets_bytes:             // @one_slot_16_tasklets_bytes
.Lfunc_begin1:
	.loc	1 22 0                          // pim-reproduction/research/wram_budget_probe.c:22:0
	.cfi_startproc
// %bb.0:
	.cfi_def_cfa_offset 0
	move r0, 512
.Ltmp2:
	.loc	1 23 5 prologue_end             // pim-reproduction/research/wram_budget_probe.c:23:5
	jump r23
.Ltmp3:
.Lfunc_end1:
	.size	one_slot_16_tasklets_bytes, .Lfunc_end1-one_slot_16_tasklets_bytes
	.cfi_endproc
	.section	.stack_sizes,"o",@progbits,.text.one_slot_16_tasklets_bytes
	.long	.Lfunc_begin1
	.byte	0
	.section	.text.one_slot_16_tasklets_bytes,"ax",@progbits
                                        // -- End function
	.section	.text.one_slot_24_tasklets_bytes,"ax",@progbits
	.globl	one_slot_24_tasklets_bytes      // -- Begin function one_slot_24_tasklets_bytes
	.type	one_slot_24_tasklets_bytes,@function
one_slot_24_tasklets_bytes:             // @one_slot_24_tasklets_bytes
.Lfunc_begin2:
	.loc	1 25 0                          // pim-reproduction/research/wram_budget_probe.c:25:0
	.cfi_startproc
// %bb.0:
	.cfi_def_cfa_offset 0
	move r0, 768
.Ltmp4:
	.loc	1 26 5 prologue_end             // pim-reproduction/research/wram_budget_probe.c:26:5
	jump r23
.Ltmp5:
.Lfunc_end2:
	.size	one_slot_24_tasklets_bytes, .Lfunc_end2-one_slot_24_tasklets_bytes
	.cfi_endproc
	.section	.stack_sizes,"o",@progbits,.text.one_slot_24_tasklets_bytes
	.long	.Lfunc_begin2
	.byte	0
	.section	.text.one_slot_24_tasklets_bytes,"ax",@progbits
                                        // -- End function
	.section	.text.eight_slots_16_tasklets_bytes,"ax",@progbits
	.globl	eight_slots_16_tasklets_bytes   // -- Begin function eight_slots_16_tasklets_bytes
	.type	eight_slots_16_tasklets_bytes,@function
eight_slots_16_tasklets_bytes:          // @eight_slots_16_tasklets_bytes
.Lfunc_begin3:
	.loc	1 28 0                          // pim-reproduction/research/wram_budget_probe.c:28:0
	.cfi_startproc
// %bb.0:
	.cfi_def_cfa_offset 0
	move r0, 4096
.Ltmp6:
	.loc	1 29 5 prologue_end             // pim-reproduction/research/wram_budget_probe.c:29:5
	jump r23
.Ltmp7:
.Lfunc_end3:
	.size	eight_slots_16_tasklets_bytes, .Lfunc_end3-eight_slots_16_tasklets_bytes
	.cfi_endproc
	.section	.stack_sizes,"o",@progbits,.text.eight_slots_16_tasklets_bytes
	.long	.Lfunc_begin3
	.byte	0
	.section	.text.eight_slots_16_tasklets_bytes,"ax",@progbits
                                        // -- End function
	.section	.debug_abbrev,"",@progbits
	.byte	1                               // Abbreviation Code
	.byte	17                              // DW_TAG_compile_unit
	.byte	1                               // DW_CHILDREN_yes
	.byte	37                              // DW_AT_producer
	.byte	14                              // DW_FORM_strp
	.byte	19                              // DW_AT_language
	.byte	5                               // DW_FORM_data2
	.byte	3                               // DW_AT_name
	.byte	14                              // DW_FORM_strp
	.byte	16                              // DW_AT_stmt_list
	.byte	23                              // DW_FORM_sec_offset
	.byte	27                              // DW_AT_comp_dir
	.byte	14                              // DW_FORM_strp
	.byte	17                              // DW_AT_low_pc
	.byte	1                               // DW_FORM_addr
	.byte	85                              // DW_AT_ranges
	.byte	23                              // DW_FORM_sec_offset
	.byte	0                               // EOM(1)
	.byte	0                               // EOM(2)
	.byte	2                               // Abbreviation Code
	.byte	46                              // DW_TAG_subprogram
	.byte	0                               // DW_CHILDREN_no
	.byte	17                              // DW_AT_low_pc
	.byte	1                               // DW_FORM_addr
	.byte	18                              // DW_AT_high_pc
	.byte	6                               // DW_FORM_data4
	.byte	64                              // DW_AT_frame_base
	.byte	24                              // DW_FORM_exprloc
	.ascii	"\227B"                         // DW_AT_GNU_all_call_sites
	.byte	25                              // DW_FORM_flag_present
	.byte	3                               // DW_AT_name
	.byte	14                              // DW_FORM_strp
	.byte	58                              // DW_AT_decl_file
	.byte	11                              // DW_FORM_data1
	.byte	59                              // DW_AT_decl_line
	.byte	11                              // DW_FORM_data1
	.byte	39                              // DW_AT_prototyped
	.byte	25                              // DW_FORM_flag_present
	.byte	73                              // DW_AT_type
	.byte	19                              // DW_FORM_ref4
	.byte	63                              // DW_AT_external
	.byte	25                              // DW_FORM_flag_present
	.byte	0                               // EOM(1)
	.byte	0                               // EOM(2)
	.byte	3                               // Abbreviation Code
	.byte	36                              // DW_TAG_base_type
	.byte	0                               // DW_CHILDREN_no
	.byte	3                               // DW_AT_name
	.byte	14                              // DW_FORM_strp
	.byte	62                              // DW_AT_encoding
	.byte	11                              // DW_FORM_data1
	.byte	11                              // DW_AT_byte_size
	.byte	11                              // DW_FORM_data1
	.byte	0                               // EOM(1)
	.byte	0                               // EOM(2)
	.byte	0                               // EOM(3)
	.section	.debug_info,"",@progbits
.Lcu_begin0:
	.long	.Ldebug_info_end0-.Ldebug_info_start0 // Length of Unit
.Ldebug_info_start0:
	.short	4                               // DWARF version number
	.long	.debug_abbrev                   // Offset Into Abbrev. Section
	.byte	4                               // Address Size (in bytes)
	.byte	1                               // Abbrev [1] 0xb:0x77 DW_TAG_compile_unit
	.long	.Linfo_string0                  // DW_AT_producer
	.short	12                              // DW_AT_language
	.long	.Linfo_string1                  // DW_AT_name
	.long	.Lline_table_start0             // DW_AT_stmt_list
	.long	.Linfo_string2                  // DW_AT_comp_dir
	.long	0                               // DW_AT_low_pc
	.long	.Ldebug_ranges0                 // DW_AT_ranges
	.byte	2                               // Abbrev [2] 0x26:0x15 DW_TAG_subprogram
	.long	.Lfunc_begin0                   // DW_AT_low_pc
	.long	.Lfunc_end0-.Lfunc_begin0       // DW_AT_high_pc
	.byte	1                               // DW_AT_frame_base
	.byte	102
                                        // DW_AT_GNU_all_call_sites
	.long	.Linfo_string3                  // DW_AT_name
	.byte	1                               // DW_AT_decl_file
	.byte	21                              // DW_AT_decl_line
                                        // DW_AT_prototyped
	.long	122                             // DW_AT_type
                                        // DW_AT_external
	.byte	2                               // Abbrev [2] 0x3b:0x15 DW_TAG_subprogram
	.long	.Lfunc_begin1                   // DW_AT_low_pc
	.long	.Lfunc_end1-.Lfunc_begin1       // DW_AT_high_pc
	.byte	1                               // DW_AT_frame_base
	.byte	102
                                        // DW_AT_GNU_all_call_sites
	.long	.Linfo_string5                  // DW_AT_name
	.byte	1                               // DW_AT_decl_file
	.byte	22                              // DW_AT_decl_line
                                        // DW_AT_prototyped
	.long	122                             // DW_AT_type
                                        // DW_AT_external
	.byte	2                               // Abbrev [2] 0x50:0x15 DW_TAG_subprogram
	.long	.Lfunc_begin2                   // DW_AT_low_pc
	.long	.Lfunc_end2-.Lfunc_begin2       // DW_AT_high_pc
	.byte	1                               // DW_AT_frame_base
	.byte	102
                                        // DW_AT_GNU_all_call_sites
	.long	.Linfo_string6                  // DW_AT_name
	.byte	1                               // DW_AT_decl_file
	.byte	25                              // DW_AT_decl_line
                                        // DW_AT_prototyped
	.long	122                             // DW_AT_type
                                        // DW_AT_external
	.byte	2                               // Abbrev [2] 0x65:0x15 DW_TAG_subprogram
	.long	.Lfunc_begin3                   // DW_AT_low_pc
	.long	.Lfunc_end3-.Lfunc_begin3       // DW_AT_high_pc
	.byte	1                               // DW_AT_frame_base
	.byte	102
                                        // DW_AT_GNU_all_call_sites
	.long	.Linfo_string7                  // DW_AT_name
	.byte	1                               // DW_AT_decl_file
	.byte	28                              // DW_AT_decl_line
                                        // DW_AT_prototyped
	.long	122                             // DW_AT_type
                                        // DW_AT_external
	.byte	3                               // Abbrev [3] 0x7a:0x7 DW_TAG_base_type
	.long	.Linfo_string4                  // DW_AT_name
	.byte	7                               // DW_AT_encoding
	.byte	4                               // DW_AT_byte_size
	.byte	0                               // End Of Children Mark
.Ldebug_info_end0:
	.section	.debug_ranges,"",@progbits
.Ldebug_ranges0:
	.long	.Lfunc_begin0
	.long	.Lfunc_end0
	.long	.Lfunc_begin1
	.long	.Lfunc_end1
	.long	.Lfunc_begin2
	.long	.Lfunc_end2
	.long	.Lfunc_begin3
	.long	.Lfunc_end3
	.long	0
	.long	0
	.section	.debug_str,"MS",@progbits,1
.Linfo_string0:
	.asciz	"clang version 12.0.0 (https://github.com/upmem/llvm-project.git 846fdda8285dcc9b20ee5d2fec9e54dfea6a8928)" // string offset=0
.Linfo_string1:
	.asciz	"/home/mahaswint/cs5290/pim-reproduction/research/wram_budget_probe.c" // string offset=106
.Linfo_string2:
	.asciz	"/home/mahaswint/cs5290"        // string offset=175
.Linfo_string3:
	.asciz	"descriptor_bytes"              // string offset=198
.Linfo_string4:
	.asciz	"unsigned int"                  // string offset=215
.Linfo_string5:
	.asciz	"one_slot_16_tasklets_bytes"    // string offset=228
.Linfo_string6:
	.asciz	"one_slot_24_tasklets_bytes"    // string offset=255
.Linfo_string7:
	.asciz	"eight_slots_16_tasklets_bytes" // string offset=282
	.addrsig
	.section	.debug_line,"",@progbits
.Lline_table_start0:
