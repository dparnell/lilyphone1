/**
 * Lookup over the bundled IANA zone table.
 *
 * timezone_names.h holds the names as one newline separated string and the
 * matching POSIX TZ strings as a parallel array. This is the only translation
 * unit that includes it, so the table exists once; everything else goes through
 * these accessors.
 */
#include <Arduino.h>
#include "timezone_db.h"
#include "timezone_names.h"

// Line starts within `timezone_names`, built once on first use.
#define TIMEZONE_INDEX_MAX 512
static const char *tz_line[TIMEZONE_INDEX_MAX];
static int         tz_num = -1;

static void tz_index_build(void)
{
    if(tz_num >= 0) return;

    tz_num = 0;
    const char *p = timezone_names;

    while(*p && tz_num < TIMEZONE_INDEX_MAX) {
        tz_line[tz_num++] = p;

        const char *nl = strchr(p, '\n');
        if(nl == NULL) break;
        p = nl + 1;
    }

    Serial.printf("[TZ] %d zones\n", tz_num);
}

int timezone_count(void)
{
    tz_index_build();
    return tz_num;
}

bool timezone_name_at(int idx, char *buf, int len)
{
    tz_index_build();

    if(buf == NULL || len <= 0) return false;
    buf[0] = '\0';
    if(idx < 0 || idx >= tz_num) return false;

    const char *p = tz_line[idx];
    int         n = 0;

    while(*p && *p != '\n' && n < len - 1) buf[n++] = *p++;
    buf[n] = '\0';
    return true;
}

const char *timezone_posix_at(int idx)
{
    tz_index_build();

    if(idx < 0 || idx >= tz_num) return NULL;
    if(idx >= (int)(sizeof(timezone_offsets) / sizeof(timezone_offsets[0]))) return NULL;
    return timezone_offsets[idx];
}

int timezone_find(const char *name)
{
    tz_index_build();

    if(name == NULL || name[0] == '\0') return -1;

    int want = strlen(name);
    for(int i = 0; i < tz_num; i++) {
        const char *p  = tz_line[i];
        const char *nl = strchr(p, '\n');
        int         n  = nl ? (int)(nl - p) : (int)strlen(p);

        if(n == want && strncmp(p, name, n) == 0) return i;
    }
    return -1;
}

/* Zone names use underscores and slashes where a person types spaces, so both
 * are folded to a space before matching. */
static char tz_fold(char c)
{
    if(c == '_' || c == '/') return ' ';
    return (char)tolower((unsigned char)c);
}

static bool tz_line_matches(const char *line, const char *filter)
{
    if(filter[0] == '\0') return true;

    for(const char *start = line; *start && *start != '\n'; start++) {
        const char *h = start;
        const char *n = filter;

        while(*h && *h != '\n' && *n && tz_fold(*h) == tz_fold(*n)) {
            h++;
            n++;
        }
        if(*n == '\0') return true;
    }
    return false;
}

int timezone_search(const char *filter, int *out, int max, int *total)
{
    tz_index_build();

    if(filter == NULL) filter = "";
    if(total) *total = 0;

    int written = 0;
    for(int i = 0; i < tz_num; i++) {
        if(!tz_line_matches(tz_line[i], filter)) continue;

        if(total) (*total)++;
        if(out && written < max) out[written++] = i;
    }
    return written;
}
