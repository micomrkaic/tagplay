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

#include "bmodel.h"
#include "query.h"
#include <string.h>

void bmodel_init(bmodel *m, const table *tb) {
    memset(m, 0, sizeof *m);
    m->tb = tb;
    vec_init(&m->match, sizeof(size_t));
    vec_init(&m->last_good, sizeof(size_t));
    vec_init(&m->sel, sizeof(size_t));
}
void bmodel_free(bmodel *m) {
    vec_free(&m->match);
    vec_free(&m->last_good);
    vec_free(&m->sel);
}

long bmodel_sel_find(const bmodel *m, size_t ti) {
    for (size_t i = 0; i < m->sel.len; i++)
        if (*(size_t *)vec_at((vec *)&m->sel, i) == ti) return (long)i;
    return -1;
}
void bmodel_sel_toggle(bmodel *m, size_t ti) {
    long i = bmodel_sel_find(m, ti);
    if (i < 0) {
        vec_push(&m->sel, &ti);
    } else {
        memmove((char *)m->sel.data + (size_t)i * sizeof(size_t),
                (char *)m->sel.data + ((size_t)i + 1) * sizeof(size_t),
                (m->sel.len - (size_t)i - 1) * sizeof(size_t));
        m->sel.len--;
    }
}

struct gctx { const table *tb; const char *key; };
static int group_cmp(const void *a, const void *b, void *ud) {
    struct gctx *g = ud;
    const track *ta = table_at(g->tb, *(const size_t *)a);
    const track *tb_ = table_at(g->tb, *(const size_t *)b);
    const char *va = track_first_tag(ta, g->key);
    const char *vb = track_first_tag(tb_, g->key);
    int c = strcasecmp(va ? va : "", vb ? vb : "");
    if (c) return c;
    long d = tag_num(ta, "DISCNUMBER") - tag_num(tb_, "DISCNUMBER");
    if (d) return d < 0 ? -1 : 1;
    d = tag_num(ta, "TRACKNUMBER") - tag_num(tb_, "TRACKNUMBER");
    if (d) return d < 0 ? -1 : 1;
    const char *na = track_first_tag(ta, "TITLE");
    const char *nb = track_first_tag(tb_, "TITLE");
    return strcasecmp(na ? na : "", nb ? nb : "");
}
static void apply_group(bmodel *m) {
    if (!m->group[0] || !m->match.len) return;
    struct gctx g = { m->tb, m->group };
    tp_sort(m->match.data, m->match.len, sizeof(size_t), group_cmp, &g);
}

void bmodel_rerun(bmodel *m) {
    if (m->sel_view) {
        /* selection view: the working list mirrors the marks, in
         * selection (playlist) order; sort/group deliberately skipped */
        m->match.len = 0;
        for (size_t i = 0; i < m->sel.len; i++)
            vec_push(&m->match, vec_at(&m->sel, i));
        m->parse_ok = 1;
        return;
    }
    qnode *q = query_parse(m->buf, 1 /* tolerant */);
    if (!q && m->len > 0) {
        m->parse_ok = 0; /* keep last_good on display */
        return;
    }
    query_run(q, m->tb, &m->match);
    if (m->sortspec[0]) query_sort(m->tb, &m->match, m->sortspec);
    apply_group(m);
    query_free(q);
    m->parse_ok = 1;
    /* copy into last_good */
    m->last_good.len = 0;
    for (size_t i = 0; i < m->match.len; i++)
        vec_push(&m->last_good, vec_at(&m->match, i));
}

