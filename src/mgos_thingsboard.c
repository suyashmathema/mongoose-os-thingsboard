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
    int attr_req_id;
    int rpc_client_req_id;
    mgos_timer_id tele_delay_timer;
    char* delayed_telemetry;
    int mqtt_qos;
    bool mqtt_retain;
};

struct mgos_thingsboard_config tb_config = {
    .attr_req_id = 1,
    .rpc_client_req_id = 1,
    .tele_delay_timer = MGOS_INVALID_TIMER_ID,
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

uint16_t tb_request_attributes(const char* client_keys, const char* shared_keys) {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }

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
    LOG(LL_INFO, ("Request attributes, id:%u", res));
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

/*
This handler will be called when an attribute is requested to the thingsboard server.
The values for the requested attribute in json format is accessed with msg parameter.
The response json is parsed and stored as configuration.
*/
static void attribute_response_handler(struct mg_connection* nc, const char* topic,
                                       int topic_len, const char* msg, int msg_len, void* ud) {
    char* attr = json_asprintf("{tb:%.*s}", msg_len, msg);
    if (attr == NULL) {
        LOG(LL_ERROR, ("Failed to load attributes response for request"));
        return;
    }
    mgos_config_apply(attr, true);
    LOG(LL_INFO, ("Attributes received"));
    struct mg_str attr_kv = mg_mk_str_n(attr, strlen(attr));
    mgos_event_trigger(TB_ATTRIBUTE_RESPONSE, &attr_kv);
    free(attr);
}

/*
This handler will be called when an attribute is updated in the thingsboard server
with the changed attribute as json format in the msg parameter. The response json is parsed
and stored as configuration.
*/
static void attribute_update_handler(struct mg_connection* nc, const char* topic,
                                     int topic_len, const char* msg, int msg_len, void* ud) {
    char* attr = json_asprintf("{tb:{shared:%.*s}}", msg_len, msg);
    if (attr == NULL) {
        LOG(LL_ERROR, ("Failed to load attributes response for update"));
        return;
    }
    LOG(LL_INFO, ("Attributes update received"));
    mgos_config_apply(attr, true);
    struct mg_str attr_kv = mg_mk_str_n(attr, strlen(attr));
    mgos_event_trigger(TB_ATTRIBUTE_UPDATE, &attr_kv);
    free(attr);
}

uint16_t tb_publish_client_attributes() {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
    struct mbuf msg_mbuf;
    mbuf_init(&msg_mbuf, 0);
    mgos_conf_emit_cb(&mgos_sys_config, NULL, mgos_config_schema_tb_client(), true, &msg_mbuf, NULL, NULL);
    res = mgos_mqtt_pub(ATTR_TOPIC, msg_mbuf.buf, msg_mbuf.len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    LOG(LL_DEBUG, ("Publish client config attributes, id:%u", res));
    mbuf_free(&msg_mbuf);
    return res;
}

uint16_t tb_publish_attributes(const char* attributes, int attributes_len) {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
    res = mgos_mqtt_pub(ATTR_TOPIC, attributes, attributes_len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    LOG(LL_DEBUG, ("Publish client config attributes, id:%u", res));
    return res;
}

uint16_t tb_publish_attributesv(const char* json_fmt, va_list ap) {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
    res = mgos_mqtt_pubv(ATTR_TOPIC, tb_config.mqtt_qos, tb_config.mqtt_retain, json_fmt, ap);
    LOG(LL_DEBUG, ("Publish client config attributes, id:%u", res));
    return res;
}

uint16_t tb_publish_attributesf(const char* json_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, json_fmt);
    res = tb_publish_attributesv(json_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_publish_device_attributes() {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }

    struct mgos_net_ip_info ip_info;
    char sta_ip[16];
    char* sta = NULL;
    memset(sta_ip, 0, sizeof(sta_ip));
    if (mgos_net_get_ip_info(MGOS_NET_IF_TYPE_WIFI, MGOS_NET_IF_WIFI_STA, &ip_info)) {
        mgos_net_ip_to_str(&ip_info.ip, sta_ip);
        sta = sta_ip;
    }
    char ppp_ip[16];
    char* ppp = NULL;
    memset(ppp_ip, 0, sizeof(ppp_ip));
    if (mgos_net_get_ip_info(MGOS_NET_IF_TYPE_PPP, 0, &ip_info)) {
        mgos_net_ip_to_str(&ip_info.ip, ppp_ip);
        ppp = ppp_ip;
    }

    tb_publish_attributesf(
        "{mac:%Q, arch:%Q, app:%Q, fw_version:%Q, fw_timestamp:%Q, fw_id:%Q,"
        "build_id:%Q, build_timestamp:%Q, build_version:%Q, mg_build_id:%Q, mg_build_timestamp:%Q, mg_build_version:%Q,"
        "device_id: %Q, app: %Q, ram_size: %u, fs_size: %u, fs_free: %u, wifi_ip:%.*Q, ppp_ip:%.*Q}",
        mgos_sys_ro_vars_get_mac_address(),
        mgos_sys_ro_vars_get_arch(),
        mgos_sys_ro_vars_get_app(),
        mgos_sys_ro_vars_get_fw_version(),
        mgos_sys_ro_vars_get_fw_timestamp(),
        mgos_sys_ro_vars_get_fw_id(),
        build_id, build_timestamp, build_version,
        mg_build_id, mg_build_timestamp, mg_build_version,
        mgos_sys_config_get_device_id(), MGOS_APP,
        mgos_get_heap_size(), mgos_get_fs_size(),
        mgos_get_free_fs_size(), sizeof(sta_ip), sta, sizeof(ppp_ip), ppp);
    return res;
}

static void pub_delayed_telemetry_cb(void* arg) {
    uint16_t res = mgos_mqtt_pub(TELE_PUB_TOPIC, tb_config.delayed_telemetry, strlen(tb_config.delayed_telemetry),
                                 tb_config.mqtt_qos, tb_config.mqtt_retain);
    LOG(LL_DEBUG, ("Publish delayed telemetry, id:%u", res));
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
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }

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
    LOG(LL_DEBUG, ("Publish telemetry, id:%u", res));
    return res;
}

uint16_t tb_publish_telemetryv(int flags, int64_t time, const char* telemetry_fmt, va_list ap) {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
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
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
    char* topic = NULL;
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, req_id)) == NULL) {
        return res;
    }
    res = mgos_mqtt_pub(topic, msg, msg_len, tb_config.mqtt_qos, tb_config.mqtt_retain);
    LOG(LL_INFO, ("Publish server rpc response, id:%u", res));
    free(topic);
    return res;
}

uint16_t tb_send_server_rpc_respv(int req_id, const char* fmt, va_list ap) {
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
        return res;
    }
    char* topic = NULL;
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, req_id)) == NULL) {
        return res;
    }
    res = mgos_mqtt_pubv(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, fmt, ap);
    LOG(LL_INFO, ("Publish formatted server rpc response, id:%u", res));
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
    uint16_t res = 0;
    if (mgos_mqtt_get_global_conn() == NULL) {
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

    res = mgos_mqtt_pub(topic, msg, strlen(msg), tb_config.mqtt_qos, tb_config.mqtt_retain);
    LOG(LL_INFO, ("Publish client rpc request, id:%u", res));
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
    if ((topic = create_topic(RPC_RESP_PUB_TOPIC, rpc_data->request_id)) == NULL || mgos_mqtt_get_global_conn() == NULL) {
        goto out;
    }

    if (error_code == 0) {
        if (result.p != NULL) {
            //Device rpc call is executed successfully
            LOG(LL_INFO, ("RPC successful, publish result"));
            mgos_mqtt_pub(topic, result.p, result.len, tb_config.mqtt_qos, tb_config.mqtt_retain);
        }
    } else if (error_code == 404 && mg_str_starts_with(error_msg, mg_mk_str("No handler")) == 1) {
        LOG(LL_INFO, ("RPC method not found, handle externally"));
        int count = mgos_event_trigger(TB_SERVER_RPC_REQUEST, rpc_data);
        if (count == 0) {
            //Default response if rpc call is not handled externally
            mgos_mqtt_pubf(topic, tb_config.mqtt_qos, tb_config.mqtt_retain, "{code:%d, error:%.*Q}",
                           error_code, error_msg.len, error_msg.p);
        }
    } else {
        //Device rpc call failed
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
    //Convert thingsboard server rpc request to device rpc call
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
        tb_request_shared_attributes();
        tb_publish_client_attributes();
        tb_publish_device_attributes();
    }
}

bool mgos_thingsboard_init(void) {
    LOG(LL_INFO, ("Initializing thingsboard"));
    if (!mgos_sys_config_get_tb_enable()) {
        return true;
    }
    if (mgos_sys_config_get_tb_mqtt_qos() >= 0 && mgos_sys_config_get_tb_mqtt_qos() <= 3) {
        tb_config.mqtt_qos = mgos_sys_config_get_tb_mqtt_qos();
    }
    tb_config.mqtt_retain = mgos_sys_config_get_tb_mqtt_retain();

    mgos_event_register_base(TBP_EVENT_BASE, "Thingsboard Preesu Event");

    mgos_mqtt_sub(ATTR_RESP_SUB_TOPIC, attribute_response_handler, NULL);
    mgos_mqtt_sub(ATTR_TOPIC, attribute_update_handler, NULL);
    mgos_mqtt_sub(RPC_REQ_SUB_TOPIC, server_rpc_req_handler, NULL);
    mgos_mqtt_sub(RPC_RESP_SUB_TOPIC, client_rpc_resp_handler, NULL);

    mgos_mqtt_add_global_handler(mqtt_event_handler, NULL);

    return true;
}