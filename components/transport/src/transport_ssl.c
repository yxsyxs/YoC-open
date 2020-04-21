// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#if defined(CONFIG_USING_TLS)
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <aos/log.h>
#include <aos/kernel.h>
#include "tls.h"
#include "transport.h"
#include "transport_ssl.h"
#include "transport_utils.h"

static const char *TAG = "TRANS_SSL";

typedef enum {
    TRANS_SSL_INIT = 0,
    TRANS_SSL_CONNECTING,
} transport_ssl_conn_state_t;

/**
 *  mbedtls specific transport data
 */
typedef struct {
    tls_t                *tls;
    tls_cfg_t            cfg;
    bool                     ssl_initialized;
    transport_ssl_conn_state_t conn_state;
} transport_ssl_t;

static int ssl_close(transport_handle_t t);

static int ssl_connect_async(transport_handle_t t, const char *host, int port, int timeout_ms)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (ssl->conn_state == TRANS_SSL_INIT) {
        ssl->cfg.timeout_ms = timeout_ms;
        ssl->cfg.non_block = true;
        ssl->ssl_initialized = true;
        ssl->tls = calloc(1, sizeof(tls_t));
        if (!ssl->tls) {
            return -1;
        }
        ssl->conn_state = TRANS_SSL_CONNECTING;
    }
    if (ssl->conn_state == TRANS_SSL_CONNECTING) {
        return tls_conn_new_async(host, strlen(host), port, &ssl->cfg, ssl->tls);
    }
    return 0;
}

static int ssl_connect(transport_handle_t t, const char *host, int port, int timeout_ms)
{
    transport_ssl_t *ssl = transport_get_context_data(t);

    ssl->cfg.timeout_ms = timeout_ms;
    ssl->ssl_initialized = true;
    ssl->tls = tls_conn_new(host, strlen(host), port, &ssl->cfg);
    if (!ssl->tls) {
        LOGE(TAG, "Failed to open a new connection");
        return -1;
    }
    return 0;
}

static int ssl_poll_read(transport_handle_t t, int timeout_ms)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    fd_set readset;
    FD_ZERO(&readset);
    FD_SET(ssl->tls->sockfd, &readset);
    struct timeval timeout;
    transport_utils_ms_to_timeval(timeout_ms, &timeout);

    return select(ssl->tls->sockfd + 1, &readset, NULL, NULL, &timeout);
}

static int ssl_poll_write(transport_handle_t t, int timeout_ms)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    fd_set writeset;
    FD_ZERO(&writeset);
    FD_SET(ssl->tls->sockfd, &writeset);
    struct timeval timeout;
    transport_utils_ms_to_timeval(timeout_ms, &timeout);
    return select(ssl->tls->sockfd + 1, NULL, &writeset, NULL, &timeout);
}

static int ssl_write(transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    int poll, ret;
    transport_ssl_t *ssl = transport_get_context_data(t);

    if ((poll = transport_poll_write(t, timeout_ms)) <= 0) {
        LOGW(TAG, "Poll timeout or error, errno=%s, fd=%d, timeout_ms=%d", strerror(errno), ssl->tls->sockfd, timeout_ms);
        return poll;
    }
    ret = tls_conn_write(ssl->tls, (const unsigned char *) buffer, len);
    if (ret < 0) {
        LOGE(TAG, "tls_conn_write error, errno=%s", strerror(errno));
    }
    return ret;
}

static int ssl_read(transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    int poll, ret;
    transport_ssl_t *ssl = transport_get_context_data(t);

    LOGD(TAG, "ssl read...");
    if (tls_get_bytes_avail(ssl->tls) <= 0) {
        if ((poll = transport_poll_read(t, timeout_ms)) <= 0) {
            return poll;
        }
    }
    // ret = tls_conn_read(ssl->tls, (unsigned char *)buffer, len, timeout_ms);
    ret = tls_conn_read(ssl->tls, (unsigned char *)buffer, len, 10);
    if (ret < 0) {
        LOGE(TAG, "tls_conn_read error, errno=%s", strerror(errno));
    }
    if (ret == 0) {
        ret = -1;
    }
    return ret;
}

static int ssl_close(transport_handle_t t)
{
    int ret = -1;
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (ssl->ssl_initialized) {
        tls_conn_delete(ssl->tls);
        ssl->ssl_initialized = false;
    }
    return ret;
}

static int ssl_destroy(transport_handle_t t)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    transport_close(t);
    free(ssl);
    return 0;
}

void transport_ssl_enable_global_ca_store(transport_handle_t t)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (t && ssl) {
        ssl->cfg.use_global_ca_store = true;
    }
}

void transport_ssl_set_cert_data(transport_handle_t t, const char *data, int len)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (t && ssl) {
        ssl->cfg.cacert_pem_buf = (void *)data;
        ssl->cfg.cacert_pem_bytes = len + 1;
    }
}

void transport_ssl_set_client_cert_data(transport_handle_t t, const char *data, int len)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (t && ssl) {
        ssl->cfg.clientcert_pem_buf = (void *)data;
        ssl->cfg.clientcert_pem_bytes = len + 1;
    }
}

void transport_ssl_set_client_key_data(transport_handle_t t, const char *data, int len)
{
    transport_ssl_t *ssl = transport_get_context_data(t);
    if (t && ssl) {
        ssl->cfg.clientkey_pem_buf = (void *)data;
        ssl->cfg.clientkey_pem_bytes = len + 1;
    }
}

transport_handle_t transport_ssl_init()
{
    transport_handle_t t = transport_init();
    transport_ssl_t *ssl = calloc(1, sizeof(transport_ssl_t));
    TRANSPORT_MEM_CHECK(TAG, ssl, return NULL);
    transport_set_context_data(t, ssl);
    transport_set_func(t, ssl_connect, ssl_read, ssl_write, ssl_close, ssl_poll_read, ssl_poll_write, ssl_destroy);
    transport_set_async_connect_func(t, ssl_connect_async);
    return t;
}

#endif