/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include "mg_rpc_channel_http.h"
#include "mg_rpc_channel_tcp_common.h"
#include "mg_rpc_channel.h"
#include "mg_rpc.h"

#include "common/cs_dbg.h"
#include "frozen/frozen.h"

#include "mgos_hal.h"

#if defined(MGOS_HAVE_HTTP_SERVER) && MGOS_ENABLE_RPC_CHANNEL_HTTP

struct mg_rpc_channel_http_data {
  struct mg_connection *nc;
  struct http_message *hm;
  unsigned int is_rest : 1;
  unsigned int sent : 1;
};

static void mg_rpc_channel_http_ch_connect(struct mg_rpc_channel *ch) {
  (void) ch;
}

static void mg_rpc_channel_http_ch_close(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  if (chd->nc != NULL) {
    if (!chd->sent) {
      mg_http_send_error(chd->nc, 400, "Invalid request");
    }
    chd->nc->flags |= MG_F_SEND_AND_CLOSE;
  }
}

static bool mg_rpc_channel_http_get_authn_info(struct mg_rpc_channel *ch,
                                               struct mg_rpc_authn *authn) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;

  struct mg_str *hdr;
  char username[50];

  /* Parse "Authorization:" header, fail fast on parse error */
  if (chd->hm == NULL ||
      (hdr = mg_get_http_header(chd->hm, "Authorization")) == NULL ||
      mg_http_parse_header(hdr, "username", username, sizeof(username)) == 0) {
    /* No auth header is present */
    return false;
  }

  /* Got username from the Authorization header */
  authn->username = mg_strdup(mg_mk_str(username));

  return true;
}

static bool mg_rpc_channel_http_is_persistent(struct mg_rpc_channel *ch) {
  (void) ch;
  /*
   * New channel is created for each incoming HTTP request, so the channel
   * is not persistent.
   *
   * Rationale for this behaviour, instead of updating channel's destination on
   * each incoming frame, is that this won't work with asynchronous responses.
   */
  return false;
}

static const char *mg_rpc_channel_http_get_type(struct mg_rpc_channel *ch) {
  (void) ch;
  return "HTTP";
}

static char *mg_rpc_channel_http_get_info(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  return mg_rpc_channel_tcp_get_info(chd->nc);
}

/*
 * Timer callback which emits SENT and CLOSED events to mg_rpc.
 */
static void mg_rpc_channel_http_frame_sent(void *param) {
  struct mg_rpc_channel *ch = (struct mg_rpc_channel *) param;
  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_SENT, (void *) 1);
  ch->ev_handler(ch, MG_RPC_CHANNEL_CLOSED, NULL);
}

static void mg_rpc_channel_http_ch_destroy(struct mg_rpc_channel *ch) {
  free(ch->channel_data);
  free(ch);
}

static bool mg_rpc_channel_http_send_frame(struct mg_rpc_channel *ch,
                                           const struct mg_str f) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  if (chd->nc == NULL || chd->sent) {
    return false;
  }

  if (chd->is_rest) {
    struct json_token result_tok = JSON_INVALID_TOKEN;
    int error_code = 0;
    char *error_msg = NULL;
    json_scanf(f.p, f.len, "{result: %T, error: {code: %d, message: %Q}}",
               &result_tok, &error_code, &error_msg);

    if (result_tok.type != JSON_TYPE_INVALID) {
      /* Got some result */
      mg_send_response_line(
          chd->nc, 200,
          "Content-Type: application/json\r\nConnection: close\r\n");
      mg_printf(chd->nc, "%.*s\r\n", (int) result_tok.len, result_tok.ptr);
    } else if (error_code != 0) {
      if (error_code != 404) error_code = 500;
      /* Got some error */
      mg_http_send_error(chd->nc, error_code, error_msg);
    } else {
      /* Empty result - that is legal. */
      mg_send_response_line(
          chd->nc, 200,
          "Content-Type: application/json\r\nConnection: close\r\n");
    }
    if (error_msg != NULL) {
      free(error_msg);
    }
  } else {
    mg_send_response_line(
        chd->nc, 200,
        "Content-Type: application/json\r\nConnection: close\r\n");
    mg_printf(chd->nc, "%.*s\r\n", (int) f.len, f.p);
  }

  chd->nc->flags |= MG_F_SEND_AND_CLOSE;
  chd->sent = true;

  /*
   * Schedule a callback which will emit SENT and CLOSED events. mg_rpc expects
   * those to be emitted asynchronously, therefore we can't emit them right
   * here.
   */
  mgos_invoke_cb(mg_rpc_channel_http_frame_sent, ch, false /* from_isr */);

  return true;
}

struct mg_rpc_channel *mg_rpc_channel_http(struct mg_connection *nc) {
  struct mg_rpc_channel *ch = (struct mg_rpc_channel *) calloc(1, sizeof(*ch));
  ch->ch_connect = mg_rpc_channel_http_ch_connect;
  ch->send_frame = mg_rpc_channel_http_send_frame;
  ch->ch_close = mg_rpc_channel_http_ch_close;
  ch->ch_destroy = mg_rpc_channel_http_ch_destroy;
  ch->get_type = mg_rpc_channel_http_get_type;
  ch->is_persistent = mg_rpc_channel_http_is_persistent;
  ch->get_authn_info = mg_rpc_channel_http_get_authn_info;
  ch->get_info = mg_rpc_channel_http_get_info;

  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) calloc(1, sizeof(*chd));
  ch->channel_data = chd;
  nc->user_data = ch;
  return ch;
}

void mg_rpc_channel_http_recd_frame(struct mg_connection *nc,
                                    struct http_message *hm,
                                    struct mg_rpc_channel *ch,
                                    const struct mg_str frame) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  chd->nc = nc;
  chd->hm = hm;
  ch->ev_handler(ch, MG_RPC_CHANNEL_OPEN, NULL);
  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_RECD, (void *) &frame);
}

void mg_rpc_channel_http_recd_parsed_frame(struct mg_connection *nc,
                                           struct http_message *hm,
                                           struct mg_rpc_channel *ch,
                                           const struct mg_str method,
                                           const struct mg_str args) {
  struct mg_rpc_channel_http_data *chd =
      (struct mg_rpc_channel_http_data *) ch->channel_data;
  chd->nc = nc;
  chd->hm = hm;
  chd->is_rest = true;

  /* Prepare "parsed" frame */
  struct mg_rpc_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.method = method;
  frame.args = args;

  /* "Open" the channel and send the frame */
  ch->ev_handler(ch, MG_RPC_CHANNEL_OPEN, NULL);
  ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_RECD_PARSED, &frame);
}

#endif /* defined(MGOS_HAVE_HTTP_SERVER) && MGOS_ENABLE_RPC_CHANNEL_HTTP */
