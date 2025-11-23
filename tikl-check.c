#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "subst.h"

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
    const char *filename;
    size_t line_no;
    char *check_label;
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
        free(d->check_label);
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

static char *
build_check_label(const char *prefix, const char *suffix)
{
    if (!suffix)
        suffix = "";
    size_t lenp = strlen(prefix);
    size_t lens = strlen(suffix);
    char *label = malloc(lenp + lens + 1);
    if (!label)
        die("tikl-check: OOM");
    memcpy(label, prefix, lenp);
    memcpy(label + lenp, suffix, lens);
    label[lenp + lens] = '\0';
    return label;
}

static void
usage(const char *arg0)
{
    fprintf(stderr,
            "usage: %s [--check-prefix=NAME|-p NAME]... [--print-output-on-fail|-x] TESTFILE\n",
            arg0);
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

static const char *
lookup_subst_cb(void *userdata, const char *key, size_t len)
{
    const subst_table *table = userdata;
    if (!table)
        return NULL;
    return lookup_subst(table, key, len);
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
    int err = snprintf(needle, sizeof(needle), "%s%s", prefix, suffix);
    if (err < 0) {
        abort();
    }
    const char *pos = strstr(line, needle);
    if (!pos)
        return NULL;
    return pos + strlen(needle);
}

static void
add_directive(vecdir *dirs, check_kind kind, prefix_state *state,
              char *pattern, bool lit_compat, int *status,
              unsigned count_target, const char *filename, size_t line_no,
              const char *suffix)
{
    directive dir = {0};
    dir.kind = kind;
    dir.prefix = state;
    dir.pattern_text = pattern;
    dir.count_target = count_target;
    dir.filename = filename;
    dir.line_no = line_no;
    dir.check_label = build_check_label(state->name, suffix);
    if (kind != CHECK_EMPTY) {
        char *regex_src = build_regex_from_pattern(pattern ? pattern : "", lit_compat);
        if (!regex_src) {
            *status = 1;
            free(dir.pattern_text);
            free(dir.check_label);
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
            free(dir.pattern_text);
            free(dir.check_label);
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
    size_t line_no = 0;
    while (true) {
        ssize_t n = getline(&line, &cap, f);
        if (n < 0)
            break;
        line_no++;
        strip_trailing(line);
        bool matched = false;
        for (size_t i = 0; i < prefixes->n && !matched; i++) {
            prefix_state *state = &states[i];
            const char *rest;
            if ((rest = match_directive(line, prefixes->v[i], "-NEXT:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = tikl_expand_placeholders(
                    pat, !lit_compat, !lit_compat, lookup_subst_cb,
                    (void *)subs, "tikl-check", status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_NEXT, state, expanded, lit_compat, status, 0,
                              path, line_no, "-NEXT");
            } else if ((rest = match_directive(line, prefixes->v[i], "-SAME:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = tikl_expand_placeholders(
                    pat, !lit_compat, !lit_compat, lookup_subst_cb,
                    (void *)subs, "tikl-check", status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_SAME, state, expanded, lit_compat, status, 0,
                              path, line_no, "-SAME");
            } else if ((rest = match_directive(line, prefixes->v[i], "-EMPTY:"))) {
                add_directive(dirs, CHECK_EMPTY, state, NULL, lit_compat, status, 0,
                              path, line_no, "-EMPTY");
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
                char *expanded = tikl_expand_placeholders(
                    pat, !lit_compat, !lit_compat, lookup_subst_cb,
                    (void *)subs, "tikl-check", status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_COUNT, state, expanded, lit_compat, status,
                              (unsigned)count, path, line_no, "-COUNT");
            } else if ((rest = match_directive(line, prefixes->v[i], "-NOT:"))) {
                const char *pat = trim_leading(rest);
                char *expanded = tikl_expand_placeholders(
                    pat, !lit_compat, !lit_compat, lookup_subst_cb,
                    (void *)subs, "tikl-check", status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_NOT, state, expanded, lit_compat, status, 0,
                              path, line_no, "-NOT");
            } else if ((rest = match_directive(line, prefixes->v[i], ":"))) {
                const char *pat = trim_leading(rest);
                char *expanded = tikl_expand_placeholders(
                    pat, !lit_compat, !lit_compat, lookup_subst_cb,
                    (void *)subs, "tikl-check", status);
                matched = true;
                if (!expanded)
                    continue;
                add_directive(dirs, CHECK_FORWARD, state, expanded, lit_compat, status, 0,
                              path, line_no, "");
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
report_failure(const directive *dir, const char *extra)
{
    const char *file = dir->filename ? dir->filename : "<unknown>";
    const char *label = dir->check_label ? dir->check_label :
                        (dir->prefix ? dir->prefix->name : "CHECK");
    const char *pattern = dir->pattern_text ? dir->pattern_text : "";
    fprintf(stderr, "tikl-check: failed %s:%zu: %s: %s",
            file, dir->line_no, label, pattern);
    if (extra && *extra)
        fprintf(stderr, " (%s)", extra);
    fputc('\n', stderr);
}

static void
dump_program_output(const vecstr *lines)
{
    fprintf(stderr, "tikl-check: program output:\n");
    for (size_t i = 0; i < lines->n; i++)
        fprintf(stderr, "%s\n", lines->v[i]);
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
    report_failure(dir, "pattern not found in remaining output");
    *status = 1;
}

static void
handle_check_next(const directive *dir, const vecstr *lines, int *status)
{
    if (!dir->prefix->have_last) {
        report_failure(dir, "requires prior match");
        *status = 1;
        return;
    }
    size_t expected = dir->prefix->last_line + 1;
    if (expected == 0)
        expected = 1;
    if (expected > lines->n) {
        report_failure(dir, "not enough output lines");
        *status = 1;
        return;
    }
    if (!regex_matches(&dir->regex, lines->v[expected - 1])) {
        report_failure(dir, "next line mismatch");
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
        report_failure(dir, "requires prior match");
        *status = 1;
        return;
    }
    size_t target = dir->prefix->last_line;
    if (target == 0 || target > lines->n) {
        report_failure(dir, "referenced line missing");
        *status = 1;
        return;
    }
    if (!regex_matches(&dir->regex, lines->v[target - 1])) {
        report_failure(dir, "line mismatch");
        *status = 1;
    }
}

static void
handle_check_empty(const directive *dir, const vecstr *lines, int *status)
{
    size_t expected = dir->prefix->have_last ? dir->prefix->last_line + 1 : 1;
    if (expected > lines->n) {
        report_failure(dir, "not enough output lines");
        *status = 1;
        return;
    }
    if (lines->v[expected - 1][0] != '\0') {
        report_failure(dir, "expected blank line");
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
            report_failure(dir, "pattern should not appear");
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
        char extra[64];
        snprintf(extra, sizeof(extra), "expected %u matches, got %u",
                 dir->count_target, found);
        report_failure(dir, extra);
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
    bool print_output_on_fail = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "--check-prefix=", 15) == 0) {
            add_prefix(&prefixes, arg + 15);
        } else if (strcmp(arg, "--check-prefix") == 0) {
            if (i + 1 >= argc)
                usage(argv[0]);
            add_prefix(&prefixes, argv[++i]);
        } else if (strncmp(arg, "-p", 2) == 0) {
            const char *name = arg + 2;
            if (*name == '\0') {
                if (i + 1 >= argc)
                    usage(argv[0]);
                name = argv[++i];
            }
            add_prefix(&prefixes, name);
        } else if (strcmp(arg, "--print-output-on-fail") == 0 ||
                   strcmp(arg, "-x") == 0) {
            print_output_on_fail = true;
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

    if (print_output_on_fail && status != 0)
        dump_program_output(&output);

    vecdir_free(&directives);
    vecstr_free(&output);
    free(states);
    free_substs(&substs);
    vecstr_free(&prefixes);

    return status;
}
