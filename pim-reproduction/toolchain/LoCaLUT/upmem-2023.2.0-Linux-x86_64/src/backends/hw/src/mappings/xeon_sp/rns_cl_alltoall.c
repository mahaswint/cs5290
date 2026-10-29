
#define _GNU_SOURCE
#include <stdint.h>

#include "dpu_region_address_translation.h"
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <dpu.h>
#include <errno.h>
#include <limits.h>
#include <numa.h>
#include <numaif.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <x86intrin.h>
#include <immintrin.h>
#include <sys/sysinfo.h>
#include <time.h>
#include "static_verbose.h"

#define BANK_OFFSET_NEXT_DATA_a2a(i) (i * 16) // For each 64bit word, you must jump 16 * 64bit (2 cache lines)
#define BANK_CHUNK_SIZE_a2a 0x20000 //128kb
#define BANK_NEXT_CHUNK_OFFSET_a2a 0x100000 //1mb
#define MRAM_SIZE_a2a 0x4000000 // 64MB

#define CACHE_LINE_a2a 64
#define CACHE_LINE2_a2a 128

#define CACHE_LINE 64
#define CACHE_LINE2 128

static uint32_t apply_address_translation_on_mram_offset_a2a(uint32_t byte_offset)
{
    /* We have observed that, within the 26 address bits of the MRAM address, we need to apply an address translation:
     *
     * virtual[13: 0] = physical[13: 0]
     * virtual[20:14] = physical[21:15]
     * virtual[   21] = physical[   14]
     * virtual[25:22] = physical[25:22]
     *
     * This function computes the "virtual" mram address based on the given "physical" mram address.
     */

    uint32_t mask_21_to_15 = ((1 << (21 - 15 + 1)) - 1) << 15;
    uint32_t mask_21_to_14 = ((1 << (21 - 14 + 1)) - 1) << 14;
    uint32_t bits_21_to_15 = (byte_offset & mask_21_to_15) >> 15;
    uint32_t bit_14 = (byte_offset >> 14) & 1;
    uint32_t unchanged_bits = byte_offset & ~mask_21_to_14;

    return unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
}

/*static uint64_t address_offset_change_g(uint32_t byte_offset)
{
    uint32_t mask_21_to_15 = ((1 << (21 - 15 + 1)) - 1) << 15;
    uint32_t mask_21_to_14 = ((1 << (21 - 14 + 1)) - 1) << 14;
    uint32_t bits_21_to_15 = (byte_offset & mask_21_to_15) >> 15;
    uint32_t bit_14 = (byte_offset >> 14) & 1;
    uint32_t unchanged_bits = byte_offset & ~mask_21_to_14;
    uint32_t mram_64_bit_word_offset = unchanged_bits | (bits_21_to_15 << 14) | (bit_14 << 21);
    uint64_t next_data = mram_64_bit_word_offset << 4;
    uint64_t host_side_offset = (next_data & (BANK_CHUNK_SIZE_a2a - 1)) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
    return host_side_offset;
}*/

/////////////////////////////////////////////////////////

#define RNS_FLUSH_DST(iter, dst_rank_bgwise_addr)                                  \
    do                                                                             \
    {                                                                              \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                        \
        void *dst_rank_clwise_addr2 = dst_rank_bgwise_addr + (CACHE_LINE2 * iter); \
                                                                                   \
        for (int cl = 0; cl < iter; cl++)                                          \
        {                                                                          \
            __builtin_ia32_clflushopt((uint8_t *)dst_rank_clwise_addr1);           \
            __builtin_ia32_clflushopt((uint8_t *)(dst_rank_clwise_addr2));         \
            dst_rank_clwise_addr1 += CACHE_LINE2;                                  \
            dst_rank_clwise_addr2 += CACHE_LINE2;                                  \
        }                                                                          \
    } while (0)

#define RNS_FLUSH_SRC(iter, src_rank_bgwise_addr)                                      \
    do                                                                                 \
    {                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                             \
                                                                                       \
        for (int cl = 0; cl < iter; cl++)                                              \
        {                                                                              \
            __builtin_ia32_clflushopt((uint8_t *)src_rank_clwise_addr);                \
            __builtin_ia32_clflushopt((uint8_t *)(src_rank_clwise_addr + CACHE_LINE)); \
            src_rank_clwise_addr += CACHE_LINE2;                                       \
        }                                                                              \
    } while (0)

#define RNS_SRC_BG_a2a 4
#define RNS_TAR_BG_a2a 8
#define chip_rank_a2a 64

#define RNS_COPY_a2a(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                               \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg1_rot;                                                                 \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
            reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));              \
            reg1_rot = _mm512_rol_epi64(reg1, rotate_bit);                                \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot);               \
            /*_mm512_store_si512((void *)(dst_rank_clwise_addr1), reg1_rot);               */\
            /*src_rank_clwise_addr += CACHE_LINE2;                                          \
            dst_rank_clwise_addr1 += CACHE_LINE2;*/                                         \
            /*_mm512_store_si512((void *)(dst_rank_clwise_addr1), reg1_rot); \
            __m512i reg_zero_store = _mm512_setzero_si512(); \
            _mm512_store_si512((void *)(src_rank_clwise_addr), reg_zero_store); \
            */ \
            /*uint8_t a[64], b[64]; \
            if(rotate==0){_mm512_store_si512((void *)a, reg1); \
            _mm512_store_si512((void *)b, reg1_rot); \
            printf("Check the elements\n"); \
            printf("a: "); for(int i=0; i<chip_rank; i++) {printf("%d ", a[i]);} \
            printf("\nVA data(b): "); for(int i=0; i<chip_rank; i++) {printf("%d ", b[i]);} \
            printf("\n"); }*/\
        }                                                                                 \
    } while (0)

#define RNS_COPY_a2a_24(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, a_length)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                               \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg1_rot;                                                                 \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
            reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));              \
            if(a_length==4) {reg1_rot = _mm512_rol_epi32(reg1, rotate_bit); _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot); } \
            else if(a_length==2) { \
            /*__m512i x2_index = _mm512_setr_epi32(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14); */ \
            /*short indices[32] = {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13 ,12, 15, 14, 17, 16, 19, 18, 21, 20, 23, 22, 25, 24, 27, 26, 29, 28, 31, 30}; \
            short indices[32] = {30, 31, 28, 29, 26, 27, 24, 25, 22, 23, 20, 21, 18, 19, 16, 17, 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1}; */\
            if(rotate==0) {_mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1); } \
            else if(rotate==1) {/*short indices[64] = {62, 63, 60, 61, 58, 59, 56, 57, 54, 55, 52, 53, 50, 51, 48, 49, 46, 47, 44, 45, 42, 43, 40, 41, 38, 39, 36, 37, 34, 35, 32, 33, 30, 31, 28, 29, 26, 27, 24, 25, 22, 23, 20, 21, 18, 19, 16, 17, 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1}; \
            __m512i x2_index = _mm512_loadu_si512(indices); \
            reg1_rot = _mm512_permutexvar_epi8(x2_index, reg1); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot); }*/\
            /*__m512i mask = _mm512_set_epi64(\
                                            0x0f0007030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);*/\
            __m512i mask = _mm512_set_epi64(                  /*mask의 맨 윗줄은 실제 data의 뒤를 담당*/          \
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL); \
            reg1_rot = _mm512_shuffle_epi8(reg1, mask); \
            \
            /*uint8_t a[64], b[64]; \
            _mm512_store_si512((void *)a, reg1); \
            _mm512_store_si512((void *)b, reg1_rot); \
            printf("Check the elements\n"); \
            printf("a: \n"); for(int i=0; i<64; i++) {printf("%.2d ", a[i]); if(i%16==15){printf("\n");}} \
            printf("\n"); \
            printf("b: \n"); for(int i=0; i<64; i++) {printf("%.2d ", b[i]); if(i%16==15){printf("\n");}} \
            printf("\n"); */\
            \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot); }\
            /*if(rotate==0) {_mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1); } \
            else if(rotate==1) {short indices[32] = {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13 ,12, 15, 14, 17, 16, 19, 18, 21, 20, 23, 22, 25, 24, 27, 26, 29, 28, 31, 30}; \
            __m512i x2_index = _mm512_loadu_si512(indices); \
            reg1_rot = _mm512_permutexvar_epi16(x2_index, reg1); } \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1_rot); */\
            } \
        }                                                                                 \
    } while (0)

#define S_COPY_a2a(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1 = dst_rank_bgwise_addr;                               \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
            reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));              \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1), reg1);               \
        }                                                                                 \
    } while (0)

#define COMM_FLUSH_a2a(iter, src_rank_bgwise_addr)                                      \
    do                                                                                 \
    {                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                             \
                                                                                       \
        for (int cl = 0; cl < iter; cl++)                                              \
        {                                                                              \
            __builtin_ia32_clflushopt((uint8_t *)src_rank_clwise_addr);                \
            /*__builtin_ia32_clflushopt((uint8_t *)(src_rank_clwise_addr + CACHE_LINE)); */\
            /*_mm_mfence(); */                                                         \
            src_rank_clwise_addr += CACHE_LINE2_a2a;                                       \
        }                                                                              \
    } while (0)

#define RNS_COPY_ag(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[8*iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
                                                                                \
        for (uint32_t cl = 0; cl < 8*iter; cl++)        /**/                                       \
        {                                                                                 \
            dst_rank_clwise_addr1[cl] = dst_rank_bgwise_addr_array[cl];                    \
        }                                                                                   \
        reg1 = _mm512_rol_epi64(reg1, 0); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+0]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+1]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+2]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+3]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+4]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+5]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+6]), reg1);               \
        } \
        reg1 = _mm512_rol_epi64(reg1, 8); \
        for(uint32_t cl=0; cl<iter; cl++){ \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+7]), reg1);               \
        } \
        /*for(uint32_t cl=0; cl<iter; cl++){ \
            reg1_rot = _mm512_rol_epi64(reg1, 0);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+0]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 8);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+1]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 16);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+2]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 24);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+3]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 32);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+4]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 40);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+5]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 48);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+6]), reg1_rot);               \
            reg1_rot = _mm512_rol_epi64(reg1, 56);                                    \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[8*cl+7]), reg1_rot);               \
        } */\
                                                                                        \
    } while (0)

#define RNS_COPY_ag_24(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array, a_length)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[a_length*iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
                                                                                \
        for (uint32_t cl = 0; cl < a_length*iter; cl++)        /**/                                       \
        {                                                                                 \
            dst_rank_clwise_addr1[cl] = dst_rank_bgwise_addr_array[cl];                    \
        }                                                                                   \
        if(a_length==4){\
            reg1 = _mm512_rol_epi32(reg1, 0); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[0]), reg1); \
            reg1 = _mm512_rol_epi32(reg1, 8); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[1]), reg1); \
            reg1 = _mm512_rol_epi32(reg1, 8); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[2]), reg1); \
            reg1 = _mm512_rol_epi32(reg1, 8); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[3]), reg1); \
        } \
        else if(a_length==2){\
            reg1 = _mm512_rol_epi64(reg1, 0); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[0]), reg1); \
            __m512i mask = _mm512_set_epi64(                  /*mask의 맨 윗줄은 실제 data의 뒤를 담당*/          \
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL,\
                                            0x0e0f0c0d0a0b0809ULL,\
                                            0x0607040502030001ULL); \
            __m512i reg1_shuf;\
            reg1_shuf = _mm512_shuffle_epi8(reg1, mask); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[1]), reg1_shuf); \
        } \
    } while (0)

#define S_COPY_ag(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
                                                                                \
        for (uint32_t cl = 0; cl < iter; cl++)        /**/                                       \
        {                                                                                 \
            dst_rank_clwise_addr1[cl] = dst_rank_bgwise_addr_array[cl];                    \
        }                                                                                   \
        for (uint32_t cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cl]), reg1);               \
        }                                                                                 \
    } while (0)

#define S_COPY_ag_24(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array, a_length)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[(8/a_length)*iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
                                                                                \
        for (uint32_t cl = 0; cl < (8/a_length)*iter; cl++)        /**/                                       \
        {                                                                                 \
            dst_rank_clwise_addr1[cl] = dst_rank_bgwise_addr_array[cl];                    \
        }                                                                                   \
        if(a_length==4){\
            reg1 = _mm512_rol_epi64(reg1, 0); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+0]), reg1); \
            }\
            reg1 = _mm512_rol_epi64(reg1, 32); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+1]), reg1); \
            }\
        }\
        else if(a_length==2){\
            reg1 = _mm512_rol_epi64(reg1, 0); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+0]), reg1); \
            }\
            reg1 = _mm512_rol_epi64(reg1, 16); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+1]), reg1); \
            }\
            reg1 = _mm512_rol_epi64(reg1, 16); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+2]), reg1); \
            }\
            reg1 = _mm512_rol_epi64(reg1, 16); \
            for(uint32_t cl=0; cl<iter; cl++){\
                _mm512_stream_si512((void *)(dst_rank_clwise_addr1[(8/a_length)*cl+3]), reg1); \
            }\
        } \
    } while (0)

#define RNS_COPY_g(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array, cal_rg, cal_x)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[8*iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
                                                                                \
        dst_rank_clwise_addr1[cal_rg*8+cal_x] = dst_rank_bgwise_addr_array[cal_rg*8+cal_x]; \
        if(cal_x==0){ \
            reg1=_mm512_rol_epi64(reg1, 0); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==1){ \
            reg1=_mm512_rol_epi64(reg1, 8); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==2){ \
            reg1=_mm512_rol_epi64(reg1, 16); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==3){ \
            reg1=_mm512_rol_epi64(reg1, 24); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==4){ \
            reg1=_mm512_rol_epi64(reg1, 32); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==5){ \
            reg1=_mm512_rol_epi64(reg1, 40); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==6){ \
            reg1=_mm512_rol_epi64(reg1, 48); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
        else if(cal_x==7){ \
            reg1=_mm512_rol_epi64(reg1, 56); \
            _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg*8+cal_x]), reg1);} \
                                                                                        \
    } while (0)

#define S_COPY_g(rotate, rotate_bit, iter, src_rank_bgwise_addr, dst_rank_bgwise_addr_array, cal_rg)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr1[iter]; /*iter==num_iter_dst*/                                                     \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr));                  \
        /*uint8_t a[64]; \
        _mm512_store_si512((void *)a, reg1); \
        printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]); \*/ \
        dst_rank_clwise_addr1[cal_rg] = dst_rank_bgwise_addr_array[cal_rg]; \
        _mm512_stream_si512((void *)(dst_rank_clwise_addr1[cal_rg]), reg1);               \
    } while (0)





#define S_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
        __m512i sum = _mm512_set_epi64(                                                    \
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                        \
                                                                                          \
                                                                                          \
        if(size == 1){                                                                      \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    sum = _mm512_add_epi8(sum, reg1);                          \
                }                                                                           \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
            }                                                                                 \
        }                                                                                  \
        else{                                                                               \
                                                                                            \
            __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
            __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
            __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                                \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                    reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                    reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                \
                                                                                                \
                    if(size == 2) sum = _mm512_add_epi16(sum, reg1);                          \
                    else if(size == 4) sum = _mm512_add_epi32(sum, reg1);                          \
                    else sum = _mm512_add_epi64(sum, reg1);                          \
                }                                                                           \
                                                                                            \
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
            }                                                                                 \
        }                                                                                       \
    } while (0)

#define S_SUM_RS_DPU(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, src_off_gran)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
                                                                                            \
            for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                            \
                reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + i*src_off_gran), reg1);            \
            }                                                                           \
                                                                                          \
                                                                                          \
                                                                                            \
        }                                                                                 \
    } while (0)

#define RNS_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                          \
                                                                                          \
        if(size == 1){                                                                        \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                \
                                                                                                \
                    sum = _mm512_add_epi8(sum, reg1);                          \
                    sum = _mm512_add_epi8(sum, reg2);                          \
                    sum = _mm512_add_epi8(sum, reg3);                                  \
                    sum = _mm512_add_epi8(sum, reg4);                                  \
                    sum = _mm512_add_epi8(sum, reg5);                                  \
                    sum = _mm512_add_epi8(sum, reg6);                                  \
                    sum = _mm512_add_epi8(sum, reg7);                                  \
                    sum = _mm512_add_epi8(sum, reg8);                                  \
                                                                                        \
                }                                                                           \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
            }                                                                                 \
        }                                                                                   \
        else{                                                                                  \
                                                                                            \
            __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
            __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
            __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                            \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                    reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                    reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                    reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                    reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                    reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                    reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                    reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                    reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                    reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                    reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                    reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                    reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                    reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                    reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                    reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                    reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                    reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                    reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                    reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                    reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                    reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                    reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                    reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                    reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                                \
                                                                                                \
                    if(size == 2){                                                               \
                        sum = _mm512_add_epi16(sum, reg1);                          \
                        sum = _mm512_add_epi16(sum, reg2);                          \
                        sum = _mm512_add_epi16(sum, reg3);                                  \
                        sum = _mm512_add_epi16(sum, reg4);                                  \
                        sum = _mm512_add_epi16(sum, reg5);                                  \
                        sum = _mm512_add_epi16(sum, reg6);                                  \
                        sum = _mm512_add_epi16(sum, reg7);                                  \
                        sum = _mm512_add_epi16(sum, reg8);                                  \
                    }                                                                       \
                    else if(size == 4){                                                               \
                        sum = _mm512_add_epi32(sum, reg1);                          \
                        sum = _mm512_add_epi32(sum, reg2);                          \
                        sum = _mm512_add_epi32(sum, reg3);                                  \
                        sum = _mm512_add_epi32(sum, reg4);                                  \
                        sum = _mm512_add_epi32(sum, reg5);                                  \
                        sum = _mm512_add_epi32(sum, reg6);                                  \
                        sum = _mm512_add_epi32(sum, reg7);                                  \
                        sum = _mm512_add_epi32(sum, reg8);                                  \
                    }                                                                       \
                    else{                                                                    \
                        sum = _mm512_add_epi64(sum, reg1);                          \
                        sum = _mm512_add_epi64(sum, reg2);                          \
                        sum = _mm512_add_epi64(sum, reg3);                                  \
                        sum = _mm512_add_epi64(sum, reg4);                                  \
                        sum = _mm512_add_epi64(sum, reg5);                                  \
                        sum = _mm512_add_epi64(sum, reg6);                                  \
                        sum = _mm512_add_epi64(sum, reg7);                                  \
                        sum = _mm512_add_epi64(sum, reg8);                                  \
                    }                                                                       \
                                                                                        \
                }                                                                           \
                                                                                            \
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
            }                                                                                 \
        }                                                                                       \
    } while (0)

#define RNS_SUM_RS_DPU(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, src_off_gran)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                          \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
                                                                                            \
            for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                            \
                reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i)*src_off_gran), reg1);   \
                                                                                            \
                reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+1)*src_off_gran), reg2);   \
                                                                                            \
                reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+2)*src_off_gran), reg3);   \
                                                                                            \
                reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+3)*src_off_gran), reg4);   \
                                                                                            \
                reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+4)*src_off_gran), reg5);   \
                                                                                            \
                reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+5)*src_off_gran), reg6);   \
                                                                                            \
                reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+6)*src_off_gran), reg7);   \
                                                                                            \
                reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9 + (8*i+7)*src_off_gran), reg8);   \
                                                                                            \
            }                                                                           \
        }                                                                                 \
    } while (0)

#define S_SUM_AR(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size, reduce_type)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                        \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                          \
        if(reduce_type == 0){                                                                 \
            if(size == 1){                                                                      \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                                                                                                    \
                                                                                                    \
                                                                                                    \
                        sum = _mm512_add_epi8(sum, reg1);                          \
                    }                                                                           \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[i];                                \
                                                                                                            \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                  \
            else{                                                                                  \
                                                                                                            \
                __m512i mask = _mm512_set_epi64(\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL);\
                __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
                __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                                \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                    \
                                                                                                    \
                        if(size == 1) sum = _mm512_add_epi8(sum, reg1);                                                                            \
                        else if(size == 2) sum = _mm512_add_epi16(sum, reg1);                          \
                        else if(size == 4) sum = _mm512_add_epi32(sum, reg1);                          \
                        else sum = _mm512_add_epi64(sum, reg1);                                 \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                    sum = _mm512_shuffle_epi8(sum, mask);                                     \
                    sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[i];                                \
                                                                                                            \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                   \
        }                                                                                       \
        else if(reduce_type == 1){                                                                 \
            if(size == 0){                                                                      \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        sum = _mm512_or_epi64(sum, reg1);                          \
                    }                                                                           \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[i];                                \
                                                                                                            \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                  \
            else if(size == 1){                                                                      \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                                                                                                    \
                                                                                                    \
                                                                                                    \
                        sum = _mm512_max_epi8(sum, reg1);                          \
                    }                                                                           \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[i];                                \
                                                                                                            \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                  \
            else{                                                                                  \
                                                                                                            \
                __m512i mask = _mm512_set_epi64(\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL);\
                __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
                __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                                \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                    \
                                                                                                    \
                        if(size == 1) sum = _mm512_max_epi8(sum, reg1);                                                                            \
                        else if(size == 2) sum = _mm512_max_epi16(sum, reg1);                          \
                        else if(size == 4) sum = _mm512_max_epi32(sum, reg1);                          \
                        else sum = _mm512_max_epi64(sum, reg1);                                 \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                    sum = _mm512_shuffle_epi8(sum, mask);                                     \
                    sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[i];                                \
                                                                                                            \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                   \
        }                                                                                       \
    } while (0)


#define S_SUM_AR_24(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size, a_length)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
        a_length+=0;                                                                                  \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                          \
        if(size == 1){                                                                        \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                \
                                                                                                \
                    sum = _mm512_add_epi8(sum, reg1);                          \
                    sum = _mm512_add_epi8(sum, reg2);                          \
                    sum = _mm512_add_epi8(sum, reg3);                                  \
                    sum = _mm512_add_epi8(sum, reg4);                                  \
                    sum = _mm512_add_epi8(sum, reg5);                                  \
                    sum = _mm512_add_epi8(sum, reg6);                                  \
                    sum = _mm512_add_epi8(sum, reg7);                                  \
                    sum = _mm512_add_epi8(sum, reg8);                                  \
                                                                                           \
                }                                                                           \
                                                                                            \
                sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                            \
                for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                        \
                    void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                    void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                    void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                    void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                    void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                    void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                    void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                    void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                        \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                }                                                                                   \
            }                                                                                 \
        }                                                                                  \
        else{                                                                                  \
                                                                                                        \
            __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
            __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
            __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                            \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                    reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                    reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                    reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                    reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                    reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                    reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                    reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                    reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                    reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                    reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                    reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                    reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                    reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                    reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                    reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                    reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                    reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                    reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                    reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                    reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                    reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                    reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                    reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                    reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                                \
                                                                                                \
                    if(size == 2){                                                               \
                        sum = _mm512_add_epi16(sum, reg1);                          \
                        sum = _mm512_add_epi16(sum, reg2);                          \
                        sum = _mm512_add_epi16(sum, reg3);                                  \
                        sum = _mm512_add_epi16(sum, reg4);                                  \
                        sum = _mm512_add_epi16(sum, reg5);                                  \
                        sum = _mm512_add_epi16(sum, reg6);                                  \
                        sum = _mm512_add_epi16(sum, reg7);                                  \
                        sum = _mm512_add_epi16(sum, reg8);                                  \
                    }                                                                       \
                    else if(size == 4){                                                               \
                        sum = _mm512_add_epi32(sum, reg1);                          \
                        sum = _mm512_add_epi32(sum, reg2);                          \
                        sum = _mm512_add_epi32(sum, reg3);                                  \
                        sum = _mm512_add_epi32(sum, reg4);                                  \
                        sum = _mm512_add_epi32(sum, reg5);                                  \
                        sum = _mm512_add_epi32(sum, reg6);                                  \
                        sum = _mm512_add_epi32(sum, reg7);                                  \
                        sum = _mm512_add_epi32(sum, reg8);                                  \
                    }                                                                       \
                    else{                                                                    \
                        sum = _mm512_add_epi64(sum, reg1);                          \
                        sum = _mm512_add_epi64(sum, reg2);                          \
                        sum = _mm512_add_epi64(sum, reg3);                                  \
                        sum = _mm512_add_epi64(sum, reg4);                                  \
                        sum = _mm512_add_epi64(sum, reg5);                                  \
                        sum = _mm512_add_epi64(sum, reg6);                                  \
                        sum = _mm512_add_epi64(sum, reg7);                                  \
                        sum = _mm512_add_epi64(sum, reg8);                                  \
                    }                                                                       \
                }                                                                           \
                                                                                            \
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                            \
                for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                        \
                    void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                    void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                    void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                    void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                    void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                    void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                    void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                    void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                        \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                    \
                    sum = _mm512_rol_epi64(sum, 56);                                               \
                    _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                }                                                                                   \
            }                                                                                 \
        }                                                                                          \
    } while (0)


#define RNS_SUM_AR(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size, reduce_type)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                          \
        if(reduce_type == 0){                                                                   \
            if(size == 1){                                                                        \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                        void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                        void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                        void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                        void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                        void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                    \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                    \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                    \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                    \
                        reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                        reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                    \
                        reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                        reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                    \
                        reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                        reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                    \
                        reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                        reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                    \
                                                                                                    \
                        sum = _mm512_add_epi8(sum, reg1);                          \
                        sum = _mm512_add_epi8(sum, reg2);                          \
                        sum = _mm512_add_epi8(sum, reg3);                                  \
                        sum = _mm512_add_epi8(sum, reg4);                                  \
                        sum = _mm512_add_epi8(sum, reg5);                                  \
                        sum = _mm512_add_epi8(sum, reg6);                                  \
                        sum = _mm512_add_epi8(sum, reg7);                                  \
                        sum = _mm512_add_epi8(sum, reg8);                                  \
                                                                                            \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                        void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                        void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                        void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                        void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                        void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                  \
            else{                                                                                  \
                                                                                                            \
                __m512i mask = _mm512_set_epi64(\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL);\
                __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
                __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                                \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                        void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                        void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                        void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                        void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                        void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                    \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                        reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                        reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                        reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                                    \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                        reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                        reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                        reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                                    \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                        reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                        reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                        reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                                    \
                        reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                        reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                        reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                        reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                        reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                                    \
                        reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                        reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                        reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                        reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                        reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                                    \
                        reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                        reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                        reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                        reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                        reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                                    \
                        reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                        reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                        reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                        reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                        reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                                    \
                        if(size == 1){                                                               \
                            sum = _mm512_add_epi8(sum, reg1);                          \
                            sum = _mm512_add_epi8(sum, reg2);                          \
                            sum = _mm512_add_epi8(sum, reg3);                                  \
                            sum = _mm512_add_epi8(sum, reg4);                                  \
                            sum = _mm512_add_epi8(sum, reg5);                                  \
                            sum = _mm512_add_epi8(sum, reg6);                                  \
                            sum = _mm512_add_epi8(sum, reg7);                                  \
                            sum = _mm512_add_epi8(sum, reg8);                                  \
                        }                                                                               \
                        else if(size == 2){                                                               \
                            sum = _mm512_add_epi16(sum, reg1);                          \
                            sum = _mm512_add_epi16(sum, reg2);                          \
                            sum = _mm512_add_epi16(sum, reg3);                                  \
                            sum = _mm512_add_epi16(sum, reg4);                                  \
                            sum = _mm512_add_epi16(sum, reg5);                                  \
                            sum = _mm512_add_epi16(sum, reg6);                                  \
                            sum = _mm512_add_epi16(sum, reg7);                                  \
                            sum = _mm512_add_epi16(sum, reg8);                                  \
                        }                                                                       \
                        else if(size == 4){                                                               \
                            sum = _mm512_add_epi32(sum, reg1);                          \
                            sum = _mm512_add_epi32(sum, reg2);                          \
                            sum = _mm512_add_epi32(sum, reg3);                                  \
                            sum = _mm512_add_epi32(sum, reg4);                                  \
                            sum = _mm512_add_epi32(sum, reg5);                                  \
                            sum = _mm512_add_epi32(sum, reg6);                                  \
                            sum = _mm512_add_epi32(sum, reg7);                                  \
                            sum = _mm512_add_epi32(sum, reg8);                                  \
                        }                                                                       \
                        else{                                                                    \
                            sum = _mm512_add_epi64(sum, reg1);                          \
                            sum = _mm512_add_epi64(sum, reg2);                          \
                            sum = _mm512_add_epi64(sum, reg3);                                  \
                            sum = _mm512_add_epi64(sum, reg4);                                  \
                            sum = _mm512_add_epi64(sum, reg5);                                  \
                            sum = _mm512_add_epi64(sum, reg6);                                  \
                            sum = _mm512_add_epi64(sum, reg7);                                  \
                            sum = _mm512_add_epi64(sum, reg8);                                  \
                        }                                                                       \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                    sum = _mm512_shuffle_epi8(sum, mask);                                     \
                    sum = _mm512_permutexvar_epi32(perm, sum);                                \
                    sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                        void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                        void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                        void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                        void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                        void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                          \
        }                                                                                               \
        else if(reduce_type == 1){                                                                   \
            if(size == 0){                                                                        \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                        void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                        void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                        void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                        void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                        void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                    \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                    \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                    \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                    \
                        reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                        reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                    \
                        reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                        reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                    \
                        reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                        reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                    \
                        reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                        reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                    \
                                                                                                    \
                        sum = _mm512_or_epi64(sum, reg1);                          \
                        sum = _mm512_or_epi64(sum, reg2);                          \
                        sum = _mm512_or_epi64(sum, reg3);                                  \
                        sum = _mm512_or_epi64(sum, reg4);                                  \
                        sum = _mm512_or_epi64(sum, reg5);                                  \
                        sum = _mm512_or_epi64(sum, reg6);                                  \
                        sum = _mm512_or_epi64(sum, reg7);                                  \
                        sum = _mm512_or_epi64(sum, reg8);                                  \
                                                                                            \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                        void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                        void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                        void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                        void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                        void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                       \
            else if(size == 1){                                                                        \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                        void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                        void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                        void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                        void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                        void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                    \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                    \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                    \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                    \
                        reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                        reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                    \
                        reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                        reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                    \
                        reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                        reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                    \
                        reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                        reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                    \
                                                                                                    \
                        sum = _mm512_max_epi8(sum, reg1);                          \
                        sum = _mm512_max_epi8(sum, reg2);                          \
                        sum = _mm512_max_epi8(sum, reg3);                                  \
                        sum = _mm512_max_epi8(sum, reg4);                                  \
                        sum = _mm512_max_epi8(sum, reg5);                                  \
                        sum = _mm512_max_epi8(sum, reg6);                                  \
                        sum = _mm512_max_epi8(sum, reg7);                                  \
                        sum = _mm512_max_epi8(sum, reg8);                                  \
                                                                                            \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                        void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                        void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                        void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                        void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                        void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                  \
            else{                                                                                  \
                                                                                                            \
                __m512i mask = _mm512_set_epi64(\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL,\
                                                0x0f0b07030e0a0602ULL,\
                                                0x0d0905010c080400ULL);\
                __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
                __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                                \
                for (int cl = 0; cl < iter; cl++)                                                 \
                {                                                                                 \
                                                                                                    \
                    for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                        void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                        void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                        void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                        void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                        void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                        void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                                \
                                                                                                    \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                    \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                        reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                        reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                        reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                                    \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                        reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                        reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                        reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                                    \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                        reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                        reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                        reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                                    \
                        reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                        reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                        reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                        reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                        reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                                    \
                        reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                        reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                        reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                        reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                        reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                                    \
                        reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                        reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                        reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                        reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                        reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                                    \
                        reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                        reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                        reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                        reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                        reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                                    \
                        if(size == 1){                                                               \
                            sum = _mm512_max_epi8(sum, reg1);                          \
                            sum = _mm512_max_epi8(sum, reg2);                          \
                            sum = _mm512_max_epi8(sum, reg3);                                  \
                            sum = _mm512_max_epi8(sum, reg4);                                  \
                            sum = _mm512_max_epi8(sum, reg5);                                  \
                            sum = _mm512_max_epi8(sum, reg6);                                  \
                            sum = _mm512_max_epi8(sum, reg7);                                  \
                            sum = _mm512_max_epi8(sum, reg8);                                  \
                        }                                                                               \
                        else if(size == 2){                                                               \
                            sum = _mm512_max_epi16(sum, reg1);                          \
                            sum = _mm512_max_epi16(sum, reg2);                          \
                            sum = _mm512_max_epi16(sum, reg3);                                  \
                            sum = _mm512_max_epi16(sum, reg4);                                  \
                            sum = _mm512_max_epi16(sum, reg5);                                  \
                            sum = _mm512_max_epi16(sum, reg6);                                  \
                            sum = _mm512_max_epi16(sum, reg7);                                  \
                            sum = _mm512_max_epi16(sum, reg8);                                  \
                        }                                                                       \
                        else if(size == 4){                                                               \
                            sum = _mm512_max_epi32(sum, reg1);                          \
                            sum = _mm512_max_epi32(sum, reg2);                          \
                            sum = _mm512_max_epi32(sum, reg3);                                  \
                            sum = _mm512_max_epi32(sum, reg4);                                  \
                            sum = _mm512_max_epi32(sum, reg5);                                  \
                            sum = _mm512_max_epi32(sum, reg6);                                  \
                            sum = _mm512_max_epi32(sum, reg7);                                  \
                            sum = _mm512_max_epi32(sum, reg8);                                  \
                        }                                                                       \
                        else{                                                                    \
                            sum = _mm512_max_epi64(sum, reg1);                          \
                            sum = _mm512_max_epi64(sum, reg2);                          \
                            sum = _mm512_max_epi64(sum, reg3);                                  \
                            sum = _mm512_max_epi64(sum, reg4);                                  \
                            sum = _mm512_max_epi64(sum, reg5);                                  \
                            sum = _mm512_max_epi64(sum, reg6);                                  \
                            sum = _mm512_max_epi64(sum, reg7);                                  \
                            sum = _mm512_max_epi64(sum, reg8);                                  \
                        }                                                                       \
                    }                                                                           \
                                                                                                \
                    sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                    sum = _mm512_shuffle_epi8(sum, mask);                                     \
                    sum = _mm512_permutexvar_epi32(perm, sum);                                \
                    sum = _mm512_rol_epi64(sum, 8);                                               \
                                                                                                \
                    for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                            \
                        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[8*i];                                \
                        void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[8*i+1];                                \
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[8*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[8*i+3];                                \
                        void *dst_rank_clwise_addr13 = dst_rank_bgwise_addr[8*i+4];                                \
                        void *dst_rank_clwise_addr14 = dst_rank_bgwise_addr[8*i+5];                                \
                        void *dst_rank_clwise_addr15 = dst_rank_bgwise_addr[8*i+6];                                \
                        void *dst_rank_clwise_addr16 = dst_rank_bgwise_addr[8*i+7];                                \
                                                                                                            \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                                                                                                    \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr13), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr14), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr15), sum);                   \
                                                                                                        \
                        sum = _mm512_rol_epi64(sum, 56);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr16), sum);                   \
                    }                                                                                   \
                }                                                                                 \
            }                                                                                          \
        }                                                                                               \
    } while (0)

#define RNS_SUM_AR_24(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size, a_length)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
                                                                                        \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
        __m512i mask_ar = _mm512_set_epi64(                  /*mask의 맨 윗줄은 실제 data의 뒤를 담당*/          \
                                                        0x0e0f0c0d0a0b0809ULL,\
                                                        0x0607040502030001ULL,\
                                                        0x0e0f0c0d0a0b0809ULL,\
                                                        0x0607040502030001ULL,\
                                                        0x0e0f0c0d0a0b0809ULL,\
                                                        0x0607040502030001ULL,\
                                                        0x0e0f0c0d0a0b0809ULL,\
                                                        0x0607040502030001ULL); \
                                                                                          \
        if(size == 8){                                                                        \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[a_length*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[a_length*i+1];                                \
                                                                                                            \
                    if(a_length==4){\
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[a_length*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[a_length*i+3];                                \
                        \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi32(reg1, 0);                                           \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi32(reg2, 8);                                         \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi32(reg3, 16);                                        \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi32(reg4, 24);                                        \
                        sum = _mm512_add_epi8(sum, reg1);                          \
                        sum = _mm512_add_epi8(sum, reg2);                          \
                        sum = _mm512_add_epi8(sum, reg3);                          \
                        sum = _mm512_add_epi8(sum, reg4);                          \
                    }\
                    else if(a_length==2){\
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi64(reg1, 0); \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_shuffle_epi8(reg2, mask_ar); \
                        sum = _mm512_add_epi8(sum, reg1);\
                        sum = _mm512_add_epi8(sum, reg2);\
                    }\
                                                                                                \
                }                                                                           \
                                                                                            \
                /*sum = _mm512_rol_epi64(sum, 56);                                               */\
                                                                                            \
                for(uint32_t i=0; i<num_iter_src; i++){                                         \
                    void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[a_length*i];                                \
                    void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[a_length*i+1];                                \
                    if(a_length==4){\
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[a_length*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[a_length*i+3];                                \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                    }\
                    else if(a_length==2){\
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                        sum = _mm512_shuffle_epi8(sum, mask_ar); \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                    }\
                }                                                                                   \
            }                                                                                 \
        }                                                                                  \
        else{                                                                                  \
                                                                                                        \
            __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
            __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
            __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                            \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[a_length*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[a_length*i+1];                                \
                    if(a_length==4){\
                        void *src_rank_clwise_addr3 = src_rank_bgwise_addr[a_length*i+2];                                \
                        void *src_rank_clwise_addr4 = src_rank_bgwise_addr[a_length*i+3];                                \
                        \
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi32(reg1, 0);                                           \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                        \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_rol_epi32(reg2, 8);                                         \
                        reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                        reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                        reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                        \
                        reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                        reg3 = _mm512_rol_epi32(reg3, 16);                                        \
                        reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                        reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                        reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                        \
                        reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                        reg4 = _mm512_rol_epi32(reg4, 24);                                        \
                        reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                        reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                        reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                    }                                                                                                \
                    else if(a_length==2){\
                        reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                        reg1 = _mm512_rol_epi32(reg1, 0); \
                        reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                        reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                        reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                        \
                        reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                        reg2 = _mm512_shuffle_epi8(reg2, mask_ar); \
                        reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                        reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                        reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                    }\
                    \
                    if(size == 16){                                                               \
                        sum = _mm512_add_epi16(sum, reg1);                          \
                        sum = _mm512_add_epi16(sum, reg2);                          \
                        if(a_length==4){\
                            sum = _mm512_add_epi16(sum, reg3);                                  \
                            sum = _mm512_add_epi16(sum, reg4);                                  \
                        }\
                    }                                                                       \
                    else if(size == 32){                                                               \
                        sum = _mm512_add_epi32(sum, reg1);                          \
                        sum = _mm512_add_epi32(sum, reg2);                          \
                        if(a_length==4){\
                            sum = _mm512_add_epi32(sum, reg3);                                  \
                            sum = _mm512_add_epi32(sum, reg4);                                  \
                        }\
                    }                                                                       \
                    else{                                                                    \
                        sum = _mm512_add_epi64(sum, reg1);                          \
                        sum = _mm512_add_epi64(sum, reg2);                          \
                        if(a_length==4){\
                            sum = _mm512_add_epi64(sum, reg3);                                  \
                            sum = _mm512_add_epi64(sum, reg4);                                  \
                        }\
                    }                                                                       \
                }                                                                           \
                                                                                            \
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                /*sum = _mm512_rol_epi64(sum, 56);                                               */\
                                                                                            \
                for(uint32_t i=0; i<num_iter_src; i++){                                         \
                                                                                                        \
                    void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr[a_length*i];                                \
                    void *dst_rank_clwise_addr10 = dst_rank_bgwise_addr[a_length*i+1];                                \
                    if(a_length==4){\
                        void *dst_rank_clwise_addr11 = dst_rank_bgwise_addr[a_length*i+2];                                \
                        void *dst_rank_clwise_addr12 = dst_rank_bgwise_addr[a_length*i+3];                                \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr11), sum);                   \
                        sum = _mm512_rol_epi32(sum, 8);                                               \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr12), sum);                   \
                    }\
                    else if(a_length==2){ \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   \
                        sum = _mm512_shuffle_epi8(sum, mask_ar); \
                        _mm512_stream_si512((void *)(dst_rank_clwise_addr10), sum);                   \
                    }\
                }                                                                                   \
            }                                                                                 \
        }                                                                                          \
    } while (0)








#define GATHER_S_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i mask = _mm512_set_epi64(\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
            __mmask8 mask_all = 0xFF;                                                                                \
            for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                            \
                reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                \
                reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9+i, mask_all, reg1); \
                                                                                            \
                                                                                            \
                /*sum = _mm512_add_epi64(sum, reg1);                          */\
            }                                                                           \
                                                                                          \
            /*sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
            sum = _mm512_shuffle_epi8(sum, mask);                                     \
            sum = _mm512_permutexvar_epi32(perm, sum);                                */\
                                                                                          \
            /*_mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
            /*__mmask8 mask = 0xFF; \
            _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask, sum); */\
        }                                                                                 \
    } while (0)


#define REDUCE_RNS_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                          \
        __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                          \
        if(size == 8){                                                                        \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                                                                                                \
                                                                                                \
                    sum = _mm512_add_epi8(sum, reg1);                          \
                    sum = _mm512_add_epi8(sum, reg2);                          \
                    sum = _mm512_add_epi8(sum, reg3);                                  \
                    sum = _mm512_add_epi8(sum, reg4);                                  \
                    sum = _mm512_add_epi8(sum, reg5);                                  \
                    sum = _mm512_add_epi8(sum, reg6);                                  \
                    sum = _mm512_add_epi8(sum, reg7);                                  \
                    sum = _mm512_add_epi8(sum, reg8);                                  \
                                                                                        \
                }                                                                           \
                                                                                            \
                /*_mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum); \
                __mmask8 mask_all = 0xFF; \
                _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
            }                                                                                 \
        }                                                                                   \
        else{                                                                                  \
                                                                                            \
            \
                                                                                            \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                    void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                    void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                    void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                    void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                    void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                    void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                    void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                    reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                    reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                    reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                \
                    reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                    reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                    reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                    reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                    reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                                \
                    reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                    reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                    reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                    reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                    reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                                \
                    reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                    reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                    reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                    reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                    reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                                \
                    reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                    reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                    reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                    reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                    reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                                \
                    reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                    reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                    reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                    reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                    reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                                \
                    reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                    reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                    reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                    reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                    reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                                \
                    reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                    reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                    reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                    reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                    reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                                \
                                                                                                \
                    if(size == 16){                                                               \
                        sum = _mm512_add_epi16(sum, reg1);                          \
                        sum = _mm512_add_epi16(sum, reg2);                          \
                        sum = _mm512_add_epi16(sum, reg3);                                  \
                        sum = _mm512_add_epi16(sum, reg4);                                  \
                        sum = _mm512_add_epi16(sum, reg5);                                  \
                        sum = _mm512_add_epi16(sum, reg6);                                  \
                        sum = _mm512_add_epi16(sum, reg7);                                  \
                        sum = _mm512_add_epi16(sum, reg8);                                  \
                    }                                                                       \
                    else if(size == 32){                                                               \
                        sum = _mm512_add_epi32(sum, reg1);                          \
                        sum = _mm512_add_epi32(sum, reg2);                          \
                        sum = _mm512_add_epi32(sum, reg3);                                  \
                        sum = _mm512_add_epi32(sum, reg4);                                  \
                        sum = _mm512_add_epi32(sum, reg5);                                  \
                        sum = _mm512_add_epi32(sum, reg6);                                  \
                        sum = _mm512_add_epi32(sum, reg7);                                  \
                        sum = _mm512_add_epi32(sum, reg8);                                  \
                    }                                                                       \
                    else{                                                                    \
                        sum = _mm512_add_epi64(sum, reg1);                          \
                        sum = _mm512_add_epi64(sum, reg2);                          \
                        sum = _mm512_add_epi64(sum, reg3);                                  \
                        sum = _mm512_add_epi64(sum, reg4);                                  \
                        sum = _mm512_add_epi64(sum, reg5);                                  \
                        sum = _mm512_add_epi64(sum, reg6);                                  \
                        sum = _mm512_add_epi64(sum, reg7);                                  \
                        sum = _mm512_add_epi64(sum, reg8);                                  \
                    }                                                                       \
                                                                                        \
                }                                                                           \
                                                                                            \
                /*sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
                __mmask8 mask_all = 0xFF; \
                _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
            }                                                                                 \
        }                                                                                       \
    } while (0)


#define PREV_REDUCE_RNS_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
        __m512i reg2;                                                                     \
        __m512i reg3;                                                                     \
        __m512i reg4;                                                                     \
        __m512i reg5;                                                                     \
        __m512i reg6;                                                                     \
        __m512i reg7;                                                                     \
        __m512i reg8;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
        __m512i mask = _mm512_set_epi64(\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                          \
                                                                                          \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
                                                                                            \
            for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                void *src_rank_clwise_addr1 = src_rank_bgwise_addr[8*i];                                \
                void *src_rank_clwise_addr2 = src_rank_bgwise_addr[8*i+1];                                \
                void *src_rank_clwise_addr3 = src_rank_bgwise_addr[8*i+2];                                \
                void *src_rank_clwise_addr4 = src_rank_bgwise_addr[8*i+3];                                \
                void *src_rank_clwise_addr5 = src_rank_bgwise_addr[8*i+4];                                \
                void *src_rank_clwise_addr6 = src_rank_bgwise_addr[8*i+5];                                \
                void *src_rank_clwise_addr7 = src_rank_bgwise_addr[8*i+6];                                \
                void *src_rank_clwise_addr8 = src_rank_bgwise_addr[8*i+7];                                \
                                                                                            \
                /*uint8_t a[64]; if(i==0){_mm512_store_si512((void *)a, reg1); printf("%d, %d, %d, %d\n", a[0], a[1], a[2], a[3]);} */\
                reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                reg1 = _mm512_rol_epi64(reg1, 0);                                           \
                reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                            \
                reg2 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr2));             \
                reg2 = _mm512_rol_epi64(reg2, 8);                                         \
                /*uint8_t b[64]; if(i==0){_mm512_store_si512((void *)b, reg2); printf("%d, %d, %d, %d\n", b[0], b[1], b[2], b[3]);} */\
                reg2 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg2, 1);                  \
                reg2 = _mm512_shuffle_epi8(reg2, mask);                                     \
                reg2 = _mm512_permutexvar_epi32(perm, reg2);                                \
                                                                                            \
                reg3 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr3));             \
                reg3 = _mm512_rol_epi64(reg3, 16);                                        \
                reg3 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg3, 1);                  \
                reg3 = _mm512_shuffle_epi8(reg3, mask);                                     \
                reg3 = _mm512_permutexvar_epi32(perm, reg3);                                \
                                                                                            \
                reg4 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr4));             \
                reg4 = _mm512_rol_epi64(reg4, 24);                                        \
                reg4 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg4, 1);                  \
                reg4 = _mm512_shuffle_epi8(reg4, mask);                                     \
                reg4 = _mm512_permutexvar_epi32(perm, reg4);                                \
                                                                                            \
                reg5 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr5));             \
                reg5 = _mm512_rol_epi64(reg5, 32);                                        \
                reg5 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg5, 1);                  \
                reg5 = _mm512_shuffle_epi8(reg5, mask);                                     \
                reg5 = _mm512_permutexvar_epi32(perm, reg5);                                \
                                                                                            \
                reg6 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr6));             \
                reg6 = _mm512_rol_epi64(reg6, 40);                                        \
                reg6 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg6, 1);                  \
                reg6 = _mm512_shuffle_epi8(reg6, mask);                                     \
                reg6 = _mm512_permutexvar_epi32(perm, reg6);                                \
                                                                                            \
                reg7 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr7));             \
                reg7 = _mm512_rol_epi64(reg7, 48);                                        \
                reg7 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg7, 1);                  \
                reg7 = _mm512_shuffle_epi8(reg7, mask);                                     \
                reg7 = _mm512_permutexvar_epi32(perm, reg7);                                \
                                                                                            \
                reg8 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr8));             \
                reg8 = _mm512_rol_epi64(reg8, 56);                                        \
                reg8 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg8, 1);                  \
                reg8 = _mm512_shuffle_epi8(reg8, mask);                                     \
                reg8 = _mm512_permutexvar_epi32(perm, reg8);                                \
                                                                                            \
                                                                                            \
                sum = _mm512_add_epi64(sum, reg1);                          \
                sum = _mm512_add_epi64(sum, reg2);                          \
                sum = _mm512_add_epi64(sum, reg3);                                  \
                sum = _mm512_add_epi64(sum, reg4);                                  \
                sum = _mm512_add_epi64(sum, reg5);                                  \
                sum = _mm512_add_epi64(sum, reg6);                                  \
                sum = _mm512_add_epi64(sum, reg7);                                  \
                sum = _mm512_add_epi64(sum, reg8);                                  \
                                                                                    \
            }                                                                           \
                                                                                          \
            /*sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
            sum = _mm512_shuffle_epi8(sum, mask);                                     \
            sum = _mm512_permutexvar_epi32(perm, sum);                                */\
                                                                                          \
            /*_mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
            /*_mm512_store_epi64((void *)((__m512i *)dst_rank_clwise_addr9), sum); */\
            __mmask8 mask_all = 0xFF; \
            _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
        }                                                                                 \
    } while (0)



#define REDUCE_S_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src, size)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
        __m512i sum = _mm512_set_epi64(                                                    \
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
                                                                                        \
        __m512i mask = _mm512_set_epi64(\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL,\
                                            0x0f0b07030e0a0602ULL,\
                                            0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                          \
                                                                                          \
        if(size == 8){                                                                      \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    sum = _mm512_add_epi8(sum, reg1);                          \
                }                                                                           \
                                                                                            \
                /*_mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
                sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum); \
                __mmask8 mask_all = 0xFF; \
                _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
            }                                                                                 \
        }                                                                                  \
        else{                                                                               \
                                                                                            \
            \
                                                                                                \
            for (int cl = 0; cl < iter; cl++)                                                 \
            {                                                                                 \
                                                                                                \
                for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                    void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                                \
                    reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                    reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                    reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                    reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                                \
                                                                                                \
                    if(size == 16) sum = _mm512_add_epi16(sum, reg1);                          \
                    else if(size == 32) sum = _mm512_add_epi32(sum, reg1);                          \
                    else sum = _mm512_add_epi64(sum, reg1);                          \
                }                                                                           \
                                                                                            \
                /*sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
                sum = _mm512_shuffle_epi8(sum, mask);                                     \
                sum = _mm512_permutexvar_epi32(perm, sum);                                \
                                                                                            \
                _mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
                __mmask8 mask_all = 0xFF; \
                _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
            }                                                                                 \
        }                                                                                       \
    } while (0)


#define PREV_REDUCE_S_SUM_RS(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr, num_iter_src)    \
    do                                                                                    \
    {                                                                                    \
                                                                                          \
                                                                                          \
        __m512i reg1;                                                                     \
                                                                                        \
        void *dst_rank_clwise_addr9 = dst_rank_bgwise_addr;                               \
                                                                                          \
                                                                                        \
        __m512i sum = _mm512_set_epi64(\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL,\
                                        0x0000000000000000ULL);\
        __m512i mask = _mm512_set_epi64(\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
                                                                                          \
                                                                                          \
                                                                                          \
        for (int cl = 0; cl < iter; cl++)                                                 \
        {                                                                                 \
                                                                                            \
            for(uint32_t i=0; i<num_iter_src; i++){                                                  \
                void *src_rank_clwise_addr1 = src_rank_bgwise_addr[i];                                \
                                                                                            \
                reg1 = _mm512_stream_load_si512((void *)(src_rank_clwise_addr1));             \
                reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
                reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
                reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
                                                                                            \
                                                                                            \
                sum = _mm512_add_epi64(sum, reg1);                          \
            }                                                                           \
                                                                                          \
            /*sum = _mm512_i32gather_epi32(vindex, (int64_t*)&sum, 1);                  \
            sum = _mm512_shuffle_epi8(sum, mask);                                     \
            sum = _mm512_permutexvar_epi32(perm, sum);                                */\
                                                                                          \
            /*_mm512_stream_si512((void *)(dst_rank_clwise_addr9), sum);                   */\
            __mmask8 mask_all = 0xFF; \
            _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr9, mask_all, sum); \
        }                                                                                 \
    } while (0)



#define GATHER_COPY(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)    \
    do                                                                                    \
    {   /*printf("\nRNS_COPY works~!~!~!! **********\n");*/                                                                                  \
        __m512i mask = _mm512_set_epi64(\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
        uint64_t *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        void *dst_rank_clwise_addr = dst_rank_bgwise_addr; \
        __m512i reg1;                                                                     \
        /*__m512i reg1_rot;*/                                                                 \
        for(uint32_t i=0; i<1; i++){ \
            reg1 = _mm512_loadu_si512((void *)(src_rank_clwise_addr));                  \
            \
            /*uint8_t b[64]; if(i==0){_mm512_store_si512((void *)b, reg1); printf("%d, %d, %d, %d\n", b[0], b[1], b[2], b[3]);} */\
            reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
            reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
            reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
            \
            __mmask8 mask_all = 0xFF; \
            _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr, mask_all, reg1); \
            /*_mm512_store_si512((void *)(dst_rank_clwise_addr), reg1);               */\
        } \
    } while (0)


#define SCATTER_COPY(iter, src_rank_bgwise_addr, dst_rank_bgwise_addr)    \
    do                                                                                    \
    {                                                                            \
        __m512i mask = _mm512_set_epi64(\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL,\
                                        0x0f0b07030e0a0602ULL,\
                                        0x0d0905010c080400ULL);\
        __m512i vindex = _mm512_setr_epi32(0, 8, 16, 24, 32, 40, 48, 56, 4, 12, 20, 28, 36, 44, 52, 60);\
        __m512i perm = _mm512_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15);\
        void *src_rank_clwise_addr = src_rank_bgwise_addr;                                \
        uint64_t *dst_rank_clwise_addr = dst_rank_bgwise_addr; \
        __m512i reg1;                                                                     \
        for(uint32_t i=0; i<1; i++){ \
            reg1 = _mm512_loadu_si512((void *)(src_rank_clwise_addr));                  \
            \
            reg1 = _mm512_i32gather_epi32(vindex, (int64_t*)&reg1, 1);                  \
            reg1 = _mm512_shuffle_epi8(reg1, mask);                                     \
            reg1 = _mm512_permutexvar_epi32(perm, reg1);                                \
            \
            /*__mmask8 mask_all = 0xFF; \
            _mm512_mask_storeu_epi64((__m512i*)dst_rank_clwise_addr, mask_all, reg1); */\
            _mm512_stream_si512((void *)(dst_rank_clwise_addr), reg1);               \
        } \
    } while (0)



void xeon_sp_trans_all_to_all_8(void *base_region_addr_src, uint32_t start_offset, uint32_t byte_length){
    printf("xeon_sp_trans_all_to_all works~~~~~~!! *****\n");
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr = base_region_addr_src; //일단 한 rank니까~!
    src_rank_base_addr+=0;
    dst_rank_base_addr+=0;
    byte_length+=0; //여기까지는 잘됨~!
    // 8KB offset will be inside 1MB address space - Comment
    //몇 바이트인지 체크할것~! ppt보고
    printf("success~! a2a 8\n");

    //do address translation.
    //int num_packets = 1;
    int packet_size = 8;
    //int offset_gran = packet_size * 64;
    int iteration = packet_size / 8;
    int src_off_gran = CACHE_LINE2_a2a * iteration;
    int dst_off_gran = src_off_gran * 2;
    int src_bg_offset = (packet_size * 1024) / (4);

    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    uint32_t src_mram_offset = start_offset + 1024*1024 -64;
    uint32_t dst_mram_offset = start_offset + 1024*1024 -64;
    mram_src_offset_1mb_wise+=0; mram_dst_offset_1mb_wise+=0; src_mram_offset+=0; dst_mram_offset+=0;

    printf("start offset: %d\n", start_offset);
    src_bg_offset+=0;
    dst_off_gran+=0;

    uint32_t iter_length = (62*1024*1024/64); // byte_length/8 //62~!~!
    iter_length+=0;
    uint32_t rns_chip_id=0;
    rns_chip_id+=0;
    uint32_t u_byte_length = byte_length;
    u_byte_length+=0; //없어도 되는 변수일지도?
    for(uint32_t i=0; i<iter_length; i++){ //0->1, 0->2보니까 제대로 동작은 한다~!
        //일단 살려서 64bytesT씩~!
        src_mram_offset+=64;
        dst_mram_offset+=64;
        rns_chip_id = i/(iter_length/8); // u_byte_length/ i;


        uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
        //uint32_t mram_64_bit_word_offset = address_offset_change(src_mram_offset);
        uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
        //mram_64_bit_word_offset = address_offset_change(dst_mram_offset);
        next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        //printf("print offset : %d, %d, %ld, %ld, %d, %ld, %ld\n", src_mram_offset, mram_64_bit_word_offset, next_data, mram_src_offset_1mb_wise, mram_64_bit_word_offset, next_data, mram_dst_offset_1mb_wise);
        
        void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
        void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        src_rank_addr+=0; //unused case
        dst_rank_addr+=0; //unused case
        //printf("src_rank_addr=%p, src_rank_base_addr=%p, mram_src_offset_1mb_wise=%ld\n", src_rank_addr, src_rank_base_addr, mram_src_offset_1mb_wise);

        //flush before RnS
        /*void *src_rank_addr_before_rns = src_rank_addr;
        src_rank_addr_before_rns+=0;
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //0 //여기서부터 segmentation fault가 뜬다~!
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //1
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //2
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //3
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //4
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //5
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //6
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        src_rank_addr_before_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_before_rns); //7
        COMM_FLUSH(iteration, src_rank_addr_before_rns);
        //src_rank_addr_before_rns += (src_off_gran);
        */
        //RnS
        void *src_rank_addr_iter = src_rank_addr;
        if(rns_chip_id==0){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }    
        }
        else if(rns_chip_id==1){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==2){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==3){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==4){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==5){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==6){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        else if(rns_chip_id==7){
            for(int iter_8=0; iter_8<8; iter_8++){
                RNS_COPY_a2a(7, 56, iteration, src_rank_addr_iter, src_rank_addr_iter);        
                src_rank_addr_iter += (src_off_gran);
            }
        }
        _mm_mfence();

        //flush src
        /*void *src_rank_addr_after_rns = src_rank_addr;
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //0
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //1
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //2
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //3
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //4
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //5
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //6
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        src_rank_addr_after_rns += (src_off_gran);
        //RNS_FLUSH_SRC(iteration, src_rank_addr_after_rns); //7
        COMM_FLUSH(iteration, src_rank_addr_after_rns);
        //src_rank_addr_after_rns += (src_off_gran);
        */
       
        //flush dst
        //RNS_FLUSH_SRC(iteration, dst_rank_addr);
    }

    printf("alltoall 8 end!\n");
    return;
}



typedef struct {
    void *p_src_rank_base_addr;
    void *p_dst_rank_base_addr;
    uint32_t p_byte_length;
    int p_iteration;
    int p_src_off_gran;
    int64_t p_mram_src_offset_1mb_wise;
    int64_t p_mram_dst_offset_1mb_wise;
    uint32_t p_src_mram_offset;
    uint32_t p_dst_mram_offset;
    uint32_t p_iter_length;
    uint32_t p_rns_chip_id;
    uint32_t p_src_rotate_group;
    uint32_t p_dst_rotate_group;
}st_thread_alltoall64_parameter_a2a;

void *thread_alltoall64(void *thread_alltoall64_parameter){
    st_thread_alltoall64_parameter_a2a *each_thread_alltoall64_parameter = (st_thread_alltoall64_parameter_a2a *)thread_alltoall64_parameter;
    void *src_rank_base_addr= each_thread_alltoall64_parameter->p_src_rank_base_addr;
    void *dst_rank_base_addr= each_thread_alltoall64_parameter->p_dst_rank_base_addr;
    uint32_t byte_length= each_thread_alltoall64_parameter->p_byte_length;
    int iteration= each_thread_alltoall64_parameter->p_iteration;
    int src_off_gran= each_thread_alltoall64_parameter->p_src_off_gran;
    int64_t mram_src_offset_1mb_wise= each_thread_alltoall64_parameter->p_mram_src_offset_1mb_wise;
    int64_t mram_dst_offset_1mb_wise= each_thread_alltoall64_parameter->p_mram_dst_offset_1mb_wise;
    uint32_t src_mram_offset = each_thread_alltoall64_parameter->p_src_mram_offset;
    uint32_t dst_mram_offset = each_thread_alltoall64_parameter->p_dst_mram_offset;
    uint32_t iter_length = each_thread_alltoall64_parameter->p_iter_length;
    uint32_t rns_chip_id;
    uint32_t src_rotate_group= each_thread_alltoall64_parameter->p_src_rotate_group;
    ////////////////////
    uint32_t my_dst_rotate_group= each_thread_alltoall64_parameter->p_dst_rotate_group;

    void *src_rank_base_addr_rotate_group = src_rank_base_addr; 
    //for(uint32_t dst_rotate_group=0; dst_rotate_group<8; dst_rotate_group++){
    for(uint32_t dst_rotate_group=4*my_dst_rotate_group; dst_rotate_group<(4*my_dst_rotate_group+4); dst_rotate_group++){
        void *dst_rank_base_addr_rotate_group = dst_rank_base_addr;

        uint32_t src_mram_offset_rotate_group = src_mram_offset  +dst_rotate_group*8*byte_length; //이부분 틀렸던건 확실~!~!~! dst도 마찬가지
        uint32_t dst_mram_offset_rotate_group = dst_mram_offset  +src_rotate_group*8*byte_length; //dst?
        uint32_t src_rotate_group_offset_256_64= (src_rotate_group%4) * (256*1024) + (src_rotate_group/4) * 64;
        uint32_t dst_rotate_group_offset_256_64= (dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64;

        for(uint32_t i=0; i<iter_length; i++){ //0->1, 0->2보니까 제대로 동작은 한다~! 64에서는 아닌듯?

            rns_chip_id = i/(iter_length/8); //원래는 0~63이지만 내 구현상 8로 나눈 나머지인 0~7~!

            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset_rotate_group);
            //uint32_t mram_64_bit_word_offset = address_offset_change(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset_rotate_group);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

            void *src_rank_addr = src_rank_base_addr_rotate_group + mram_src_offset_1mb_wise;
            void *dst_rank_addr = dst_rank_base_addr_rotate_group + mram_dst_offset_1mb_wise;
            src_rank_addr+=0; //unused case
            dst_rank_addr+=0; //unused case
            //flush before RnS

            
            //RnS
            void *src_rank_addr_iter = src_rank_addr   + src_rotate_group_offset_256_64; //(src_rotate_group%4) * (256*1024) + (src_rotate_group/4) * 64;
            void *dst_rank_addr_iter = dst_rank_addr   + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64;
            if(rns_chip_id==0){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
                //break;
            }
            else if(rns_chip_id==1){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);        
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
            }
            else if(rns_chip_id==2){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);        
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
            }
            else if(rns_chip_id==3){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);       
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
            }
            else if(rns_chip_id==4){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);      
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
            }
            else if(rns_chip_id==5){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);   
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
            }
            else if(rns_chip_id==6){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);       
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
                //break;
            }
            else if(rns_chip_id==7){
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_a2a(7, 56, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                    //RNS_COPY_ORIGINAL(7, 56, iteration, src_rank_addr_iter, dst_rank_addr_iter);       
                    src_rank_addr_iter += (src_off_gran);
                    dst_rank_addr_iter += (src_off_gran);
                }
                //break;
            }

            //flush src
            
        
            //flush dst
            
            
            //일단 살려서 64bytes씩~!
            src_mram_offset_rotate_group+=64; //이러면 2중 for문 안에서 처리하는 data가 64*iter_length=8*byte_length이며 이는 8chip구현과 같다~!
            dst_mram_offset_rotate_group+=64;
            //break;
        }
    }
    return 0;
}

void xeon_sp_trans_all_to_all_64(void *base_region_addr_src, uint32_t start_offset, uint32_t byte_length){
    printf("xeon_sp_trans_all_to_all works~~~~~~!! *****\n");
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr = base_region_addr_src; //일단 한 rank니까~!
    src_rank_base_addr+=0;
    dst_rank_base_addr+=0;
    byte_length+=0; //여기까지는 잘됨~!
    // 8KB offset will be inside 1MB address space - Comment
    //몇 바이트인지 체크할것~! ppt보고

    //do address translation.
    //int num_packets = 1;
    int packet_size = 8;
    //int offset_gran = packet_size * 64;
    int iteration = packet_size / 8;
    int src_off_gran = CACHE_LINE2_a2a * iteration;
    int dst_off_gran = src_off_gran * 2;
    int src_bg_offset = (packet_size * 1024) / (4);

    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    uint32_t src_mram_offset = start_offset + 1024*1024;
    uint32_t dst_mram_offset = start_offset + 1024*1024;
    mram_src_offset_1mb_wise+=0; mram_dst_offset_1mb_wise+=0; src_mram_offset+=0; dst_mram_offset+=0;

    //printf("start offset: %d\n", start_offset);
    printf("success~! a2a 64\n");
    src_bg_offset+=0;
    dst_off_gran+=0;

    uint32_t iter_length = (62*1024*1024/64); // byte_length/8 //62~!~!

    iter_length = (uint32_t) byte_length/8; //지금은 8byte씩 할거니까~!
    iter_length = 31*1024*1024/64/8;
    iter_length+=0;
    uint32_t rns_chip_id=0;
    rns_chip_id+=0;
    src_mram_offset += 1024*1024  /2*63; //31.5mb에 data있으므로. 경계를 32mb로 바꿈~! 왜햐나면 8192bytes씩 나눠지니까.

    //for bandwidth check~! maximum:16GB/s <-- 여기에 dpu1,2와 address calculation이 들어간다~!(왔다갔다 하니까 *2)
    /*uint32_t c_src_mram_offset_rotate_group=src_mram_offset;
    uint32_t c_dst_mram_offset_rotate_group=dst_mram_offset;
    uint32_t c_mram_64_bit_word_offset = apply_address_translation_on_mram_offset(c_src_mram_offset_rotate_group);
    uint64_t c_next_data = BANK_OFFSET_NEXT_DATA(c_mram_64_bit_word_offset);
    mram_src_offset_1mb_wise = (c_next_data % BANK_CHUNK_SIZE) + (c_next_data / BANK_CHUNK_SIZE) * BANK_NEXT_CHUNK_OFFSET;
    c_mram_64_bit_word_offset = apply_address_translation_on_mram_offset(c_dst_mram_offset_rotate_group);
    c_next_data = BANK_OFFSET_NEXT_DATA(c_mram_64_bit_word_offset);
    mram_dst_offset_1mb_wise = (c_next_data % BANK_CHUNK_SIZE) + (c_next_data / BANK_CHUNK_SIZE) * BANK_NEXT_CHUNK_OFFSET;
    void *c_src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
    void *c_dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
    c_src_rank_addr+=0; c_dst_rank_addr+=0;
    for(uint32_t i=0; i<8*8*iter_length *8; i++){
        RNS_COPY(1, 8, iteration, c_src_rank_addr, c_dst_rank_addr);
    }
    for(uint32_t i=0; i<64; i++){ //이건 좀 이상한듯?
        void *c2_src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
        void *c2_dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        for(uint32_t j=0; j<iter_length; j++){
            c2_src_rank_addr+=8;
            c2_dst_rank_addr+=8;
        }
    }*/
    
    //#thread=8 //얘도 결과 보장 X
    /*st_thread_alltoall64_parameter thread_params={base_region_addr_src, base_region_addr_src, byte_length, 1, 128, 0, 0, \
    (start_offset + 1024*1024 + 1024*1024  /2*63), (start_offset + 1024*1024), byte_length/8, 0, 0};
    int status;
    pthread_t array_thread[8];
    for(uint32_t i=0; i<8; i++){
        thread_params.p_src_rotate_group=i;
        pthread_create(&array_thread[i], NULL, thread_alltoall64, (void *) &thread_params);
    }
    for(uint32_t i=0; i<8; i++){
        pthread_join(array_thread[i], (void **)&status);
    }*/

    //#thread=16
    st_thread_alltoall64_parameter_a2a thread_params[16];
    /*st_thread_alltoall64_parameter thread_params={base_region_addr_src, base_region_addr_src, byte_length, 1, 128, 0, 0, \
    (start_offset + 1024*1024 + 1024*1024  /2*63), (start_offset + 1024*1024), byte_length/8, 0, 0, 0};*/
    int status;
    pthread_t array_thread[16];
    for(uint32_t i=0; i<16; i++){
        thread_params[i].p_src_rank_base_addr=base_region_addr_src;
        thread_params[i].p_dst_rank_base_addr=base_region_addr_src;
        thread_params[i].p_byte_length=byte_length;
        thread_params[i].p_iteration=1;
        thread_params[i].p_src_off_gran=128;
        thread_params[i].p_mram_src_offset_1mb_wise=0;
        thread_params[i].p_mram_dst_offset_1mb_wise=0;
        thread_params[i].p_src_mram_offset= start_offset + 1024*1024 + 1024*1024/2*63;
        thread_params[i].p_dst_mram_offset= start_offset + 1024*1024;
        thread_params[i].p_iter_length=byte_length/8;
        thread_params[i].p_rns_chip_id=0;
        thread_params[i].p_src_rotate_group=i/2;
        thread_params[i].p_dst_rotate_group=i%2;
        pthread_create(&array_thread[i], NULL, thread_alltoall64, (void *) &thread_params[i]);
        //pthread_create(&array_thread[i], NULL, thread_alltoall64, (void *) &thread_params);
    }
    for(uint32_t i=0; i<16; i++){
        pthread_join(array_thread[i], (void **)&status);
    }

    _mm_mfence();

    printf("alltoall 64 end!!!!!! hihi~\n");
    return;
}

void xeon_sp_trans_all_to_all_rg(void *base_region_addr_src, void *base_region_addr_dst, uint32_t src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    //src/dst_start_offset이 8192n-64 넘어서 존재하면 오류인 코드이다~!
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;

    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0){
        iter_length=byte_length/64; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
        remain_length = (byte_length%64 + 7)/8;
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
    }

    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //because of bank structure~!
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;

    if(alltoall_comm_type==0){
        for(uint32_t rns_chip_id=0; rns_chip_id<8; rns_chip_id++){
            for(uint32_t i=0; i<iter_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

                //flush before RnS
                /*void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                    src_rank_addr_before_rns += (src_off_gran);
                }*/
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                if(rns_chip_id==0){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==1){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==2){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==3){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==4){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==5){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==6){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==7){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(7, 56, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }

                //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
                void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                    src_rank_addr_after_rns += (src_off_gran);
                }
            
                //flush dst
                void *dst_rank_addr_after_rns = dst_rank_addr+ dst_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                    dst_rank_addr_after_rns += (src_off_gran);
                }
                
                //일단 살려서 64bytes씩~!
                src_mram_offset+=64;
                dst_mram_offset+=64;
            }
            for(uint32_t i=0; i<remain_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

                //flush before RnS
                /*void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                */
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                if(rns_chip_id==0){
                    RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==1){
                    RNS_COPY_a2a(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==2){
                    RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==3){
                    RNS_COPY_a2a(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==4){
                    RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==5){
                    RNS_COPY_a2a(5, 40, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==6){
                    RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==7){
                    RNS_COPY_a2a(7, 54, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }

                //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
                /*void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                
                //flush dst
                void *dst_rank_addr_after_rns = dst_rank_addr+ dst_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                */
                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    else{ //alltoall_comm_type==1
        for(uint32_t i=0; i<iter_length; i++){
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

            void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

            //flush before RnS
            /*void *src_rank_addr_before_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //얘만 해도 140msec걸린다~!
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }*/
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!
            for(int iter_8=0; iter_8<8; iter_8++){
                S_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                src_rank_addr_iter += (src_off_gran);
                dst_rank_addr_iter += (src_off_gran);
            }
            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            /*void *src_rank_addr_after_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            //flush dst
            void *dst_rank_addr_after_rns = dst_rank_addr+ (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, dst_rank_addr_after_rns);
                dst_rank_addr_after_rns += (src_off_gran);
            }*/
            
            //일단 살려서 64bytes씩~!
            src_mram_offset+=64;
            dst_mram_offset+=64;
        }
        for(uint32_t i=0; i<remain_length; i++){
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

            void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;

            //flush before RnS
            /*void *src_rank_addr_before_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //얘만 해도 140msec걸린다~!
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }*/
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!
            S_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            /*void *src_rank_addr_after_rns = src_rank_addr+ (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            //flush dst
            void *dst_rank_addr_after_rns = dst_rank_addr+ (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
            for(int j=0; j<8; j++){
                COMM_FLUSH(iteration, dst_rank_addr_after_rns);
                dst_rank_addr_after_rns += (src_off_gran);
            }*/
            
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_all_to_all_rg_24(void *base_region_addr_src, void *base_region_addr_dst, uint32_t src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t a_length){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    //src/dst_start_offset이 8192n-64 넘어서 존재하면 오류인 코드
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;

    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0){
        iter_length=byte_length/64; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
        remain_length = (byte_length%64 + 7)/8;
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
    }

    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //because of bank structure~!
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;

    if(alltoall_comm_type==0){
        for(uint32_t rns_chip_id=0; rns_chip_id<a_length; rns_chip_id++){
            for(uint32_t i=0; i<iter_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                //for check
                /*if(rns_chip_id==1 && i==0){
                    RNS_COPY_a2a_24(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                }*/
                if(rns_chip_id==0){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a_24(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==1){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a_24(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==2){ //If a_len==2, it doesn't work.
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a_24(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==3){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a_24(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                
                //일단 살려서 64bytes씩~!
                src_mram_offset+=64;
                dst_mram_offset+=64;
            }
            for(uint32_t i=0; i<remain_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                if(rns_chip_id==0){
                    RNS_COPY_a2a_24(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                }
                else if(rns_chip_id==1){
                    RNS_COPY_a2a_24(1, 8, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                }
                else if(rns_chip_id==2){
                    RNS_COPY_a2a_24(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                }
                else if(rns_chip_id==3){
                    RNS_COPY_a2a_24(3, 24, iteration, src_rank_addr_iter, dst_rank_addr_iter, a_length);
                }

                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    else{ //alltoall_comm_type==1
        for(uint32_t rns_chip_id=0; rns_chip_id<(8/a_length); rns_chip_id++){
            for(uint32_t i=0; i<iter_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                if(rns_chip_id==0){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==1 && a_length==4){ //a_length==4
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==1){ //a_length==2
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==2){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                else if(rns_chip_id==3){
                    for(int iter_8=0; iter_8<8; iter_8++){
                        RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                        src_rank_addr_iter += (src_off_gran);
                        dst_rank_addr_iter += (src_off_gran);
                    }
                }
                
                //일단 살려서 64bytes씩~!
                src_mram_offset+=64;
                dst_mram_offset+=64;
            }
            for(uint32_t i=0; i<remain_length; i++){
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;

                void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!
                //S_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                if(rns_chip_id==0){
                    RNS_COPY_a2a(0, 0, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==1 && a_length==4){
                    RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==1){ //a_length==2
                    RNS_COPY_a2a(2, 16, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==2){
                    RNS_COPY_a2a(4, 32, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                else if(rns_chip_id==3){
                    RNS_COPY_a2a(6, 48, iteration, src_rank_addr_iter, dst_rank_addr_iter);
                }
                
                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    _mm_mfence();
    return;
}

//all_gather
void xeon_sp_trans_all_gather_rg(void *base_region_addr_src, void **base_region_addr_dst, uint32_t src_rg_id, uint32_t* dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t allgather_comm_type, uint32_t communication_buffer_offset, uint32_t num_iter_dst){
    //printf("xeon_sp_trans_all_gather works~~~~~~! *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id[0]);
    //src/dst_start_offset이 8192n-64 넘어서 존재하면 오류인 코드이다~!
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr_arr[num_iter_dst]; //rg당
    uint32_t dst_mram_offset; //dst target offset
    uint32_t dst_rotate_group_offset_256_64[num_iter_dst]; //rg당
    int64_t mram_dst_offset_1mb_wise[8]; //rg내에 번호당

    for(uint32_t i=0; i<num_iter_dst; i++){
        dst_rank_base_addr_arr[i]=base_region_addr_dst[i];
        dst_rotate_group_offset_256_64[i] = (dst_rg_id[i]%4) * (256*1024) + (dst_rg_id[i]/4) * 64;
    }
    dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset; //if(allgather_comm_type==0){

    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    int64_t mram_src_offset_1mb_wise=0;

    uint32_t src_mram_offset=0;
    src_mram_offset = src_start_offset + 1024*1024; //31.5mb //if(allgather_comm_type==0){ //x-axis include O 1이면 x-axis X

    uint32_t iter_length;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0 && byte_length%64==0){
        iter_length=byte_length/64; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
        remain_length = (byte_length%64 + 7)/8;
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
    }
    //remain_length+=0;

    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //because of bank structure~!
    
    if(allgather_comm_type==0){
        {
            for(uint32_t i=0; i<iter_length; i++){
                void *dst_rank_addr_array[8*num_iter_dst];
                void *src_rank_addr;
                uint32_t rg_offset=0;
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

                for(rg_offset=0; rg_offset<8; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){    
                    void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/8] + mram_dst_offset_1mb_wise[rg_offset%8];
                    dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset/8]);
                }
                //if(i==0){printf("check0,1 dst_rank_final_VA: %p, %p\n", dst_rank_addr_array[0], dst_rank_addr_array[1]);}
                //printf("%ld, %ld\n", mram_src_offset_1mb_wise, mram_dst_offset_1mb_wise[0]);

                //flush before RnS
                /*void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                    src_rank_addr_before_rns += (src_off_gran);
                }*/
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);
                    src_rank_addr_iter += (src_off_gran); //잘 더해지는거는 확인 완료
                    for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){
                        dst_rank_addr_array[rg_offset]+=src_off_gran;
                    }
                    //dst_rank_addr_iter += (src_off_gran);
                    //printf("%p, %p \n", src_rank_addr_iter, dst_rank_addr_array[iter_8]);
                }
                //break;

                //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
                /*void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                    src_rank_addr_after_rns += (src_off_gran);
                }
            
                //flush dst
                for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){
                    void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                    for(int j=0; j<8; j++){
                        COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                        dst_rank_addr_after_rns += (src_off_gran);
                    }    
                }*/
                
                
                //일단 살려서 64bytes씩~!
                src_mram_offset+=64;
                dst_mram_offset+=64;
                
            }
            for(uint32_t i=0; i<remain_length; i++){
                void *dst_rank_addr_array[8*num_iter_dst];
                void *src_rank_addr;
                uint32_t rg_offset=0;
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

                for(rg_offset=0; rg_offset<8; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+rg_offset*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){   
                    void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/8] + mram_dst_offset_1mb_wise[rg_offset%8];
                    dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset/8]);
                }

                //flush before RnS
                /*void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                */
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                //void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
                RNS_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);
                
                //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
                /*void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                
                //flush dst
                for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){
                    void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                    COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                }*/
                
                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    else{ //allgather_comm_type==1
       for(uint32_t i=0; i<iter_length; i++){
            void *dst_rank_addr_array[num_iter_dst];
            void *src_rank_addr;
            uint32_t rg_offset=0;
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise[0] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a; //x-axis아니라서 통신하는 애들은 rg내의 번호가 모두 같음
            for(rg_offset=0; rg_offset<num_iter_dst; rg_offset++){    
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset] + mram_dst_offset_1mb_wise[0];
                dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset]);
            }
            //printf("%ld, %ld\n", mram_src_offset_1mb_wise, mram_dst_offset_1mb_wise[0]);

            //flush before RnS
            void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            for(int iter_8=0; iter_8<8; iter_8++){
                S_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);
                src_rank_addr_iter += (src_off_gran); //잘 더해지는거는 확인 완료
                for(rg_offset=0; rg_offset<num_iter_dst; rg_offset++){
                    dst_rank_addr_array[rg_offset]+=src_off_gran;
                }
                //dst_rank_addr_iter += (src_off_gran);
                //printf("%p, %p \n", src_rank_addr_iter, dst_rank_addr_array[iter_8]);
            }
            //break;

            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            //flush dst
            for(rg_offset=0; rg_offset<num_iter_dst; rg_offset++){
                void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                    dst_rank_addr_after_rns += (src_off_gran);
                }    
            }
            
            
            //일단 살려서 64bytes씩~!
            src_mram_offset+=64;
            dst_mram_offset+=64;
            
        }
        for(uint32_t i=0; i<remain_length; i++){
            void *dst_rank_addr_array[8*num_iter_dst];
            void *src_rank_addr;
            uint32_t rg_offset=0;
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

            for(rg_offset=0; rg_offset<8; rg_offset++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+rg_offset*byte_length);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            }
            for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){   
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/8] + mram_dst_offset_1mb_wise[rg_offset%8];
                dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset/8]);
            }

            //flush before RnS
            void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
            COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
            
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            //void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64;
            //RNS_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);
            S_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);
            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
            COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
            
            //flush dst
            for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){
                void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
            }
            
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_all_gather_rg_24(void *base_region_addr_src, void **base_region_addr_dst, uint32_t src_rg_id, uint32_t* dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t allgather_comm_type, uint32_t communication_buffer_offset, uint32_t num_iter_dst, uint32_t a_length){
    //printf("xeon_sp_trans_all_gather works~~~~~~! *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id[0]);
    //printf("%d, 8/a_length=%d, num_iter_dst=%d\n", (8/a_length)*num_iter_dst, (8/a_length), num_iter_dst);
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr_arr[num_iter_dst]; //rg당
    uint32_t dst_mram_offset; //dst target offset
    uint32_t dst_rotate_group_offset_256_64[num_iter_dst]; //rg당
    int64_t mram_dst_offset_1mb_wise[8]; //rg내에 번호당

    for(uint32_t i=0; i<num_iter_dst; i++){
        dst_rank_base_addr_arr[i]=base_region_addr_dst[i];
        dst_rotate_group_offset_256_64[i] = (dst_rg_id[i]%4) * (256*1024) + (dst_rg_id[i]/4) * 64;
    }
    dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset; //if(allgather_comm_type==0){

    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    int64_t mram_src_offset_1mb_wise=0;

    uint32_t src_mram_offset=0;
    src_mram_offset = src_start_offset + 1024*1024; //31.5mb //if(allgather_comm_type==0){ //x-axis include O 1이면 x-axis X

    uint32_t iter_length;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0 && byte_length%64==0){
        iter_length=byte_length/64; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
        remain_length = (byte_length%64 + 7)/8;
        //printf("64n, src_offset=%d, dst_offset=%d, byte_length=%d\n", src_start_offset, dst_start_offset, byte_length);
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
        //printf("8n, src_offset=%d, dst_offset=%d, byte_length=%d\n", src_start_offset, dst_start_offset, byte_length);
    }
    //remain_length+=0;

    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //because of bank structure~!
    
    if(allgather_comm_type==0){
        {
            for(uint32_t i=0; i<iter_length; i++){
                void *dst_rank_addr_array[a_length];
                void *src_rank_addr;
                uint32_t rg_offset=0;
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

                for(rg_offset=0; rg_offset<a_length; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<a_length; rg_offset++){    
                    void *dst_rank_addr = dst_rank_base_addr_arr[0] + mram_dst_offset_1mb_wise[rg_offset];
                    dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[0]);
                }
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                for(int iter_8=0; iter_8<8; iter_8++){
                    RNS_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
                    src_rank_addr_iter += (src_off_gran); //잘 더해지는거는 확인 완료
                    for(rg_offset=0; rg_offset<a_length; rg_offset++){
                        dst_rank_addr_array[rg_offset]+=src_off_gran;
                    }
                    //dst_rank_addr_iter += (src_off_gran);
                    //printf("%p, %p \n", src_rank_addr_iter, dst_rank_addr_array[iter_8]);
                }
                
                //일단 살려서 64bytes씩~!
                src_mram_offset+=64;
                dst_mram_offset+=64;
                
            }
            for(uint32_t i=0; i<remain_length; i++){
                void *dst_rank_addr_array[a_length];
                void *src_rank_addr;
                uint32_t rg_offset=0;
                uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

                for(rg_offset=0; rg_offset<a_length; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<a_length; rg_offset++){    
                    void *dst_rank_addr = dst_rank_base_addr_arr[0] + mram_dst_offset_1mb_wise[rg_offset];
                    dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[0]);
                }
                
                //RnS
                void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                RNS_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
                
                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    else{ //allgather_comm_type==1
       for(uint32_t i=0; i<iter_length; i++){
            void *dst_rank_addr_array[(8/a_length) * num_iter_dst];
            void *src_rank_addr;
            uint32_t rg_offset=0;
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

            for(rg_offset=0; rg_offset<(8/a_length); rg_offset++){ //0or1 * dpu_byte_length, while a_length=4
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            }
            for(rg_offset=0; rg_offset<(8/a_length) * num_iter_dst; rg_offset++){ //0~255, while a_length=4
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/(8/a_length)] + mram_dst_offset_1mb_wise[rg_offset%(8/a_length)]; //!
                dst_rank_addr_array[rg_offset]=(dst_rank_addr + dst_rotate_group_offset_256_64[rg_offset/(8/a_length)]);
            }

            /*mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise[0] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a; //x-axis아니라서 통신하는 애들은 rg내의 번호가 모두 같음
            for(rg_offset=0; rg_offset<num_iter_dst; rg_offset++){    
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset] + mram_dst_offset_1mb_wise[0];
                dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset]);
            }*/
            //printf("%ld, %ld\n", mram_src_offset_1mb_wise, mram_dst_offset_1mb_wise[0]);

            //flush before RnS
            /*void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
                src_rank_addr_before_rns += (src_off_gran);
            }*/
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            for(int iter_8=0; iter_8<8; iter_8++){
                S_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
                //RNS_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
                src_rank_addr_iter += (src_off_gran); //잘 더해지는거는 확인 완료
                for(rg_offset=0; rg_offset<(8/a_length)*num_iter_dst; rg_offset++){
                    dst_rank_addr_array[rg_offset]+=src_off_gran;
                }
                //dst_rank_addr_iter += (src_off_gran);
                //printf("%p, %p \n", src_rank_addr_iter, dst_rank_addr_array[iter_8]);
            }
            //break;

            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            /*void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
            for(int j=0; j<8; j++){
                COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
                src_rank_addr_after_rns += (src_off_gran);
            }
        
            //flush dst
            for(rg_offset=0; rg_offset<(8/a_length)*num_iter_dst; rg_offset++){
                void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                for(int j=0; j<8; j++){
                    COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
                    dst_rank_addr_after_rns += (src_off_gran);
                }    
            }*/
            
            
            //일단 살려서 64bytes씩~!
            src_mram_offset+=64;
            dst_mram_offset+=64;
            
        }
        for(uint32_t i=0; i<remain_length; i++){
            void *dst_rank_addr_array[(8/a_length) * num_iter_dst];
            void *src_rank_addr;
            uint32_t rg_offset=0;
            uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
            uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;

            for(rg_offset=0; rg_offset<(8/a_length); rg_offset++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            }
            for(rg_offset=0; rg_offset<(8/a_length) * num_iter_dst; rg_offset++){
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/(8/a_length)] + mram_dst_offset_1mb_wise[rg_offset%(8/a_length)]; //!
                dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[rg_offset/(8/a_length)]);
            }

            //flush before RnS
            void *src_rank_addr_before_rns = src_rank_addr+ src_rotate_group_offset_256_64; //얘만 해도 140msec걸린다~!
            COMM_FLUSH_a2a(iteration, src_rank_addr_before_rns);
            
            
            //RnS
            void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
            S_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
            //RNS_COPY_ag(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array);

            //flush src //쓰고 싶으면 COMM_FLUSH도 재정의~!
            void *src_rank_addr_after_rns = src_rank_addr+ src_rotate_group_offset_256_64;
            COMM_FLUSH_a2a(iteration, src_rank_addr_after_rns);
            
            //flush dst
            for(rg_offset=0; rg_offset<8*num_iter_dst; rg_offset++){
                void *dst_rank_addr_after_rns = dst_rank_addr_array[rg_offset];
                COMM_FLUSH_a2a(iteration, dst_rank_addr_after_rns);
            }
            
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    _mm_mfence();
    return;
}











void xeon_sp_trans_gather_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, void **host_buffer, uint32_t host_buffer_first_index, uint32_t data_type){
    //printf("Final printf : %d %d %d *****src/dst_rg_id: %d, %d\n", **host_buffer8, **host_buffer32, data_type, src_rg_id[0], dst_rg_id);
    data_type+=0;
    src_rg_id[0]+=0;
    base_region_addr_dst+=0;
    src_start_offset+=0;
    dst_rg_id+=0;
    void *src_rank_base_addr=base_region_addr_src[0];
    src_rank_base_addr+=0;
    num_iter_src+=0;
    communication_buffer_offset+=0;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    iteration+=0;
    
    alltoall_comm_type+=0;
    int64_t mram_dst_offset_1mb_wise=0;
    uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    dst_mram_offset = dst_start_offset + 1024*1024;

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;

    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;

    //uint32_t flag=1;
    if(alltoall_comm_type==0){
        for(uint32_t i=0; i<iter_length; i++){
            
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!
            
            //void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ 8*(64/data_type)*i); //얜 1당 8byte라서 /8해줘야함~!
            void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ (64)*i); //void라서 data_type 상관없이 byte크기만큼 이동
            //GATHER_RNS_SUM_RS부를때마다 gather_x=(a/8)*8*64=64a, gather_y=(b)*64*1=64b, reduce_x=64, reduce_y=64

            for(int iter_8=0; iter_8<1; iter_8++){
                GATHER_COPY(iteration, dst_rank_addr_iter, dst_host_buffer);
            }

            //일단 살려서 64bytes씩~!
            //src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    
    _mm_mfence();
    return;
}







void xeon_sp_trans_scatter_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, void **host_buffer, uint32_t host_buffer_first_index, uint32_t data_type){
    //printf("Final printf : %d %d %d *****src/dst_rg_id: %d, %d\n", **host_buffer8, **host_buffer32, data_type, src_rg_id[0], dst_rg_id);
    src_rg_id[0]+=0;
    data_type+=0;
    src_start_offset+=0;
    dst_rg_id+=0;
    //printf("src_rg_id=%d, alltoall_comm_type=%d \n", src_rg_id[0], alltoall_comm_type);
    void *src_rank_base_addr=base_region_addr_src[0];
    src_rank_base_addr+=0;
    num_iter_src+=0;
    communication_buffer_offset+=0;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    iteration+=0;
    
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    //int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    //uint32_t src_mram_offset=0;
    //uint32_t dst_mram_offset=0;
    //src_mram_offset = src_start_offset + 1024*1024 + communication_buffer_offset; //31.5mb
    dst_mram_offset = dst_start_offset + 1024*1024;

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;

    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;

    //uint32_t flag=1;
    if(alltoall_comm_type==0){
        for(uint32_t i=0; i<iter_length; i++){
            
            mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!
            
            //void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ 8*(64/data_type)*i); //얜 1당 8byte라서 /8해줘야함~!
            void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ (64)*i); //void라서 data_type 상관없이 byte크기만큼 이동
            //GATHER_RNS_SUM_RS부를때마다 gather_x=(a/8)*8*64=64a, gather_y=(b)*64*1=64b, reduce_x=64, reduce_y=64
            //flag+=0;
            //printf("dst_host_buffer[0]=%d\n", dst_host_buffer[0]);
            for(int iter_8=0; iter_8<1; iter_8++){                    
                //SCATTER_COPY(iteration, src_host_buffer, dst_rank_addr_iter);
                //GATHER_COPY(iteration, dst_rank_addr_iter, dst_host_buffer);
                SCATTER_COPY(iteration, dst_host_buffer, dst_rank_addr_iter);
            }

            //일단 살려서 64bytes씩~!
            //src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    
    _mm_mfence();
    return;
}







void xeon_sp_trans_reduce_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, void **host_buffer, uint32_t host_buffer_first_index, uint32_t data_type){
    //printf("xeon_sp_trans_reduce works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id[0], dst_rg_id);
    base_region_addr_dst+=0;
    dst_rg_id+=0;
    communication_buffer_offset+=0;
    //printf("src_rg_id=%d, alltoall_comm_type=%d \n", src_rg_id[0], alltoall_comm_type);
    void *src_rank_base_addr;
    //void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    //int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    src_mram_offset = src_start_offset + 1024*1024; // + communication_buffer_offset; //31.5mb
    dst_mram_offset = dst_start_offset + 1024*1024;

    /*if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024 + communication_buffer_offset; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }*/
    uint32_t iter_length= byte_length/64; //0816_1. 원래 8로 나누지만 gather은 rg당 8*byte_length를 저장하는 반면, reduce는 byte_length를 저장하기 떄문~!~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    //uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;
    //uint32_t flag=1;
    if(alltoall_comm_type==0){
        for(uint32_t i=0; i<iter_length; i++){
            
            /*mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!*/

            for(uint32_t j=0; j<num_iter_src; j++){
                src_rank_base_addr = base_region_addr_src[j];
                src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

                for(uint32_t k=0; k<8; k++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length/8*k); //모양은 매우 이상하지만 큰 8개로 나눴으니 ㄱㅊ
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                    src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                    src_rank_addr_iter[j*8 + k] = src_rank_addr + src_rotate_group_offset_256_64;
                }
            }

            //void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ 8*(64/data_type)*i); //x는 byte_length*k //int8:8, int16:4, int32:2, int64:1
            void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ (64)*i); //void라서 data_type 상관없이 byte크기만큼 이동
            //GATHER_RNS_SUM_RS부를때마다 gather_x=(a/8)*8*64=64a, gather_y=(b)*64*1=64b, reduce_x=64, reduce_y=64 (reduce는 조금 다름)
            (dst_host_buffer)+=0;
            for(int iter_8=0; iter_8<1; iter_8++){
                REDUCE_RNS_SUM_RS(iteration, src_rank_addr_iter, dst_host_buffer, num_iter_src, data_type);
                //PREV_REDUCE_RNS_SUM_RS(iteration, src_rank_addr_iter, dst_host_buffer, num_iter_src);
            }
                
            //일단 살려서 64bytes씩~!
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    else{
        for(uint32_t i=0; i<iter_length; i++){
            
            /*mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
            next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
            mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
            void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!*/

            for(uint32_t j=0; j<num_iter_src; j++){
                src_rank_base_addr = base_region_addr_src[j];
                src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

                for(uint32_t k=0; k<1; k++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length/8*k*0);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                    src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                    src_rank_addr_iter[j + k] = src_rank_addr + src_rotate_group_offset_256_64;
                }
            }

            //void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ 8*(64/data_type)*i); //x는 byte_length*k //int8:8, int16:4, int32:2, int64:1
            void *dst_host_buffer = (*(host_buffer+host_buffer_first_index)+ (64)*i); //void라서 data_type 상관없이 byte크기만큼 이동
            //GATHER_RNS_SUM_RS부를때마다 gather_x=(a/8)*8*64=64a, gather_y=(b)*64*1=64b, reduce_x=64, reduce_y=64 (reduce는 조금 다름)
            for(int iter_8=0; iter_8<1; iter_8++){
                REDUCE_S_SUM_RS(iteration, src_rank_addr_iter, dst_host_buffer, num_iter_src, data_type);
                //PREV_REDUCE_S_SUM_RS(iteration, src_rank_addr_iter, dst_host_buffer, num_iter_src);
            }
                
            //일단 살려서 64bytes씩~!
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    
    _mm_mfence();
    return;
}




//xeon ReduceScatter CPU/DPU + AllReduce
void xeon_sp_trans_all_reduce_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t iter_dst_a, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t size, uint32_t reduce_type){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset + byte_length*8*iter_dst_a;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;

    void *dst_rank_addr;
    //void *dst_rank_addr_iter[8];
    void *dst_rank_addr_iter[num_iter_src * 8];

    dst_rank_base_addr = base_region_addr_dst;


    for(uint32_t i=0; i<iter_length; i++){

        for(uint32_t j=0; j<num_iter_src; j++){
            dst_rank_base_addr = base_region_addr_src[j];
            dst_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<8; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                dst_rank_addr_iter[j*8 + k] = dst_rank_addr + dst_rotate_group_offset_256_64;
            }
        }

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<8; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j*8 + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }

        for(int iter_8=0; iter_8<1; iter_8++){
            RNS_SUM_AR(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, size, reduce_type);

            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src*8; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }
            for(uint32_t j=0; j<num_iter_src*8; j++){
                RNS_FLUSH_DST(iteration, dst_rank_addr_iter[j]);
            }
        }
            
        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8;
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_all_reduce_rg_24(void *base_region_addr_src, void **base_region_addr_dst, uint32_t src_rg_id, uint32_t* dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t allgather_comm_type, uint32_t communication_buffer_offset, uint32_t num_iter_dst, uint32_t a_length, uint32_t size){
    //printf("xeon_sp_trans_all_gather works~~~~~~! *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id[0]);
    //printf("%d, 8/a_length=%d, num_iter_dst=%d\n", (8/a_length)*num_iter_dst, (8/a_length), num_iter_dst);

    //num_iter_dst==b/ (8/a)
    //다 dst로부터 들고와야함!
    void *src_rank_base_addr = base_region_addr_src;
    void *dst_rank_base_addr_arr[num_iter_dst]; //rg당
    uint32_t dst_mram_offset; //dst target offset
    uint32_t dst_rotate_group_offset_256_64[num_iter_dst]; //rg당
    int64_t mram_dst_offset_1mb_wise[8]; //rg내에 번호당
    for(uint32_t i=0; i<num_iter_dst; i++){
        dst_rank_base_addr_arr[i]=base_region_addr_dst[i];
        dst_rotate_group_offset_256_64[i] = (dst_rg_id[i]%4) * (256*1024) + (dst_rg_id[i]/4) * 64;
    }
    dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset; //if(allgather_comm_type==0){
    
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    //int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_src_offset_1mb_wise[8];

    uint32_t src_mram_offset=0;
    src_mram_offset = src_start_offset + 1024*1024; //31.5mb //if(allgather_comm_type==0){ //x-axis include O 1이면 x-axis X
    
    uint32_t iter_length;
    uint32_t remain_length;
    if(src_start_offset%64==0 && dst_start_offset%64==0 && byte_length%64==0){
        iter_length=byte_length/64; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
        remain_length = (byte_length%64 + 7)/8;
        //printf("64n, src_offset=%d, dst_offset=%d, byte_length=%d\n", src_start_offset, dst_start_offset, byte_length);
    }
    else{
        iter_length=0;
        remain_length = byte_length/8;
        //printf("8n, src_offset=%d, dst_offset=%d, byte_length=%d\n", src_start_offset, dst_start_offset, byte_length);
    }
    remain_length=byte_length/8;
    iter_length+=0;
    //remain_length+=0;
    uint32_t src_rotate_group_offset_256_64= (src_rg_id%4) * (256*1024) + (src_rg_id/4) * 64; //because of bank structure~!
    
    if(allgather_comm_type==0){
        {
            for(uint32_t i=0; i<remain_length; i++){
                void *dst_rank_addr_array[a_length];
                void *src_rank_addr_array[a_length];
                uint32_t rg_offset=0;
                uint32_t mram_64_bit_word_offset;
                uint64_t next_data;
                /*uint32_t mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset);
                uint64_t next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;*/
                for(rg_offset=0; rg_offset<a_length; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset+(rg_offset)*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_src_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<a_length; rg_offset++){    
                    void *src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise[rg_offset];
                    src_rank_addr_array[rg_offset]=(src_rank_addr + src_rotate_group_offset_256_64);
                }

                for(rg_offset=0; rg_offset<a_length; rg_offset++){
                    mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                    next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                    mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                }
                for(rg_offset=0; rg_offset<a_length; rg_offset++){    
                    void *dst_rank_addr = dst_rank_base_addr_arr[0] + mram_dst_offset_1mb_wise[rg_offset];
                    dst_rank_addr_array[rg_offset]=(dst_rank_addr+dst_rotate_group_offset_256_64[0]);
                }
                
                //RnS
                //void *src_rank_addr_iter = src_rank_addr + src_rotate_group_offset_256_64;
                //RNS_COPY_ag_24(0, 0, num_iter_dst, src_rank_addr_iter, dst_rank_addr_array, a_length);
                RNS_SUM_AR_24(iteration, src_rank_addr_array, dst_rank_addr_array, num_iter_dst, size, a_length);
                src_mram_offset+=8;
                dst_mram_offset+=8;
            }
        }
    }
    else{ //allreduce_comm_type==1
        byte_length /= (8/a_length); //***************************************************************************************************
        remain_length /= (8/a_length);
        printf("hihi~\n");
        for(uint32_t i=0; i<remain_length; i++){
            void *dst_rank_addr_array[(8/a_length)*num_iter_dst];
            void *src_rank_addr_array[(8/a_length)*num_iter_dst];
            uint32_t rg_offset=0;
            uint32_t mram_64_bit_word_offset;
            uint64_t next_data;

            for(rg_offset=0; rg_offset<(8/a_length); rg_offset++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset+(rg_offset)*byte_length);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            }
            for(rg_offset=0; rg_offset<(8/a_length) * num_iter_dst; rg_offset++){    
                void *src_rank_addr = dst_rank_base_addr_arr[rg_offset/(8/a_length)] + mram_src_offset_1mb_wise[rg_offset%(8/a_length)];
                src_rank_addr_array[rg_offset]=(src_rank_addr + dst_rotate_group_offset_256_64[rg_offset/(8/a_length)]);
            }

            for(rg_offset=0; rg_offset<(8/a_length); rg_offset++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset+(rg_offset)*byte_length);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise[rg_offset] = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
            }
            for(rg_offset=0; rg_offset<(8/a_length) * num_iter_dst; rg_offset++){    
                void *dst_rank_addr = dst_rank_base_addr_arr[rg_offset/(8/a_length)] + mram_dst_offset_1mb_wise[rg_offset%(8/a_length)];
                dst_rank_addr_array[rg_offset]=(dst_rank_addr + dst_rotate_group_offset_256_64[rg_offset/(8/a_length)]);
            }
            
            //RnS
            if(src_mram_offset==1) S_SUM_AR_24(iteration, src_rank_addr_array, dst_rank_addr_array, num_iter_dst, size, a_length);
            src_mram_offset+=8;
            dst_mram_offset+=8;
        }
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_all_reduce_y_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t iter_dst_b, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t size, uint32_t reduce_type){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset + byte_length*1*iter_dst_b;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;

    void *dst_rank_addr;
    //void *dst_rank_addr_iter[8];
    void *dst_rank_addr_iter[num_iter_src * 8];

    dst_rank_base_addr = base_region_addr_dst;

    for(uint32_t i=0; i<iter_length; i++){

        for(uint32_t j=0; j<num_iter_src; j++){
            dst_rank_base_addr = base_region_addr_src[j];
            dst_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<1; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
                dst_rank_addr_iter[j + k] = dst_rank_addr + dst_rotate_group_offset_256_64;
            }
        }

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<1; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }

        for(int iter_8=0; iter_8<1; iter_8++){
            S_SUM_AR(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, size, reduce_type);
            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }
            for(uint32_t j=0; j<num_iter_src; j++){
                RNS_FLUSH_DST(iteration, dst_rank_addr_iter[j]);
            }
        }
            
        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8;
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_reduce_scatter_cpu_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t size){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024 + communication_buffer_offset; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;

    for(uint32_t i=0; i<iter_length; i++){
            
        mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
        next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<8; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j*8 + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }
        for(int iter_8=0; iter_8<1; iter_8++){
            RNS_SUM_RS(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, size);
            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src*8; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }

            RNS_FLUSH_DST(iteration, dst_rank_addr_iter);
        }
        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8;
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_reduce_scatter_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;


    for(uint32_t i=0; i<iter_length; i++){
            
        mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
        next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<8; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j*8 + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }

        for(int iter_8=0; iter_8<1; iter_8++){
            RNS_SUM_RS_DPU(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, src_off_gran);
            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src*8; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }
            
            RNS_FLUSH_DST(iteration, dst_rank_addr_iter);
        }

        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8*8*num_iter_src;
    }
    _mm_mfence();
    return;
}

void xeon_sp_trans_reduce_scatter_cpu_y_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset, uint32_t size){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    //int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024 + communication_buffer_offset; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;

    for(uint32_t i=0; i<iter_length; i++){
            
        mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
        next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<1; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }

        for(int iter_8=0; iter_8<1; iter_8++){
            S_SUM_RS(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, size);
            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }
            
            RNS_FLUSH_DST(iteration, dst_rank_addr_iter);
        }
            
        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8;
    }
    
    _mm_mfence();
    return;
}    

void xeon_sp_trans_reduce_scatter_y_rg(void **base_region_addr_src, void *base_region_addr_dst, uint32_t* src_rg_id, uint32_t dst_rg_id, uint32_t src_start_offset, uint32_t dst_start_offset, uint32_t byte_length, uint32_t num_iter_src, uint32_t alltoall_comm_type, uint32_t communication_buffer_offset){
    //printf("xeon_sp_trans_all_to_all works~~~~~~!!(0719_04 new~!~!~!~) *****src/dst_rg_id: %d, %d\n", src_rg_id, dst_rg_id);
    void *src_rank_base_addr;
    void *dst_rank_base_addr = base_region_addr_dst;
    int packet_size = 8;
    int iteration = packet_size / 8;
    int src_off_gran = 128 * iteration;
    alltoall_comm_type+=0;
    int64_t mram_src_offset_1mb_wise=0;
    int64_t mram_dst_offset_1mb_wise=0;
    //uint32_t src_mram_offset = src_start_offset + 1024*1024 + 1024*1024*63/2; //31.5mb
    //uint32_t dst_mram_offset = dst_start_offset + 1024*1024;
    uint32_t src_mram_offset=0;
    uint32_t dst_mram_offset=0;
    if(alltoall_comm_type==0){ //x-axis include O
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }
    else{ //x-axis include X
        src_mram_offset = src_start_offset + 1024*1024; //31.5mb
        dst_mram_offset = dst_start_offset + 1024*1024 + communication_buffer_offset;
    }

    uint32_t iter_length = byte_length/8; //지금은 8byte rns를 8번씩해서 64번하는 구조니까~! 그러면 64*iter_length=8*byte_length이므로 하나의 rotate_group에 대해서 끝난거임~!
    //uint32_t iter_length = byte_length; //mo1_0725
    
    uint32_t dst_rotate_group_offset_256_64= (dst_rg_id%4) * (256*1024) + (dst_rg_id/4) * 64;
    uint32_t src_rotate_group_offset_256_64;

    void *src_rank_addr_iter[num_iter_src * 8];
    uint32_t mram_64_bit_word_offset;
    uint64_t next_data;
    void *src_rank_addr;

    for(uint32_t i=0; i<iter_length; i++){
            
        mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(dst_mram_offset);
        next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
        mram_dst_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
        void *dst_rank_addr = dst_rank_base_addr + mram_dst_offset_1mb_wise;
        void *dst_rank_addr_iter = dst_rank_addr + dst_rotate_group_offset_256_64; //(dst_rotate_group%4) * (256*1024) + (dst_rotate_group/4) * 64; //나중이 아니라 미리 더해야함~!

        for(uint32_t j=0; j<num_iter_src; j++){
            src_rank_base_addr = base_region_addr_src[j];
            src_rotate_group_offset_256_64= (src_rg_id[j]%4) * (256*1024) + (src_rg_id[j]/4) * 64;

            for(uint32_t k=0; k<1; k++){
                mram_64_bit_word_offset = apply_address_translation_on_mram_offset_a2a(src_mram_offset + byte_length*k);
                next_data = BANK_OFFSET_NEXT_DATA_a2a(mram_64_bit_word_offset);
                mram_src_offset_1mb_wise = (next_data % BANK_CHUNK_SIZE_a2a) + (next_data / BANK_CHUNK_SIZE_a2a) * BANK_NEXT_CHUNK_OFFSET_a2a;
                src_rank_addr = src_rank_base_addr + mram_src_offset_1mb_wise;
                src_rank_addr_iter[j + k] = src_rank_addr + src_rotate_group_offset_256_64;
            }
        }

        for(int iter_8=0; iter_8<1; iter_8++){
            S_SUM_RS_DPU(iteration, src_rank_addr_iter, dst_rank_addr_iter, num_iter_src, src_off_gran);
            _mm_mfence();

            for(uint32_t j=0; j<num_iter_src; j++){
                RNS_FLUSH_SRC(iteration, src_rank_addr_iter[j]);
            }
            
            RNS_FLUSH_DST(iteration, dst_rank_addr_iter);
        }
            
        //일단 살려서 64bytes씩~!
        src_mram_offset+=8;
        dst_mram_offset+=8*num_iter_src;
    }
    
    _mm_mfence();
    return;
}