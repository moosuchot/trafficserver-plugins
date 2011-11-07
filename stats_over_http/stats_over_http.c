/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/* stats.c:  expose traffic server stats over http
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <ts/ts.h>
#include <string.h>

#include <inttypes.h>

typedef struct stats_state_t
{
  TSVConn net_vc;
  TSVIO read_vio;
  TSVIO write_vio;

  TSIOBuffer req_buffer;
  TSIOBufferReader req_reader;

  TSIOBuffer resp_buffer;
  TSIOBufferReader resp_reader;

  TSHttpTxn http_txnp;

  int output_bytes;
  int body_written;
} stats_state;

static void
stats_cleanup(TSCont contp, stats_state * my_state)
{
  if (my_state->req_buffer) {
    TSIOBufferDestroy(my_state->req_buffer);
    my_state->req_buffer = NULL;
  }

  if (my_state->resp_buffer) {
    TSIOBufferDestroy(my_state->resp_buffer);
    my_state->resp_buffer = NULL;
  }
  TSVConnClose(my_state->net_vc);
  TSfree(my_state);
  TSContDestroy(contp);
}

static void
stats_process_accept(TSCont contp, stats_state * my_state)
{

  my_state->req_buffer = TSIOBufferCreate();
  my_state->req_reader = TSIOBufferReaderAlloc(my_state->req_buffer);
  my_state->resp_buffer = TSIOBufferCreate();
  my_state->resp_reader = TSIOBufferReaderAlloc(my_state->resp_buffer);
  my_state->read_vio = TSVConnRead(my_state->net_vc, contp, my_state->req_buffer, INT_MAX);
}

static int
stats_add_data_to_resp_buffer(const char *s, stats_state * my_state)
{
  int s_len = strlen(s);
  char *buf = (char *) TSmalloc(s_len);

  memcpy(buf, s, s_len);
  TSIOBufferWrite(my_state->resp_buffer, buf, s_len);

  TSfree(buf);
  buf = NULL;
  return s_len;
}

static int
stats_add_resp_header(stats_state * my_state)
{
  char resp[] = "HTTP/1.0 200 Ok\r\n"
    "Content-Type: text/javascript\r\nCache-Control: no-cache\r\n\r\n";
  return stats_add_data_to_resp_buffer(resp, my_state);
}

static void
stats_process_read(TSCont contp, TSEvent event, stats_state * my_state)
{
  TSDebug("istats", "stats_process_read(%d)", event);
  if (event == TS_EVENT_VCONN_READ_READY) {
    my_state->output_bytes = stats_add_resp_header(my_state);
    TSVConnShutdown(my_state->net_vc, 1, 0);
    my_state->write_vio = TSVConnWrite(my_state->net_vc, contp, my_state->resp_reader, INT_MAX);
  } else if (event == TS_EVENT_ERROR) {
    TSError("stats_process_read: Received TS_EVENT_ERROR\n");
  } else if (event == TS_EVENT_VCONN_EOS) {
    /* client may end the connection, simply return */
    return;
  } else if (event == TS_EVENT_NET_ACCEPT_FAILED) {
    TSError("stats_process_read: Received TS_EVENT_NET_ACCEPT_FAILED\n");
  } else {
    printf("Unexpected Event %d\n", event);
    TSReleaseAssert(!"Unexpected Event");
  }
}

#define APPEND(a) my_state->output_bytes += stats_add_data_to_resp_buffer(a, my_state)
#define APPEND_STAT(a, fmt, v) do { \
  char b[256]; \
  if(snprintf(b, sizeof(b), "\"%s\": \"" fmt "\",\n", a, v) < sizeof(b)) \
    APPEND(b); \
} while(0)

static void
json_out_stat(TSRecordType rec_type, void *edata, int registered,
              const char *name, TSRecordDataType data_type,
              TSRecordData *datum) {
  stats_state *my_state = edata;

  switch(data_type) {
  case TS_RECORDDATATYPE_COUNTER:
    APPEND_STAT(name, "%" PRIu64, datum->rec_counter); break;
  case TS_RECORDDATATYPE_INT:
    APPEND_STAT(name, "%" PRIu64, datum->rec_int); break;
  case TS_RECORDDATATYPE_FLOAT:
    APPEND_STAT(name, "%f", datum->rec_float); break;
  case TS_RECORDDATATYPE_STRING:
    APPEND_STAT(name, "%s", datum->rec_string); break;
  default:
    TSDebug("istats", "unkown type for %s: %d", name, data_type);
    break;
  }
}
static void
json_out_stats(stats_state * my_state)
{
  const char *version;
  APPEND("{ \"global\": {\n");

  TSRecordDump(TS_RECORDTYPE_PROCESS, json_out_stat, my_state);
  version = TSTrafficServerVersionGet();
  APPEND("\"server\": \"");
  APPEND(version);
  APPEND("\"\n");
  APPEND("  }\n}\n");
}

static void
stats_process_write(TSCont contp, TSEvent event, stats_state * my_state)
{
  if (event == TS_EVENT_VCONN_WRITE_READY) {
    if (my_state->body_written == 0) {
      TSDebug("istats", "plugin adding response body");
      my_state->body_written = 1;
      json_out_stats(my_state);
      TSVIONBytesSet(my_state->write_vio, my_state->output_bytes);
    }
    TSVIOReenable(my_state->write_vio);
  } else if (TS_EVENT_VCONN_WRITE_COMPLETE) {
    stats_cleanup(contp, my_state);
  } else if (event == TS_EVENT_ERROR) {
    TSError("stats_process_write: Received TS_EVENT_ERROR\n");
  } else {
    TSReleaseAssert(!"Unexpected Event");
  }
}

static int
stats_dostuff(TSCont contp, TSEvent event, void *edata)
{
  stats_state *my_state = TSContDataGet(contp);
  if (event == TS_EVENT_NET_ACCEPT) {
    my_state->net_vc = (TSVConn) edata;
    stats_process_accept(contp, my_state);
  } else if (edata == my_state->read_vio) {
    stats_process_read(contp, event, my_state);
  } else if (edata == my_state->write_vio) {
    stats_process_write(contp, event, my_state);
  } else {
    TSReleaseAssert(!"Unexpected Event");
  }
  return 0;
}
static int
stats_origin(TSCont contp, TSEvent event, void *edata)
{
  TSCont icontp;
  stats_state *my_state;
  TSHttpTxn txnp = (TSHttpTxn) edata;
  TSMBuffer reqp;
  TSMLoc hdr_loc = NULL, url_loc = NULL;
  TSEvent reenable = TS_EVENT_HTTP_CONTINUE;

  TSDebug("istats", "in the read stuff");
 
  if (TSHttpTxnClientReqGet(txnp, &reqp, &hdr_loc) != TS_SUCCESS)
    goto cleanup;
  
  if (TSHttpHdrUrlGet(reqp, hdr_loc, &url_loc) != TS_SUCCESS)
    goto cleanup;
  
  int path_len = 0;
  const char* path = TSUrlPathGet(reqp,url_loc,&path_len);
  TSDebug("istats","Path: %.*s",path_len,path);
  
  if (! (path_len != 0 && path_len == 6 && !memcmp(path,"_stats",6)) ) {
    goto notforme;
  }
  
  TSSkipRemappingSet(txnp,1); //not strictly necessary, but speed is everything these days

  /* This is us -- register our intercept */
  TSDebug("istats", "Intercepting request");

  icontp = TSContCreate(stats_dostuff, TSMutexCreate());
  my_state = (stats_state *) TSmalloc(sizeof(*my_state));
  memset(my_state, 0, sizeof(*my_state));
  TSContDataSet(icontp, my_state);
  TSHttpTxnIntercept(icontp, txnp);
  goto cleanup;

 notforme:

 cleanup:
#if (TS_VERSION_NUMBER < 2001005)
  if(path) TSHandleStringRelease(reqp, url_loc, path);
#endif
  if(url_loc) TSHandleMLocRelease(reqp, hdr_loc, url_loc);
  if(hdr_loc) TSHandleMLocRelease(reqp, TS_NULL_MLOC, hdr_loc);

  TSHttpTxnReenable(txnp, reenable);
  return 0;
}

int
check_ts_version()
{

  const char *ts_version = TSTrafficServerVersionGet();
  int result = 0;

  if (ts_version) {
    int major_ts_version = 0;
    int minor_ts_version = 0;
    int patch_ts_version = 0;

    if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version, &patch_ts_version) != 3) {
      return 0;
    }

    /* Need at least TS 2.0 */
    if (major_ts_version >= 2) {
      result = 1;
    }
  }

  return result;
}

void
TSPluginInit(int argc, const char *argv[])
{
  TSPluginRegistrationInfo info;

  info.plugin_name = "stats";
  info.vendor_name = "Apache Software Foundation";
  info.support_email = "jesus@omniti.com";

  if (TSPluginRegister(TS_SDK_VERSION_2_0, &info) != TS_SUCCESS)
    TSError("Plugin registration failed. \n");

  if (!check_ts_version()) {
    TSError("Plugin requires Traffic Server 2.0 or later\n");
    return;
  }

  /* Create a continuation with a mutex as there is a shared global structure
     containing the headers to add */
  TSHttpHookAdd(TS_HTTP_READ_REQUEST_HDR_HOOK, TSContCreate(stats_origin, NULL));
  TSDebug("istats", "stats module registered");
}
