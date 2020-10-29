#include "mgos.h"
#include "mgos_mqtt.h"

#define TBP_EVENT_BASE MGOS_EVENT_BASE('T', 'B', 'P')
#define TBP_TELEMETRY_TIMED (1 << 0)
#define TBP_TELEMETRY_DELAYED (1 << 1)

const char* ATTR_PUB_TOPIC = "v1/devices/me/attributes";
const char* ATTR_REQ_PUB_TOPIC = "v1/devices/me/attributes/request/1";
const char* ATTR_UPDATE_SUB_TOPIC = "v1/devices/me/attributes";
const char* ATTR_RESP_SUB_TOPIC = "v1/devices/me/attributes/response/+";
const char* TELE_PUB_TOPIC = "v1/devices/me/telemetry";

enum tb_event {
    TB_INITIALIZED = TBP_EVENT_BASE,
    TB_ATTRIBUTE_UPDATE,
    TB_ATTRIBUTE_RESPONSE,
};

struct mgos_thingsboard_config {
    bool user_active;
    mgos_timer_id tele_delay_timer;
    char* delayed_telemetry;
};

struct mgos_thingsboard_config tb_config = {.user_active = false, .tele_delay_timer = 0};

uint16_t tb_request_attributes(const char* client_keys, const char* shared_keys) {
    uint16_t res = 0;
    if (client_keys != NULL && shared_keys != NULL) {
        res = mgos_mqtt_pubf(ATTR_REQ_PUB_TOPIC, 1, false, "{ clientKeys:%Q, sharedKeys: %Q }",
                             client_keys, shared_keys);
    } else if (shared_keys != NULL) {
        res = mgos_mqtt_pubf(ATTR_REQ_PUB_TOPIC, 1, false, "{ sharedKeys: %Q }", shared_keys);
    } else if (client_keys != NULL) {
        res = mgos_mqtt_pubf(ATTR_REQ_PUB_TOPIC, 1, false, "{ clientKeys: %Q }", client_keys);
    }
    LOG(LL_INFO, ("tb_request_attributes - request attributes for id:%d", res));
    return res;
}

uint16_t tb_publish_config_attributes() {
    LOG(LL_INFO, ("tb_publish_config_attributes - publishing client attributes"));
    struct mbuf msg_mbuf;
    mbuf_init(&msg_mbuf, 0);
    mgos_conf_emit_cb(&mgos_sys_config, NULL, mgos_config_schema_tb_client(), true, &msg_mbuf, NULL, NULL);
    uint16_t res = mgos_mqtt_pub(ATTR_PUB_TOPIC, msg_mbuf.buf, msg_mbuf.len, 1, false);
    mbuf_free(&msg_mbuf);
    return res;
}

uint16_t tb_publish_attributes(const char* attributes, int attributes_len) {
    return mgos_mqtt_pub(ATTR_PUB_TOPIC, attributes, attributes_len, 1, false);
}

uint16_t tb_publish_attributesf(const char* json_fmt, ...) {
    uint16_t res = 0;
    va_list ap;
    va_start(ap, json_fmt);
    res = mgos_mqtt_pubv(ATTR_PUB_TOPIC, 1, false, json_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_publish_attributesv(const char* json_fmt, va_list ap) {
    return mgos_mqtt_pubv(ATTR_PUB_TOPIC, 1, false, json_fmt, ap);
}

static void pub_telemetry_cb(void* arg) {
    LOG(LL_INFO, ("pub_telemetry_cb - publishing delayed telemetry %.*s",
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
        tb_config.tele_delay_timer = mgos_set_timer(5000, 0, pub_telemetry_cb, NULL);
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

static void mqtt_event_handler(struct mg_connection* nc, int ev, void* ev_data, void* user_data) {
    if (ev == MG_EV_MQTT_CONNACK) {
        LOG(LL_INFO, ("mqtt_event_handler - MQTT connection acknowledge"));
        tb_sync_shared_attributes();
        tb_publish_config_attributes();
    }
}

int btn_idx = 0;
void btn_cb(int pin, void* arg) {
    // tb_sync_shared_attributes();
    LOG(LL_INFO, ("btn_cb - Button pressed count: %d", btn_idx));

    char* a;
    char* b;
    char* c;

    switch (btn_idx % 5) {
        case 0:
            a = "{\"temp\":23.43,\"hum\":122,\"address\":\"lalitpur\"}";
            tb_publish_telemetry(TBP_TELEMETRY_DELAYED, 0, a, strlen(a));
            break;
        case 1:
            b = "{\"temp\":35.23,\"hum\":98,\"address\":\"patan\"}";
            tb_publish_telemetry(TBP_TELEMETRY_TIMED, 0, b, strlen(b));
            break;
        case 2:
            c = "{\"temp\":33.43,\"hum\":222,\"address\":\"kalimati\"}";
            tb_publish_telemetry(0, 0, c, strlen(c));
            break;
        case 3:
            tb_publish_telemetryf(0, 0, "{temp:%f,hum:%d,address:%s}", 4.23, 99, "kathmandu");
            break;
        case 4:
            tb_publish_telemetryf(TBP_TELEMETRY_DELAYED, 0, "{temp:%d,hum:%d,address:%s}", 423, 99, "kathmandu");
            break;
        default:
            break;
    }
    btn_idx++;
}

enum mgos_app_init_result mgos_app_init(void) {
    mgos_event_register_base(TBP_EVENT_BASE, "Thingsboard Preesu Event");
    mgos_mqtt_sub(ATTR_RESP_SUB_TOPIC, attribute_request_handler, NULL);
    mgos_mqtt_sub(ATTR_UPDATE_SUB_TOPIC, attribute_update_handler, NULL);
    mgos_mqtt_add_global_handler(mqtt_event_handler, NULL);

    mgos_gpio_set_button_handler(0, MGOS_GPIO_PULL_UP, MGOS_GPIO_INT_EDGE_NEG, 100, btn_cb, NULL);

    LOG(LL_INFO, ("mgos_app_init - app initialized"));
    return MGOS_APP_INIT_SUCCESS;
}
