#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} vecstr;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

typedef struct {
    char *key;
    char *val;
} subst_entry;

typedef struct {
    subst_entry *v;
    size_t n;
    size_t cap;
} subst_table;

typedef struct prefix_state {
    char *name;
    size_t last_line;
    bool have_last;
} prefix_state;

typedef enum {
    CHECK_FORWARD,
    CHECK_NEXT,
    CHECK_SAME,
    CHECK_EMPTY,
    CHECK_NOT,
    CHECK_COUNT
} check_kind;

typedef struct {
    check_kind kind;
    prefix_state *prefix;
    char *pattern_text;
    regex_t regex;
    bool has_regex;
    unsigned count_target;
} directive;

typedef struct {
    directive *v;
    size_t n;
    size_t cap;
} vecdir;

static void
die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *
xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (!p)
        die("tikl-check: OOM");
    return p;
}

static char *
xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d)
        die("tikl-check: OOM");
    return d;
}

static void
vecstr_push(vecstr *vv, const char *s)
{
    if (vv->n == vv->cap) {
        vv->cap = vv->cap ? vv->cap * 2 : 8;
        vv->v = xrealloc(vv->v, vv->cap * sizeof(*vv->v));
    }
    vv->v[vv->n++] = xstrdup(s);
}

static void
vecstr_free(vecstr *vv)
{
    for (size_t i = 0; i < vv->n; i++)
        free(vv->v[i]);
    free(vv->v);
}

static void
vecdir_push(vecdir *vd, directive dir)
{
    if (vd->n == vd->cap) {
        vd->cap = vd->cap ? vd->cap * 2 : 8;
        vd->v = xrealloc(vd->v, vd->cap * sizeof(*vd->v));
    }
    vd->v[vd->n++] = dir;
}

static void
vecdir_free(vecdir *vd)
{
    for (size_t i = 0; i < vd->n; i++) {
        directive *d = &vd->v[i];
        if (d->has_regex)
            regfree(&d->regex);
        free(d->pattern_text);
    }
    free(vd->v);
}

static void
strbuf_append_char(strbuf *sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        sb->data = xrealloc(sb->data, sb->cap);
    }
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void
strbuf_append_str(strbuf *sb, const char *s)
{
    while (*s)
        strbuf_append_char(sb, *s++);
}

static char *
strbuf_steal(strbuf *sb)
{
    if (!sb->data) {
        sb->data = xstrdup("");
        sb->len = 0;
        sb->cap = 1;
    }
    char *out = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return out;
}

static void
strbuf_free(strbuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void
usage(const char *arg0)
{
    fprintf(stderr, "usage: %s [--check-prefix=NAME]... TESTFILE\n", arg0);
    exit(2);
}

static void
add_prefix(vecstr *prefixes, const char *name)
{
    if (!name || *name == '\0')
        die("tikl-check: empty --check-prefix value");
    for (const char *p = name; *p; ++p) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            die("tikl-check: invalid prefix: %s", name);
        }
    }
    vecstr_push(prefixes, name);
}

static void
strip_trailing(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') {
        s[--n] = '\0';
    }
}

static const char *
trim_leading(const char *s)
{
    while (*s && isspace((unsigned char) * s))
        s++;
    return s;
}

static subst_table
load_substs(void)
{
    subst_table table = {0};
    const char *env = getenv("TIKL_CHECK_SUBSTS");
    if (!env || !*env)
        return table;
    const char *p = env;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char *line = strndup(p, len);
            if (!line)
                die("tikl-check: OOM");
            char *eq = strchr(line, '=');
            if (eq && eq != line) {
                *eq = '\0';
                const char *val = eq + 1;
                table.v = xrealloc(table.v, (table.n + 1) * sizeof(*table.v));
                table.v[table.n].key = xstrdup(line);
                table.v[table.n].val = xstrdup(val);
                table.n++;
            }
            free(line);
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return table;
}

static void
free_substs(subst_table *table)
{
    for (size_t i = 0; i < table->n; i++) {
        free(table->v[i].key);
        free(table->v[i].val);
    }
    free(table->v);
}

static const char *
lookup_subst(const subst_table *table, const char *key, size_t len)
{
    for (size_t i = 0; i < table->n; i++) {
        if (strlen(table->v[i].key) == len && strncmp(table->v[i].key, key, len) == 0) {
            return table->v[i].val;
        }
    }
    return NULL;
}

static char *
run_builtin_function(const char *name, const char *arg, int *status)
{
    if (strcmp(name, "basename") == 0) {
        char *tmp = xstrdup(arg ? arg : "");
        char *leaf = basename(tmp);
        char *out = xstrdup(leaf);
        free(tmp);
        return out;
    }
    if (strcmp(name, "realpath") == 0) {
        if (!arg || !*arg)
            arg = ".";
        char *resolved = realpath(arg, NULL);
        if (!resolved) {
            fprintf(stderr, "tikl-check: realpath %s: %s\n", arg, strerror(errno));
            if (status)
                *status = 1;
            return NULL;
        }
        return resolved;
    }
    fprintf(stderr, "tikl-check: unknown placeholder function: %s\n", name);
    if (status)
        *status = 1;
    return NULL;
}

static char *
apply_placeholders(const char *input, bool lit_compat,
                   const subst_table *subs, int *status)
{
    if (!input)
        return NULL;
    if (lit_compat)
        return xstrdup(input);
    strbuf sb = {0};
    const char *p = input;
    while (*p) {
        if (status && *status != 0) {
            strbuf_free(&sb);
            return NULL;
        }
        if (*p == '%') {
            if (p[1] == '%') {
                strbuf_append_char(&sb, '%');
                p += 2;
                continue;
            }
            if (p[1] == '(') {
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
                    fprintf(stderr, "tikl-check: unterminated %%( in pattern: %s\n", input);
                    if (status)
                        *status = 1;
                    strbuf_free(&sb);
                    return NULL;
                }
                size_t inner_len = (size_t)(q - start);
                char *expr = strndup(start, inner_len);
                if (!expr)
                    die("tikl-check: OOM");
                char *cursor = expr;
                while (*cursor && isspace((unsigned char) * cursor))
                    cursor++;
                if (!*cursor) {
                    fprintf(stderr, "tikl-check: empty %%( ) expression\n");
                    if (status)
                        *status = 1;
                    free(expr);
                    strbuf_free(&sb);
                    return NULL;
                }
                char *fname = cursor;
                while (*cursor && !isspace((unsigned char) * cursor))
                    cursor++;
                if (*cursor)
                    *cursor++ = '\0';
                while (*cursor && isspace((unsigned char) * cursor))
                    cursor++;
                char *arg = cursor;
                char *end = expr + inner_len;
                while (end > arg && isspace((unsigned char) * (end - 1)))
                    end--;
                *end = '\0';
                if (*arg == '\0') {
                    fprintf(stderr, "tikl-check: missing argument for %s\n", fname);
                    if (status)
                        *status = 1;
                    free(expr);
                    strbuf_free(&sb);
                    return NULL;
                }
                char *arg_expanded = apply_placeholders(arg, lit_compat, subs, status);
                char *replacement = NULL;
                if (!status || *status == 0) {
                    replacement = run_builtin_function(fname, arg_expanded, status);
                }
                free(arg_expanded);
                free(expr);
                if (!replacement) {
                    strbuf_free(&sb);
                    return NULL;
                }
                strbuf_append_str(&sb, replacement);
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
                const char *val = (!lit_compat &&
                                   subs->n > 0) ? lookup_subst(subs, p + 1, len) : NULL;
                if (val) {
                    strbuf_append_str(&sb, val);
                } else {
                    strbuf_append_char(&sb, '%');
                    for (size_t i = 0; i < len; i++)
                        strbuf_append_char(&sb, p[1 + i]);
                }
                p = q;
            } else {
                strbuf_append_char(&sb, '%');
                p++;
            }
        } else {
            strbuf_append_char(&sb, *p++);
        }
    }
    return strbuf_steal(&sb);
}

static void
append_literal_segment(strbuf *sb, const char *seg, size_t len,
                       bool escape_literals)
{
    if (!escape_literals) {
        for (size_t i = 0; i < len; i++)
            strbuf_append_char(sb, seg[i]);
        return;
    }
    const char *meta = "][][.^$\\*/+?{}()|";
    for (size_t i = 0; i < len;) {
        char c = seg[i];
        if (c == '\\' && i + 1 < len) {
            char next = seg[i + 1];
            if (strchr(meta, next))
                strbuf_append_char(sb, '\\');
            strbuf_append_char(sb, next);
            i += 2;
            continue;
        }
        if (strchr(meta, c))
            strbuf_append_char(sb, '\\');
        strbuf_append_char(sb, c);
        i++;
    }
}

static const char *
find_block_close(const char *start)
{
    const char *p = start;
    while ((p = strstr(p, "}}")) != NULL) {
        if (p > start && *(p - 1) == '\\') {
            p++; /* skip escaped closing */
            continue;
        }
        return p;
    }
    return NULL;
}

static char *
build_regex_from_pattern(const char *pattern, bool lit_compat)
{
    strbuf sb = {0};
    const char *p = pattern;
    bool escape_literals = !lit_compat;
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) {
            append_literal_segment(&sb, p, strlen(p), escape_literals);
            break;
        }
        append_literal_segment(&sb, p, (size_t)(open - p), escape_literals);
        const char *inner = open + 2;
        const char *close = find_block_close(inner);
        if (!close) {
            strbuf_free(&sb);
            fprintf(stderr, "tikl-check: unterminated {{ in pattern: %s\n", pattern);
            return NULL;
        }
        for (const char *q = inner; q < close; ++q)
            strbuf_append_char(&sb, *q);
        p = close + 2;
    }
    return strbuf_steal(&sb);
}

static const char *
match_directive(const char *line, const char *prefix, const char *suffix)
{
    char needle[256];
    size_t need = strlen(prefix) + strlen(suffix) + 1;
    if (need > sizeof(needle))
        die("tikl-check: prefix too long");
    snprintf(needle, sizeof(needle), "%s%s", prefix, suffix);
    const char *pos = strstr(line, needle);
    if (!pos)
        return NULL;
    return pos + strlen(needle);
}

static void
add_directive(vecdir *dirs, check_kind kind, prefix_state *state,
              char *pattern, bool lit_compat, int *status,
              unsigned count_target)
{
    directive dir = {0};
    dir.kind = kind;
    dir.prefix = state;
    dir.pattern_text = pattern;
    dir.count_target = count_target;
    if (kind != CHECK_EMPTY) {
        char *regex_src = build_regex_from_pattern(pattern ? pattern : "", lit_compat);
        if (!regex_src) {
            *status = 1;
            free(pattern);
            return;
        }
        int rc = regcomp(&dir.regex, regex_src, REG_EXTENDED);
        free(regex_src);
        if (rc != 0) {
            char buf[256];
            regerror(rc, &dir.regex, buf, sizeof(buf));
            fprintf(stderr, "tikl-check: regex error in pattern '%s': %s\n",
                    pattern ? pattern : "", buf);
            *status = 1;
            free(pattern);
            return;
        }
        dir.has_regex = true;
    }
    vecdir_push(dirs, dir);
}

static void
parse_test_file(const char *path, const vecstr *prefixes,
                prefix_state *states, bool lit_compat,
                const subst_table *subs, vecdir *dirs, int *status)
{
    FILE *f = fopen(path, "r");
    if (!f)
        die("tikl-check: cannot open %s: %s", path, strerror(errno));
    char *line = NULL;
    size_t cap = 0;
    while (true) {
        ssize_t n = getline(&line, &cap, f);
        if (n < 0)
            break;
        strip_trailing(line);
        bool matched = false;
        for (size_t i = 0; i < prefixes->n && !matched; i++) {
            prefix_state *state = &states[i];
            const char *rest;
            if ((rest = match_directive(line, prefixes->v[i], "-NEXT:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = apply_placeholders(pat, lit_compat, subs, status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_NEXT, state, expanded, lit_compat, status, 0);
            } else if ((rest = match_directive(line, prefixes->v[i], "-SAME:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = apply_placeholders(pat, lit_compat, subs, status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_SAME, state, expanded, lit_compat, status, 0);
            } else if ((rest = match_directive(line, prefixes->v[i], "-EMPTY:"))) {
                add_directive(dirs, CHECK_EMPTY, state, NULL, lit_compat, status, 0);
                matched = true;
            } else if ((rest = match_directive(line, prefixes->v[i], "-COUNT:"))) {
                const char *content = trim_leading(rest);
                const char *digits = content;
                unsigned long count = 0;
                while (*digits && isspace((unsigned char) * digits))
                    digits++;
                if (!isdigit((unsigned char) * digits)) {
                    fprintf(stderr, "tikl-check: invalid %s-COUNT directive: %s\n",
                            prefixes->v[i], content);
                    *status = 1;
                    matched = true;
                    continue;
                }
                char *endptr;
                errno = 0;
                count = strtoul(digits, &endptr, 10);
                if (errno || endptr == digits) {
                    fprintf(stderr, "tikl-check: invalid %s-COUNT directive: %s\n",
                            prefixes->v[i], content);
                    *status = 1;
                    matched = true;
                    continue;
                }
                const char *pat = trim_leading(endptr);
                char *expanded = apply_placeholders(pat, lit_compat, subs, status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_COUNT, state, expanded, lit_compat, status,
                              (unsigned)count);
            } else if ((rest = match_directive(line, prefixes->v[i], "-NOT:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = apply_placeholders(pat, lit_compat, subs, status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_NOT, state, expanded, lit_compat, status, 0);
            } else if ((rest = match_directive(line, prefixes->v[i], ":"))) {
                const char *pat = trim_leading(rest);
                char *expanded = apply_placeholders(pat, lit_compat, subs, status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_FORWARD, state, expanded, lit_compat, status, 0);
            }
        }
    }
    free(line);
    fclose(f);
}

static void
read_output(vecstr *lines)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) != -1) {
        if (n > 0 && line[n - 1] == '\n')
            line[--n] = '\0';
        vecstr_push(lines, line);
    }
    free(line);
}

static bool
regex_matches(const regex_t *re, const char *line)
{
    return regexec(re, line, 0, NULL, 0) == 0;
}

static void
handle_check_forward(const directive *dir, const vecstr *lines, int *status)
{
    size_t start = dir->prefix->last_line;
    for (size_t i = 0; i < lines->n; i++) {
        size_t nr = i + 1;
        if (nr <= start)
            continue;
        if (regex_matches(&dir->regex, lines->v[i])) {
            dir->prefix->last_line = nr;
            dir->prefix->have_last = true;
            return;
        }
    }
    fprintf(stderr, "tikl-check: missing pattern: %s\n",
            dir->pattern_text ? dir->pattern_text : "");
    *status = 1;
}

static void
handle_check_next(const directive *dir, const vecstr *lines, int *status)
{
    if (!dir->prefix->have_last) {
        fprintf(stderr, "tikl-check: %s-NEXT without prior match\n",
                dir->prefix->name);
        *status = 1;
        return;
    }
    size_t expected = dir->prefix->last_line + 1;
    if (expected == 0)
        expected = 1;
    if (expected > lines->n) {
        fprintf(stderr, "tikl-check: %s-NEXT failed on line %zu for: %s\n",
                dir->prefix->name, expected,
                dir->pattern_text ? dir->pattern_text : "");
        *status = 1;
        return;
    }
    if (!regex_matches(&dir->regex, lines->v[expected - 1])) {
        fprintf(stderr, "tikl-check: %s-NEXT failed on line %zu for: %s\n",
                dir->prefix->name, expected,
                dir->pattern_text ? dir->pattern_text : "");
        *status = 1;
        return;
    }
    dir->prefix->last_line = expected;
    dir->prefix->have_last = true;
}

static void
handle_check_same(const directive *dir, const vecstr *lines, int *status)
{
    if (!dir->prefix->have_last) {
        fprintf(stderr, "tikl-check: %s-SAME without prior match\n",
                dir->prefix->name);
        *status = 1;
        return;
    }
    size_t target = dir->prefix->last_line;
    if (target == 0 || target > lines->n) {
        fprintf(stderr, "tikl-check: %s-SAME failed on line %zu for: %s\n",
                dir->prefix->name, target,
                dir->pattern_text ? dir->pattern_text : "");
        *status = 1;
        return;
    }
    if (!regex_matches(&dir->regex, lines->v[target - 1])) {
        fprintf(stderr, "tikl-check: %s-SAME failed on line %zu for: %s\n",
                dir->prefix->name, target,
                dir->pattern_text ? dir->pattern_text : "");
        *status = 1;
    }
}

static void
handle_check_empty(const directive *dir, const vecstr *lines, int *status)
{
    size_t expected = dir->prefix->have_last ? dir->prefix->last_line + 1 : 1;
    if (expected > lines->n) {
        fprintf(stderr, "tikl-check: %s-EMPTY failed on line %zu\n",
                dir->prefix->name, expected);
        *status = 1;
        return;
    }
    if (lines->v[expected - 1][0] != '\0') {
        fprintf(stderr, "tikl-check: %s-EMPTY failed on line %zu\n",
                dir->prefix->name, expected);
        *status = 1;
        return;
    }
    dir->prefix->last_line = expected;
    dir->prefix->have_last = true;
}

static void
handle_check_not(const directive *dir, const vecstr *lines, int *status)
{
    for (size_t i = 0; i < lines->n; i++) {
        if (regex_matches(&dir->regex, lines->v[i])) {
            fprintf(stderr, "tikl-check: unexpected pattern: %s\n",
                    dir->pattern_text ? dir->pattern_text : "");
            *status = 1;
            return;
        }
    }
}

static void
handle_check_count(const directive *dir, const vecstr *lines, int *status)
{
    unsigned found = 0;
    for (size_t i = 0; i < lines->n; i++) {
        if (regex_matches(&dir->regex, lines->v[i]))
            found++;
    }
    if (found != dir->count_target) {
        fprintf(stderr,
                "tikl-check: CHECK-COUNT expected %u, got %u for: %s\n",
                dir->count_target, found,
                dir->pattern_text ? dir->pattern_text : "");
        *status = 1;
    }
}

static void
run_directives(const vecdir *dirs, const vecstr *lines, int *status)
{
    for (size_t i = 0; i < dirs->n; i++) {
        const directive *dir = &dirs->v[i];
        switch (dir->kind) {
            case CHECK_FORWARD:
                handle_check_forward(dir, lines, status);
                break;
            case CHECK_NEXT:
                handle_check_next(dir, lines, status);
                break;
            case CHECK_SAME:
                handle_check_same(dir, lines, status);
                break;
            case CHECK_EMPTY:
                handle_check_empty(dir, lines, status);
                break;
            case CHECK_NOT:
                handle_check_not(dir, lines, status);
                break;
            case CHECK_COUNT:
                handle_check_count(dir, lines, status);
                break;
        }
    }
}

int
main(int argc, char **argv)
{
    vecstr prefixes = {0};
    const char *testfile = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "--check-prefix=", 15) == 0) {
            add_prefix(&prefixes, arg + 15);
        } else if (strcmp(arg, "--check-prefix") == 0) {
            if (i + 1 >= argc)
                usage(argv[0]);
            add_prefix(&prefixes, argv[++i]);
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
        } else if (strcmp(arg, "--") == 0) {
            if (i + 1 >= argc)
                usage(argv[0]);
            testfile = argv[++i];
            if (i + 1 != argc)
                usage(argv[0]);
            break;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "tikl-check: unknown option: %s\n", arg);
            vecstr_free(&prefixes);
            return 2;
        } else {
            if (testfile) {
                fprintf(stderr, "tikl-check: unexpected argument: %s\n", arg);
                vecstr_free(&prefixes);
                return 2;
            }
            testfile = arg;
        }
    }

    if (!testfile)
        usage(argv[0]);
    if (prefixes.n == 0)
        add_prefix(&prefixes, "CHECK");

    bool lit_compat = false;
    const char *lit_env = getenv("TIKL_LIT_COMPAT");
    if (lit_env && *lit_env && *lit_env != '0')
        lit_compat = true;

    subst_table substs = load_substs();
    prefix_state *states = calloc(prefixes.n, sizeof(*states));
    if (!states)
        die("tikl-check: OOM");
    for (size_t i = 0; i < prefixes.n; i++) {
        states[i].name = prefixes.v[i];
    }

    vecdir directives = {0};
    int status = 0;
    parse_test_file(testfile, &prefixes, states, lit_compat, &substs, &directives,
                    &status);

    vecstr output = {0};
    read_output(&output);

    run_directives(&directives, &output, &status);

    vecdir_free(&directives);
    vecstr_free(&output);
    free(states);
    free_substs(&substs);
    vecstr_free(&prefixes);

    return status;
}
