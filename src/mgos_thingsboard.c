#include "mgos.h"
#include "mgos_mqtt.h"

#define TBP_EVENT_BASE MGOS_EVENT_BASE('T', 'B', 'P')

const char* ATTR_PUB_TOPIC = "v1/devices/me/attributes";
const char* ATTR_REQ_PUB_TOPIC = "v1/devices/me/attributes/request/1";
const char* ATTR_UPDATE_SUB_TOPIC = "v1/devices/me/attributes";
const char* ATTR_RESP_SUB_TOPIC = "v1/devices/me/attributes/response/+";

enum tb_event {
    TB_INITIALIZED = TBP_EVENT_BASE,
    TB_ATTRIBUTE_UPDATE,
    TB_ATTRIBUTE_RESPONSE,
};

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

uint16_t tb_publish_client_attributes() {
    LOG(LL_INFO, ("tb_publish_client_attributes - publishing client attributes"));
    struct mbuf msg_mbuf;
    mbuf_init(&msg_mbuf, 0);
    mgos_conf_emit_cb(&mgos_sys_config, NULL, mgos_config_schema_tb_client(), true, &msg_mbuf, NULL, NULL);
    uint16_t res = mgos_mqtt_pub(ATTR_PUB_TOPIC, msg_mbuf.buf, msg_mbuf.len, 0, false);
    mbuf_free(&msg_mbuf);
    return res;
}

uint16_t tb_publish_client_attributesf(const char* json_fmt, ...) {
    uint16_t res;
    va_list ap;
    va_start(ap, json_fmt);
    res = mgos_mqtt_pubv(ATTR_PUB_TOPIC, 0, false, json_fmt, ap);
    va_end(ap);
    return res;
}

uint16_t tb_publish_client_attributesv(const char* json_fmt, va_list ap) {
    return mgos_mqtt_pubv(ATTR_PUB_TOPIC, 0, false, json_fmt, ap);
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
        tb_publish_client_attributes();
    }
}

void btn_cb(int pin, void* arg) {
    tb_sync_shared_attributes();
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
