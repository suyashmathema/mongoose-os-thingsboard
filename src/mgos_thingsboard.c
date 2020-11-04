#include "mgos_thingsboard.h"

#include "mg_rpc_channel_loopback.h"
#include "mgos.h"
#include "mgos_mqtt.h"
#include "mgos_rpc.h"

const char* ATTR_TOPIC = "v1/devices/me/attributes";
const char* ATTR_REQ_PUB_TOPIC = "v1/devices/me/attributes/request/%d";
const char* ATTR_RESP_SUB_TOPIC = "v1/devices/me/attributes/response/+";
const char* TELE_PUB_TOPIC = "v1/devices/me/telemetry";
const char* RPC_REQ_SUB_TOPIC = "v1/devices/me/rpc/request/+";
const char* RPC_REQ_PUB_TOPIC = "v1/devices/me/rpc/request/%d";
const char* RPC_RESP_SUB_TOPIC = "v1/devices/me/rpc/response/+";
const char* RPC_RESP_PUB_TOPIC = "v1/devices/me/rpc/response/%d";

struct mgos_thingsboard_config {
    bool user_active;
    int attr_req_id;
    int rpc_client_req_id;
    mgos_timer_id tele_delay_timer;
    char* delayed_telemetry;
    int mqtt_qos;
    bool mqtt_retain;
};

struct mgos_thingsboard_config tb_config = {
    .user_active = false,
    .attr_req_id = 1,
    .rpc_client_req_id = 1,
    .tele_delay_timer = 0,
    .delayed_telemetry = NULL,
    .mqtt_qos = 1,
    .mqtt_retain = false};

static int get_topic_req_id(const char* topic) {
    char* id_ptr = strrchr(topic, '/');
    return atoi(++id_ptr);
}

//Caller needs to free returned string
static char* create_topic(const char* topic_fmt, int request_id) {
    char* topic = NULL;
    int size = mg_asprintf(&topic, 0, topic_fmt, request_id);
    if (size == -1) {
        LOG(LL_ERROR, ("create_topic - Failed to create topic"));
        return NULL;
    }
    return topic;
}

void set_user_active(const char* attrs) {
    LOG(LL_VERBOSE_DEBUG, ("Set user active status"));
    char* active = NULL;
    json_scanf(attrs, strlen(attrs), "{tb: {shared: {user_active: %Q}}}", &active);
    if (active != NULL) {
        LOG(LL_DEBUG, ("User active: %s", active));
        if (strcmp("true", active) == 0) {
            tb_config.user_active = true;
        } else {
            tb_config.user_active = false;
        }
        free(active);
    }
}

uint16_t tb_request_attributes(const char* client_keys, const char* shared_keys) {
    uint16_t res = 0;
    char* topic = NULL;
    if ((topic = create_topic(ATTR_REQ_PUB_TOPIC, tb_config.attr_req_id)) == NULL) {
        return res;
    }

    if (client_keys != NULL && shared_keys != NULL) {
        res = mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{ clientKeys:%Q, sharedKeys: %Q }",
                             client_keys, shared_keys);
    } else if (shared_keys != NULL) {
        res = mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{ sharedKeys: %Q }", shared_keys);
    } else if (client_keys != NULL) {
        res = mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{ clientKeys: %Q }", client_keys);
    }
    LOG(LL_INFO, ("Request attributes, id:%d", res));
    tb_config.attr_req_id++;
    free(topic);
    return res;
}

uint16_t tb_request_shared_attributes() {
    struct mbuf shared_keys;
    mbuf_init(&shared_keys, 0);
    const struct mgos_conf_entry* shared_schema = mgos_config_schema_tb_shared();
    for (int i = 1; i <= shared_schema->num_desc; i++) {
        const struct mgos_conf_entry* e = shared_schema + i;
        mbuf_append(&shared_keys, e->key, strlen(e->key));
        if (e->type == CONF_TYPE_OBJECT) {
            i += e->num_desc;
        }
        if (i < shared_schema->num_desc) {
            mbuf_append(&shared_keys, ",", 1);
        }
    }
    mbuf_append(&shared_keys, "", 1);
    LOG(LL_DEBUG, ("Request shared config attributes"));
    uint16_t res = tb_request_attributes(NULL, shared_keys.buf);

    mbuf_free(&shared_keys);
    return res;
}

static void attribute_response_handler(struct mg_connection* nc, const char* topic,
                                       int topic_len, const char* msg, int msg_len, void* ud) {
    char* attr = json_asprintf("{tb:%.*s}", msg_len, msg);
    if (attr == NULL) {
        LOG(LL_ERROR, ("Failed to load attributes response for request"));
        return;
    }
    set_user_active(attr);
    mgos_config_apply(attr, true);
    LOG(LL_INFO, ("Attributes received"));
    struct mg_str attr_kv = mg_mk_str_n(msg, msg_len);
    mgos_event_trigger(TB_ATTRIBUTE_RESPONSE, &attr_kv);
    free(attr);
}

static void attribute_update_handler(struct mg_connection* nc, const char* topic,
                                     int topic_len, const char* msg, int msg_len, void* ud) {
    char* attr = json_asprintf("{tb:{shared:%.*s}}", msg_len, msg);
    if (attr == NULL) {
        LOG(LL_ERROR, ("Failed to load attributes response for update"));
        return;
    }
    set_user_active(attr);
    LOG(LL_INFO, ("Attributes update received"));
    mgos_config_apply(attr, true);
    struct mg_str attr_kv = mg_mk_str_n(msg, msg_len);
    mgos_event_trigger(TB_ATTRIBUTE_UPDATE, &attr_kv);
    free(attr);
}

uint16_t tb_publish_client_attributes() {
    LOG(LL_DEBUG, ("Publish client config attributes"));
    struct mbuf msg_mbuf;
    mbuf_init(&msg_mbuf, 0);
    mgos_conf_emit_cb(&mgos_sys_config, NULL, mgos_config_schema_tb_client(), true, &msg_mbuf, NULL, NULL);
    uint16_t res = mgos_mqtt_pub(ATTR_TOPIC, msg_mbuf.buf, msg_mbuf.len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    mbuf_free(&msg_mbuf);
    return res;
}

uint16_t tb_publish_attributes(const char* attributes, int attributes_len) {
    LOG(LL_DEBUG, ("Publish client config attributes"));
    return mgos_mqtt_pub(ATTR_TOPIC, attributes, attributes_len, tb_config.mqtt_qos, tb_config.mqtt_retain);
}

uint16_t tb_publish_attributesf(const char* json_fmt, ...) {
    LOG(LL_DEBUG, ("Publish client config attributes"));
    uint16_t res = 0;
    va_list ap;
    va_start(ap, json_fmt);
    res = mgos_mqtt_pubv(ATTR_TOPIC, tb_config.mqtt_qos, tb_config.mqtt_retain, json_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_publish_attributesv(const char* json_fmt, va_list ap) {
    LOG(LL_DEBUG, ("Publish client config attributes"));
    return mgos_mqtt_pubv(ATTR_TOPIC, tb_config.mqtt_qos, tb_config.mqtt_retain, json_fmt, ap);
}

static void pub_delayed_telemetry_cb(void* arg) {
    LOG(LL_DEBUG, ("Publish delayed telemetry"));
    mgos_mqtt_pub(TELE_PUB_TOPIC, tb_config.delayed_telemetry, strlen(tb_config.delayed_telemetry),
                  tb_config.mqtt_qos, tb_config.mqtt_retain);
    if (tb_config.delayed_telemetry != NULL) {
        free(tb_config.delayed_telemetry);
        tb_config.delayed_telemetry = NULL;
    }
    mgos_clear_timer(tb_config.tele_delay_timer);
    tb_config.tele_delay_timer = MGOS_INVALID_TIMER_ID;
    (void)arg;
}

uint16_t tb_publish_telemetry(int flags, int64_t time, const char* telemetry, int telemetry_len) {
    uint16_t res = 0;
    if (!tb_config.user_active) {
        return res;
    }
    LOG(LL_DEBUG, ("Publish telemetry"));

    if (flags & TBP_TELEMETRY_TIMED) {
        if (time == 0) {
            time = mgos_time_micros() / 1000;
        }
        //Because json_asprintf does not work with %jd specifier
        char time_str[19], *p = time_str;
        mg_asprintf(&p, sizeof(time_str), "%jd", time);
        char* timed_telemetry = json_asprintf("{ts:%s, values:%.*s}", p, telemetry_len, telemetry);
        if (p != time_str) {
            free(p);
        }
        if (timed_telemetry == NULL) {
            LOG(LL_ERROR, ("Failed to create timed telemetry"));
            return res;
        }
        res = mgos_mqtt_pub(TELE_PUB_TOPIC, timed_telemetry, strlen(timed_telemetry),
                            tb_config.mqtt_qos, tb_config.mqtt_retain);
        free(timed_telemetry);
    } else if (flags & TBP_TELEMETRY_DELAYED) {
        if (tb_config.delayed_telemetry != NULL) {
            free(tb_config.delayed_telemetry);
            tb_config.delayed_telemetry = NULL;
        }
        mgos_clear_timer(tb_config.tele_delay_timer);
        tb_config.tele_delay_timer = MGOS_INVALID_TIMER_ID;

        tb_config.delayed_telemetry = strndup(telemetry, telemetry_len);
        if (tb_config.delayed_telemetry == NULL) {
            LOG(LL_ERROR, ("Failed to create delayed telemetry"));
            return res;
        }
        tb_config.tele_delay_timer = mgos_set_timer(time, 0, pub_delayed_telemetry_cb, NULL);
    } else {
        res = mgos_mqtt_pub(TELE_PUB_TOPIC, telemetry, telemetry_len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    }
    return res;
}

uint16_t tb_publish_telemetryv(int flags, int64_t time, const char* telemetry_fmt, va_list ap) {
    uint16_t res = 0;
    //TODO check if we can directly send format string and use json_asprintf once
    char* telemetry = json_vasprintf(telemetry_fmt, ap);
    if (telemetry == NULL) {
        LOG(LL_ERROR, ("Failed to create formatted telemetry"));
        return res;
    }
    res = tb_publish_telemetry(flags, time, telemetry, strlen(telemetry));
    free(telemetry);
    return res;
}

uint16_t tb_publish_telemetryf(int flags, int64_t time, const char* telemetry_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, telemetry_fmt);
    tb_publish_telemetryv(flags, time, telemetry_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_send_server_rpc_resp(int req_id, const char* msg, int msg_len) {
    uint16_t res = 0;
    char* topic = NULL;
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, req_id)) == NULL) {
        return res;
    }
    LOG(LL_INFO, ("Publish server rpc response"));
    res = mgos_mqtt_pub(topic, msg, msg_len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    free(topic);
    return res;
}

uint16_t tb_send_server_rpc_respv(int req_id, const char* fmt, va_list ap) {
    uint16_t res = 0;
    char* topic = NULL;
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, req_id)) == NULL) {
        return res;
    }
    LOG(LL_INFO, ("Publish formatted server rpc response"));
    res = mgos_mqtt_pubv(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, fmt, ap);
    free(topic);
    return res;
}

uint16_t tb_send_server_rpc_respf(int req_id, const char* fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, fmt);
    tb_send_server_rpc_respv(req_id, fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_send_client_rpc_req(const char* method, const char* param, int* req_id) {
    int res = 0;
    if (!tb_config.user_active) {
        return res;
    }
    req_id = NULL;

    char* msg = NULL;
    if (method == NULL) {
        return res;
    } else if (param == NULL) {
        msg = json_asprintf("{method: %Q, params:%Q}", method, NULL);
    } else {
        msg = json_asprintf("{method: %Q, params:%s}", method, param);
    }

    if (msg == NULL) {
        LOG(LL_ERROR, ("Failed to create client rpc request"));
        return res;
    }

    char* topic = NULL;
    if ((topic = create_topic(RPC_REQ_PUB_TOPIC, tb_config.rpc_client_req_id)) == NULL) {
        goto out;
    }
    LOG(LL_INFO, ("Publish client rpc request"));

    res = mgos_mqtt_pub(topic, msg, strlen(msg), tb_config.mqtt_qos, tb_config.mqtt_retain);
    if (res > 0 && req_id != NULL) {
        *req_id = tb_config.rpc_client_req_id;
    }
    tb_config.rpc_client_req_id++;
out:
    free(msg);
    free(topic);
    return res;
}

uint16_t tb_send_client_rpc_reqv(int* req_id, const char* method, const char* param_fmt, va_list ap) {
    uint16_t res = 0;
    //TODO Check if we can dynamically create format and use json_asprintf once
    char* param = json_vasprintf(param_fmt, ap);
    if (param == NULL) {
        LOG(LL_ERROR, ("Failed to create formatted client rpc request"));
        return res;
    }
    res = tb_send_client_rpc_req(method, param, req_id);
    free(param);
    return res;
}

uint16_t tb_send_client_rpc_reqf(int* req_id, const char* method, const char* param_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, param_fmt);
    res = tb_send_client_rpc_reqv(req_id, method, param_fmt, ap);
    va_end(ap);
    return res;
}

static void mgos_rpc_resp_handler(struct mg_rpc* c, void* cb_arg,
                                  struct mg_rpc_frame_info* fi,
                                  struct mg_str result, int error_code, struct mg_str error_msg) {
    struct tb_rpc_server_data* rpc_data = (struct tb_rpc_server_data*)cb_arg;

    char* topic = NULL;
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, rpc_data->request_id)) == NULL) {
        goto out;
    }

    if (error_code == 0) {
        if (result.p != NULL) {
            LOG(LL_INFO, ("RPC successful, publish result"));
            mgos_mqtt_pub(topic, result.p, result.len, tb_config.mqtt_qos, tb_config.mqtt_retain);
        }
    } else if (error_code == 404 && mg_str_starts_with(error_msg, mg_mk_str("No handler")) == 1) {
        LOG(LL_INFO, ("RPC method not found, handle externally"));
        int count = 0;
        if (tb_config.user_active) {
            count = mgos_event_trigger(TB_SERVER_RPC_REQUEST, rpc_data);
        }
        if (count == 0) {
            mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{code:%d, error:%.*Q}",
                           error_code, error_msg.len, error_msg.p);
        }
    } else {
        LOG(LL_INFO, ("RPC failed, publish result"));
        mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{code:%d, error:%.*Q}",
                       error_code, error_msg.len, error_msg.p);
    }
out:
    free(topic);
    free(rpc_data->method);
    free(rpc_data->params);
    free(rpc_data);
}

static void server_rpc_req_handler(struct mg_connection* nc, const char* topic,
                                   int topic_len, const char* msg, int msg_len, void* ud) {
    char* rpc_method = NULL;
    char* rpc_param = NULL;
    int scan = json_scanf(msg, msg_len, "{ method:%Q, params:%Q }", &rpc_method, &rpc_param);
    if (scan > 0 && rpc_method != NULL) {
        //Handler needs to free the struct, rpc_method and rpc_param
        struct tb_rpc_server_data* rpc_data = malloc(sizeof(struct tb_rpc_server_data));
        rpc_data->request_id = get_topic_req_id(topic);
        rpc_data->params = rpc_param;
        rpc_data->method = rpc_method;

        struct mg_rpc_call_opts opts = {.dst = mg_mk_str(MGOS_RPC_LOOPBACK_ADDR)};
        char* fmt = NULL;
        if (rpc_param != NULL) {
            fmt = "%s";
        }
        mg_rpc_callf(mgos_rpc_get_global(), mg_mk_str(rpc_method), mgos_rpc_resp_handler,
                     rpc_data, &opts, fmt, rpc_param);
    } else {
        free(rpc_method);
        free(rpc_param);
    }
}

static void client_rpc_resp_handler(struct mg_connection* nc, const char* topic,
                                    int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("Client rpc response received"));
    int req_id = get_topic_req_id(topic);
    struct tb_rpc_client_data rpc_data = {.msg = msg, .msg_len = msg_len, .request_id = req_id};
    mgos_event_trigger(TB_CLIENT_RPC_RESPONSE, &rpc_data);
}

static void mqtt_event_handler(struct mg_connection* nc, int ev, void* ev_data, void* user_data) {
    if (ev == MG_EV_MQTT_CONNACK) {
        tb_request_attributes(NULL, "user_active");
        tb_request_shared_attributes();
        tb_publish_client_attributes();
    }
}

static void send_server_rpc_resp_callback(int ev, void* ev_data, void* userdata) {
    struct tb_rpc_server_data* dt = (struct tb_rpc_server_data*)ev_data;
    if (dt->params != NULL) {
        LOG(LL_INFO, ("Test 18 - Server rpc resp handler callback: %s - %.*s", dt->method, strlen(dt->params), dt->params));
    } else {
        LOG(LL_INFO, ("Test 18 - Server rpc resp handler callback: %s", dt->method));
    }
    char* testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "serverRpcResp", mgos_rand_range(-30.1, 500.23), 18);
    tb_send_server_rpc_resp(dt->request_id, testStr, strlen(testStr));
    free(testStr);
    (void)userdata;
}

static void send_server_rpc_respf_callback(int ev, void* ev_data, void* userdata) {
    struct tb_rpc_server_data* dt = (struct tb_rpc_server_data*)ev_data;
    if (dt->params != NULL) {
        LOG(LL_INFO, ("Test 20 - Server rpc resp handler, with json format string callback: %s - %.*s",
                      dt->method, strlen(dt->params), dt->params));
    } else {
        LOG(LL_INFO, ("Test 20 - Server rpc resp handler, with json format string callback: %s", dt->method));
    }
    tb_send_server_rpc_respf(dt->request_id, "{telType:%Q,rand:%f,remark:%d}", "serverRpcResp json format",
                             mgos_rand_range(-30.1, 500.23), 20);
    (void)userdata;
}

static void client_rpc_resp_callback(int ev, void* ev_data, void* userdata) {
    struct tb_rpc_client_data* dt = (struct tb_rpc_client_data*)ev_data;
    if (dt->msg != NULL) {
        LOG(LL_INFO, ("Test - Client rpc response: %d - %.*s", dt->request_id, dt->msg_len, dt->msg));
    } else {
        LOG(LL_INFO, ("Test - Client rpc response: %d", dt->request_id));
    }
    (void)userdata;
}

int btn_idx = 0;
void btn_cb(int pin, void* arg) {
    LOG(LL_INFO, ("btn_cb - Button pressed count: %d", btn_idx));

    int testInt = 0;
    char* testStr;
    switch (btn_idx) {
        case 0:
            LOG(LL_INFO, ("Test 0 - Request client and shared attributes"));
            tb_request_attributes("ctestBool,ctestDouble,ctestJson,ctestStr,notPresent0", "testBool,testDouble,testJson,testStr,notPresent0");
            break;
        case 1:
            LOG(LL_INFO, ("Test 1 - Request client attributes"));
            tb_request_attributes("ctestBool,ctestDouble,ctestJson,ctestStr,notPresent1", NULL);
            break;
        case 2:
            LOG(LL_INFO, ("Test 2 - Request shared attributes"));
            tb_request_attributes(NULL, "testBool,testDouble,testJson,testStr,notPresent2");
            break;
        case 3:
            LOG(LL_INFO, ("Test 3 - Request empty attributes"));
            tb_request_attributes("", "");
            break;
        case 4:
            LOG(LL_INFO, ("Test 4 - Request shared config attributes"));
            tb_request_shared_attributes();
            break;
        case 5:
            LOG(LL_INFO, ("Test 5 - Publish client config attributes"));
            tb_publish_client_attributes();
            break;
        case 6:
            LOG(LL_INFO, ("Test 6 - Publish custom client attributes, formatted string"));
            testStr = json_asprintf("{test:%Q,rand:%f,remark:%d}", "tb_publish_attributes", mgos_rand_range(-3.1, 100.23), 6);
            tb_publish_attributes(testStr, strlen(testStr));
            free(testStr);
            break;
        case 7:
            LOG(LL_INFO, ("Test 7 - Publish custom client attributes, json format"));
            tb_publish_attributesf("{test:%Q,rand:%f,remark:%d}", "tb_publish_attributesf", mgos_rand_range(-103.1, 1000.23), 7);
            break;
        case 8:
            LOG(LL_INFO, ("Test 8 - Publish timed telemetry with given time"));
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "given timed", mgos_rand_range(-30.1, 500.23), 8);
            tb_publish_telemetry(TBP_TELEMETRY_TIMED, 1572854626000, testStr, strlen(testStr));
            free(testStr);
            break;
        case 9:
            LOG(LL_INFO, ("Test 9 - Publish timed telemetry with current time"));
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "device timed", mgos_rand_range(-30.1, 500.23), 9);
            tb_publish_telemetry(TBP_TELEMETRY_TIMED, 0, testStr, strlen(testStr));
            free(testStr);
            break;
        case 10:
            LOG(LL_INFO, ("Test 10 - Publish delayed telemetry"));
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "delayed", mgos_rand_range(-30.1, 500.23), 10);
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 5000, testStr, strlen(testStr));
            free(testStr);
            break;
        case 11:
            LOG(LL_INFO, ("Test 11 - Publish delayed buffer telemetry"));
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "delayed buffer old", mgos_rand_range(-30.1, 500.23), 0);
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 5000, testStr, strlen(testStr));
            free(testStr);
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "delayed buffer new", mgos_rand_range(-30.1, 500.23), 11);
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 2000, testStr, strlen(testStr));
            free(testStr);
            break;
        case 12:
            LOG(LL_INFO, ("Test 12 - Publish normal telemetry"));
            testStr = json_asprintf("{telType:%Q,rand:%f,remark:%d}", "normal", mgos_rand_range(-30.1, 500.23), 12);
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 5000, testStr, strlen(testStr));
            free(testStr);
            break;
        case 13:
            LOG(LL_INFO, ("Test 13 - Publish normal telemetry with json format string"));
            tb_publish_telemetryf(TBP_TELEMETRY_DELAYED, 5000, "{telType:%Q,rand:%f,remark:%d}",
                                  "normal json format", mgos_rand_range(-30.1, 500.23), 13);
            break;
        case 14:
            LOG(LL_INFO, ("Test 14 - Send client rpc request, null method"));
            testStr = json_asprintf("{cReqTest:%Q,rand:%f,remark:%d}", NULL, mgos_rand_range(-30.1, 1500.23), 14);
            tb_send_client_rpc_req(NULL, testStr, &testInt);
            free(testStr);
            break;
        case 15:
            LOG(LL_INFO, ("Test 15 - Send client rpc request, null param"));
            tb_send_client_rpc_req("testMethod", NULL, &testInt);
            break;
        case 16:
            LOG(LL_INFO, ("Test 16 - Send client rpc request"));
            testStr = json_asprintf("{cReqTest:%Q,rand:%f,remark:%d}", "clientRpcReqTest",
                                    mgos_rand_range(-30.1, 1500.23), 16);
            tb_send_client_rpc_req("testMethod", testStr, &testInt);
            free(testStr);
            break;
        case 17:
            LOG(LL_INFO, ("Test 17 - Send client rpc request, with json format string"));
            tb_send_client_rpc_reqf(&testInt, "testMethod", "{cReqTest:%Q,rand:%f,remark:%d}", "clientRpcReqTest json string",
                                    mgos_rand_range(-30.1, 1500.23), 17);
            break;
        case 18:
            LOG(LL_INFO, ("Test 18 - Add server rpc resp handler"));
            mgos_event_add_handler(TB_SERVER_RPC_REQUEST, send_server_rpc_resp_callback, NULL);
            break;
        case 19:
            LOG(LL_INFO, ("Test 19 - Remove server rpc resp handler"));
            mgos_event_remove_handler(TB_SERVER_RPC_REQUEST, send_server_rpc_resp_callback, NULL);
            break;
        case 20:
            LOG(LL_INFO, ("Test 20 - Add server rpc resp handler with json format string"));
            mgos_event_add_handler(TB_SERVER_RPC_REQUEST, send_server_rpc_respf_callback, NULL);
            break;
        case 21:
            LOG(LL_INFO, ("Test 21 - Remove server rpc resp handler with json format string"));
            mgos_event_remove_handler(TB_SERVER_RPC_REQUEST, send_server_rpc_respf_callback, NULL);
            break;
        default:
            break;
    }

    btn_idx++;
    if (btn_idx > 21) {
        btn_idx = 0;
    }
}

enum mgos_app_init_result mgos_app_init(void) {
    mgos_event_register_base(TBP_EVENT_BASE, "Thingsboard Preesu Event");
    mgos_mqtt_sub(ATTR_RESP_SUB_TOPIC, attribute_response_handler, NULL);
    mgos_mqtt_sub(ATTR_TOPIC, attribute_update_handler, NULL);
    mgos_mqtt_sub(RPC_REQ_SUB_TOPIC, server_rpc_req_handler, NULL);
    mgos_mqtt_sub(RPC_RESP_SUB_TOPIC, client_rpc_resp_handler, NULL);
    mgos_mqtt_add_global_handler(mqtt_event_handler, NULL);

    LOG(LL_INFO, ("mgos_app_init - app initialized"));
    mgos_event_add_handler(TB_CLIENT_RPC_RESPONSE, client_rpc_resp_callback, NULL);
    mgos_gpio_set_button_handler(0, MGOS_GPIO_PULL_UP, MGOS_GPIO_INT_EDGE_NEG, 100, btn_cb, NULL);

    return MGOS_APP_INIT_SUCCESS;
}
