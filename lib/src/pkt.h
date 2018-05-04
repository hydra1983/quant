// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2018, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "quic.h"

#define MAX_PKT_LEN 1252
#define MIN_INI_LEN 1200

#define F_LONG_HDR 0x80

#define F_LH_INIT 0x7F
#define F_LH_RTRY 0x7E
#define F_LH_HSHK 0x7D
#define F_LH_0RTT 0x7C

#define F_SH_KYPH 0x40
#define F_SH_RSV1 0x20
#define F_SH_RSV2 0x10
#define F_SH_GOOG 0x08
#define F_SH_EXP 0x04

#define F_SH 0x30

#define F_SH_4OCT 0x02
#define F_SH_2OCT 0x01
#define F_SH_1OCT 0x00

#define ERR_NO_ERR 0x0
#define ERR_INTERNAL_ERR 0x1
#define ERR_FLOW_CONTROL_ERR 0x3
// #define ERR_STREAM_ID_ERR 0x4
// #define ERR_STREAM_STATE_ERR 0x5
// #define ERR_FINAL_OFFSET_ERR 0x6
// #define ERR_FRAME_FORMAT_ERR 0x7
// #define ERR_TRANSPORT_PARAMETER_ERR 0x8
// #define ERR_VERSION_NEGOTIATION_ERR 0x9
// #define ERR_PROTOCOL_VIOLATION 0xA
#define ERR_TLS_HSHAKE_FAIL 0x201
// #define ERR_TLS_FATL_ALRT_GEN 0x202
// #define ERR_TLS_FATL_ALRT_REC 0x203
#define ERR_FRAME_ERR(type) ((0x1 << 8) | (type))


#define pkt_type(flags) ((flags) & (is_set(F_LONG_HDR, (flags)) ? ~0x80 : 0x03))


struct q_conn;
struct q_stream;
struct w_iov;
struct w_iov_sq;

extern void __attribute__((nonnull))
dec_pkt_hdr_initial(const struct w_iov * const v, const bool is_clnt);

extern void __attribute__((nonnull))
dec_pkt_hdr_remainder(struct w_iov * const v,
                      struct q_conn * const c,
                      struct w_iov_sq * const i);

extern bool __attribute__((nonnull)) enc_pkt(struct q_stream * const s,
                                             const bool rtx,
                                             struct w_iov * const v,
                                             struct w_iov_sq * const q);

#ifndef NDEBUG
extern void __attribute__((nonnull)) log_pkt(const char * const dir,
                                             const struct w_iov * const v,
                                             const uint16_t len);
#else
#define log_pkt(...)                                                           \
    do {                                                                       \
    } while (0)
#endif
