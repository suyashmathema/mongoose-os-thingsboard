#include "mgos.h"
#include "mgos_mqtt.h"

static void attribute_request_handler(struct mg_connection* nc, const char* topic,
                                      int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("attribute_request_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));

    char* attr = json_asprintf("{tb:%.*s}", msg_len, msg);
    if (attr != NULL) {
        mgos_config_apply(attr, true);
    } else {
        LOG(LL_INFO, ("attribute_request_handler - unable to load updated values to memory"));
    }
    free(attr);
}

static void attribute_update_handler(struct mg_connection* nc, const char* topic,
                                     int topic_len, const char* msg, int msg_len, void* ud) {
    LOG(LL_INFO, ("tb_attribute_handler - topic: %.*s, message: %.*s", topic_len, topic, msg_len, msg));
    char* attr = json_asprintf("{tb:{shared:%.*s}}", msg_len, msg);

    if (attr != NULL) {
        mgos_config_apply(attr, true);
    } else {
        LOG(LL_INFO, ("tb_attribute_handler - unable to load updated values to memory"));
    }
    free(attr);
}

static void mqtt_connack_timer_cb(void* arg) {
    LOG(LL_INFO, ("mqtt_connack_timer_cb - Requesting device shared attributes"));
    mgos_mqtt_pubf("v1/devices/me/attributes/request/1", 1, false,
                   "{ clientKeys:%Q, sharedKeys: %Q }",
                   "ctestInt,ctestString,ctestBool",
                   "testInt,testDouble,testJson");
    mgos_mqtt_pubf("v1/devices/me/attributes", 0, false,
                   "{ctestInt: %d, ctestString: %s, ctestBool: %B}",
                   9876, "a987362.23", true);
    (void)arg;
}

static void mqtt_event_handler(struct mg_connection* nc, int ev, void* ev_data, void* user_data) {
    if (ev == MG_EV_MQTT_CONNACK) {
        LOG(LL_INFO, ("mqtt_event_handler - MQTT connection acknowledge"));
        // mgos_set_timer(5000, 0, mqtt_connack_timer_cb, NULL);
        mgos_mqtt_pubf("v1/devices/me/attributes/request/1", 1, false,
                       "{ clientKeys:%Q, sharedKeys: %Q }",
                       "ctestInt,ctestString,ctestBool",
                       "testInt,testDouble,testJson");
    }
}

enum mgos_app_init_result mgos_app_init(void) {
    mgos_mqtt_sub("v1/devices/me/attributes/response/+", attribute_request_handler, NULL);
    mgos_mqtt_sub("v1/devices/me/attributes", attribute_update_handler, NULL);
    mgos_mqtt_add_global_handler(mqtt_event_handler, NULL);

    LOG(LL_INFO, ("mgos_app_init - app initialized"));
    return MGOS_APP_INIT_SUCCESS;
}
