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

#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <http_parser.h>

#include <quant/quant.h>
#include <warpcore/warpcore.h>


struct conn_cache_entry {
    struct sockaddr_in dst;
    struct q_conn * c;
    splay_entry(conn_cache_entry) node;
};


struct conn_cache {
    splay_head(, conn_cache_entry);
};


static uint64_t timeout = 10;
static bool do_h3 = false;


static uint32_t __attribute__((nonnull))
conn_cmp(const struct conn_cache_entry * const a,
         const struct conn_cache_entry * const b)
{
    const uint32_t diff = (a->dst.sin_addr.s_addr > b->dst.sin_addr.s_addr) -
                          (a->dst.sin_addr.s_addr < b->dst.sin_addr.s_addr);
    if (diff)
        return diff;
    return (a->dst.sin_port > b->dst.sin_port) -
           (a->dst.sin_port < b->dst.sin_port);
}


SPLAY_PROTOTYPE(conn_cache, conn_cache_entry, node, conn_cmp)
SPLAY_GENERATE(conn_cache, conn_cache_entry, node, conn_cmp)


struct stream_entry {
    sl_entry(stream_entry) next;
    struct q_conn * c;
    struct q_stream * s;
    char * url;
};


static sl_head(stream_list, stream_entry) sl = sl_head_initializer(sl);


static void __attribute__((noreturn, nonnull)) usage(const char * const name,
                                                     const char * const ifname,
                                                     const char * const cache,
                                                     const char * const tls_log,
                                                     const bool verify_certs,
                                                     const bool flip_keys)
{
    printf("%s [options] URL\n", name);
    printf("\t[-i interface]\tinterface to run over; default %s\n", ifname);
    printf("\t[-s cache]\tTLS 0-RTT state cache; default %s\n", cache);
    printf("\t[-l log]\tlog file for TLS keys; default %s\n", tls_log);
    printf("\t[-t timeout]\tidle timeout in seconds; default %" PRIu64 "\n",
           timeout);
    printf("\t[-c]\t\tverify TLS certificates; default %s\n",
           verify_certs ? "true" : "false");
    printf("\t[-u]\tupdate TLS keys; default %s\n",
           flip_keys ? "true" : "false");
    printf("\t[-3]\t\tsend a static H3 request; default %s\n",
           do_h3 ? "true" : "false");
#ifndef NDEBUG
    printf("\t[-v verbosity]\tverbosity level (0-%d, default %d)\n", DLEVEL,
           util_dlevel);
#endif
    exit(0);
}


static void __attribute__((nonnull))
set_from_url(char * const var,
             const size_t len,
             const char * const url,
             const struct http_parser_url * const u,
             const enum http_parser_url_fields f,
             const char * const def)
{
    if ((u->field_set & (1 << f)) == 0) {
        strncpy(var, def, len);
        var[len - 1] = 0;
    } else {
        strncpy(var, &url[u->field_data[f].off], u->field_data[f].len);
        var[u->field_data[f].len] = 0;
    }
}

static struct q_conn * __attribute__((nonnull))
get(struct w_engine * const w,
    struct conn_cache * const cc,
    const char * const dest,
    const char * const port,
    const char * const path)
{
    struct addrinfo * peer;
    const struct addrinfo hints = {.ai_family = PF_INET,
                                   .ai_socktype = SOCK_DGRAM,
                                   .ai_protocol = IPPROTO_UDP};
    const int err = getaddrinfo(dest, port, &hints, &peer);
    if (err) {
        warn(ERR, "getaddrinfo: %s", gai_strerror(err));
        freeaddrinfo(peer);
        return 0;
    }

    // add to stream list
    struct stream_entry * se = calloc(1, sizeof(*se));
    ensure(se, "calloc failed");
    sl_insert_head(&sl, se, next);

    struct w_iov_sq req = w_iov_sq_initializer(req);
    if (do_h3) {
        // static const uint8_t h3_get_large[] = {
        //     0x01, 0x00, 0x00, 0xd1, 0x5f, 0x07, 0x04, 0x48, 0x54, 0x54, 0x50,
        //     0x51, 0x87, 0x62, 0x53, 0x50, 0x55, 0x84, 0x0c, 0xdf, 0x50, 0x8f,
        //     0xf1, 0xe3, 0xc2, 0xf4, 0x19, 0x25, 0x45, 0x65, 0x2c, 0x89, 0x29,
        //     0x27, 0x5c, 0x87, 0xa7, 0x5f, 0x50, 0x93, 0xce, 0x3b, 0x10, 0xa6,
        //     0x09, 0xa6, 0x2d, 0x89, 0xff, 0x48, 0x52, 0x91, 0xcc, 0x62, 0x29,
        //     0x1f, 0xa4, 0x95, 0x1f};

        // use a canned H/3 request that is equivalent of "GET /"
        static const uint8_t h3_get_slash[] = {
            0x32, 0x01, 0x00, 0x00, 0xd1, 0x5f, 0x07, 0x04, 0x48, 0x54, 0x54,
            0x50, 0xc1, 0x50, 0x8f, 0xf1, 0xe3, 0xc2, 0xf4, 0x19, 0x25, 0x45,
            0x65, 0x2c, 0x89, 0x29, 0x27, 0x5c, 0x87, 0xa7, 0x5f, 0x50, 0x93,
            0xce, 0x3b, 0x10, 0xa6, 0x09, 0xa6, 0x2d, 0x89, 0xff, 0x48, 0x52,
            0x91, 0xcc, 0x62, 0x29, 0x1f, 0xa4, 0x95, 0x1f};
        q_chunk_str(w, (const char *)h3_get_slash, sizeof(h3_get_slash), &req);
    } else {
        // assemble an HTTP/0.9 request
        char req_str[MAXPATHLEN + 6];
        const int req_str_len =
            snprintf(req_str, sizeof(req_str), "GET %s\r\n", path);
        q_chunk_str(w, req_str, (uint32_t)req_str_len, &req);
    }

    // do we have a connection open to this peer?
    const struct conn_cache_entry which = {
        .dst = *(struct sockaddr_in *)&peer->ai_addr};
    struct conn_cache_entry * cce = splay_find(conn_cache, cc, &which);
    if (cce == 0) {
        // no, open a new connection
        struct q_conn * const c =
            q_connect(w, (struct sockaddr_in *)(void *)peer->ai_addr, dest,
                      &req, &se->s, true, timeout);
        if (c == 0) {
            freeaddrinfo(peer);
            return 0;
        }

        cce = calloc(1, sizeof(*cce));
        ensure(cce, "calloc failed");
        cce->c = c;

        // insert into connection cache
        cce->dst = *(struct sockaddr_in *)&peer->ai_addr;
        splay_insert(conn_cache, cc, cce);

        if (do_h3) {
            // we need to open a uni stream for an empty H/3 SETTINGS frame
            struct q_stream * const ss = q_rsv_stream(cce->c, false);
            static const uint8_t h3_empty_settings[] = {0x43, 0x00, 0x04};
            // XXX lsquic doesn't like a FIN on this stream
            q_write_str(w, ss, (const char *)h3_empty_settings,
                        sizeof(h3_empty_settings), false);
            // q_close_stream(ss);
        }

    } else {
        se->s = q_rsv_stream(cce->c, true);
        q_write(se->s, &req, true);
    }
    se->c = cce->c;
    se->url = strdup(path);

    freeaddrinfo(peer);

    return cce->c; // NOLINT
}


static void __attribute__((nonnull)) free_cc(struct conn_cache * const cc)
{
    struct conn_cache_entry *i, *next;
    for (i = splay_min(conn_cache, cc); i != 0; i = next) {
        next = splay_next(conn_cache, cc, i);
        splay_remove(conn_cache, cc, i);
        free(i);
    }
}


static void free_sl(void)
{
    struct stream_entry *i = 0, *tmp = 0;
    sl_foreach_safe (i, &sl, next, tmp) {
        sl_remove(&sl, i, stream_entry, next);
        free(i->url);
        free(i);
    }
}


int main(int argc, char * argv[])
{
#ifndef NDEBUG
    util_dlevel = DLEVEL; // default to maximum compiled-in verbosity
#endif
    char ifname[IFNAMSIZ] = "lo"
#ifndef __linux__
                            "0"
#endif
        ;
    int ch;
    char cache[MAXPATHLEN] = "/tmp/" QUANT "-session";
    char tls_log[MAXPATHLEN] = "/tmp/" QUANT "-tlslog";
    bool verify_certs = false;
    bool flip_keys = false;
    int ret = 0;

    while ((ch = getopt(argc, argv, "hi:v:s:t:l:cu3")) != -1) {
        switch (ch) {
        case 'i':
            strncpy(ifname, optarg, sizeof(ifname) - 1);
            break;
        case 's':
            strncpy(cache, optarg, sizeof(cache) - 1);
            break;
        case 't':
            timeout = MIN(600, strtoul(optarg, 0, 10)); // 10 min
            break;
        case 'l':
            strncpy(tls_log, optarg, sizeof(tls_log) - 1);
            break;
        case 'c':
            verify_certs = true;
            break;
        case 'u':
            flip_keys = true;
            break;
        case '3':
            do_h3 = true;
            break;
        case 'v':
#ifndef NDEBUG
            util_dlevel = (short)MIN(DLEVEL, strtoul(optarg, 0, 10));
#endif
            break;
        case 'h':
        case '?':
        default:
            usage(basename(argv[0]), ifname, cache, tls_log, verify_certs,
                  flip_keys);
        }
    }

    struct w_engine * const w =
        q_init(ifname, &(const struct q_conf){.num_bufs = 100000,
                                              .ticket_store = cache,
                                              .tls_log = tls_log,
                                              .verify_certs = verify_certs,
                                              .flip_keys = flip_keys});
    struct conn_cache cc = splay_initializer(cc);
    struct http_parser_url u = {0};

    while (optind < argc) {
        // parse and verify the URIs passed on the command line
        const char * const url = argv[optind++];
        http_parser_parse_url(url, strlen(url), 0, &u);
        ensure((u.field_set & (1 << UF_USERINFO)) == 0 &&
                   (u.field_set & (1 << UF_QUERY)) == 0 &&
                   (u.field_set & (1 << UF_FRAGMENT)) == 0,
               "unsupported URL components");

        // extract relevant info from URL
        char dest[1024], port[64], path[2048];
        set_from_url(dest, sizeof(dest), url, &u, UF_HOST, "localhost");
        set_from_url(port, sizeof(port), url, &u, UF_PORT, "4433");
        set_from_url(path, sizeof(path), url, &u, UF_PATH, "/index.html");

        // open a new connection, or get an open one
        warn(INF, "%s retrieving %s", basename(argv[0]), url);
        if (get(w, &cc, dest, port, path) == 0) {
            // q_connect() failed
            ret = 1;
            goto done;
        }
    }

    // collect the replies
    struct stream_entry * se = 0;
    sl_foreach (se, &sl, next) {
        // read HTTP/0.9 reply and dump it to stdout
        struct w_iov_sq i = w_iov_sq_initializer(i);
        if (se->c == 0)
            continue;

        q_readall_stream(se->s, &i);
        q_close_stream(se->s);
        if (w_iov_sq_cnt(&i) == 0) {
            // no data read
            ret = 1;
            goto done;
        }

        char * const slash = strrchr(se->url, '/');
        if (slash && *(slash + 1) == 0)
            // this URL ends in a slash, so strip that to name the file
            *slash = 0;
        const int fd =
            open(*basename(se->url) == 0 ? "index.html" : basename(se->url),
                 O_CREAT | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP);
        ensure(fd != -1, "cannot open %s", basename(se->url));

        // save the object, and print its first three packets to stdout
        struct w_iov * v;
        uint32_t n = 0;
        sq_foreach (v, &i, next) {
            ensure(write(fd, v->buf, v->len) != -1, "cannot write");
            if (n < 4 || v == sq_last(&i, w_iov, next)) {
                // don't print newlines to console log
                for (uint16_t p = 0; p < v->len; p++)
                    if (v->buf[p] == '\n' || v->buf[p] == '\r')
                        v->buf[p] = ' ';
                if (do_h3)
                    hexdump(v->buf, v->len);
                else
                    printf("%.*s", v->len, v->buf);
            } else
                printf(".");
            n++;
        }
        printf("\n");
        close(fd);
        q_free(&i);
    }

done:
    q_cleanup(w);
    free_cc(&cc);
    free_sl();
    warn(DBG, "%s exiting", basename(argv[0]));
    return ret;
}
