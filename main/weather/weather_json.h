#ifndef __WEATHER_JSON_H__
#define __WEATHER_JSON_H__

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

// Generic cJSON helpers shared by the weather parser.
// They only validate JSON shapes and primitive values; weather data contract checks
// (date continuity, value ranges, array counts) stay in weather.c.

// Parse exactly `length` bytes of JSON; reject trailing non-whitespace content.
// Caller owns the returned tree and must cJSON_Delete() it.
cJSON *json_parse(const char *text, size_t length);

// Finite JSON number.
bool json_num(const cJSON *item, double *out);

// JSON integer that fits in int.
bool json_int(const cJSON *item, int *out);

// JSON string whose decoded value is exactly `length` bytes.
bool json_string_with_length(const cJSON *item, size_t length, const char **value);

// Copy the object's string member `key` into out; fails if missing or does not fit.
bool json_str(const cJSON *object, const char *key, char *out, size_t cap);

// The object's array member `key` with exactly `count` items.
bool json_object_array(const cJSON *object, const char *key, int count, cJSON **out);

#endif
