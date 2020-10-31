#include "mg_rpc_channel_loopback.h"
#include "mgos.h"
#include "mgos_mqtt.h"
#include "mgos_rpc.h"

#define TBP_EVENT_BASE MGOS_EVENT_BASE('T', 'B', 'P')
#define TBP_TELEMETRY_TIMED (1 << 0)
#define TBP_TELEMETRY_DELAYED (1 << 1)

const char* ATTR_TOPIC = "v1/devices/me/attributes";
const char* ATTR_REQ_PUB_TOPIC = "v1/devices/me/attributes/request/%d";
const char* ATTR_RESP_SUB_TOPIC = "v1/devices/me/attributes/response/+";
const char* TELE_PUB_TOPIC = "v1/devices/me/telemetry";
const char* RPC_REQ_SUB_TOPIC = "v1/devices/me/rpc/request/+";
const char* RPC_REQ_PUB_TOPIC = "v1/devices/me/rpc/request/%d";
const char* RPC_RESP_SUB_TOPIC = "v1/devices/me/rpc/response/+";
const char* RPC_RESP_PUB_TOPIC = "v1/devices/me/rpc/response/%d";

enum tb_event {
    TB_INITIALIZED = TBP_EVENT_BASE,
    TB_ATTRIBUTE_UPDATE,
    TB_ATTRIBUTE_RESPONSE,
    TB_RPC_CLIENT_RESPONSE,
    TB_RPC_SERVER_REQUEST,
};

struct mgos_thingsboard_config {
    bool user_active;
    int attr_req_id;
    int rpc_client_req_id;
    mgos_timer_id tele_delay_timer;
    char* delayed_telemetry;
};

struct tb_rpc_server_data {
    char* params;
    char* method;
    int request_id;
};

struct tb_rpc_client_data {
    const char* msg;
    int msg_len;
    int request_id;
};

struct mgos_thingsboard_config tb_config = {
    .user_active = false,
    .attr_req_id = 1,
    .rpc_client_req_id = 1,
    .tele_delay_timer = 0,
    .delayed_telemetry = NULL};

static int get_topic_req_id(const char* topic) {
    char* id_ptr = strrchr(topic, '/');
    return atoi(++id_ptr);
}

uint16_t tb_request_attributes(const char* client_keys, const char* shared_keys) {
    uint16_t res = 0;
    char* req_topic = NULL;
    int size = mg_asprintf(&req_topic, 0, ATTR_REQ_PUB_TOPIC, tb_config.attr_req_id);
    if (size == -1) {
        LOG(LL_INFO, ("tb_request_attributes - failed to create topic"));
        return res;
    }

    if (client_keys != NULL && shared_keys != NULL) {
        res = mgos_mqtt_pubf(req_topic, 1, false, "{ clientKeys:%Q, sharedKeys: %Q }",
                             client_keys, shared_keys);
    } else if (shared_keys != NULL) {
        res = mgos_mqtt_pubf(req_topic, 1, false, "{ sharedKeys: %Q }", shared_keys);
    } else if (client_keys != NULL) {
        res = mgos_mqtt_pubf(req_topic, 1, false, "{ clientKeys: %Q }", client_keys);
    }
    LOG(LL_INFO, ("tb_request_attributes - request attributes, id:%d", res));
    tb_config.attr_req_id++;
    free(req_topic);
    return res;
}

uint16_t tb_publish_config_attributes() {
    LOG(LL_INFO, ("tb_publish_config_attributes - publishing client attributes"));
    struct mbuf msg_mbuf;
    mbuf_init(&msg_mbuf, 0);
    mgos_conf_emit_cb(&mgos_sys_config, NULL, mgos_config_schema_tb_client(), true, &msg_mbuf, NULL, NULL);
    uint16_t res = mgos_mqtt_pub(ATTR_TOPIC, msg_mbuf.buf, msg_mbuf.len, 1, false);
    mbuf_free(&msg_mbuf);
    return res;
}

uint16_t tb_publish_attributes(const char* attributes, int attributes_len) {
    return mgos_mqtt_pub(ATTR_TOPIC, attributes, attributes_len, 1, false);
}

uint16_t tb_publish_attributesf(const char* json_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, json_fmt);
    res = mgos_mqtt_pubv(ATTR_TOPIC, 1, false, json_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_publish_attributesv(const char* json_fmt, va_list ap) {
    return mgos_mqtt_pubv(ATTR_TOPIC, 1, false, json_fmt, ap);
}

static void pub_delayed_telemetry_cb(void* arg) {
    LOG(LL_INFO, ("pub_delayed_telemetry_cb - published delayed telemetry %.*s",
                  strlen(tb_config.delayed_telemetry), tb_config.delayed_telemetry));
    mgos_mqtt_pub(TELE_PUB_TOPIC, tb_config.delayed_telemetry, strlen(tb_config.delayed_telemetry), 1, false);
    if (tb_config.delayed_telemetry != NULL) {
        free(tb_config.delayed_telemetry);
        tb_config.delayed_telemetry = NULL;
    }
    mgos_clear_timer(tb_config.tele_delay_timer);
    tb_config.tele_delay_timer = 0;
    (void)arg;
}

uint16_t tb_publish_telemetry(int flags, unsigned int time, const char* telemetry, int telemetry_len) {
    uint16_t res = 0;

    if (flags & TBP_TELEMETRY_TIMED) {
        if (time == 0) {
            //TODO get timestamp not uptime
            time = mgos_uptime_micros() / 1000;
        }
        char* timed_telemetry = json_asprintf("{ts:%d, values:%.*s}", time, telemetry_len, telemetry);
        LOG(LL_INFO, ("tb_publish_telemetry - published timed telemetry %.*s", strlen(timed_telemetry), timed_telemetry));
        res = mgos_mqtt_pub(TELE_PUB_TOPIC, timed_telemetry, strlen(timed_telemetry), 1, false);
        free(timed_telemetry);
    } else if (flags & TBP_TELEMETRY_DELAYED) {
        if (tb_config.delayed_telemetry != NULL) {
            free(tb_config.delayed_telemetry);
            tb_config.delayed_telemetry = NULL;
        }
        mgos_clear_timer(tb_config.tele_delay_timer);
        tb_config.tele_delay_timer = 0;

        tb_config.delayed_telemetry = strndup(telemetry, telemetry_len);
        tb_config.tele_delay_timer = mgos_set_timer(5000, 0, pub_delayed_telemetry_cb, NULL);
    } else {
        LOG(LL_INFO, ("tb_publish_telemetry - published telemetry %.*s", telemetry_len, telemetry));
        res = mgos_mqtt_pub(TELE_PUB_TOPIC, telemetry, telemetry_len, 1, false);
    }
    return res;
}

uint16_t tb_publish_telemetryf(int flags, unsigned int time, const char* telemetry_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, telemetry_fmt);
    char* telemetry = json_vasprintf(telemetry_fmt, ap);
    res = tb_publish_telemetry(flags, time, telemetry, strlen(telemetry));
    free(telemetry);
    va_end(ap);
    return res;
}

uint16_t tb_publish_telemetryv(int flags, unsigned int time, const char* telemetry_fmt, va_list ap) {
    uint16_t res = 0;
    char* telemetry = json_vasprintf(telemetry_fmt, ap);
    res = tb_publish_telemetry(flags, time, telemetry, strlen(telemetry));
    free(telemetry);
    return res;
}

uint16_t tb_send_server_rpc_resp(int req_id, const char* msg, int msg_len) {
    uint16_t res = 0;
    char* topic = NULL;
    int size = mg_asprintf(&topic, 0, RPC_RESP_PUB_TOPIC, req_id);
    if (size == -1) {
        LOG(LL_INFO, ("tb_request_attributes - failed to create topic"));
        return res;
    }
    LOG(LL_INFO, ("tb_send_server_rpc_resp - published server response %.*s", msg_len, msg));
    res = mgos_mqtt_pub(topic, msg, msg_len, 1, false);
    free(topic);
    return res;
}

uint16_t tb_send_server_rpc_respf(int req_id, const char* fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, fmt);
    char* msg = json_vasprintf(fmt, ap);
    res = tb_send_server_rpc_resp(req_id, msg, strlen(msg));
    free(msg);
    va_end(ap);
    return res;
}

uint16_t tb_send_server_rpc_respv(int req_id, const char* fmt, va_list ap) {
    uint16_t res = 0;
    char* msg = json_vasprintf(fmt, ap);
    res = tb_send_server_rpc_resp(req_id, msg, strlen(msg));
    free(msg);
    return res;
}

uint16_t client_rpc_req_handler(const char* method, const char* param, int* req_id) {
    int res = 0;
    char* rpc_req_topic = NULL;
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
        return res;
    }
    LOG(LL_INFO, ("client_rpc_req_handler - published client rpc request msg: %s", msg));

    int size = mg_asprintf(&rpc_req_topic, 0, RPC_REQ_PUB_TOPIC, tb_config.rpc_client_req_id);
    if (size == -1) {
        LOG(LL_INFO, ("client_rpc_req_handler - failed to create topic"));
        goto out;
    }
    LOG(LL_INFO, ("client_rpc_req_handler - published client rpc request topic %s", rpc_req_topic));

    res = mgos_mqtt_pub(rpc_req_topic, msg, strlen(msg), 1, false);
    if (res > 0 && req_id != NULL) {
        *req_id = tb_config.rpc_client_req_id;
    }
    tb_config.rpc_client_req_id++;
out:
    free(msg);
    free(rpc_req_topic);
    return res;
}

uint16_t tb_rpc_client_reqf(int* req_id, const char* method, const char* param_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, param_fmt);
    char* param = json_vasprintf(param_fmt, ap);
    res = client_rpc_req_handler(method, param, req_id);
    free(param);
    va_end(ap);
    return res;
}

uint16_t tb_rpc_client_reqv(int* req_id, const char* method, const char* param_fmt, va_list ap) {
    uint16_t res = 0;
    char* param = json_vasprintf(param_fmt, ap);
    res = client_rpc_req_handler(method, param, req_id);
    free(param);
    return res;
}

uint16_t tb_sync_shared_attributes() {
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
    uint16_t res = tb_request_attributes(NULL, shared_keys.buf);

    mbuf_free(&shared_keys);
    return res;
}

static void attribute_request_handler(struct mg_connection* nc, const char* topic,
                                      int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("attribute_request_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));

    char* attr = json_asprintf("{tb:%.*s}", msg_len, msg);
    if (attr != NULL) {
        mgos_config_apply(attr, true);
        struct mg_str attr_kv = mg_mk_str_n(msg, msg_len);
        mgos_event_trigger(TB_ATTRIBUTE_RESPONSE, &attr_kv);
    } else {
        LOG(LL_INFO, ("attribute_request_handler - unable to load updated values to memory"));
    }
    free(attr);
}

static void attribute_update_handler(struct mg_connection* nc, const char* topic,
                                     int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("attribute_update_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));
    char* attr = json_asprintf("{tb:{shared:%.*s}}", msg_len, msg);

    if (attr != NULL) {
        mgos_config_apply(attr, true);
        struct mg_str attr_kv = mg_mk_str_n(msg, msg_len);
        mgos_event_trigger(TB_ATTRIBUTE_UPDATE, &attr_kv);
    } else {
        LOG(LL_INFO, ("attribute_update_handler - unable to load updated values to memory"));
    }
    free(attr);
}

static void server_rpc_req_handler(struct mg_rpc* c, void* cb_arg,
                                   struct mg_rpc_frame_info* fi,
                                   struct mg_str result, int error_code, struct mg_str error_msg) {
    struct tb_rpc_server_data* rpc_data = (struct tb_rpc_server_data*)cb_arg;

    char* topic = NULL;
    int size = mg_asprintf(&topic, 0, RPC_RESP_PUB_TOPIC, rpc_data->request_id);
    if (size == -1) {
        LOG(LL_INFO, ("tb_request_attributes - failed to create topic"));
        goto out;
    }

    LOG(LL_INFO, ("server_rpc_req_handler - topic:%s", topic));
    if (error_code == 0) {
        if (result.p != NULL) {
            LOG(LL_INFO, ("server_rpc_req_handler - SUCCESS"));
            mgos_mqtt_pub(topic, result.p, result.len, 1, false);
        }
    } else if (error_code == 404) {
        int count = mgos_event_trigger(TB_RPC_SERVER_REQUEST, rpc_data);
        if (count == 0) {
            LOG(LL_INFO, ("server_rpc_req_handler - FAILURE - code: %d", error_code));
            mgos_mqtt_pubf(topic, 1, false, "{code:%d, msg:%.*Q}", error_code, error_msg.len, error_msg.p);
        }
    } else {
        LOG(LL_INFO, ("server_rpc_req_handler - FAILURE - code: %d", error_code));
        mgos_mqtt_pubf(topic, 1, false, "{code:%d, msg:%.*Q}", error_code, error_msg.len, error_msg.p);
    }
out:
    free(topic);
    free(rpc_data->method);
    free(rpc_data->params);
    free(rpc_data);
}

static void rpc_server_req_handler(struct mg_connection* nc, const char* topic,
                                   int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("rpc_server_req_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));

    char* rpc_method = NULL;
    char* rpc_param = NULL;
    int scan = json_scanf(msg, msg_len, "{ method:%Q, params:%Q }", &rpc_method, &rpc_param);
    if (scan > 0 && rpc_method != NULL) {
        struct tb_rpc_server_data* rpc_data = malloc(sizeof(struct tb_rpc_server_data));
        rpc_data->request_id = get_topic_req_id(topic);
        LOG(LL_INFO, ("rpc_server_req_handler - request id: %d", rpc_data->request_id));
        rpc_data->params = rpc_param;
        rpc_data->method = rpc_method;

        struct mg_rpc_call_opts opts = {.dst = mg_mk_str(MGOS_RPC_LOOPBACK_ADDR)};
        char* fmt = NULL;
        if (rpc_param != NULL) {
            fmt = "%s";
        }
        mg_rpc_callf(mgos_rpc_get_global(), mg_mk_str(rpc_method), server_rpc_req_handler,
                     rpc_data, &opts, fmt, rpc_param);
    } else {
        free(rpc_method);
        free(rpc_param);
    }
}

static void rpc_client_resp_handler(struct mg_connection* nc, const char* topic,
                                    int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("rpc_client_resp_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));
    int req_id = get_topic_req_id(topic);
    struct tb_rpc_client_data rpc_data = {.msg = msg, .msg_len = msg_len, .request_id = req_id};
    mgos_event_trigger(TB_RPC_CLIENT_RESPONSE, &rpc_data);
}

static void mqtt_event_handler(struct mg_connection* nc, int ev, void* ev_data, void* user_data) {
    if (ev == MG_EV_MQTT_CONNACK) {
        LOG(LL_INFO, ("mqtt_event_handler - MQTT connection acknowledge"));
        tb_sync_shared_attributes();
        tb_publish_config_attributes();
    }
}

int btn_idx = 0;
static void server_rpc_event_test_cb(int ev, void* ev_data, void* userdata) {
    LOG(LL_INFO, ("Going to reboot!"));
    struct tb_rpc_server_data* asd = (struct tb_rpc_server_data*)ev_data;
    if (btn_idx % 2 == 0) {
        tb_send_server_rpc_resp(asd->request_id, asd->params, strlen(asd->params));
    } else {
        tb_send_server_rpc_respf(asd->request_id, "{myresp:%Q}", "hello");
    }
    (void)ev_data;
    (void)userdata;
}

void btn_cb(int pin, void* arg) {
    // tb_sync_shared_attributes();
    LOG(LL_INFO, ("btn_cb - Button pressed count: %d", btn_idx % 12));

    char* a;
    char* b;
    char* c;
    int asd;

    switch (btn_idx % 12) {
        case 0:
            tb_rpc_client_reqf(&asd, "getAdd", "{mathema:%Q, reeha:%f}", "suyash", 123.123);
            break;
        case 1:
            tb_rpc_client_reqf(&asd, "getName", "%s", "suyash");
            // b = "{\"temp\":35.23,\"hum\":98,\"address\":\"patan\"}";
            // tb_publish_telemetry(TBP_TELEMETRY_TIMED, 0, b, strlen(b));
            break;
        case 2:
            mgos_event_add_handler(TB_RPC_SERVER_REQUEST, server_rpc_event_test_cb, NULL);
            // c = "{\"temp\":33.43,\"hum\":222,\"address\":\"kalimati\"}";
            // tb_publish_telemetry(0, 0, c, strlen(c));
            break;
        case 3:
            tb_rpc_client_reqf(&asd, NULL, "{mathema:%Q, reeha:%d}", "suyash", 123);
            // tb_publish_telemetryf(0, 0, "{temp:%f,hum:%d,address:%s}", 4.23, 99, "kathmandu");
            break;
        case 4:
            tb_rpc_client_reqf(&asd, "getSurname", "{mathema:%Q, reeha:%d}", NULL, 123);
            // tb_publish_telemetryf(TBP_TELEMETRY_DELAYED, 0, "{temp:%d,hum:%d,address:%s}", 423, 99, "kathmandu");
            break;
        case 5:
            tb_rpc_client_reqf(&asd, "getSurname", "%Q", NULL);
            // tb_request_attributes("ctestInt,ctestDouble,ctestJson,suyash", "testInttestJson,testDouble,testBool,mynewjson,mathema");
            break;
        case 6:
            tb_request_attributes(NULL, "testInttestJson,testDouble,testBool,mynewjson,mathema");
            break;
        case 7:
            tb_request_attributes("ctestInt,ctestDouble,ctestJson,mathema", NULL);
            break;
        case 8:
            a = "{\"temp\":23.43,\"hum\":122,\"address\":\"lalitpur\"}";
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 0, a, strlen(a));
            break;
        case 9:
            tb_rpc_client_reqf(&asd, "getSurname", "{ mytest: %Q, testdy: %f }", "suyashmathema", 99.11);
            break;
        case 10:
            tb_rpc_client_reqf(&asd, "getName", "%Q", "suyash");
            break;
        case 11:
            mgos_event_remove_handler(TB_RPC_SERVER_REQUEST, server_rpc_event_test_cb, NULL);
            break;
        default:
            break;
    }
    btn_idx++;
}

enum mgos_app_init_result mgos_app_init(void) {
    mgos_event_register_base(TBP_EVENT_BASE, "Thingsboard Preesu Event");
    mgos_mqtt_sub(ATTR_RESP_SUB_TOPIC, attribute_request_handler, NULL);
    mgos_mqtt_sub(ATTR_TOPIC, attribute_update_handler, NULL);
    mgos_mqtt_sub(RPC_REQ_SUB_TOPIC, rpc_server_req_handler, NULL);
    mgos_mqtt_sub(RPC_RESP_SUB_TOPIC, rpc_client_resp_handler, NULL);
    mgos_mqtt_add_global_handler(mqtt_event_handler, NULL);

    mgos_gpio_set_button_handler(0, MGOS_GPIO_PULL_UP, MGOS_GPIO_INT_EDGE_NEG, 100, btn_cb, NULL);

    LOG(LL_INFO, ("mgos_app_init - app initialized"));
    return MGOS_APP_INIT_SUCCESS;
}
