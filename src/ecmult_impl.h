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
/******************************************************************************
 * Copyright (c) 2013, 2014, 2017 Pieter Wuille, Andrew Poelstra, Jonas Nick  *
 * Distributed under the MIT software license, see the accompanying           *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.       *
 ******************************************************************************/

#ifndef SECP256K1_ECMULT_IMPL_H
#define SECP256K1_ECMULT_IMPL_H

#include <string.h>
#include <stdint.h>

#include "util.h"
#include "group.h"
#include "scalar.h"
#include "ecmult.h"
#include "ecmult_static_pre_g.h"

#if defined(EXHAUSTIVE_TEST_ORDER)
/* We need to lower these values for exhaustive tests because
 * the tables cannot have infinities in them (this breaks the
 * affine-isomorphism stuff which tracks z-ratios) */
#  if EXHAUSTIVE_TEST_ORDER > 128
#    define WINDOW_A 5
#  elif EXHAUSTIVE_TEST_ORDER > 8
#    define WINDOW_A 4
#  else
#    define WINDOW_A 2
#  endif
#else
/* optimal for 128-bit and 256-bit exponents. */
#  define WINDOW_A 5
/** Larger values for ECMULT_WINDOW_SIZE result in possibly better
 *  performance at the cost of an exponentially larger precomputed
 *  table. The exact table size is
 *      (1 << (WINDOW_G - 2)) * sizeof(secp256k1_ge_storage)  bytes,
 *  where sizeof(secp256k1_ge_storage) is typically 64 bytes but can
 *  be larger due to platform-specific padding and alignment.
 *  Two tables of this size are used (due to the endomorphism
 *  optimization).
 */
#endif

#define WNAF_BITS 128
#define WNAF_SIZE_BITS(bits, w) (((bits) + (w) - 1) / (w))
#define WNAF_SIZE(w) WNAF_SIZE_BITS(WNAF_BITS, w)

/* The number of objects allocated on the scratch space for ecmult_multi algorithms */
#define PIPPENGER_SCRATCH_OBJECTS 6
#define STRAUSS_SCRATCH_OBJECTS 6

#define PIPPENGER_MAX_BUCKET_WINDOW 12

/* Minimum number of points for which pippenger_wnaf is faster than strauss wnaf */
#define ECMULT_PIPPENGER_THRESHOLD 88

#define ECMULT_MAX_POINTS_PER_BATCH 5000000

/** Fill a table 'prej' with precomputed odd multiples of a. Prej will contain
 *  the values [1*a,3*a,...,(2*n-1)*a], so it space for n values. zr[0] will
 *  contain prej[0].z / a.z. The other zr[i] values = prej[i].z / prej[i-1].z.
 *  Prej's Z values are undefined, except for the last value.
 */
static void secp256k1_ecmult_odd_multiples_table(int n, secp256k1_gej *prej, secp256k1_fe *zr, const secp256k1_gej *a) {
    secp256k1_gej d;
    secp256k1_ge a_ge, d_ge;
    int i;

    VERIFY_CHECK(!a->infinity);

    secp256k1_gej_double_var(&d, a, NULL);

    /*
     * Perform the additions on an isomorphism where 'd' is affine: drop the z coordinate
     * of 'd', and scale the 1P starting value's x/y coordinates without changing its z.
     */
    d_ge.x = d.x;
    d_ge.y = d.y;
    d_ge.infinity = 0;

    secp256k1_ge_set_gej_zinv(&a_ge, a, &d.z);
    prej[0].x = a_ge.x;
    prej[0].y = a_ge.y;
    prej[0].z = a->z;
    prej[0].infinity = 0;

    zr[0] = d.z;
    for (i = 1; i < n; i++) {
        secp256k1_gej_add_ge_var(&prej[i], &prej[i-1], &d_ge, &zr[i]);
    }

    /*
     * Each point in 'prej' has a z coordinate too small by a factor of 'd.z'. Only
     * the final point's z coordinate is actually used though, so just update that.
     */
    secp256k1_fe_mul(&prej[n-1].z, &prej[n-1].z, &d.z);
}

/** Fill a table 'pre' with precomputed odd multiples of a.
 *
 *  The resulting point set is brought to a single constant Z denominator, stores the X and Y
 *  coordinates as ge_storage points in pre, and stores the global Z in rz.
 *  It only operates on tables sized for WINDOW_A wnaf multiples.
 *
 *  To compute a*P + b*G, we compute a table for P using this function,
 *  and use the precomputed table in <ecmult_static_pre_g.h> for G.
 */
static void secp256k1_ecmult_odd_multiples_table_globalz_windowa(secp256k1_ge *pre, secp256k1_fe *globalz, const secp256k1_gej *a) {
    secp256k1_gej prej[ECMULT_TABLE_SIZE(WINDOW_A)];
    secp256k1_fe zr[ECMULT_TABLE_SIZE(WINDOW_A)];

    /* Compute the odd multiples in Jacobian form. */
    secp256k1_ecmult_odd_multiples_table(ECMULT_TABLE_SIZE(WINDOW_A), prej, zr, a);
    /* Bring them to the same Z denominator. */
    secp256k1_ge_globalz_set_table_gej(ECMULT_TABLE_SIZE(WINDOW_A), pre, globalz, prej, zr);
}

/** The following two macro retrieves a particular odd multiple from a table
 *  of precomputed multiples. */
#define ECMULT_TABLE_GET_GE(r,pre,n,w) do { \
    VERIFY_CHECK(((n) & 1) == 1); \
    VERIFY_CHECK((n) >= -((1 << ((w)-1)) - 1)); \
    VERIFY_CHECK((n) <=  ((1 << ((w)-1)) - 1)); \
    if ((n) > 0) { \
        *(r) = (pre)[((n)-1)/2]; \
    } else { \
        *(r) = (pre)[(-(n)-1)/2]; \
        secp256k1_fe_negate(&((r)->y), &((r)->y), 1); \
    } \
} while(0)

#define ECMULT_TABLE_GET_GE_STORAGE(r,pre,n,w) do { \
    VERIFY_CHECK(((n) & 1) == 1); \
    VERIFY_CHECK((n) >= -((1 << ((w)-1)) - 1)); \
    VERIFY_CHECK((n) <=  ((1 << ((w)-1)) - 1)); \
    if ((n) > 0) { \
        secp256k1_ge_from_storage((r), &(pre)[((n)-1)/2]); \
    } else { \
        secp256k1_ge_from_storage((r), &(pre)[(-(n)-1)/2]); \
        secp256k1_fe_negate(&((r)->y), &((r)->y), 1); \
    } \
} while(0)

/** Convert a number to WNAF notation. The number becomes represented by sum(2^i * wnaf[i], i=0..bits),
 *  with the following guarantees:
 *  - each wnaf[i] is either 0, or an odd integer between -(1<<(w-1) - 1) and (1<<(w-1) - 1)
 *  - two non-zero entries in wnaf are separated by at least w-1 zeroes.
 *  - the number of set values in wnaf is returned. This number is at most 256, and at most one more
 *    than the number of bits in the (absolute value) of the input.
 */
static int secp256k1_ecmult_wnaf(int *wnaf, int len, const secp256k1_scalar *a, int w) {
    secp256k1_scalar s;
    int last_set_bit = -1;
    int bit = 0;
    int sign = 1;
    int carry = 0;

    VERIFY_CHECK(wnaf != NULL);
    VERIFY_CHECK(0 <= len && len <= 256);
    VERIFY_CHECK(a != NULL);
    VERIFY_CHECK(2 <= w && w <= 31);

    memset(wnaf, 0, len * sizeof(wnaf[0]));

    s = *a;
    if (secp256k1_scalar_get_bits(&s, 255, 1)) {
        secp256k1_scalar_negate(&s, &s);
        sign = -1;
    }

    while (bit < len) {
        int now;
        int word;
        if (secp256k1_scalar_get_bits(&s, bit, 1) == (unsigned int)carry) {
            bit++;
            continue;
        }

        now = w;
        if (now > len - bit) {
            now = len - bit;
        }

        word = secp256k1_scalar_get_bits_var(&s, bit, now) + carry;

        carry = (word >> (w-1)) & 1;
        word -= carry << w;

        wnaf[bit] = sign * word;
        last_set_bit = bit;

        bit += now;
    }
#ifdef VERIFY
    CHECK(carry == 0);
    while (bit < 256) {
        CHECK(secp256k1_scalar_get_bits(&s, bit++, 1) == 0);
    }
#endif
    return last_set_bit + 1;
}

struct secp256k1_strauss_point_state {
    secp256k1_scalar na_1, na_lam;
    int wnaf_na_1[129];
    int wnaf_na_lam[129];
    int bits_na_1;
    int bits_na_lam;
    size_t input_pos;
};

struct secp256k1_strauss_state {
    secp256k1_gej* prej;
    secp256k1_fe* zr;
    secp256k1_ge* pre_a;
    secp256k1_ge* pre_a_lam;
    struct secp256k1_strauss_point_state* ps;
};

static void secp256k1_ecmult_strauss_wnaf(const struct secp256k1_strauss_state *state, secp256k1_gej *r, size_t num, const secp256k1_gej *a, const secp256k1_scalar *na, const secp256k1_scalar *ng) {
    secp256k1_ge tmpa;
    secp256k1_fe Z;
    /* Splitted G factors. */
    secp256k1_scalar ng_1, ng_128;
    int wnaf_ng_1[129];
    int bits_ng_1 = 0;
    int wnaf_ng_128[129];
    int bits_ng_128 = 0;
    int i;
    int bits = 0;
    size_t np;
    size_t no = 0;

    for (np = 0; np < num; ++np) {
        if (secp256k1_scalar_is_zero(&na[np]) || secp256k1_gej_is_infinity(&a[np])) {
            continue;
        }
        state->ps[no].input_pos = np;
        /* split na into na_1 and na_lam (where na = na_1 + na_lam*lambda, and na_1 and na_lam are ~128 bit) */
        secp256k1_scalar_split_lambda(&state->ps[no].na_1, &state->ps[no].na_lam, &na[np]);

        /* build wnaf representation for na_1 and na_lam. */
        state->ps[no].bits_na_1   = secp256k1_ecmult_wnaf(state->ps[no].wnaf_na_1,   129, &state->ps[no].na_1,   WINDOW_A);
        state->ps[no].bits_na_lam = secp256k1_ecmult_wnaf(state->ps[no].wnaf_na_lam, 129, &state->ps[no].na_lam, WINDOW_A);
        VERIFY_CHECK(state->ps[no].bits_na_1 <= 129);
        VERIFY_CHECK(state->ps[no].bits_na_lam <= 129);
        if (state->ps[no].bits_na_1 > bits) {
            bits = state->ps[no].bits_na_1;
        }
        if (state->ps[no].bits_na_lam > bits) {
            bits = state->ps[no].bits_na_lam;
        }
        ++no;
    }

    /* Calculate odd multiples of a.
     * All multiples are brought to the same Z 'denominator', which is stored
     * in Z. Due to secp256k1' isomorphism we can do all operations pretending
     * that the Z coordinate was 1, use affine addition formulae, and correct
     * the Z coordinate of the result once at the end.
     * The exception is the precomputed G table points, which are actually
     * affine. Compared to the base used for other points, they have a Z ratio
     * of 1/Z, so we can use secp256k1_gej_add_zinv_var, which uses the same
     * isomorphism to efficiently add with a known Z inverse.
     */
    if (no > 0) {
        /* Compute the odd multiples in Jacobian form. */
        secp256k1_ecmult_odd_multiples_table(ECMULT_TABLE_SIZE(WINDOW_A), state->prej, state->zr, &a[state->ps[0].input_pos]);
        for (np = 1; np < no; ++np) {
            secp256k1_gej tmp = a[state->ps[np].input_pos];
#ifdef VERIFY
            secp256k1_fe_normalize_var(&(state->prej[(np - 1) * ECMULT_TABLE_SIZE(WINDOW_A) + ECMULT_TABLE_SIZE(WINDOW_A) - 1].z));
#endif
            secp256k1_gej_rescale(&tmp, &(state->prej[(np - 1) * ECMULT_TABLE_SIZE(WINDOW_A) + ECMULT_TABLE_SIZE(WINDOW_A) - 1].z));
            secp256k1_ecmult_odd_multiples_table(ECMULT_TABLE_SIZE(WINDOW_A), state->prej + np * ECMULT_TABLE_SIZE(WINDOW_A), state->zr + np * ECMULT_TABLE_SIZE(WINDOW_A), &tmp);
            secp256k1_fe_mul(state->zr + np * ECMULT_TABLE_SIZE(WINDOW_A), state->zr + np * ECMULT_TABLE_SIZE(WINDOW_A), &(a[state->ps[np].input_pos].z));
        }
        /* Bring them to the same Z denominator. */
        secp256k1_ge_globalz_set_table_gej(ECMULT_TABLE_SIZE(WINDOW_A) * no, state->pre_a, &Z, state->prej, state->zr);
    } else {
        secp256k1_fe_set_int(&Z, 1);
    }

    for (np = 0; np < no; ++np) {
        for (i = 0; i < ECMULT_TABLE_SIZE(WINDOW_A); i++) {
            secp256k1_ge_mul_lambda(&state->pre_a_lam[np * ECMULT_TABLE_SIZE(WINDOW_A) + i], &state->pre_a[np * ECMULT_TABLE_SIZE(WINDOW_A) + i]);
        }
    }

    if (ng) {
        /* split ng into ng_1 and ng_128 (where gn = gn_1 + gn_128*2^128, and gn_1 and gn_128 are ~128 bit) */
        secp256k1_scalar_split_128(&ng_1, &ng_128, ng);

        /* Build wnaf representation for ng_1 and ng_128 */
        bits_ng_1   = secp256k1_ecmult_wnaf(wnaf_ng_1,   129, &ng_1,   WINDOW_G);
        bits_ng_128 = secp256k1_ecmult_wnaf(wnaf_ng_128, 129, &ng_128, WINDOW_G);
        if (bits_ng_1 > bits) {
            bits = bits_ng_1;
        }
        if (bits_ng_128 > bits) {
            bits = bits_ng_128;
        }
    }

    secp256k1_gej_set_infinity(r);

    for (i = bits - 1; i >= 0; i--) {
        int n;
        secp256k1_gej_double_var(r, r, NULL);
        for (np = 0; np < no; ++np) {
            if (i < state->ps[np].bits_na_1 && (n = state->ps[np].wnaf_na_1[i])) {
                ECMULT_TABLE_GET_GE(&tmpa, state->pre_a + np * ECMULT_TABLE_SIZE(WINDOW_A), n, WINDOW_A);
                secp256k1_gej_add_ge_var(r, r, &tmpa, NULL);
            }
            if (i < state->ps[np].bits_na_lam && (n = state->ps[np].wnaf_na_lam[i])) {
                ECMULT_TABLE_GET_GE(&tmpa, state->pre_a_lam + np * ECMULT_TABLE_SIZE(WINDOW_A), n, WINDOW_A);
                secp256k1_gej_add_ge_var(r, r, &tmpa, NULL);
            }
        }
        if (i < bits_ng_1 && (n = wnaf_ng_1[i])) {
            ECMULT_TABLE_GET_GE_STORAGE(&tmpa, secp256k1_pre_g, n, WINDOW_G);
            secp256k1_gej_add_zinv_var(r, r, &tmpa, &Z);
        }
        if (i < bits_ng_128 && (n = wnaf_ng_128[i])) {
            ECMULT_TABLE_GET_GE_STORAGE(&tmpa, secp256k1_pre_g_128, n, WINDOW_G);
            secp256k1_gej_add_zinv_var(r, r, &tmpa, &Z);
        }
    }

    if (!r->infinity) {
        secp256k1_fe_mul(&r->z, &r->z, &Z);
    }
}

static void secp256k1_ecmult(secp256k1_gej *r, const secp256k1_gej *a, const secp256k1_scalar *na, const secp256k1_scalar *ng) {
    secp256k1_gej prej[ECMULT_TABLE_SIZE(WINDOW_A)];
    secp256k1_fe zr[ECMULT_TABLE_SIZE(WINDOW_A)];
    secp256k1_ge pre_a[ECMULT_TABLE_SIZE(WINDOW_A)];
    struct secp256k1_strauss_point_state ps[1];
    secp256k1_ge pre_a_lam[ECMULT_TABLE_SIZE(WINDOW_A)];
    struct secp256k1_strauss_state state;

    state.prej = prej;
    state.zr = zr;
    state.pre_a = pre_a;
    state.pre_a_lam = pre_a_lam;
    state.ps = ps;
    secp256k1_ecmult_strauss_wnaf(&state, r, 1, a, na, ng);
}

static size_t secp256k1_strauss_scratch_size(size_t n_points) {
    static const size_t point_size = (2 * sizeof(secp256k1_ge) + sizeof(secp256k1_gej) + sizeof(secp256k1_fe)) * ECMULT_TABLE_SIZE(WINDOW_A) + sizeof(struct secp256k1_strauss_point_state) + sizeof(secp256k1_gej) + sizeof(secp256k1_scalar);
    return n_points*point_size;
}

static int secp256k1_ecmult_strauss_batch(const secp256k1_callback* error_callback, secp256k1_scratch *scratch, secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n_points, size_t cb_offset) {
    secp256k1_gej* points;
    secp256k1_scalar* scalars;
    struct secp256k1_strauss_state state;
    size_t i;
    const size_t scratch_checkpoint = secp256k1_scratch_checkpoint(error_callback, scratch);

    secp256k1_gej_set_infinity(r);
    if (inp_g_sc == NULL && n_points == 0) {
        return 1;
    }

    points = (secp256k1_gej*)secp256k1_scratch_alloc(error_callback, scratch, n_points * sizeof(secp256k1_gej));
    scalars = (secp256k1_scalar*)secp256k1_scratch_alloc(error_callback, scratch, n_points * sizeof(secp256k1_scalar));
    state.prej = (secp256k1_gej*)secp256k1_scratch_alloc(error_callback, scratch, n_points * ECMULT_TABLE_SIZE(WINDOW_A) * sizeof(secp256k1_gej));
    state.zr = (secp256k1_fe*)secp256k1_scratch_alloc(error_callback, scratch, n_points * ECMULT_TABLE_SIZE(WINDOW_A) * sizeof(secp256k1_fe));
    state.pre_a = (secp256k1_ge*)secp256k1_scratch_alloc(error_callback, scratch, n_points * ECMULT_TABLE_SIZE(WINDOW_A) * sizeof(secp256k1_ge));
    state.pre_a_lam = (secp256k1_ge*)secp256k1_scratch_alloc(error_callback, scratch, n_points * ECMULT_TABLE_SIZE(WINDOW_A) * sizeof(secp256k1_ge));
    state.ps = (struct secp256k1_strauss_point_state*)secp256k1_scratch_alloc(error_callback, scratch, n_points * sizeof(struct secp256k1_strauss_point_state));

    if (points == NULL || scalars == NULL || state.prej == NULL || state.zr == NULL || state.pre_a == NULL || state.pre_a_lam == NULL || state.ps == NULL) {
        secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
        return 0;
    }

    for (i = 0; i < n_points; i++) {
        secp256k1_ge point;
        if (!cb(&scalars[i], &point, i+cb_offset, cbdata)) {
            secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
            return 0;
        }
        secp256k1_gej_set_ge(&points[i], &point);
    }
    secp256k1_ecmult_strauss_wnaf(&state, r, n_points, points, scalars, inp_g_sc);
    secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
    return 1;
}

/* Wrapper for secp256k1_ecmult_multi_func interface */
static int secp256k1_ecmult_strauss_batch_single(const secp256k1_callback* error_callback, secp256k1_scratch *scratch, secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n) {
    return secp256k1_ecmult_strauss_batch(error_callback, scratch, r, inp_g_sc, cb, cbdata, n, 0);
}

static size_t secp256k1_strauss_max_points(const secp256k1_callback* error_callback, secp256k1_scratch *scratch) {
    return secp256k1_scratch_max_allocation(error_callback, scratch, STRAUSS_SCRATCH_OBJECTS) / secp256k1_strauss_scratch_size(1);
}

/** Convert a number to WNAF notation.
 *  The number becomes represented by sum(2^{wi} * wnaf[i], i=0..WNAF_SIZE(w)+1) - return_val.
 *  It has the following guarantees:
 *  - each wnaf[i] is either 0 or an odd integer between -(1 << w) and (1 << w)
 *  - the number of words set is always WNAF_SIZE(w)
 *  - the returned skew is 0 or 1
 */
static int secp256k1_wnaf_fixed(int *wnaf, const secp256k1_scalar *s, int w) {
    int skew = 0;
    int pos;
    int max_pos;
    int last_w;
    const secp256k1_scalar *work = s;

    if (secp256k1_scalar_is_zero(s)) {
        for (pos = 0; pos < WNAF_SIZE(w); pos++) {
            wnaf[pos] = 0;
        }
        return 0;
    }

    if (secp256k1_scalar_is_even(s)) {
        skew = 1;
    }

    wnaf[0] = secp256k1_scalar_get_bits_var(work, 0, w) + skew;
    /* Compute last window size. Relevant when window size doesn't divide the
     * number of bits in the scalar */
    last_w = WNAF_BITS - (WNAF_SIZE(w) - 1) * w;

    /* Store the position of the first nonzero word in max_pos to allow
     * skipping leading zeros when calculating the wnaf. */
    for (pos = WNAF_SIZE(w) - 1; pos > 0; pos--) {
        int val = secp256k1_scalar_get_bits_var(work, pos * w, pos == WNAF_SIZE(w)-1 ? last_w : w);
        if(val != 0) {
            break;
        }
        wnaf[pos] = 0;
    }
    max_pos = pos;
    pos = 1;

    while (pos <= max_pos) {
        int val = secp256k1_scalar_get_bits_var(work, pos * w, pos == WNAF_SIZE(w)-1 ? last_w : w);
        if ((val & 1) == 0) {
            wnaf[pos - 1] -= (1 << w);
            wnaf[pos] = (val + 1);
        } else {
            wnaf[pos] = val;
        }
        /* Set a coefficient to zero if it is 1 or -1 and the proceeding digit
         * is strictly negative or strictly positive respectively. Only change
         * coefficients at previous positions because above code assumes that
         * wnaf[pos - 1] is odd.
         */
        if (pos >= 2 && ((wnaf[pos - 1] == 1 && wnaf[pos - 2] < 0) || (wnaf[pos - 1] == -1 && wnaf[pos - 2] > 0))) {
            if (wnaf[pos - 1] == 1) {
                wnaf[pos - 2] += 1 << w;
            } else {
                wnaf[pos - 2] -= 1 << w;
            }
            wnaf[pos - 1] = 0;
        }
        ++pos;
    }

    return skew;
}

struct secp256k1_pippenger_point_state {
    int skew_na;
    size_t input_pos;
};

struct secp256k1_pippenger_state {
    int *wnaf_na;
    struct secp256k1_pippenger_point_state* ps;
};

/*
 * pippenger_wnaf computes the result of a multi-point multiplication as
 * follows: The scalars are brought into wnaf with n_wnaf elements each. Then
 * for every i < n_wnaf, first each point is added to a "bucket" corresponding
 * to the point's wnaf[i]. Second, the buckets are added together such that
 * r += 1*bucket[0] + 3*bucket[1] + 5*bucket[2] + ...
 */
static int secp256k1_ecmult_pippenger_wnaf(secp256k1_gej *buckets, int bucket_window, struct secp256k1_pippenger_state *state, secp256k1_gej *r, const secp256k1_scalar *sc, const secp256k1_ge *pt, size_t num) {
    size_t n_wnaf = WNAF_SIZE(bucket_window+1);
    size_t np;
    size_t no = 0;
    int i;
    int j;

    for (np = 0; np < num; ++np) {
        if (secp256k1_scalar_is_zero(&sc[np]) || secp256k1_ge_is_infinity(&pt[np])) {
            continue;
        }
        state->ps[no].input_pos = np;
        state->ps[no].skew_na = secp256k1_wnaf_fixed(&state->wnaf_na[no*n_wnaf], &sc[np], bucket_window+1);
        no++;
    }
    secp256k1_gej_set_infinity(r);

    if (no == 0) {
        return 1;
    }

    for (i = n_wnaf - 1; i >= 0; i--) {
        secp256k1_gej running_sum;

        for(j = 0; j < ECMULT_TABLE_SIZE(bucket_window+2); j++) {
            secp256k1_gej_set_infinity(&buckets[j]);
        }

        for (np = 0; np < no; ++np) {
            int n = state->wnaf_na[np*n_wnaf + i];
            struct secp256k1_pippenger_point_state point_state = state->ps[np];
            secp256k1_ge tmp;
            int idx;

            if (i == 0) {
                /* correct for wnaf skew */
                int skew = point_state.skew_na;
                if (skew) {
                    secp256k1_ge_neg(&tmp, &pt[point_state.input_pos]);
                    secp256k1_gej_add_ge_var(&buckets[0], &buckets[0], &tmp, NULL);
                }
            }
            if (n > 0) {
                idx = (n - 1)/2;
                secp256k1_gej_add_ge_var(&buckets[idx], &buckets[idx], &pt[point_state.input_pos], NULL);
            } else if (n < 0) {
                idx = -(n + 1)/2;
                secp256k1_ge_neg(&tmp, &pt[point_state.input_pos]);
                secp256k1_gej_add_ge_var(&buckets[idx], &buckets[idx], &tmp, NULL);
            }
        }

        for(j = 0; j < bucket_window; j++) {
            secp256k1_gej_double_var(r, r, NULL);
        }

        secp256k1_gej_set_infinity(&running_sum);
        /* Accumulate the sum: bucket[0] + 3*bucket[1] + 5*bucket[2] + 7*bucket[3] + ...
         *                   = bucket[0] +   bucket[1] +   bucket[2] +   bucket[3] + ...
         *                   +         2 *  (bucket[1] + 2*bucket[2] + 3*bucket[3] + ...)
         * using an intermediate running sum:
         * running_sum = bucket[0] +   bucket[1] +   bucket[2] + ...
         *
         * The doubling is done implicitly by deferring the final window doubling (of 'r').
         */
        for(j = ECMULT_TABLE_SIZE(bucket_window+2) - 1; j > 0; j--) {
            secp256k1_gej_add_var(&running_sum, &running_sum, &buckets[j], NULL);
            secp256k1_gej_add_var(r, r, &running_sum, NULL);
        }

        secp256k1_gej_add_var(&running_sum, &running_sum, &buckets[0], NULL);
        secp256k1_gej_double_var(r, r, NULL);
        secp256k1_gej_add_var(r, r, &running_sum, NULL);
    }
    return 1;
}

/**
 * Returns optimal bucket_window (number of bits of a scalar represented by a
 * set of buckets) for a given number of points.
 */
static int secp256k1_pippenger_bucket_window(size_t n) {
    if (n <= 1) {
        return 1;
    } else if (n <= 4) {
        return 2;
    } else if (n <= 20) {
        return 3;
    } else if (n <= 57) {
        return 4;
    } else if (n <= 136) {
        return 5;
    } else if (n <= 235) {
        return 6;
    } else if (n <= 1260) {
        return 7;
    } else if (n <= 4420) {
        return 9;
    } else if (n <= 7880) {
        return 10;
    } else if (n <= 16050) {
        return 11;
    } else {
        return PIPPENGER_MAX_BUCKET_WINDOW;
    }
}

/**
 * Returns the maximum optimal number of points for a bucket_window.
 */
static size_t secp256k1_pippenger_bucket_window_inv(int bucket_window) {
    switch(bucket_window) {
        case 1: return 1;
        case 2: return 4;
        case 3: return 20;
        case 4: return 57;
        case 5: return 136;
        case 6: return 235;
        case 7: return 1260;
        case 8: return 1260;
        case 9: return 4420;
        case 10: return 7880;
        case 11: return 16050;
        case PIPPENGER_MAX_BUCKET_WINDOW: return SIZE_MAX;
    }
    return 0;
}


SECP256K1_INLINE static void secp256k1_ecmult_endo_split(secp256k1_scalar *s1, secp256k1_scalar *s2, secp256k1_ge *p1, secp256k1_ge *p2) {
    secp256k1_scalar tmp = *s1;
    secp256k1_scalar_split_lambda(s1, s2, &tmp);
    secp256k1_ge_mul_lambda(p2, p1);

    if (secp256k1_scalar_is_high(s1)) {
        secp256k1_scalar_negate(s1, s1);
        secp256k1_ge_neg(p1, p1);
    }
    if (secp256k1_scalar_is_high(s2)) {
        secp256k1_scalar_negate(s2, s2);
        secp256k1_ge_neg(p2, p2);
    }
}

/**
 * Returns the scratch size required for a given number of points (excluding
 * base point G) without considering alignment.
 */
static size_t secp256k1_pippenger_scratch_size(size_t n_points, int bucket_window) {
    size_t entries = 2*n_points + 2;
    size_t entry_size = sizeof(secp256k1_ge) + sizeof(secp256k1_scalar) + sizeof(struct secp256k1_pippenger_point_state) + (WNAF_SIZE(bucket_window+1)+1)*sizeof(int);
    return (sizeof(secp256k1_gej) << bucket_window) + sizeof(struct secp256k1_pippenger_state) + entries * entry_size;
}

static int secp256k1_ecmult_pippenger_batch(const secp256k1_callback* error_callback, secp256k1_scratch *scratch, secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n_points, size_t cb_offset) {
    const size_t scratch_checkpoint = secp256k1_scratch_checkpoint(error_callback, scratch);
    /* Use 2(n+1) with the endomorphism, when calculating batch
     * sizes. The reason for +1 is that we add the G scalar to the list of
     * other scalars. */
    size_t entries = 2*n_points + 2;
    secp256k1_ge *points;
    secp256k1_scalar *scalars;
    secp256k1_gej *buckets;
    struct secp256k1_pippenger_state *state_space;
    size_t idx = 0;
    size_t point_idx = 0;
    int i, j;
    int bucket_window;

    secp256k1_gej_set_infinity(r);
    if (inp_g_sc == NULL && n_points == 0) {
        return 1;
    }

    bucket_window = secp256k1_pippenger_bucket_window(n_points);
    points = (secp256k1_ge *) secp256k1_scratch_alloc(error_callback, scratch, entries * sizeof(*points));
    scalars = (secp256k1_scalar *) secp256k1_scratch_alloc(error_callback, scratch, entries * sizeof(*scalars));
    state_space = (struct secp256k1_pippenger_state *) secp256k1_scratch_alloc(error_callback, scratch, sizeof(*state_space));
    if (points == NULL || scalars == NULL || state_space == NULL) {
        secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
        return 0;
    }

    state_space->ps = (struct secp256k1_pippenger_point_state *) secp256k1_scratch_alloc(error_callback, scratch, entries * sizeof(*state_space->ps));
    state_space->wnaf_na = (int *) secp256k1_scratch_alloc(error_callback, scratch, entries*(WNAF_SIZE(bucket_window+1)) * sizeof(int));
    buckets = (secp256k1_gej *) secp256k1_scratch_alloc(error_callback, scratch, (1<<bucket_window) * sizeof(*buckets));
    if (state_space->ps == NULL || state_space->wnaf_na == NULL || buckets == NULL) {
        secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
        return 0;
    }

    if (inp_g_sc != NULL) {
        scalars[0] = *inp_g_sc;
        points[0] = secp256k1_ge_const_g;
        idx++;
        secp256k1_ecmult_endo_split(&scalars[0], &scalars[1], &points[0], &points[1]);
        idx++;
    }

    while (point_idx < n_points) {
        if (!cb(&scalars[idx], &points[idx], point_idx + cb_offset, cbdata)) {
            secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
            return 0;
        }
        idx++;
        secp256k1_ecmult_endo_split(&scalars[idx - 1], &scalars[idx], &points[idx - 1], &points[idx]);
        idx++;
        point_idx++;
    }

    secp256k1_ecmult_pippenger_wnaf(buckets, bucket_window, state_space, r, scalars, points, idx);

    /* Clear data */
    for(i = 0; (size_t)i < idx; i++) {
        secp256k1_scalar_clear(&scalars[i]);
        state_space->ps[i].skew_na = 0;
        for(j = 0; j < WNAF_SIZE(bucket_window+1); j++) {
            state_space->wnaf_na[i * WNAF_SIZE(bucket_window+1) + j] = 0;
        }
    }
    for(i = 0; i < 1<<bucket_window; i++) {
        secp256k1_gej_clear(&buckets[i]);
    }
    secp256k1_scratch_apply_checkpoint(error_callback, scratch, scratch_checkpoint);
    return 1;
}

/* Wrapper for secp256k1_ecmult_multi_func interface */
static int secp256k1_ecmult_pippenger_batch_single(const secp256k1_callback* error_callback, secp256k1_scratch *scratch, secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n) {
    return secp256k1_ecmult_pippenger_batch(error_callback, scratch, r, inp_g_sc, cb, cbdata, n, 0);
}

/**
 * Returns the maximum number of points in addition to G that can be used with
 * a given scratch space. The function ensures that fewer points may also be
 * used.
 */
static size_t secp256k1_pippenger_max_points(const secp256k1_callback* error_callback, secp256k1_scratch *scratch) {
    size_t max_alloc = secp256k1_scratch_max_allocation(error_callback, scratch, PIPPENGER_SCRATCH_OBJECTS);
    int bucket_window;
    size_t res = 0;

    for (bucket_window = 1; bucket_window <= PIPPENGER_MAX_BUCKET_WINDOW; bucket_window++) {
        size_t n_points;
        size_t max_points = secp256k1_pippenger_bucket_window_inv(bucket_window);
        size_t space_for_points;
        size_t space_overhead;
        size_t entry_size = sizeof(secp256k1_ge) + sizeof(secp256k1_scalar) + sizeof(struct secp256k1_pippenger_point_state) + (WNAF_SIZE(bucket_window+1)+1)*sizeof(int);

        entry_size = 2*entry_size;
        space_overhead = (sizeof(secp256k1_gej) << bucket_window) + entry_size + sizeof(struct secp256k1_pippenger_state);
        if (space_overhead > max_alloc) {
            break;
        }
        space_for_points = max_alloc - space_overhead;

        n_points = space_for_points/entry_size;
        n_points = n_points > max_points ? max_points : n_points;
        if (n_points > res) {
            res = n_points;
        }
        if (n_points < max_points) {
            /* A larger bucket_window may support even more points. But if we
             * would choose that then the caller couldn't safely use any number
             * smaller than what this function returns */
            break;
        }
    }
    return res;
}

/* Computes ecmult_multi by simply multiplying and adding each point. Does not
 * require a scratch space */
static int secp256k1_ecmult_multi_simple_var(secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n_points) {
    size_t point_idx;
    secp256k1_scalar szero;
    secp256k1_gej tmpj;

    secp256k1_scalar_set_int(&szero, 0);
    secp256k1_gej_set_infinity(r);
    secp256k1_gej_set_infinity(&tmpj);
    /* r = inp_g_sc*G */
    secp256k1_ecmult(r, &tmpj, &szero, inp_g_sc);
    for (point_idx = 0; point_idx < n_points; point_idx++) {
        secp256k1_ge point;
        secp256k1_gej pointj;
        secp256k1_scalar scalar;
        if (!cb(&scalar, &point, point_idx, cbdata)) {
            return 0;
        }
        /* r += scalar*point */
        secp256k1_gej_set_ge(&pointj, &point);
        secp256k1_ecmult(&tmpj, &pointj, &scalar, NULL);
        secp256k1_gej_add_var(r, r, &tmpj, NULL);
    }
    return 1;
}

/* Compute the number of batches and the batch size given the maximum batch size and the
 * total number of points */
static int secp256k1_ecmult_multi_batch_size_helper(size_t *n_batches, size_t *n_batch_points, size_t max_n_batch_points, size_t n) {
    if (max_n_batch_points == 0) {
        return 0;
    }
    if (max_n_batch_points > ECMULT_MAX_POINTS_PER_BATCH) {
        max_n_batch_points = ECMULT_MAX_POINTS_PER_BATCH;
    }
    if (n == 0) {
        *n_batches = 0;
        *n_batch_points = 0;
        return 1;
    }
    /* Compute ceil(n/max_n_batch_points) and ceil(n/n_batches) */
    *n_batches = 1 + (n - 1) / max_n_batch_points;
    *n_batch_points = 1 + (n - 1) / *n_batches;
    return 1;
}

typedef int (*secp256k1_ecmult_multi_func)(const secp256k1_callback* error_callback, secp256k1_scratch*, secp256k1_gej*, const secp256k1_scalar*, secp256k1_ecmult_multi_callback cb, void*, size_t);
static int secp256k1_ecmult_multi_var(const secp256k1_callback* error_callback, secp256k1_scratch *scratch, secp256k1_gej *r, const secp256k1_scalar *inp_g_sc, secp256k1_ecmult_multi_callback cb, void *cbdata, size_t n) {
    size_t i;

    int (*f)(const secp256k1_callback* error_callback, secp256k1_scratch*, secp256k1_gej*, const secp256k1_scalar*, secp256k1_ecmult_multi_callback cb, void*, size_t, size_t);
    size_t n_batches;
    size_t n_batch_points;

    secp256k1_gej_set_infinity(r);
    if (inp_g_sc == NULL && n == 0) {
        return 1;
    } else if (n == 0) {
        secp256k1_scalar szero;
        secp256k1_scalar_set_int(&szero, 0);
        secp256k1_ecmult(r, r, &szero, inp_g_sc);
        return 1;
    }
    if (scratch == NULL) {
        return secp256k1_ecmult_multi_simple_var(r, inp_g_sc, cb, cbdata, n);
    }

    /* Compute the batch sizes for Pippenger's algorithm given a scratch space. If it's greater than
     * a threshold use Pippenger's algorithm. Otherwise use Strauss' algorithm.
     * As a first step check if there's enough space for Pippenger's algo (which requires less space
     * than Strauss' algo) and if not, use the simple algorithm. */
    if (!secp256k1_ecmult_multi_batch_size_helper(&n_batches, &n_batch_points, secp256k1_pippenger_max_points(error_callback, scratch), n)) {
        return secp256k1_ecmult_multi_simple_var(r, inp_g_sc, cb, cbdata, n);
    }
    if (n_batch_points >= ECMULT_PIPPENGER_THRESHOLD) {
        f = secp256k1_ecmult_pippenger_batch;
    } else {
        if (!secp256k1_ecmult_multi_batch_size_helper(&n_batches, &n_batch_points, secp256k1_strauss_max_points(error_callback, scratch), n)) {
            return secp256k1_ecmult_multi_simple_var(r, inp_g_sc, cb, cbdata, n);
        }
        f = secp256k1_ecmult_strauss_batch;
    }
    for(i = 0; i < n_batches; i++) {
        size_t nbp = n < n_batch_points ? n : n_batch_points;
        size_t offset = n_batch_points*i;
        secp256k1_gej tmp;
        if (!f(error_callback, scratch, &tmp, i == 0 ? inp_g_sc : NULL, cb, cbdata, nbp, offset)) {
            return 0;
        }
        secp256k1_gej_add_var(r, r, &tmp, NULL);
        n -= nbp;
    }
    return 1;
}

#endif /* SECP256K1_ECMULT_IMPL_H */
