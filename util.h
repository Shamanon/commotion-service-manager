#ifndef UTIL_H
#define UTIL_H

#include <avahi-core/core.h>

#define ESCAPE_QUOTE "&quot;"
#define ESCAPE_QUOTE_LEN 6
#define ESCAPE_LF "&#10;"
#define ESCAPE_LF_LEN 5
#define ESCAPE_CR "&#13;"
#define ESCAPE_CR_LEN 5

#define OPEN_DELIMITER "\""
#define OPEN_DELIMITER_LEN 1
#define CLOSE_DELIMITER "\""
#define CLOSE_DELIMITER_LEN 1
#define FIELD_DELIMITER ","
#define FIELD_DELIMITER_LEN 1

int isHex(const char *str, size_t len);
int isNumeric (const char *s);
int isUCIEncoded(const char *s, size_t s_len);
int cmpstringp(const void *p1, const void *p2);
char *escape(char *to_escape, int *escaped_len);
char *txt_list_to_string(AvahiStringList *txt);

#endif