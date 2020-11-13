#pragma once

#include "mgos.h"
#include "mgos_ro_vars.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TBP_EVENT_BASE MGOS_EVENT_BASE('T', 'B', 'P')

#define TBP_TELEMETRY_TIMED (1 << 0)
#define TBP_TELEMETRY_DELAYED (1 << 1)

extern const char* build_id;
extern const char* build_timestamp;
extern const char* build_version;
extern const char* mg_build_id;
extern const char* mg_build_timestamp;
extern const char* mg_build_version;

enum tb_event {
    TB_INITIALIZED = TBP_EVENT_BASE,
    TB_ATTRIBUTE_UPDATE,
    TB_ATTRIBUTE_RESPONSE,
    TB_CLIENT_RPC_RESPONSE,
    TB_SERVER_RPC_REQUEST,
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

/*
Request client and shared attributes from the thingsboard server.
Parameters
client_keys: The keys for the required client attributes in comma seperate string format.
shared_keys: The keys for the required shared attributes in comma seperate string format.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available, 0 is returned.
*/
uint16_t tb_request_attributes(const char* client_keys, const char* shared_keys);

/*
Request shared attributes from the thingsboard server which are specified as mongoose os configuration (tb.shared.*).
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available, 0 is returned.
*/
uint16_t tb_request_shared_attributes();

/*
Send client attributes to the thingsboard server which are specified as mongoose os configuration (tb.client.*).
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available, 0 is returned.
*/
uint16_t tb_publish_client_attributes();

/*
Send custom client attributes to the thingsboard server.
Parameters
attributes: A json string with key as client key and value as client key value.
attributes_len: The length of the json string.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_publish_attributes(const char* attributes, int attributes_len);

/*
Send custom client attributes to the thingsboard server.
Parameters
json_fmt: A json format string compatible with mgos JSON/frozen library.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_publish_attributesf(const char* json_fmt, ...);

uint16_t tb_publish_attributesv(const char* json_fmt, va_list ap);

/*
Send default client attributes to the thingsboard server.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_publish_device_attributes();

/*
Send telemetry to the thingsboard server.
Parameters
flags: A flag to set the telemetry mode. 0 for normal, TBP_TELEMETRY_TIMED to add current system time to telemetry
            and TBP_TELEMETRY_DELAYED for delaying telemetry.
time: For TBP_TELEMETRY_TIMED flag, epoch time in milliseconds to append to the telemetry.
        For TBP_TELEMETRY_DELAYED flag, time in milliseconds to delay the publish telemetry request.
telemetry: A Json format string with key as telemetry key and value as telemetry value.
telemetry_len: Length of json string.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_publish_telemetry(int flags, int64_t time, const char* telemetry, int telemetry_len);

uint16_t tb_publish_telemetryv(int flags, int64_t time, const char* telemetry_fmt, va_list ap);

uint16_t tb_publish_telemetryf(int flags, int64_t time, const char* telemetry_fmt, ...);

/*
To send a response to a rpc request from the thingsboard server. To be called when the event TB_SERVER_RPC_REQUEST is triggered.
Parameters
req_id: The request id of the server rpc request.
msg: A string with as a reponse to the rpc request.
msg_len: Length of the string.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_send_server_rpc_resp(int req_id, const char* msg, int msg_len);

uint16_t tb_send_server_rpc_respv(int req_id, const char* fmt, va_list ap);

uint16_t tb_send_server_rpc_respf(int req_id, const char* fmt, ...);

/*
To send a client rpc request to the thingsboard server. The response will be provided with the event TB_CLIENT_RPC_RESPONSE.
Parameters
method: The name of the server rpc method.
param: The parameter required for the rpc method in a json formatted string.
req_id: A pointer to an integer to store the generated request id.
Returns
The packet id (> 0) if there is a connection to the server and the message has been queued for sending.
In case no connection is available or incase of an error, 0 is returned.
*/
uint16_t tb_send_client_rpc_req(const char* method, const char* param, int* req_id);

uint16_t tb_send_client_rpc_reqv(int* req_id, const char* method, const char* param_fmt, va_list ap);

uint16_t tb_send_client_rpc_reqf(int* req_id, const char* method, const char* param_fmt, ...);

#ifdef __cplusplus
}
#endif