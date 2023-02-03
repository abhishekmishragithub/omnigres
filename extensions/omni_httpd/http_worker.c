/**
 * @file http_worker.c
 * @brief HTTP worker
 *
 * This file contains code that runs in omni_http http workers that serve requests. These
 * workers are started by the master worker.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>

// clang-format off
#include <postgres.h>
#include <fmgr.h>
// clang-format on

#include <access/xact.h>
#include <executor/spi.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <postmaster/bgworker.h>
#include <storage/latch.h>
#include <tcop/utility.h>
#include <utils/builtins.h>
#include <utils/inet.h>
#include <utils/snapmgr.h>
#include <utils/timestamp.h>
#if PG_MAJORVERSION_NUM >= 13
#include <postmaster/interrupt.h>
#endif

#include <metalang99.h>

#include <libpgaug.h>

#include "fd.h"
#include "omni_httpd.h"

#if H2O_USE_LIBUV == 1
#error "only evloop is supported, ensure H2O_USE_LIBUV is not set to 1"
#endif

/**
 * @brief Number of fields in http_request type
 *
 */
#define REQUEST_PLAN_PARAMS 5
/**
 * @brief Parameter index in a prepared query
 *
 */
#define REQUEST_PLAN_PARAM(x) ML99_STRINGIFY(ML99_INC(x))

/**
 * @brief Path parameter index
 *
 */
#define REQUEST_PLAN_PATH 0
/**
 * @brief Method parameter index
 *
 */
#define REQUEST_PLAN_METHOD 1
/**
 * @brief Query string parameter index
 *
 */
#define REQUEST_PLAN_QUERY_STRING 2
/**
 * @brief Body parameter index
 *
 */
#define REQUEST_PLAN_BODY 3
/**
 * @brief Headers parameter index
 *
 */
#define REQUEST_PLAN_HEADERS 4

/**
 * @brief Each listener has a context
 *
 */
typedef struct {
  /**
   * @brief Query plan
   *
   */
  SPIPlanPtr plan;
  /**
   * @brief Associated socket
   *
   */
  h2o_socket_t *socket;
  /**
   * @brief Underlying file descriptor
   *
   */
  int fd;
  /**
   * @brief Accept context
   *
   */
  h2o_accept_ctx_t accept_ctx;
  /**
   * @brief H2O context
   *
   * Inclusion of this context into `listener_ctx` allows us to retrieve `listener_ctx`:
   *
   * ```
   * listener_ctx *lctx = H2O_STRUCT_FROM_MEMBER(listener_ctx, context, req->conn->ctx);
   * ```
   *
   */
  h2o_context_t context;
} listener_ctx;

static inline int listener_ctx_cmp(const listener_ctx *l, const listener_ctx *r) {
  return (l->plan == r->plan && l->socket == r->socket && l->fd == r->fd) ? 0 : -1;
}

#define i_tag listener_contexts
#define i_val listener_ctx
#define i_eq c_memcmp_eq
#define i_cmp listener_ctx_cmp
#include <stc/clist.h>

static h2o_globalconf_t config;
static h2o_evloop_t *worker_event_loop;

/**
 * @brief Accept socket
 *
 * @param listener
 * @param err
 */
static void on_accept(h2o_socket_t *listener, const char *err) {
  h2o_socket_t *sock;

  if (err != NULL) {
    return;
  }

  if ((sock = h2o_evloop_socket_accept(listener)) == NULL) {
    return;
  }
  h2o_accept(&((listener_ctx *)listener->data)->accept_ctx, sock);
}

/**
 * @brief Create a listening socket
 *
 * @param family
 * @param port
 * @param address
 * @return int
 */
int create_listening_socket(sa_family_t family, in_port_t port, char *address) {
  struct sockaddr_in addr;
  struct sockaddr_in6 addr6;
  void *sockaddr;
  size_t socksize;
  int fd, reuseaddr_flag = 1;

  if (family == AF_INET) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, address, &addr.sin_addr);
    addr.sin_port = htons(port);
    sockaddr = &addr;
    socksize = sizeof(addr);
  } else if (family == AF_INET6) {
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET;
    inet_pton(AF_INET6, address, &addr6.sin6_addr);
    addr6.sin6_port = htons(port);
    sockaddr = &addr6;
    socksize = sizeof(addr6);
  } else {
    return -1;
  }

  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1 ||
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag, sizeof(reuseaddr_flag)) != 0 ||
      bind(fd, (struct sockaddr *)sockaddr, socksize) != 0 || listen(fd, SOMAXCONN) != 0) {
    return -1;
  }

  return fd;
}

/**
 * @brief Create a H2O listener socket
 *
 * @param fd
 * @param listener_ctx
 * @return int
 */
static int create_listener(int fd, listener_ctx *listener_ctx) {
  h2o_socket_t *sock;

  if (fd == -1) {
    return -1;
  }

  sock = h2o_evloop_socket_create(worker_event_loop, fd, H2O_SOCKET_FLAG_DONT_READ);
  sock->data = listener_ctx;
  listener_ctx->socket = sock;
  h2o_socket_read_start(sock, on_accept);

  return 0;
}

/**
 * @brief Request handler
 *
 * @param self
 * @param req
 * @return int
 */
static int handler(h2o_handler_t *self, h2o_req_t *req) {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  PushActiveSnapshot(GetTransactionSnapshot());
  SPI_connect();

  listener_ctx *lctx = H2O_STRUCT_FROM_MEMBER(listener_ctx, context, req->conn->ctx);
  SPIPlanPtr plan = lctx->plan;

  int ret;

  MemoryContext memory_context = CurrentMemoryContext;
  PG_TRY();
  {
    char nulls[REQUEST_PLAN_PARAMS] = {[REQUEST_PLAN_METHOD] = ' ',
                                       [REQUEST_PLAN_PATH] = ' ',
                                       [REQUEST_PLAN_QUERY_STRING] =
                                           req->query_at == SIZE_MAX ? 'n' : ' ',
                                       [REQUEST_PLAN_BODY] = ' ',
                                       [REQUEST_PLAN_HEADERS] = ' '};
    ret = SPI_execute_plan(
        plan,
        (Datum[REQUEST_PLAN_PARAMS]){
            [REQUEST_PLAN_METHOD] =
                PointerGetDatum(cstring_to_text_with_len(req->method.base, req->method.len)),
            [REQUEST_PLAN_PATH] = PointerGetDatum(
                cstring_to_text_with_len(req->path_normalized.base, req->path_normalized.len)),
            [REQUEST_PLAN_QUERY_STRING] =
                req->query_at == SIZE_MAX
                    ? PointerGetDatum(NULL)
                    : PointerGetDatum(cstring_to_text_with_len(req->path.base + req->query_at + 1,
                                                               req->path.len - req->query_at - 1)),
            [REQUEST_PLAN_BODY] = ({
              while (req->proceed_req != NULL) {
                req->proceed_req(req, NULL);
              }
              bytea *result = (bytea *)palloc(req->entity.len + VARHDRSZ);
              SET_VARSIZE(result, req->entity.len + VARHDRSZ);
              memcpy(VARDATA(result), req->entity.base, req->entity.len);
              PointerGetDatum(result);
            }),
            [REQUEST_PLAN_HEADERS] = ({
              TupleDesc header_tupledesc = TypeGetTupleDesc(http_header_oid(), NULL);
              BlessTupleDesc(header_tupledesc);

              Datum *elems = (Datum *)palloc(sizeof(Datum) * req->headers.size);
              bool *nulls = (bool *)palloc(sizeof(bool) * req->headers.size);
              for (int i = 0; i < req->headers.size; i++) {
                h2o_header_t header = req->headers.entries[i];
                nulls[i] = 0;
                HeapTuple header_tuple =
                    heap_form_tuple(header_tupledesc,
                                    (Datum[3]){
                                        PointerGetDatum(cstring_to_text_with_len(header.name->base,
                                                                                 header.name->len)),
                                        PointerGetDatum(cstring_to_text_with_len(header.value.base,
                                                                                 header.value.len)),
                                        BoolGetDatum(true),
                                    },
                                    (bool[3]){false, false, false});
                elems[i] = HeapTupleGetDatum(header_tuple);
              }
              ArrayType *result =
                  construct_md_array(elems, nulls, 1, (int[1]){req->headers.size}, (int[1]){1},
                                     http_header_oid(), -1, false, TYPALIGN_DOUBLE);
              PointerGetDatum(result);
            })},
        nulls, false, 1);
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(memory_context);
    WITH_TEMP_MEMCXT {
      ErrorData *error = CopyErrorData();
      ereport(WARNING, errmsg("Error executing query"),
              errdetail("%s: %s", error->message, error->detail));
    }

    FlushErrorState();

    // Abort the transaction
    AbortCurrentTransaction();
    req->res.status = 500;
    h2o_send_inline(req, H2O_STRLIT("Internal server error"));
    goto cleanup;
  }
  PG_END_TRY();
  int proc = SPI_processed;
  if (ret == SPI_OK_SELECT && proc > 0) {
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    HeapTuple tuple = tuptable->vals[0];

    int c = 1;
    for (int i = 0; i < tupdesc->natts; i++) {
      if (http_response_oid() == tupdesc->attrs[i].atttypid) {
        c = i + 1;
        break; // TODO: continue but issue a warning if another response is found?
      }
    }

    bool isnull = false;
    Datum response = SPI_getbinval(tuple, tupdesc, c, &isnull);
    if (!isnull) {
      HeapTupleHeader response_tuple = DatumGetHeapTupleHeader(response);

      // Status
      req->res.status = DatumGetUInt16(GetAttributeByNum(response_tuple, 1, &isnull));
      if (isnull) {
        req->res.status = 200;
      }

      // Headers
      Datum array_datum = GetAttributeByNum(response_tuple, 2, &isnull);
      if (!isnull) {
        ArrayType *headers = DatumGetArrayTypeP(array_datum);
        ArrayIterator iter = array_create_iterator(headers, 0, NULL);
        Datum header;
        while (array_iterate(iter, &header, &isnull)) {
          if (!isnull) {
            HeapTupleHeader header_tuple = DatumGetHeapTupleHeader(header);
            Datum name = GetAttributeByNum(header_tuple, 1, &isnull);
            if (!isnull) {
              text *name_text = DatumGetTextPP(name);
              Datum value = GetAttributeByNum(header_tuple, 2, &isnull);
              if (!isnull) {
                text *value_text = DatumGetTextPP(value);
                Datum append = GetAttributeByNum(header_tuple, 3, &isnull);
                if (isnull || !DatumGetBool(append)) {
                  h2o_set_header_by_str(&req->pool, &req->res.headers, VARDATA_ANY(name_text),
                                        VARSIZE_ANY_EXHDR(name_text), 0, VARDATA_ANY(value_text),
                                        VARSIZE_ANY_EXHDR(value_text), true);
                } else {
                  h2o_add_header_by_str(&req->pool, &req->res.headers, VARDATA_ANY(name_text),
                                        VARSIZE_ANY_EXHDR(name_text), 0, NULL,
                                        VARDATA_ANY(value_text), VARSIZE_ANY_EXHDR(value_text));
                }
              }
            }
          }
        }
        array_free_iterator(iter);
      }

      // Body
      Datum body = GetAttributeByNum(response_tuple, 3, &isnull);

      if (!isnull) {
        bytea *body_content = DatumGetByteaPP(body);
        h2o_send_inline(req, VARDATA_ANY(body_content), VARSIZE_ANY_EXHDR(body_content));
      } else {
        h2o_send_inline(req, "", 0);
      }

    } else {
      h2o_send_inline(req, "", 0);
    }
  } else {
    if (ret == SPI_OK_SELECT) {
      // No result
      req->res.status = 204;
      h2o_send_inline(req, H2O_STRLIT(""));
    } else {
      req->res.status = 500;
      ereport(WARNING, errmsg("Error executing query: %s", SPI_result_code_string(ret)));
      h2o_send_inline(req, H2O_STRLIT("Internal server error"));
    }
  }
cleanup:
  SPI_finish();
  PopActiveSnapshot();
  CommitTransactionCommand();
  return 0;
}

static h2o_pathconf_t *register_handler(h2o_hostconf_t *hostconf, const char *path,
                                        int (*on_req)(h2o_handler_t *, h2o_req_t *)) {
  h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, path, 0);
  h2o_handler_t *request_handler = h2o_create_handler(pathconf, sizeof(h2o_handler_t));
  request_handler->on_req = on_req;
  return pathconf;
}

static void setup_server() {
  h2o_hostconf_t *hostconf;

  h2o_config_init(&config);
  config.server_name = h2o_iovec_init(H2O_STRLIT("omni_httpd-" EXT_VERSION));
  hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);

  worker_event_loop = h2o_evloop_create();

  h2o_pathconf_t *pathconf = register_handler(hostconf, "/", handler);
}

#define i_val int
#define i_tag fd
#include <stc/cset.h>

static cvec_fd accept_fds(char *socket_name) {
  struct sockaddr_un address;
  int socket_fd;

  socket_fd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    ereport(ERROR, errmsg("can't create sharing socket"));
  }

  memset(&address, 0, sizeof(struct sockaddr_un));

  address.sun_family = AF_UNIX;
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_name);

try_connect:
  if (connect(socket_fd, (struct sockaddr *)&address, sizeof(struct sockaddr_un)) != 0) {
    int e = errno;
    if (e == ECONNREFUSED) {
      goto try_connect;
    }
    ereport(ERROR, errmsg("error connecting to sharing socket: %s", strerror(e)));
  }

  cvec_fd result = recv_fds(socket_fd);
  int l = cvec_fd_size(&result);
  close(socket_fd);

  return result;
}

static bool worker_running = true;
static bool worker_reload = false;
static void reload_worker() { worker_reload = true; }
static void stop_worker() { worker_running = false; }

static clist_listener_contexts listener_contexts;

void http_worker(Datum db_oid) {
#if PG_MAJORVERSION_NUM >= 13
  pqsignal(SIGHUP, SignalHandlerForConfigReload);
#else
#warning "TODO: SignalHandlerForConfigReload for Postgres 12"
#endif
  pqsignal(SIGTERM, die);

  BackgroundWorkerUnblockSignals();
  BackgroundWorkerInitializeConnectionByOid(db_oid, InvalidOid, 0);

  setup_server();
  pqsignal(SIGTERM, stop_worker);
  pqsignal(SIGUSR2, reload_worker);

  listener_contexts = clist_listener_contexts_init();

  while (worker_running) {
    worker_reload = false;
    cvec_fd fds = accept_fds(MyBgworkerEntry->bgw_extra);

    // Disposing allocated listener/query pairs and their associated data
    // (such as sockets)
    {
      clist_listener_contexts_iter iter = clist_listener_contexts_begin(&listener_contexts);
      while (iter.ref != NULL) {
        if (cvec_fd_get(&fds, iter.ref->fd) != NULL) {
          // We still have this fd, don't dispose it
          clist_listener_contexts_next(&iter);
          continue;
        }
        if (iter.ref->plan != NULL) {
          SPI_freeplan(iter.ref->plan);
        }
        if (iter.ref->socket != NULL) {
          h2o_socket_t *socket = iter.ref->socket;
          h2o_socket_read_stop(socket);
          h2o_socket_export_t info;
          h2o_socket_export(socket, &info);
          h2o_evloop_run(worker_event_loop, 0);
          close(info.fd);
        }
        h2o_context_dispose(&iter.ref->context);
        iter = clist_listener_contexts_erase_at(&listener_contexts, iter);
      }
    }

    c_FOREACH(i, cvec_fd, fds) {
      int fd = *i.ref;

      listener_ctx c = {.fd = fd,
                        .socket = NULL,
                        .plan = NULL,
                        .accept_ctx = (h2o_accept_ctx_t){.hosts = config.hosts}};

      listener_ctx *lctx = clist_listener_contexts_push(&listener_contexts, c);

    try_create_listener:
        if (create_listener(fd, lctx) == 0) {
          h2o_context_init(&(lctx->context), worker_event_loop, &config);
          lctx->accept_ctx.ctx = &lctx->context;
        } else {
          if (errno == EINTR) {
            goto try_create_listener; // retry
          }
          int e = errno;
          ereport(WARNING, errmsg("socket error: %s", strerror(e)));
        }
    }

    cvec_fd_drop(&fds);

    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    SPI_connect();

    int ret = SPI_execute("SELECT listen.addr, listen.port, query FROM omni_httpd.listeners, "
                          "unnest(omni_httpd.listeners.listen) AS listen",
                          false, 0);
    if (ret == SPI_OK_SELECT) {
      TupleDesc tupdesc = SPI_tuptable->tupdesc;
      SPITupleTable *tuptable = SPI_tuptable;
      for (int i = 0; i < SPI_processed; i++) {
        HeapTuple tuple = tuptable->vals[i];
        bool addr_is_null = false;
        Datum addr = SPI_getbinval(tuple, tupdesc, 1, &addr_is_null);
        bool port_is_null = false;
        Datum port = SPI_getbinval(tuple, tupdesc, 2, &port_is_null);
        bool query_is_null = false;
        Datum query = SPI_getbinval(tuple, tupdesc, 3, &query_is_null);

        c_FOREACH(iter, clist_listener_contexts, listener_contexts) {
            int fd = iter.ref->fd;
            struct sockaddr_in sin;
            socklen_t len = sizeof(sin);
            struct sockaddr_in6 sin6;
            socklen_t len6 = sizeof(sin6);

            socklen_t socklen;
            void *sockaddr;

            const char *fdaddr;
            int port_no;

            char _address[MAX_ADDRESS_SIZE];
            char _address1[MAX_ADDRESS_SIZE];

            int family = 0;
            if (getsockname(fd, (struct sockaddr *)&sin, &len) == 0) {
            family = sin.sin_family;
          }

          if (family == AF_INET) {
            sockaddr = &sin;
            socklen = len;
          } else if (family == AF_INET6) {
            sockaddr = &sin6;
            socklen = len6;
          } else {
            continue;
          }

          if (getsockname(fd, (struct sockaddr *)sockaddr, &len) == 0) {
            if (family == AF_INET) {
              port_no = ntohs(sin.sin_port);
              fdaddr = inet_ntop(AF_INET, &sin.sin_addr, _address, sizeof(_address));
            } else if (family == AF_INET6) {
              port_no = ntohs(sin6.sin6_port);
              fdaddr = inet_ntop(AF_INET6, &sin6.sin6_addr, _address, sizeof(_address));
            } else {
              continue;
            }
          } else {
            continue;
          }

          inet *inet_address = DatumGetInetPP(addr);
          char *address = pg_inet_net_ntop(ip_family(inet_address), ip_addr(inet_address),
                                           ip_bits(inet_address), _address1, sizeof(_address1));

          if (strncmp(address, fdaddr, strlen(address)) == 0) {
            // Found matching socket
            char *query_string = text_to_cstring(DatumGetTextPP(query));
            MemoryContext memory_context = CurrentMemoryContext;
            char *request_cte = psprintf(
                // clang-format off
                      "SELECT $" REQUEST_PLAN_PARAM(REQUEST_PLAN_PATH) " AS path, "
                       "$" REQUEST_PLAN_PARAM(REQUEST_PLAN_METHOD) "::text::omni_httpd.http_method AS method, "
                       "$" REQUEST_PLAN_PARAM(REQUEST_PLAN_QUERY_STRING) " AS query_string, "
                       "$" REQUEST_PLAN_PARAM(REQUEST_PLAN_BODY) " AS body, "
                       "$" REQUEST_PLAN_PARAM(REQUEST_PLAN_HEADERS) " AS headers "
                // clang-format on
            );
            int prep_ret = SPI_execute_with_args(
                "SELECT omni_sql.add_cte($1::text::omni_sql.statement, 'request', "
                "$2::text::omni_sql.statement, prepend => true) WHERE NOT "
                "omni_sql.is_parameterized($1::text::omni_sql.statement)",
                2, (Oid[2]){CSTRINGOID, CSTRINGOID},
                (Datum[2]){CStringGetDatum(query_string), CStringGetDatum(request_cte)}, "  ",
                false, 1);
            if (prep_ret != SPI_OK_SELECT) {
              ereport(WARNING, errmsg("Can't prepare SQL for: %s", query_string));
            } else {
              if (SPI_processed == 1) {

                char *query = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
                PG_TRY();
                {
                  SPIPlanPtr plan =
                      SPI_prepare(query, REQUEST_PLAN_PARAMS,
                                  (Oid[REQUEST_PLAN_PARAMS]){
                                      [REQUEST_PLAN_METHOD] = TEXTOID,
                                      [REQUEST_PLAN_PATH] = TEXTOID,
                                      [REQUEST_PLAN_QUERY_STRING] = TEXTOID,
                                      [REQUEST_PLAN_BODY] = BYTEAOID,
                                      [REQUEST_PLAN_HEADERS] = http_header_array_oid(),
                                  });
                  pfree(query);

                  // We have to keep the plan as we're going to disconnect from SPI
                  SPI_keepplan(plan);
                  iter.ref->plan = plan;
                }
                PG_CATCH();
                {
                  MemoryContextSwitchTo(memory_context);
                  WITH_TEMP_MEMCXT {
                    ErrorData *error = CopyErrorData();
                    ereport(WARNING, errmsg("Error preparing query"),
                            errdetail("%s: %s", error->message, error->detail));
                  }

                  FlushErrorState();
                }
                PG_END_TRY();
              } else {
                ereport(WARNING, errmsg("Listener query is parameterized and is rejected:\n %s",
                                        query_string));
              }
            }
            pfree(query_string);
          }
        }
      }
    } else {
      ereport(WARNING, errmsg("Error fetching configuration: %s", SPI_result_code_string(ret)));
    }

    SPI_finish();
    AbortCurrentTransaction();

    while (worker_running && !worker_reload && h2o_evloop_run(worker_event_loop, INT32_MAX) == 0)
      ;
  }

  clist_listener_contexts_drop(&listener_contexts);
}