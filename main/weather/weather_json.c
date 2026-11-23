#include "weather_json.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static bool
is_json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

cJSON *
json_parse(const char *text, size_t length)
{
    if (!text || !length) {
        return NULL;
    }

    const char *end = NULL;
    cJSON      *root = cJSON_ParseWithLengthOpts(text, length, &end, 0);
    if (!root || !end) {
        cJSON_Delete(root);
        return NULL;
    }

    while ((size_t)(end - text) < length && is_json_space(*end)) {
        end++;
    }

    if ((size_t)(end - text) != length) {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

bool
json_num(const cJSON *item, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }

    *out = item->valuedouble;
    return true;
}

bool
json_int(const cJSON *item, int *out)
{
    double value;
    if (!json_num(item, &value) || floor(value) != value || value < INT_MIN || value > INT_MAX) {
        return false;
    }

    *out = (int)value;
    return true;
}

bool
json_string_with_length(const cJSON *item, size_t length, const char **value)
{
    const char *decoded = cJSON_GetStringValue(item);
    if (!decoded || strlen(decoded) != length) {
        return false;
    }

    *value = decoded;
    return true;
}

bool
json_str(const cJSON *object, const char *key, char *out, size_t cap)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    if (!value || strlen(value) >= cap) {
        return false;
    }

    strcpy(out, value);

    return true;
}

bool
json_object_array(const cJSON *object, const char *key, int count, cJSON **out)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != count) {
        return false;
    }

    *out = array;

    return true;
}
