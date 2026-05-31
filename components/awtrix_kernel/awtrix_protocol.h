#pragma once

#ifdef __cplusplus

#include "awtrix_command.h"
#include <string>

struct AwtrixProtocolError
{
    int code = 0;
    std::string message;
};

bool awtrix_protocol_parse_http(const char* path, const char* body,
                                AwtrixCommand& out, AwtrixProtocolError& err);
bool awtrix_protocol_parse_mqtt(const char* topic, const char* payload,
                                AwtrixCommand& out, AwtrixProtocolError& err);

bool awtrix_protocol_http_command(const char* path, const char* body,
                                  AwtrixCommand& out, AwtrixProtocolError& err);

bool awtrix_protocol_validate_json_object(const char* body, bool allowEmpty,
                                          AwtrixProtocolError& err);
bool awtrix_protocol_validate_http_body(const char* path, const char* body,
                                        AwtrixProtocolError& err);
bool awtrix_protocol_validate_mqtt_body(const char* topic, const char* body,
                                        AwtrixProtocolError& err);

#endif /* __cplusplus */
