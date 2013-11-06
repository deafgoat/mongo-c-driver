/*
 * Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE

#include <string.h>
#include <unistd.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "mongoc-counters-private.h"
#include "mongoc-stream-tls.h"
#include "mongoc-ssl-private.h"
#include "mongoc-trace.h"
#include "mongoc-log.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "stream-tls"

/** Private storage for handling callbacks from mongoc_stream and BIO_*
 *
 * The one funny wrinkle comes with timeout, which we use statefully to
 * statefully pass timeouts through from the mongoc-stream api.
 *
 * TODO: is there a cleaner way to manage that?
 */
typedef struct
{
   mongoc_stream_t  parent;
   mongoc_stream_t *base_stream;
   BIO             *bio;
   SSL_CTX         *ctx;
   bson_int32_t     timeout;
   bson_bool_t      weak_cert_validation;
} mongoc_stream_tls_t;


static int
mongoc_stream_tls_bio_create (BIO *b);
static int
mongoc_stream_tls_bio_destroy (BIO *b);
static int
mongoc_stream_tls_bio_read (BIO  *b,
                            char *buf,
                            int   len);
static int
mongoc_stream_tls_bio_write (BIO        *b,
                             const char *buf,
                             int         len);
static long
mongoc_stream_tls_bio_ctrl (BIO  *b,
                            int   cmd,
                            long  num,
                            void *ptr);
static int
mongoc_stream_tls_bio_gets (BIO  *b,
                            char *buf,
                            int   len);
static int
mongoc_stream_tls_bio_puts (BIO        *b,
                            const char *str);


/* Magic vtable to make our BIO shim */
static BIO_METHOD gMongocStreamTlsRawMethods = {
   BIO_TYPE_FILTER,
   "mongoc-stream-tls-glue",
   mongoc_stream_tls_bio_write,
   mongoc_stream_tls_bio_read,
   mongoc_stream_tls_bio_puts,
   mongoc_stream_tls_bio_gets,
   mongoc_stream_tls_bio_ctrl,
   mongoc_stream_tls_bio_create,
   mongoc_stream_tls_bio_destroy
};


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_create --
 *
 *       BIO callback to create a new BIO instance.
 *
 * Returns:
 *       1 if successful.
 *
 * Side effects:
 *       @b is initialized.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_create (BIO *b) /* IN */
{
   BSON_ASSERT (b);

   b->init = 1;
   b->num = 0;
   b->ptr = NULL;
   b->flags = 0;

   return 1;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_destroy --
 *
 *       Release resources associated with BIO.
 *
 * Returns:
 *       1 if successful.
 *
 * Side effects:
 *       @b is destroyed.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_destroy (BIO *b) /* IN */
{
   mongoc_stream_tls_t *tls;

   BSON_ASSERT (b);

   if (!(tls = b->ptr)) {
      return -1;
   }

   b->ptr = NULL;
   b->init = 0;
   b->flags = 0;

   tls->bio = NULL;

   return 1;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_read --
 *
 *       Read from the underlying stream to BIO.
 *
 * Returns:
 *       -1 on failure; otherwise the number of bytes read.
 *
 * Side effects:
 *       @buf is filled with data read from underlying stream.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_read (BIO  *b,   /* IN */
                            char *buf, /* OUT */
                            int   len) /* IN */
{
   mongoc_stream_tls_t *tls;
   int ret;

   BSON_ASSERT (b);
   BSON_ASSERT (buf);

   if (!(tls = b->ptr)) {
      return -1;
   }

   errno = 0;
   ret = mongoc_stream_read (tls->base_stream, buf, len, 0, tls->timeout);
   BIO_clear_retry_flags (b);

   if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      BIO_set_retry_read (b);
   }

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_write --
 *
 *       Write to the underlying stream on behalf of BIO.
 *
 * Returns:
 *       -1 on failure; otherwise the number of bytes written.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_write (BIO        *b,   /* IN */
                             const char *buf, /* IN */
                             int         len) /* IN */
{
   mongoc_stream_tls_t *tls;
   struct iovec iov;
   int ret;

   BSON_ASSERT (b);
   BSON_ASSERT (buf);

   if (!(tls = b->ptr)) {
      return -1;
   }

   iov.iov_base = (void *)buf;
   iov.iov_len = len;

   errno = 0;
   ret = mongoc_stream_writev (tls->base_stream, &iov, 1, tls->timeout);
   BIO_clear_retry_flags (b);

   if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      BIO_set_retry_write (b);
   }

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_ctrl --
 *
 *       Handle ctrl callback for BIO.
 *
 * Returns:
 *       ioctl dependent.
 *
 * Side effects:
 *       ioctl dependent.
 *
 *--------------------------------------------------------------------------
 */

static long
mongoc_stream_tls_bio_ctrl (BIO  *b,   /* IN */
                            int   cmd, /* IN */
                            long  num, /* IN */
                            void *ptr) /* INOUT */
{
   switch (cmd) {
   case BIO_CTRL_FLUSH:
      return 1;
   default:
      return 0;
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_gets --
 *
 *       BIO callback for gets(). Not supported.
 *
 * Returns:
 *       -1 always.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_gets (BIO  *b,   /* IN */
                            char *buf, /* OUT */
                            int   len) /* IN */
{
   return -1;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_bio_puts --
 *
 *       BIO callback to perform puts(). Just calls the actual write
 *       callback.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_bio_puts (BIO        *b,   /* IN */
                            const char *str) /* IN */
{
   return mongoc_stream_tls_bio_write (b, str, strlen (str));
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_destroy --
 *
 *       Cleanup after usage of a mongoc_stream_tls_t. Free all allocated
 *       resources and ensure connections are closed.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
mongoc_stream_tls_destroy (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   BIO_free_all (tls->bio);
   tls->bio = NULL;

   mongoc_stream_destroy (tls->base_stream);
   tls->base_stream = NULL;

   SSL_CTX_free (tls->ctx);
   tls->ctx = NULL;

   bson_free (stream);

   mongoc_counter_streams_active_dec();
   mongoc_counter_streams_disposed_inc();
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_close --
 *
 *       Close the underlying socket.
 *
 *       Linus dictates that you should not check the result of close()
 *       since there is a race condition with EAGAIN and a new file
 *       descriptor being opened.
 *
 * Returns:
 *       0 on success; otherwise -1.
 *
 * Side effects:
 *       The BIO fd is closed.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_close (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   return mongoc_stream_close (tls->base_stream);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_flush --
 *
 *       Flush the underlying stream.
 *
 * Returns:
 *       0 if successful; otherwise -1.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_flush (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   return BIO_flush (tls->bio);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_writev --
 *
 *       Write the iovec to the stream. This function will try to write
 *       all of the bytes or fail. If the number of bytes is not equal
 *       to the number requested, a failure or EOF has occurred.
 *
 * Returns:
 *       -1 on failure, otherwise the number of bytes written.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static ssize_t
mongoc_stream_tls_writev (mongoc_stream_t *stream,       /* IN */
                          struct iovec    *iov,          /* IN */
                          size_t           iovcnt,       /* IN */
                          bson_int32_t     timeout_msec) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   ssize_t ret = 0;
   size_t i;
   int write_ret;

   BSON_ASSERT (tls);
   BSON_ASSERT (iov);
   BSON_ASSERT (iovcnt);

   tls->timeout = timeout_msec;

   for (i = 0; i < iovcnt; i++) {
      write_ret = BIO_write (tls->bio, iov[i].iov_base, iov[i].iov_len);

      if (write_ret != iov[i].iov_len) {
         return write_ret;
      }

      ret += write_ret;
   }

   if (ret >= 0) {
      mongoc_counter_streams_egress_add(ret);
   }

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_readv --
 *
 *       Read from the stream into iov. This function will try to read
 *       all of the bytes or fail. If the number of bytes is not equal
 *       to the number requested, a failure or EOF has occurred.
 *
 * Returns:
 *       -1 on failure, 0 on EOF, otherwise the number of bytes read.
 *
 * Side effects:
 *       iov buffers will be written to.
 *
 *--------------------------------------------------------------------------
 */

static ssize_t
mongoc_stream_tls_readv (mongoc_stream_t *stream,       /* IN */
                         struct iovec    *iov,          /* INOUT */
                         size_t           iovcnt,       /* IN */
                         size_t           min_bytes,    /* IN */
                         bson_int32_t     timeout_msec) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   ssize_t ret = 0;
   size_t i;
   int read_ret;
   int iov_pos = 0;
   bson_int64_t now;
   bson_int64_t expire;

   BSON_ASSERT (tls);
   BSON_ASSERT (iov);
   BSON_ASSERT (iovcnt);

   tls->timeout = timeout_msec;

   expire = bson_get_monotonic_time () + (timeout_msec * 1000UL);

   for (i = 0; i < iovcnt; i++) {
      iov_pos = 0;

      while (iov_pos < iov[i].iov_len - 1) {
         read_ret = BIO_read (tls->bio, iov[i].iov_base + iov_pos,
                              iov[i].iov_len - iov_pos);

         now = bson_get_monotonic_time ();

         if (((expire - now) < 0) && (read_ret == 0)) {
            mongoc_counter_streams_timeout_inc();
            errno = ETIMEDOUT;
            return -1;
         }

         if (read_ret == -1) {
            return read_ret;
         }

         ret += read_ret;

         if (read_ret != iov[i].iov_len) {
            if (read_ret >= min_bytes) {
               return read_ret;
            }

            iov_pos += read_ret;
         }
      }
   }

   if (ret >= 0) {
      mongoc_counter_streams_ingress_add(ret);
   }

   return ret;
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_cork --
 *
 *       This function is not supported on mongoc_stream_tls_t.
 *
 * Returns:
 *       0 always.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_cork (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (stream);

   return mongoc_stream_cork (tls->base_stream);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_uncork --
 *
 *       The function is not supported on mongoc_stream_tls_t.
 *
 * Returns:
 *       0 always.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_uncork (mongoc_stream_t *stream) /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (stream);

   return mongoc_stream_uncork (tls->base_stream);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_setsockopt --
 *
 *       Perform a setsockopt on the underlying stream.
 *
 * Returns:
 *       -1 on failure, otherwise opt specific value.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static int
mongoc_stream_tls_setsockopt (mongoc_stream_t *stream,  /* IN */
                              int              level,   /* IN */
                              int              optname, /* IN */
                              void            *optval,  /* INOUT */
                              socklen_t        optlen)  /* IN */
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   return mongoc_stream_setsockopt (tls->base_stream,
                                    level,
                                    optname,
                                    optval,
                                    optlen);
}


/** force an ssl handshake
 *
 * This will happen on the first read or write otherwise
 */
bson_bool_t
mongoc_stream_tls_do_handshake (mongoc_stream_t *stream,
                                bson_int32_t     timeout_msec)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;

   BSON_ASSERT (tls);

   tls->timeout = timeout_msec;

   return BIO_do_handshake (tls->bio) == 1;
}


/** check the cert returned by the other party */
bson_bool_t
mongoc_stream_tls_check_cert (mongoc_stream_t *stream,
                              const char      *host)
{
   mongoc_stream_tls_t *tls = (mongoc_stream_tls_t *)stream;
   SSL *ssl;

   BSON_ASSERT (tls);
   BSON_ASSERT (host);

   BIO_get_ssl (tls->bio, &ssl);

   return mongoc_ssl_check_cert (ssl, host, tls->weak_cert_validation);
}


/*
 *--------------------------------------------------------------------------
 *
 * mongoc_stream_tls_new --
 *
 *       Creates a new mongoc_stream_tls_t to communicate with a remote
 *       server using a TLS stream.
 *
 *       @base_stream should be a stream that will become owned by the
 *       resulting tls stream. It will be used for raw I/O.
 *
 *       @trust_store_dir should be a path to the SSL cert db to use for
 *       verifying trust of the remote server.
 *
 * Returns:
 *       NULL on failure, otherwise a mongoc_stream_t.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

mongoc_stream_t *
mongoc_stream_tls_new (mongoc_stream_t  *base_stream,
                       mongoc_ssl_opt_t *opt,
                       int               client)
{
   mongoc_stream_tls_t *tls;
   SSL_CTX *ssl_ctx = NULL;

   BIO *bio_ssl = NULL;
   BIO *bio_mongoc_shim = NULL;

   BSON_ASSERT(base_stream);
   BSON_ASSERT(opt);

   ssl_ctx = mongoc_ssl_ctx_new (opt);

   if (!ssl_ctx) {
      return NULL;
   }

   bio_ssl = BIO_new_ssl (ssl_ctx, client);
   bio_mongoc_shim = BIO_new (&gMongocStreamTlsRawMethods);

   BIO_push (bio_ssl, bio_mongoc_shim);

   tls = bson_malloc0 (sizeof *tls);
   tls->base_stream = base_stream;
   tls->parent.destroy = mongoc_stream_tls_destroy;
   tls->parent.close = mongoc_stream_tls_close;
   tls->parent.flush = mongoc_stream_tls_flush;
   tls->parent.writev = mongoc_stream_tls_writev;
   tls->parent.readv = mongoc_stream_tls_readv;
   tls->parent.cork = mongoc_stream_tls_cork;
   tls->parent.uncork = mongoc_stream_tls_uncork;
   tls->parent.setsockopt = mongoc_stream_tls_setsockopt;
   tls->weak_cert_validation = opt->weak_cert_validation;
   tls->bio = bio_ssl;
   tls->ctx = ssl_ctx;
   tls->timeout = -1;
   bio_mongoc_shim->ptr = tls;

   mongoc_counter_streams_active_inc();

   return (mongoc_stream_t *)tls;
}
