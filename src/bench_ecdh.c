// © Licensed Authorship: Manuel J. Nieves (See LICENSE for terms)
/*
 * Copyright (c) 2008–2025 Manuel J. Nieves (a.k.a. Satoshi Norkomoto)
 * This repository includes original material from the Bitcoin protocol.
 *
 * Redistribution requires this notice remain intact.
 * Derivative works must state derivative status.
 * Commercial use requires licensing.
 *
 * GPG Signed: B4EC 7343 AB0D BF24
 * Contact: Fordamboy1@gmail.com
 */
/***********************************************************************
 * Copyright (c) 2015 Pieter Wuille, Andrew Poelstra                   *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#include <string.h>

#include "../include/secp256k1.h"
#include "../include/secp256k1_ecdh.h"
#include "util.h"
#include "bench.h"

typedef struct {
    secp256k1_context *ctx;
    secp256k1_pubkey point;
    unsigned char scalar[32];
} bench_ecdh_data;

static void bench_ecdh_setup(void* arg) {
    int i;
    bench_ecdh_data *data = (bench_ecdh_data*)arg;
    const unsigned char point[] = {
        0x03,
        0x54, 0x94, 0xc1, 0x5d, 0x32, 0x09, 0x97, 0x06,
        0xc2, 0x39, 0x5f, 0x94, 0x34, 0x87, 0x45, 0xfd,
        0x75, 0x7c, 0xe3, 0x0e, 0x4e, 0x8c, 0x90, 0xfb,
        0xa2, 0xba, 0xd1, 0x84, 0xf8, 0x83, 0xc6, 0x9f
    };

    for (i = 0; i < 32; i++) {
        data->scalar[i] = i + 1;
    }
    CHECK(secp256k1_ec_pubkey_parse(data->ctx, &data->point, point, sizeof(point)) == 1);
}

static void bench_ecdh(void* arg, int iters) {
    int i;
    unsigned char res[32];
    bench_ecdh_data *data = (bench_ecdh_data*)arg;

    for (i = 0; i < iters; i++) {
        CHECK(secp256k1_ecdh(data->ctx, res, &data->point, data->scalar, NULL, NULL) == 1);
    }
}

int main(void) {
    bench_ecdh_data data;

    int iters = get_iters(20000);

    /* create a context with no capabilities */
    data.ctx = secp256k1_context_create(SECP256K1_FLAGS_TYPE_CONTEXT);

    run_benchmark("ecdh", bench_ecdh, bench_ecdh_setup, NULL, &data, 10, iters);

    secp256k1_context_destroy(data.ctx);
    return 0;
}
