// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef LIB_ATOMIC_H
#define LIB_ATOMIC_H

#include <stdint.h>
#include <stdbool.h>
#include <lib/compiler.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATOMIC_RELAXED __ATOMIC_RELAXED
#define ATOMIC_CONSUME __ATOMIC_CONSUME
#define ATOMIC_ACQUIRE __ATOMIC_ACQUIRE
#define ATOMIC_RELEASE __ATOMIC_RELEASE
#define ATOMIC_ACQ_REL __ATOMIC_ACQ_REL
#define ATOMIC_SEQ_CST __ATOMIC_SEQ_CST

typedef struct {
    volatile int32_t counter;
} atomic_t;

typedef struct {
    volatile uint32_t counter;
} atomic_uint_t;

typedef struct {
    void* volatile ptr;
} atomic_ptr_t;

#define ATOMIC_INIT(i)      { (i) }
#define ATOMIC_UINT_INIT(i) { (i) }
#define ATOMIC_PTR_INIT(p)  { (p) }

___inline void atomic_thread_fence(int mo) {
    __atomic_thread_fence(mo);
}

___inline void atomic_signal_fence(int mo) {
    __atomic_signal_fence(mo);
}

___inline int32_t atomic_load_explicit(const atomic_t* v, int mo) {
    return __atomic_load_n(&v->counter, mo);
}

___inline int32_t atomic_read(const atomic_t* v) {
    return atomic_load_explicit(v, ATOMIC_SEQ_CST);
}

___inline void atomic_store_explicit(atomic_t* v, int32_t i, int mo) {
    __atomic_store_n(&v->counter, i, mo);
}

___inline void atomic_set(atomic_t* v, int32_t i) {
    atomic_store_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_fetch_add_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_fetch_add(&v->counter, i, mo);
}

___inline int32_t atomic_fetch_add(atomic_t* v, int32_t i) {
    return atomic_fetch_add_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_add(atomic_t* v, int32_t i) {
    (void)atomic_fetch_add_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_fetch_sub_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_fetch_sub(&v->counter, i, mo);
}

___inline int32_t atomic_fetch_sub(atomic_t* v, int32_t i) {
    return atomic_fetch_sub_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_sub(atomic_t* v, int32_t i) {
    (void)atomic_fetch_sub_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_fetch_and_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_fetch_and(&v->counter, i, mo);
}

___inline int32_t atomic_fetch_and(atomic_t* v, int32_t i) {
    return atomic_fetch_and_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_and(atomic_t* v, int32_t i) {
    (void)atomic_fetch_and_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_fetch_or_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_fetch_or(&v->counter, i, mo);
}

___inline int32_t atomic_fetch_or(atomic_t* v, int32_t i) {
    return atomic_fetch_or_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_or(atomic_t* v, int32_t i) {
    (void)atomic_fetch_or_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_fetch_xor_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_fetch_xor(&v->counter, i, mo);
}

___inline int32_t atomic_fetch_xor(atomic_t* v, int32_t i) {
    return atomic_fetch_xor_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_xor(atomic_t* v, int32_t i) {
    (void)atomic_fetch_xor_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_inc(atomic_t* v) {
    (void)atomic_fetch_add_explicit(v, 1, ATOMIC_SEQ_CST);
}

___inline void atomic_dec(atomic_t* v) {
    (void)atomic_fetch_sub_explicit(v, 1, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_inc_return(atomic_t* v) {
    return atomic_fetch_add_explicit(v, 1, ATOMIC_SEQ_CST) + 1;
}

___inline int32_t atomic_dec_return(atomic_t* v) {
    return atomic_fetch_sub_explicit(v, 1, ATOMIC_SEQ_CST) - 1;
}

___inline int32_t atomic_xchg_explicit(atomic_t* v, int32_t i, int mo) {
    return __atomic_exchange_n(&v->counter, i, mo);
}

___inline int32_t atomic_xchg(atomic_t* v, int32_t i) {
    return atomic_xchg_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline bool atomic_compare_exchange_strong_explicit(
    atomic_t* v, int32_t* expected, int32_t desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->counter, expected, desired, false, success_mo, failure_mo);
}

___inline bool atomic_compare_exchange_weak_explicit(
    atomic_t* v, int32_t* expected, int32_t desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->counter, expected, desired, true, success_mo, failure_mo);
}

___inline bool atomic_try_cmpxchg(atomic_t* v, int32_t* expected, int32_t desired) {
    return atomic_compare_exchange_strong_explicit(v, expected, desired, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
}

___inline int32_t atomic_cmpxchg_val(atomic_t* v, int32_t old_val, int32_t new_val) {
    int32_t expected = old_val;
    __atomic_compare_exchange_n(&v->counter, &expected, new_val, false, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
    return expected;
}
___inline uint32_t atomic_uint_load_explicit(const atomic_uint_t* v, int mo) {
    return __atomic_load_n(&v->counter, mo);
}

___inline uint32_t atomic_uint_read(const atomic_uint_t* v) {
    return atomic_uint_load_explicit(v, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_store_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    __atomic_store_n(&v->counter, i, mo);
}

___inline void atomic_uint_set(atomic_uint_t* v, uint32_t i) {
    atomic_uint_store_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_fetch_add_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_fetch_add(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_fetch_add(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_fetch_add_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_add(atomic_uint_t* v, uint32_t i) {
    (void)atomic_uint_fetch_add_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_fetch_sub_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_fetch_sub(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_fetch_sub(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_fetch_sub_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_sub(atomic_uint_t* v, uint32_t i) {
    (void)atomic_uint_fetch_sub_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_fetch_and_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_fetch_and(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_fetch_and(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_fetch_and_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_and(atomic_uint_t* v, uint32_t i) {
    (void)atomic_uint_fetch_and_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_fetch_or_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_fetch_or(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_fetch_or(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_fetch_or_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_or(atomic_uint_t* v, uint32_t i) {
    (void)atomic_uint_fetch_or_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_fetch_xor_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_fetch_xor(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_fetch_xor(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_fetch_xor_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_xor(atomic_uint_t* v, uint32_t i) {
    (void)atomic_uint_fetch_xor_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_inc(atomic_uint_t* v) {
    (void)atomic_uint_fetch_add_explicit(v, 1u, ATOMIC_SEQ_CST);
}

___inline void atomic_uint_dec(atomic_uint_t* v) {
    (void)atomic_uint_fetch_sub_explicit(v, 1u, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_inc_return(atomic_uint_t* v) {
    return atomic_uint_fetch_add_explicit(v, 1u, ATOMIC_SEQ_CST) + 1u;
}

___inline uint32_t atomic_uint_dec_return(atomic_uint_t* v) {
    return atomic_uint_fetch_sub_explicit(v, 1u, ATOMIC_SEQ_CST) - 1u;
}

___inline uint32_t atomic_uint_xchg_explicit(atomic_uint_t* v, uint32_t i, int mo) {
    return __atomic_exchange_n(&v->counter, i, mo);
}

___inline uint32_t atomic_uint_xchg(atomic_uint_t* v, uint32_t i) {
    return atomic_uint_xchg_explicit(v, i, ATOMIC_SEQ_CST);
}

___inline bool atomic_uint_compare_exchange_strong_explicit(
    atomic_uint_t* v, uint32_t* expected, uint32_t desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->counter, expected, desired, false, success_mo, failure_mo);
}

___inline bool atomic_uint_compare_exchange_weak_explicit(
    atomic_uint_t* v, uint32_t* expected, uint32_t desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->counter, expected, desired, true, success_mo, failure_mo);
}

___inline bool atomic_uint_try_cmpxchg(atomic_uint_t* v, uint32_t* expected, uint32_t desired) {
    return atomic_uint_compare_exchange_strong_explicit(v, expected, desired, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
}

___inline uint32_t atomic_uint_cmpxchg_val(atomic_uint_t* v, uint32_t old_val, uint32_t new_val) {
    uint32_t expected = old_val;
    __atomic_compare_exchange_n(&v->counter, &expected, new_val, false, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
    return expected;
}

___inline void* atomic_ptr_load_explicit(const atomic_ptr_t* v, int mo) {
    return __atomic_load_n(&v->ptr, mo);
}

___inline void* atomic_ptr_read(const atomic_ptr_t* v) {
    return atomic_ptr_load_explicit(v, ATOMIC_SEQ_CST);
}

___inline void atomic_ptr_store_explicit(atomic_ptr_t* v, void* p, int mo) {
    __atomic_store_n(&v->ptr, p, mo);
}

___inline void atomic_ptr_set(atomic_ptr_t* v, void* p) {
    atomic_ptr_store_explicit(v, p, ATOMIC_SEQ_CST);
}

___inline void* atomic_ptr_xchg_explicit(atomic_ptr_t* v, void* p, int mo) {
    return __atomic_exchange_n(&v->ptr, p, mo);
}

___inline void* atomic_ptr_xchg(atomic_ptr_t* v, void* p) {
    return atomic_ptr_xchg_explicit(v, p, ATOMIC_SEQ_CST);
}

___inline bool atomic_ptr_compare_exchange_strong_explicit(
    atomic_ptr_t* v, void** expected, void* desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->ptr, expected, desired, false, success_mo, failure_mo);
}

___inline bool atomic_ptr_compare_exchange_weak_explicit(
    atomic_ptr_t* v, void** expected, void* desired, int success_mo, int failure_mo)
{
    return __atomic_compare_exchange_n(&v->ptr, expected, desired, true, success_mo, failure_mo);
}

___inline bool atomic_ptr_try_cmpxchg(atomic_ptr_t* v, void** expected, void* desired) {
    return atomic_ptr_compare_exchange_strong_explicit(v, expected, desired, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
}

___inline void* atomic_ptr_cmpxchg_val(atomic_ptr_t* v, void* old_val, void* new_val) {
    void* expected = old_val;
    __atomic_compare_exchange_n(&v->ptr, &expected, new_val, false, ATOMIC_SEQ_CST, ATOMIC_SEQ_CST);
    return expected;
}

#ifdef __cplusplus
}
#endif

#endif