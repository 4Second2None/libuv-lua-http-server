/*
 * Copyright Erik Dubbelboer. and other contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>  /* assert()             */
#include <string.h>  /* strncat(), strncpy() */
#include <stdio.h>   /* snprintf()           */

#include "http_parser.h"

#include "webserver.h"


#ifndef MIN
#define MIN(a,b) (((a)>(b))?(b):(a))
#endif


typedef struct webio_s {
  union {
    uv_pipe_t pipe;
    uv_tcp_t  tcp;
  } handle;

  http_parser parser;
  uv_timer_t  timeout;
  
  uv_write_t write_req;

#ifdef HAVE_KEEP_ALIVE
  int keep_alive;
#endif
  
  char*  header;
  size_t header_size;

  webclient_t client;
} webio_t;


static http_parser_settings parser_settings;


static void after_close_timeout(uv_handle_t* handle) {
  webio_t* io = (webio_t*)handle->data;

  /* Free all memory that was allocated for this connection. */
  pool_reset(&io->client.pool);

  free(io);
}


static void after_close(uv_handle_t* handle) {
  webio_t* io = (webio_t*)handle->data;

  --io->client.server->connected;

  assert(io->client.server->connected >= 0);

  io->client.server->close_cb(&io->client);

  /* We can't just free the io here, we need to close the timer first.
   * (even when it's not active anymore).
   */
  uv_close((uv_handle_t*)&io->timeout, after_close_timeout);
}


static void do_close(webio_t* io) {
  uv_timer_stop(&io->timeout);

  if (!uv_is_closing((uv_handle_t*)&io->handle)) {
    uv_close((uv_handle_t*)&io->handle, after_close);
  }
}


static void on_timeout(uv_timer_t* handle, int status) {
  (void)status;

  webio_t* io = (webio_t*)handle->data;

  assert(status == 0);
  (void)status;  /* For release builds. */

  do_close(io);
}


static void after_write(uv_write_t* req, int status) {
  webio_t* io = (webio_t*)req->data;

  /* status will be set to UV_ECANCELED when the connection
   * is closed durint the write.
   */
  assert((status == 0) || (status != UV_ECANCELED));
  (void)status;  /* For release builds. */

  /* Stop the write timeout. */
  uv_timer_stop(&io->timeout);

  if (!uv_is_closing((uv_handle_t*)&io->handle)) {
    if (io->client.server->_closing) {
      do_close(io);
    }
#ifdef HAVE_KEEP_ALIVE
    else if (io->keep_alive) {
      uv_timer_start(&io->timeout, on_timeout, WEBSERVER_KEEPALIVE_TIMEOUT, 0);
    }
#endif
    else {
      /* With http 1.0 client such as ApacheBench we need to close
       * the connection our selves.
       */
      if (io->client.version <= 10) {
        do_close(io);
      } else {
        /* We prefer the client to disconnect so we don't end up with
         * to many socket in the TIME_WAIT state. So set a timeout
         * after which we close the socket.
         */
        uv_timer_start(&io->timeout, on_timeout, 5000, 0);
      }
    }
  }
}


void webserver_respond(webclient_t* client, char* response, size_t size, uint32_t timeout) {
  webio_t* io = client->_io;

  /* Start a write timeout. */
  uv_timer_start(&io->timeout, on_timeout, timeout ? timeout : WEBSERVER_WRITE_TIMEOUT, 0);

  if (response) {
    uv_buf_t buf = uv_buf_init(response, size);
  
    if (uv_write(&io->write_req, (uv_stream_t*)&io->handle, &buf, 1, after_write) != 0) {
      do_close(io);
    }
  } else {
    do_close(io);
  }
}


static int on_message_complete(http_parser *p) {
  webio_t* io = (webio_t*)p->data;
  
  io->client.version = (p->http_major * 10) + p->http_minor;

#ifdef HAVE_KEEP_ALIVE
  if (io->keep_alive && http_should_keep_alive(p) && (io->client.version > 0)) {
    io->keep_alive = 1;
  }
#endif

  uv_timer_stop(&io->timeout);

  io->client.server->handle_cb(&io->client);

  return 0;
}


static int on_body(http_parser *p, const char *buf, size_t len) {
  (void)p;
  (void)buf;
  (void)len;

  return 0;
}


static int on_header_value(http_parser *p, const char *buf, size_t len) {
  webio_t* io = (webio_t*)p->data;

  if (io->header) {
    strncat(io->header, buf, MIN(io->header_size - strlen(io->header), len));
  }

  return 0;
}


static int on_header_field(http_parser *p, const char *buf, size_t len) {
  webio_t* io = (webio_t*)p->data;

  /* "If src contains n or more bytes, strncat() writes n+1 bytes to dest."
   * Because of this we need to do size - 1.
   */

  if (0) {
  }
#define WEBSERVER_PARSE(name, str, size)                            \
  else if (strncasecmp(buf, str, len) == 0) {                       \
    if (!io->client.name) {                                         \
      io->client.name = (char*)pool_malloc(&io->client.pool, size); \
      io->client.name[0] = 0;                                       \
    }                                                               \
    io->header      = io->client.name;                              \
    io->header_size = size - 1;                                     \
  }
  WEBSERVER_HEADERS(WEBSERVER_PARSE)
#undef WEBSERVER_PARSE
  else {
    io->header = 0;
  }

  return 0;
}


static int on_url(http_parser *p, const char *buf, size_t len) {
  webio_t* io = (webio_t*)p->data;

  /* Calculating the mininum length is faster because strncpy pads the destination
   * with zeros until a total of len characters have been written.
   */
  len = MIN(len, sizeof(io->client.url) - 1);
  strncpy(io->client.url, buf, len);
  io->client.url[len] = 0;

  return 0;
}


static int on_message_begin(http_parser *p) {
  webio_t* io = (webio_t*)p->data;
  
  io->client.method = p->method;

#define WEBSERVER_SET(name, str, size) \
  if (io->client.name) {               \
    io->client.name[0] = 0;            \
  }
  WEBSERVER_HEADERS(WEBSERVER_SET)
#undef WEBSERVER_SET

#ifdef HAVE_KEEP_ALIVE
  io->keep_alive = 0;
#endif
  
  /* Start a read timer.
   * This will automatically stop the timer that might be running
   * if this is a keep-alive connection.
   */
  uv_timer_start(&io->timeout, on_timeout, WEBSERVER_READ_TIMEOUT, 0);
  
  return 0;
}


static void on_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf) {
  webio_t* io = (webio_t*)tcp->data;

  if (nread >= 0) {
    ssize_t parsed = http_parser_execute(&io->parser, &parser_settings, buf.base, nread);

    if (parsed < nread) {
      if (io->client.server->error_cb) {
        char buffer[512];

        snprintf(buffer, sizeof(buffer), "http_parser_execute() = %zd, on_read = %zd\n", parsed, nread);
        io->client.server->error_cb(buffer);
      }

      do_close(io);
    }
  } else {
    do_close(io);
  }

  pool_free(&io->client.pool, buf.base);
}


static uv_buf_t on_alloc_client(uv_handle_t* handle, size_t suggested_size) {
  webio_t* io = (webio_t*)handle->data;

  char* buf = (char*)pool_malloc(&io->client.pool, suggested_size);
  return uv_buf_init(buf, suggested_size);
}


static uv_buf_t on_alloc_server(uv_handle_t* handle, size_t suggested_size) {
  (void)handle;

  char* buf = (char*)malloc(suggested_size);
  return uv_buf_init(buf, suggested_size);
}
  

static void accept_connection(uv_stream_t* handle, webio_t* io) {
  ++io->client.server->connected;

  if (uv_accept(handle, (uv_stream_t*)&io->handle)) {
    do_close(io);
  } else {
    uv_timer_init(io->client.server->loop, &io->timeout);
    uv_timer_start(&io->timeout, on_timeout, WEBSERVER_READ_TIMEOUT, 0);

    http_parser_init(&io->parser, HTTP_REQUEST);

    io->parser.data    = io;
    io->timeout.data   = io;
    io->client._io     = io;
    io->write_req.data = io;

    pool_init(&io->client.pool);

    struct sockaddr_in sockname;
    int                namelen = sizeof(sockname);

    memset(&sockname, 0, sizeof(sockname));

    /* If uv_tcp_getpeername() returns an error the IP will just be set to 0. */
    uv_tcp_getpeername((uv_tcp_t*)&io->handle, (struct sockaddr*)&sockname, &namelen);

    io->client.ip = sockname.sin_addr.s_addr;
    
    if (uv_read_start((uv_stream_t*)&io->handle, on_alloc_client, on_read) != 0) {
      do_close(io);
    }
  }
}


static void on_tcp_connection(uv_stream_t* handle, int status) {
  webserver_t* server = (webserver_t*)handle->data;
  
  assert(status == 0);
  (void)status;  /* For release builds. */

  webio_t* io = (webio_t*)calloc(1, sizeof(*io));

  uv_tcp_init(server->loop, &io->handle.tcp);

  io->client.server   = server;
  io->handle.tcp.data = io;

  accept_connection(handle, io);
}


static void on_pipe_connection(uv_pipe_t *handle, ssize_t nread, uv_buf_t buf, uv_handle_type pending) {
  (void)nread;
  
  webserver_t* server = (webserver_t*)handle->data;

  /* Are we receiving something else than a handle? */
  if (pending == UV_UNKNOWN_HANDLE) {
    return;
  }

  /* We ignore buf so free it right away. */
  free(buf.base);

  webio_t* io = (webio_t*)calloc(1, sizeof(*io));

  uv_pipe_init(server->loop, &io->handle.pipe, 0);

  io->client.server    = server;
  io->handle.pipe.data = io;

  accept_connection((uv_stream_t*)handle, io);
}


static void start_common(webserver_t* server) {
  server->connected = 0;
  server->_closing  = 0;

  /* It doesn't matter if this happens more then once when starting multiple webservers.
   * No internal state or anything is stored so it will always set the struct to the same state.
   */
  memset(&parser_settings, 0, sizeof(parser_settings));

  parser_settings.on_message_complete = on_message_complete;
  parser_settings.on_body             = on_body;
  parser_settings.on_header_value     = on_header_value;
  parser_settings.on_header_field     = on_header_field;
  parser_settings.on_url              = on_url;
  parser_settings.on_message_begin    = on_message_begin;
}


int webserver_start(webserver_t* server, const char* ip, int port) {
  assert(server->loop);
  assert(server->handle_cb);
  assert(server->close_cb );

  start_common(server);

  server->_handle       = (uv_stream_t*)calloc(1, sizeof(uv_tcp_t));
  server->_handle->data = server;

  if (uv_tcp_init(server->loop, (uv_tcp_t*)server->_handle) != 0) {
    return 1;
  }

  if (uv_tcp_bind((uv_tcp_t*)server->_handle, uv_ip4_addr(ip, port)) != 0) {
    return 1;
  }

  if (uv_listen(server->_handle, 511, on_tcp_connection) != 0) {
    return 1;
  }

  return 0;
}


int webserver_start2(webserver_t* server, uv_pipe_t* pipe) {
  assert(server->loop);
  assert(server->handle_cb);
  assert(server->close_cb );

  start_common(server);

  server->_handle       = (uv_stream_t*)pipe;
  server->_handle->data = server;
  
  if (uv_read2_start(server->_handle, on_alloc_server, on_pipe_connection) != 0) {
    return 1;
  }

  return 0;
}


static void after_close_handle(uv_handle_t* handle) {
  webserver_t* server = (webserver_t*)handle->data;

  if (server->stop_cb) {
    server->stop_cb(server);
  }

  free(handle);
}


int webserver_stop(webserver_t* server) {
  assert(!server->_closing);

  server->_closing = 1;

  if (server->_handle->type == UV_TCP) {
    uv_close((uv_handle_t*)server->_handle, after_close_handle);
    return 0;
  } else {
    int r = uv_read_stop(server->_handle);

    if ((r == 0) && (server->stop_cb)) {
      server->stop_cb(server);
    }

    return r;
  }
}


const char* webserver_error(webserver_t* server) {
  uv_err_t uverr = uv_last_error(server->loop);

  if (uverr.code != UV_OK) {
    return uv_strerror(uverr);
  }

  return 0;
}


const char* webserver_reason(int status) {
  switch (status) {
    case 100:
      return "Continue";
    case 101:
      return "Switching Protocols";
    case 102:
      return "Processing";  /* RFC 2518, obsoleted by RFC 4918. */
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 203:
      return "Non-Authoritative Information";
    case 204:
      return "No Content";
    case 205:
      return "Reset Content";
    case 206:
      return "Partial Content";
    case 207:
      return "Multi-Status";  /* RFC 4918 */
    case 300:
      return "Multiple Choices";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Moved Temporarily";
    case 303:
      return "See Other";
    case 304:
      return "Not Modified";
    case 305:
      return "Use Proxy";
    case 307:
      return "Temporary Redirect";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 402:
      return "Payment Required";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 407:
      return "Proxy Authentication Required";
    case 408:
      return "Request Time-out";
    case 409:
      return "Conflict";
    case 410:
      return "Gone";
    case 411:
      return "Length Required";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Request Entity Too Large";
    case 414:
      return "Request-URI Too Large";
    case 415:
      return "Unsupported Media Type";
    case 416:
      return "Requested Range Not Satisfiable";
    case 417:
      return "Expectation Failed";
    case 418:
      return "I\"m a teapot";          /* RFC 2324. */
    case 422:
      return "Unprocessable Entity";   /* RFC 4918. */
    case 423:
      return "Locked";                 /* RFC 4918. */
    case 424:
      return "Failed Dependency";      /* RFC 4918. */
    case 425:
      return "Unordered Collection";   /* RFC 4918. */
    case 426:
      return "Upgrade Required";       /* RFC 2817. */
    case 428:
      return "Precondition Required";  /* RFC 6585. */
    case 429:
      return "Too Many Requests";      /* RFC 6585. */
    case 431:
      return "Request Header Fields Too Large";  /* RFC 6585. */
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Time-out";
    case 505:
      return "HTTP Version not supported";
    case 506:
      return "Variant Also Negotiates";          /* RFC 2295. */
    case 507:
      return "Insufficient Storage";             /* RFC 4918. */
    case 509:
      return "Bandwidth Limit Exceeded";
    case 510:
      return "Not Extended";                     /* RFC 2774. */
    case 511:
      return "Network Authentication Required";  /* RFC 6585. */
    default:
      return "Unknown";
  }
}

