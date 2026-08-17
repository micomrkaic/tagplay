/* This file is part of tagplay.
 *
 * tagplay -- search-driven music player with audiotard DSP
 * Copyright (C) 2026  Mico
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include "query.h"
#include <pcre2.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------- lexer ---------------- */
typedef enum {
    T_EOF, T_LPAREN, T_RPAREN, T_BANG, T_AMP, T_PIPE,
    T_TILDE, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_WORD, T_STRING
} toktype;

typedef struct {
    toktype type;
    char   *text; /* for WORD/STRING, malloc'd */
} token;

typedef struct {
    const char *src;
    size_t pos, len;
    int tolerant;
    token cur;
} lexer;

static int is_wordchar(char c) {
    if (isalnum((unsigned char)c)) return 1;
    return strchr("_.-*+?[]{}^$\\/:@#%", c) != NULL;
}

static void tok_free(token *t) { free(t->text); t->text = NULL; }

static void lex_next(lexer *lx) {
    tok_free(&lx->cur);
    while (lx->pos < lx->len && isspace((unsigned char)lx->src[lx->pos])) lx->pos++;
    if (lx->pos >= lx->len) { lx->cur.type = T_EOF; return; }
    char c = lx->src[lx->pos];
    switch (c) {
    case '(': lx->pos++; lx->cur.type = T_LPAREN; return;
    case ')': lx->pos++; lx->cur.type = T_RPAREN; return;
    case '&': lx->pos++; lx->cur.type = T_AMP; return;
    case ',': lx->pos++; lx->cur.type = T_AMP; return;
    case '|': lx->pos++; lx->cur.type = T_PIPE; return;
    case '~': lx->pos++; lx->cur.type = T_TILDE; return;
    case '=': lx->pos++; lx->cur.type = T_EQ; return;
    case '!':
        lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->cur.type = T_NE; }
        else lx->cur.type = T_BANG;
        return;
    case '<':
        lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->cur.type = T_LE; }
        else lx->cur.type = T_LT;
        return;
    case '>':
        lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') { lx->pos++; lx->cur.type = T_GE; }
        else lx->cur.type = T_GT;
        return;
    case '"': case '\'': {
        char q = c;
        size_t start = ++lx->pos;
        while (lx->pos < lx->len && lx->src[lx->pos] != q) lx->pos++;
        size_t end = lx->pos;
        if (lx->pos < lx->len) lx->pos++;           /* closing quote */
        else if (!lx->tolerant) { lx->cur.type = T_EOF; return; } /* unterminated */
        lx->cur.type = T_STRING;
        lx->cur.text = xstrndup(lx->src + start, end - start);
        return;
    }
    default: {
        size_t start = lx->pos;
        while (lx->pos < lx->len && is_wordchar(lx->src[lx->pos])) lx->pos++;
        if (lx->pos == start) { lx->pos++; lex_next(lx); return; } /* skip junk char */
        lx->cur.type = T_WORD;
        lx->cur.text = xstrndup(lx->src + start, lx->pos - start);
        return;
    }
    }
}

/* ---------------- AST ---------------- */
typedef enum { N_AND, N_OR, N_NOT, N_CMP, N_BARE } ntype;
typedef enum { OP_RE, OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE } cmpop;

struct qnode {
    ntype type;
    qnode *l, *r;          /* AND/OR children, NOT uses l */
    char  *field;          /* CMP */
    cmpop  op;
    char  *value;          /* CMP raw value / BARE lowercased token */
    double num;            /* numeric interpretation of value */
    int    num_ok;
    pcre2_code *re;        /* compiled for OP_RE */
};

static qnode *node_new(ntype ty) {
    qnode *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = ty;
    return n;
}

void query_free(qnode *q) {
    if (!q) return;
    query_free(q->l);
    query_free(q->r);
    free(q->field);
    free(q->value);
    if (q->re) pcre2_code_free(q->re);
    free(q);
}

static int parse_num(const char *s, double *out) {
    /* accept plain numbers and mm:ss / h:mm:ss */
    const char *colon = strchr(s, ':');
    char *end;
    if (!colon) {
        double v = strtod(s, &end);
        if (end == s || *end) return 0;
        *out = v;
        return 1;
    }
    long a = strtol(s, &end, 10);
    if (*end != ':') return 0;
    long b = strtol(end + 1, &end, 10);
    if (*end == ':') {
        long c = strtol(end + 1, &end, 10);
        if (*end) return 0;
        *out = (double)(a * 3600 + b * 60 + c);
    } else {
        if (*end) return 0;
        *out = (double)(a * 60 + b);
    }
    return 1;
}

static pcre2_code *compile_re(const char *pat) {
    int err;
    PCRE2_SIZE eoff;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED,
                                   PCRE2_CASELESS | PCRE2_UTF, &err, &eoff, NULL);
    return re; /* NULL on bad pattern; caller decides */
}

/* ---------------- parser ---------------- */
static qnode *parse_or(lexer *lx);

static qnode *make_cmp(char *field, toktype op, char *value, int tolerant) {
    qnode *n = node_new(N_CMP);
    n->field = field;
    n->value = value;
    switch (op) {
    case T_TILDE: n->op = OP_RE; break;
    case T_EQ:    n->op = OP_EQ; break;
    case T_NE:    n->op = OP_NE; break;
    case T_LT:    n->op = OP_LT; break;
    case T_LE:    n->op = OP_LE; break;
    case T_GT:    n->op = OP_GT; break;
    default:      n->op = OP_GE; break;
    }
    n->num_ok = parse_num(value, &n->num);
    if (n->op == OP_RE) {
        n->re = compile_re(value);
        if (!n->re && !tolerant) { query_free(n); return NULL; }
        /* tolerant + bad regex: node matches nothing until pattern completes */
    }
    return n;
}

static qnode *parse_primary(lexer *lx) {
    if (lx->cur.type == T_LPAREN) {
        lex_next(lx);
        qnode *e = parse_or(lx);
        if (!e) return NULL;
        if (lx->cur.type == T_RPAREN) lex_next(lx);
        else if (!lx->tolerant) { query_free(e); return NULL; }
        return e;
    }
    if (lx->cur.type == T_BANG) {
        lex_next(lx);
        qnode *e = parse_primary(lx);
        if (!e) {
            if (lx->tolerant) { /* trailing '!' while typing: neutral true */
                qnode *n = node_new(N_BARE);
                n->value = xstrdup("");
                return n;
            }
            return NULL;
        }
        qnode *n = node_new(N_NOT);
        n->l = e;
        return n;
    }
    if (lx->cur.type == T_WORD || lx->cur.type == T_STRING) {
        char *first = lx->cur.text;
        lx->cur.text = NULL;
        toktype firstty = lx->cur.type;
        lex_next(lx);
        toktype op = lx->cur.type;
        if (firstty == T_WORD &&
            (op == T_TILDE || op == T_EQ || op == T_NE ||
             op == T_LT || op == T_LE || op == T_GT || op == T_GE)) {
            lex_next(lx);
            char *val;
            if (lx->cur.type == T_WORD || lx->cur.type == T_STRING) {
                val = lx->cur.text;
                lx->cur.text = NULL;
                lex_next(lx);
            } else if (lx->tolerant) {
                val = xstrdup(""); /* mid-typing: field op with no value yet */
            } else {
                free(first);
                return NULL;
            }
            qnode *n = make_cmp(first, op, val, lx->tolerant);
            if (!n) free(first);
            return n;
        }
        /* bare token: any-field case-insensitive substring */
        qnode *n = node_new(N_BARE);
        n->value = str_lower(first);
        free(first);
        return n;
    }
    return NULL;
}

static qnode *parse_and(lexer *lx) {
    qnode *l = parse_primary(lx);
    if (!l) return NULL;
    for (;;) {
        if (lx->cur.type == T_AMP) {
            lex_next(lx);
        } else if (lx->cur.type == T_WORD || lx->cur.type == T_STRING ||
                   lx->cur.type == T_LPAREN || lx->cur.type == T_BANG) {
            /* juxtaposition = implicit AND */
        } else {
            return l;
        }
        qnode *r = parse_primary(lx);
        if (!r) {
            if (lx->tolerant) return l;
            query_free(l);
            return NULL;
        }
        qnode *n = node_new(N_AND);
        n->l = l; n->r = r;
        l = n;
    }
}

static qnode *parse_or(lexer *lx) {
    qnode *l = parse_and(lx);
    if (!l) return NULL;
    while (lx->cur.type == T_PIPE) {
        lex_next(lx);
        qnode *r = parse_and(lx);
        if (!r) {
            if (lx->tolerant) return l;
            query_free(l);
            return NULL;
        }
        qnode *n = node_new(N_OR);
        n->l = l; n->r = r;
        l = n;
    }
    return l;
}

qnode *query_parse(const char *src, int tolerant) {
    /* Forgive expression-wrapping quotes: 'year < 1970' should mean the
     * comparison, not a substring search for the literal text. If the
     * whole input is one quoted span AND the interior contains operator
     * characters, parse the interior as an expression. Operator-free
     * quoted phrases ('dark side') keep their substring meaning. */
    char *stripped = NULL;
    if (src[0] == '"' || src[0] == '\'') {
        char q = src[0];
        size_t len = strlen(src);
        size_t end = len;                      /* unterminated (mid-typing) */
        if (len >= 2 && src[len - 1] == q) end = len - 1;
        if (strcspn(src + 1, "~=<>!") < end - 1) {
            /* operators inside and nothing after the closing quote */
            size_t rest = (end < len) ? strspn(src + end + 1, " ") : 0;
            if (end == len || src[end + 1 + rest] == 0)
                stripped = xstrndup(src + 1, end - 1);
        }
    }
    if (stripped) src = stripped;
    lexer lx = { .src = src, .pos = 0, .len = strlen(src), .tolerant = tolerant };
    lx.cur.type = T_EOF; lx.cur.text = NULL;
    lex_next(&lx);
    if (lx.cur.type == T_EOF) { free(stripped); return NULL; } /* empty */
    qnode *q = parse_or(&lx);
    if (q && lx.cur.type != T_EOF && !tolerant) { query_free(q); q = NULL; }
    tok_free(&lx.cur);
    free(stripped);
    return q;
}

/* ---------------- evaluation ---------------- */
static int numeric_field(const track *t, const char *field, double *out) {
    if (str_ieq(field, "length"))   { *out = t->duration;     return 1; }
    if (str_ieq(field, "rate"))     { *out = t->sample_rate;  return 1; }
    if (str_ieq(field, "channels")) { *out = t->channels;     return 1; }
    const char *map = NULL;
    if (str_ieq(field, "year"))  map = "DATE";
    if (str_ieq(field, "track")) map = "TRACKNUMBER";
    if (str_ieq(field, "disc"))  map = "DISCNUMBER";
    const char *v = track_first_tag(t, map ? map : field);
    if (map && !v && str_ieq(field, "year")) v = track_first_tag(t, "YEAR");
    if (!v) return 0;
    char *end;
    double d = strtod(v, &end);
    if (end == v) return 0;
    *out = d;
    return 1;
}

static int re_matches(pcre2_code *re, const char *s) {
    if (!re) return 0;
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)s, PCRE2_ZERO_TERMINATED, 0, 0, md, NULL);
    pcre2_match_data_free(md);
    return rc >= 0;
}

/* apply string predicate across all values of field; "any"/"*" = all fields+path */
static int str_pred(const qnode *q, const track *t) {
    int wild = str_ieq(q->field, "any") || !strcmp(q->field, "*");
    int matched = 0;
    if (wild || str_ieq(q->field, "path")) {
        const char *s = t->path;
        if (q->op == OP_RE) matched |= re_matches(q->re, s);
        else matched |= str_ieq(s, q->value);
        if (matched && q->op != OP_NE) return 1;
    }
    if (wild || str_ieq(q->field, "format")) {
        const char *s = fmt_name(t->fmt);
        if (q->op == OP_RE) matched |= re_matches(q->re, s);
        else matched |= str_ieq(s, q->value);
        if (matched && q->op != OP_NE) return 1;
    }
    for (size_t i = 0; i < t->tags.len; i++) {
        tagkv *kv = vec_at((vec *)&t->tags, i);
        if (!wild && !str_ieq(kv->key, q->field)) continue;
        int m;
        if (q->op == OP_RE) m = re_matches(q->re, kv->value);
        else m = str_ieq(kv->value, q->value);
        matched |= m;
        if (m && q->op != OP_NE) return 1;
    }
    if (q->op == OP_NE) return !matched;
    return matched;
}

static int cmp_eval(const qnode *q, const track *t) {
    switch (q->op) {
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
        double fv;
        if (!q->num_ok || !numeric_field(t, q->field, &fv)) return 0;
        switch (q->op) {
        case OP_LT: return fv <  q->num;
        case OP_LE: return fv <= q->num;
        case OP_GT: return fv >  q->num;
        default:    return fv >= q->num;
        }
    }
    case OP_EQ: case OP_NE: {
        /* numeric equality if both sides numeric, else string */
        double fv;
        if (q->num_ok && numeric_field(t, q->field, &fv)) {
            int eq = fv == q->num;
            return q->op == OP_EQ ? eq : !eq;
        }
        return str_pred(q, t);
    }
    default: /* OP_RE */
        return str_pred(q, t);
    }
}

static int bare_eval(const qnode *q, const track *t) {
    if (!q->value[0]) return 1; /* neutral node from tolerant parsing */
    if (str_icasestr(t->path, q->value)) return 1;
    for (size_t i = 0; i < t->tags.len; i++) {
        tagkv *kv = vec_at((vec *)&t->tags, i);
        if (str_icasestr(kv->value, q->value)) return 1;
    }
    return 0;
}

int query_eval(const qnode *q, const track *t) {
    switch (q->type) {
    case N_AND:  return query_eval(q->l, t) && query_eval(q->r, t);
    case N_OR:   return query_eval(q->l, t) || query_eval(q->r, t);
    case N_NOT:  return !query_eval(q->l, t);
    case N_CMP:  return cmp_eval(q, t);
    default:     return bare_eval(q, t);
    }
}

void query_run(const qnode *q, const table *tb, vec *out_idx) {
    out_idx->len = 0;
    for (size_t i = 0; i < table_len(tb); i++) {
        if (!q || query_eval(q, table_at(tb, i))) vec_push(out_idx, &i);
    }
}

/* ---------------- sorting ---------------- */
typedef struct { const table *tb; char fields[16][64]; int desc[16]; int n; } sortctx;

static int track_cmp(const void *a, const void *b, void *arg) {
    sortctx *sc = arg;
    const track *ta = table_at(sc->tb, *(const size_t *)a);
    const track *tb_ = table_at(sc->tb, *(const size_t *)b);
    for (int i = 0; i < sc->n; i++) {
        const char *f = sc->fields[i];
        double na, nb;
        int has_a = numeric_field(ta, f, &na), has_b = numeric_field(tb_, f, &nb);
        int c;
        if (has_a && has_b) {
            c = (na < nb) ? -1 : (na > nb) ? 1 : 0;
        } else {
            const char *va = str_ieq(f, "path") ? ta->path  : track_first_tag(ta, f);
            const char *vb = str_ieq(f, "path") ? tb_->path : track_first_tag(tb_, f);
            if (!va && !vb) c = 0;
            else if (!va) c = 1;      /* missing sorts last */
            else if (!vb) c = -1;
            else c = strcasecmp(va, vb);
        }
        if (c) return sc->desc[i] ? -c : c;
    }
    return 0;
}

void query_sort(const table *tb, vec *idx, const char *fields) {
    sortctx sc = { .tb = tb, .n = 0 };
    char *copy = xstrdup(fields);
    for (char *tok = strtok(copy, ", "); tok && sc.n < 16; tok = strtok(NULL, ", ")) {
        sc.desc[sc.n] = (*tok == '-');
        if (*tok == '-') tok++;
        snprintf(sc.fields[sc.n], sizeof sc.fields[0], "%s", tok);
        sc.n++;
    }
    free(copy);
    if (sc.n) psort(idx->data, idx->len, sizeof(size_t), track_cmp, &sc);
}
