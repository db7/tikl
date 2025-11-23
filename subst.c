#define _POSIX_C_SOURCE 200809L
#include "subst.h"

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} subst_strbuf;

static void
subst_die(const char *who)
{
    if (!who)
        who = "tikl";
    fprintf(stderr, "%s: OOM\n", who);
    exit(2);
}

static void *
subst_xrealloc(const char *who, void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (!p)
        subst_die(who);
    return p;
}

static char *
subst_xstrdup(const char *who, const char *s)
{
    char *d = strdup(s ? s : "");
    if (!d)
        subst_die(who);
    return d;
}

static void
sb_append_char(const char *who, subst_strbuf *sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        sb->data = subst_xrealloc(who, sb->data, sb->cap);
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void
sb_append_str(const char *who, subst_strbuf *sb, const char *s)
{
    while (s && *s)
        sb_append_char(who, sb, *s++);
}

static char *
sb_steal(subst_strbuf *sb)
{
    if (!sb->data) {
        sb->data = subst_xstrdup(NULL, "");
        sb->len = 0;
        sb->cap = 1;
    }
    char *out = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return out;
}

static void
sb_free(subst_strbuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static char *
run_builtin_function(const char *who, const char *name, const char *arg, int *status)
{
    if (strcmp(name, "basename") == 0) {
        char *tmp = subst_xstrdup(who, arg ? arg : "");
        char *leaf = basename(tmp);
        char *out = subst_xstrdup(who, leaf);
        free(tmp);
        return out;
    }
    if (strcmp(name, "realpath") == 0) {
        if (!arg || !*arg)
            arg = ".";
        char *resolved = realpath(arg, NULL);
        if (!resolved) {
            fprintf(stderr, "%s: realpath %s: %s\n", who ? who : "tikl",
                    arg, strerror(errno));
            if (status)
                *status = 1;
            return NULL;
        }
        return resolved;
    }
    if (strcmp(name, "dirname") == 0) {
        const char *src = (arg && *arg) ? arg : ".";
        char *tmp = subst_xstrdup(who, src);
        char *dir = dirname(tmp);
        char *out = subst_xstrdup(who, dir ? dir : ".");
        free(tmp);
        return out;
    }
    fprintf(stderr, "%s: unknown placeholder function: %s\n",
            who ? who : "tikl", name);
    if (status)
        *status = 1;
    return NULL;
}

char *
tikl_expand_placeholders(const char *input,
                         bool allow_expansion,
                         bool helpers_enabled,
                         tikl_subst_lookup_fn lookup,
                         void *lookup_ctx,
                         const char *who,
                         int *status)
{
    if (!input)
        return NULL;
    if (!allow_expansion)
        return subst_xstrdup(who, input);

    subst_strbuf sb = {0};
    const char *p = input;
    while (*p) {
        if (status && *status != 0) {
            sb_free(&sb);
            return NULL;
        }
        if (*p == '%') {
            if (p[1] == '%') {
                sb_append_char(who, &sb, '%');
                p += 2;
                continue;
            }
            if (helpers_enabled && p[1] == '(') {
                const char *start = p + 2;
                const char *q = start;
                int depth = 1;
                while (*q && depth > 0) {
                    if (*q == '(')
                        depth++;
                    else if (*q == ')') {
                        depth--;
                        if (depth == 0)
                            break;
                    }
                    q++;
                }
                if (depth != 0) {
                    fprintf(stderr, "%s: unterminated %%(: %s\n",
                            who ? who : "tikl", input);
                    if (status)
                        *status = 1;
                    sb_free(&sb);
                    return NULL;
                }
                size_t inner_len = (size_t)(q - start);
                char *expr = strndup(start, inner_len);
                if (!expr)
                    subst_die(who);
                char *cursor = expr;
                while (*cursor && isspace((unsigned char)*cursor))
                    cursor++;
                if (*cursor == '\0') {
                    fprintf(stderr, "%s: empty %%( ) expression\n",
                            who ? who : "tikl");
                    if (status)
                        *status = 1;
                    free(expr);
                    sb_free(&sb);
                    return NULL;
                }
                char *fname = cursor;
                while (*cursor && !isspace((unsigned char)*cursor))
                    cursor++;
                if (*cursor)
                    *cursor++ = '\0';
                while (*cursor && isspace((unsigned char)*cursor))
                    cursor++;
                char *arg = cursor;
                char *end = expr + inner_len;
                while (end > arg && isspace((unsigned char)*(end - 1)))
                    end--;
                *end = '\0';
                if (*arg == '\0') {
                    fprintf(stderr, "%s: missing argument for %s\n",
                            who ? who : "tikl", fname);
                    if (status)
                        *status = 1;
                    free(expr);
                    sb_free(&sb);
                    return NULL;
                }
                int inner_status = 0;
                char *arg_expanded =
                    tikl_expand_placeholders(arg, true, helpers_enabled,
                                             lookup, lookup_ctx, who,
                                             &inner_status);
                if (inner_status != 0) {
                    if (status)
                        *status = inner_status;
                    free(expr);
                    free(arg_expanded);
                    sb_free(&sb);
                    return NULL;
                }
                char *replacement =
                    run_builtin_function(who, fname, arg_expanded,
                                         status);
                free(arg_expanded);
                free(expr);
                if (!replacement) {
                    sb_free(&sb);
                    return NULL;
                }
                sb_append_str(who, &sb, replacement);
                free(replacement);
                p = q + 1;
                continue;
            }
            size_t len = 0;
            const char *q = p + 1;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                   (*q >= '0' && *q <= '9') || *q == '_') {
                len++;
                q++;
            }
            if (len > 0) {
                const char *val = lookup ? lookup(lookup_ctx, p + 1, len) : NULL;
                if (val) {
                    sb_append_str(who, &sb, val);
                } else {
                    sb_append_char(who, &sb, '%');
                    for (size_t i = 0; i < len; i++)
                        sb_append_char(who, &sb, p[1 + i]);
                }
                p = q;
            } else {
                sb_append_char(who, &sb, '%');
                p++;
            }
        } else {
            sb_append_char(who, &sb, *p++);
        }
    }
    return sb_steal(&sb);
}
