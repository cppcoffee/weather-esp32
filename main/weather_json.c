#include "weather_json.h"

#include <limits.h>
#include <math.h>
#include <string.h>

// Registry cJSON keeps only the numeric value; reserve a private bit for lexical integer syntax.
#define JSON_NUMBER_IS_INTEGER (1 << 10)

static bool is_json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_json_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool skip_json_string(const char *text, size_t length, size_t *offset)
{
    if (*offset >= length || text[*offset] != '"') {
        return false;
    }
    (*offset)++;
    while (*offset < length) {
        unsigned char c = (unsigned char)text[(*offset)++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (*offset >= length) {
                return false;
            }
            if (text[*offset] == 'u' && *offset + 4 < length && text[*offset + 1] == '0' && text[*offset + 2] == '0'
                && text[*offset + 3] == '0' && text[*offset + 4] == '0')
            {
                return false;
            }
            (*offset)++;
        } else if (c < 0x20) {
            return false;
        }
    }
    return false;
}

static bool scan_json_number(const char *text, size_t length, size_t *offset, bool *integer_syntax)
{
    size_t i = *offset;
    bool integer = true;
    if (i < length && text[i] == '-') {
        i++;
    }
    if (i >= length) {
        return false;
    }
    if (text[i] == '0') {
        i++;
        if (i < length && is_json_digit(text[i])) {
            return false;
        }
    } else {
        if (text[i] < '1' || text[i] > '9') {
            return false;
        }
        do {
            i++;
        } while (i < length && is_json_digit(text[i]));
    }
    if (i < length && text[i] == '.') {
        integer = false;
        i++;
        if (i >= length || !is_json_digit(text[i])) {
            return false;
        }
        do {
            i++;
        } while (i < length && is_json_digit(text[i]));
    }
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        integer = false;
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        if (i >= length || !is_json_digit(text[i])) {
            return false;
        }
        do {
            i++;
        } while (i < length && is_json_digit(text[i]));
    }
    if (i < length && !is_json_space(text[i]) && text[i] != ',' && text[i] != ']' && text[i] != '}') {
        return false;
    }
    *offset = i;
    *integer_syntax = integer;
    return true;
}

static bool validate_json_numbers(const char *text, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        if (text[offset] == '"') {
            if (!skip_json_string(text, length, &offset)) {
                return false;
            }
        } else if (text[offset] == '-' || is_json_digit(text[offset])) {
            bool integer_syntax;
            if (!scan_json_number(text, length, &offset, &integer_syntax)) {
                return false;
            }
        } else {
            offset++;
        }
    }
    return true;
}

struct json_number_iter {
    const char *text;
    size_t length;
    size_t offset;
    bool failed;
};

static bool next_json_number(struct json_number_iter *iter, bool *integer_syntax)
{
    while (iter->offset < iter->length) {
        if (iter->text[iter->offset] == '"') {
            if (!skip_json_string(iter->text, iter->length, &iter->offset)) {
                iter->failed = true;
                return false;
            }
        } else if (iter->text[iter->offset] == '-' || is_json_digit(iter->text[iter->offset])) {
            if (!scan_json_number(iter->text, iter->length, &iter->offset, integer_syntax)) {
                iter->failed = true;
                return false;
            }
            return true;
        } else {
            iter->offset++;
        }
    }
    return false;
}

static void mark_integer_numbers(cJSON *item, struct json_number_iter *iter)
{
    for (; item; item = item->next) {
        if (cJSON_IsNumber(item)) {
            bool integer_syntax;
            if (!next_json_number(iter, &integer_syntax)) {
                iter->failed = true;
                return;
            }
            if (integer_syntax) {
                item->type |= JSON_NUMBER_IS_INTEGER;
            }
        }
        if (item->child) {
            mark_integer_numbers(item->child, iter);
            if (iter->failed) {
                return;
            }
        }
    }
}

cJSON *json_parse(const char *text, size_t length)
{
    if (!text || !length || !validate_json_numbers(text, length)) {
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length, &end, 0);
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
    // Preserve the strict integer contract without modifying the managed cJSON source.
    struct json_number_iter iter = {.text = text, .length = length};
    mark_integer_numbers(root, &iter);
    if (iter.failed || next_json_number(&iter, &(bool){false})) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

bool json_num(const cJSON *item, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

bool json_int(const cJSON *item, int *out)
{
    double value;
    if (!json_num(item, &value) || !(item->type & JSON_NUMBER_IS_INTEGER) || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

bool json_string_with_length(const cJSON *item, size_t length, const char **value)
{
    const char *decoded = cJSON_GetStringValue(item);
    if (!decoded || strlen(decoded) != length) {
        return false;
    }
    *value = decoded;
    return true;
}

bool json_str(const cJSON *object, const char *key, char *out, size_t cap)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    if (!value || strlen(value) >= cap) {
        return false;
    }
    strcpy(out, value);
    return true;
}

bool json_object_array(const cJSON *object, const char *key, int count, cJSON **out)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != count) {
        return false;
    }
    *out = array;
    return true;
}
