#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "xiwilib.h"
#include "Common.h"

void Common_paper_init(Common_paper_t *p, unsigned int id) {
    // all entries have initial state which is 0x00
    memset(p, 0, sizeof(Common_paper_t));
    // set the paper id
    p->id = id;
}

// the keyword pool is a linked list of hash tables, each one bigger than the previous
typedef struct _keyword_pool_t {
    int size;
    Common_keyword_t *table;
    int used;
    bool full;
    struct _keyword_pool_t *next;
} keyword_pool_t;

struct _keyword_set_t {
    keyword_pool_t *pool;
};

// approximatelly doubling primes; made with Mathematica command: Table[Prime[Floor[(1.7)^n]], {n, 9, 24}]
static int doubling_primes[] = {647, 1229, 2297, 4243, 7829, 14347, 26017, 47149, 84947, 152443, 273253, 488399, 869927, 1547173, 2745121, 4861607};

Common_keyword_set_t *keyword_set_new() {
    Common_keyword_set_t *kws = m_new(Common_keyword_set_t, 1);
    kws->pool = NULL;
    return kws;
}

void Common_keyword_set_free(Common_keyword_set_t * kws) {
    if (kws == NULL) {
        return;
    }

    for (keyword_pool_t *kwp = kws->pool; kwp != NULL;) {
        keyword_pool_t *next = kwp->next;
        m_free(kwp->table);
        m_free(kwp);
        kwp = next;
    }

    m_free(kws);
}

int Common_keyword_set_get_total(Common_keyword_set_t *kws) {
    int n = 0;
    for (keyword_pool_t *kwp = kws->pool; kwp != NULL; kwp = kwp->next) {
        n += kwp->used;
    }
    return n;
}

void Common_keyword_set_clear_data(Common_keyword_set_t *kws) {
    for (keyword_pool_t *kwp = kws->pool; kwp != NULL; kwp = kwp->next) {
        for (int i = 0; i < kwp->size; i++) {
            kwp->table[i].paper = NULL;
        }
    }
}

Common_keyword_t *keyword_set_lookup_or_insert(Common_keyword_set_t *kws, const char *kw, size_t kw_len) {
    if (kw_len <= 0) {
        return NULL;
    }

    unsigned int hash = strnhash(kw, kw_len);
    keyword_pool_t *kwp;
    keyword_pool_t *avail_kwp = NULL;
    int avail_pos = 0;

    // first search for keyword to see if we already have it
    for (kwp = kws->pool; kwp != NULL; kwp = kwp->next) {
        int pos = hash % kwp->size;
        for (;;) {
            Common_keyword_t *found_kw = &kwp->table[pos];
            if (found_kw->keyword == NULL) {
                // kw not in table
                if (!kwp->full) {
                    // table not full, so we can use this entry if we don't find the kw
                    avail_kwp = kwp;
                    avail_pos = pos;
                }
                break;
            } else if (strneq(found_kw->keyword, kw, kw_len)) {
                // found it
                return found_kw;
            } else {
                // not yet found, keep searching in this table
                pos = (pos + 1) % kwp->size;
            }
        }
    }

    // keyword not found in any table

    // found an available slot, so use it
    if (avail_kwp != NULL) {
        avail_kwp->table[avail_pos].keyword = strndup(kw, kw_len);
        avail_kwp->used += 1;
        if (10 * avail_kwp->used > 8 * avail_kwp->size) {
            // set the full flag if we reached a certain fraction of used entries
            avail_kwp->full = true;
        }
        return &avail_kwp->table[avail_pos];
    }

    // no available slots, so make a new table
    kwp = m_new(keyword_pool_t, 1);
    if (kwp == NULL) {
        return NULL;
    }
    if (kws->pool == NULL) {
        // first table
        kwp->size = doubling_primes[0];
    } else {
        // successive tables
        for (int i = 0; i < sizeof(doubling_primes) / sizeof(int); i++) {
            kwp->size = doubling_primes[i];
            if (doubling_primes[i] > kws->pool->size) {
                break;
            }
        }
    }
    kwp->table = m_new0(Common_keyword_t, kwp->size);
    if (kwp->table == NULL) {
        m_free(kwp);
        return NULL;
    }
    kwp->used = 0;
    kwp->full = false;
    kwp->next = kws->pool;
    kws->pool = kwp;

    // make and insert new keyword
    kwp->table[hash % kwp->size].keyword = strndup(kw, kw_len);
    kwp->used += 1;

    // return new keyword
    return &kwp->table[hash % kwp->size];
}

static const char *category_string[] = {
    "unknown",
    "inspire",
#define CAT(id, str) str,
#include "cats.h"
#undef CAT
};

unsigned int Common_date_to_unique_id(int y, int m, int d) {
    return ((unsigned int)y - 1800) * 10000000 + (unsigned int)m * 625000 + (unsigned int)d * 15625;
}

void Common_unique_id_to_date(unsigned int id, int *y, int *m, int *d) {
    *y = id / 10000000 + 1800;
    *m = ((id % 10000000) / 625000) + 1;
    *d = ((id % 625000) / 15625) + 1;
}

const char *Common_category_enum_to_str(Common_category_t cat) {
    return category_string[cat];
}

Common_category_t category_str_to_enum(const char *str) {
    for (int i = 0; i < CAT_NUMBER_OF; i++) {
        if (streq(category_string[i], str)) {
            return i;
        }
    }
    return CAT_UNKNOWN;
}

Common_category_t category_strn_to_enum(const char *str, size_t n) {
    for (int i = 0; i < CAT_NUMBER_OF; i++) {
        if (strncmp(category_string[i], str, n) == 0 && category_string[i][n] == '\0') {
            return i;
        }
    }
    return CAT_UNKNOWN;
}

// compute the citations from the references
// allocates memory for paper->cites and fills it with pointers to citing papers
bool Common_build_citation_links(int num_papers, Common_paper_t *papers) {
    printf("building citation links\n");

    // allocate memory for cites for each paper
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *paper = &papers[i];
        if (paper->num_cites > 0) {
            paper->cites = m_new(Common_paper_t*, paper->num_cites);
            if (paper->cites == NULL) {
                return false;
            }
        }
        // use num cites to count which entry in the array we are up to when inserting cite links
        paper->num_cites = 0;
    }

    // link the cites
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *paper = &papers[i];
        for (int j = 0; j < paper->num_refs; j++) {
            Common_paper_t *ref_paper = paper->refs[j];
            ref_paper->cites[ref_paper->num_cites++] = paper;
        }
    }

    return true;
}

// compute the num_included_cites field in the Common_paper_t objects
// only includes papers that have their "included" flag set
// only counts references that have non-zero ref_freq
void Common_recompute_num_included_cites(int num_papers, Common_paper_t *papers) {
    // reset citation count
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *p = &papers[i];
        p->num_included_cites = 0;
    }

    // compute citation count by following references
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *p = &papers[i];
        if (p->included) {
            for (int j = 0; j < p->num_refs; j++) {
                if (p->refs_ref_freq[j] > 0) {
                    Common_paper_t *p2 = p->refs[j];
                    if (p2->included) {
                        p2->num_included_cites += 1;
                    }
                }
            }
        }
    }
}

typedef struct _paper_stack_t {
    int alloc;
    int used;
    Common_paper_t **stack;
} paper_stack_t;

static paper_stack_t *paper_stack_new() {
    paper_stack_t *s = m_new(paper_stack_t, 1);
    s->alloc = 1024;
    s->used = 0;
    s->stack = m_new(Common_paper_t*, s->alloc);
    return s;
}

static void paper_stack_free(paper_stack_t *s) {
    m_free(s->stack);
    m_free(s);
}

static void paper_stack_push(paper_stack_t *s, Common_paper_t *p) {
    if (s->used >= s->alloc) {
        s->alloc *= 2;
        s->stack = m_renew(Common_paper_t*, s->stack, s->alloc);
    }
    s->stack[s->used++] = p;
}

static Common_paper_t *paper_stack_pop(paper_stack_t *s) {
    assert(s->used > 0);
    return s->stack[--s->used];
}

static void paper_paint(Common_paper_t *p, int colour, paper_stack_t *stack) {
    assert(p->colour == 0);
    p->colour = colour;
    paper_stack_push(stack, p);
    while (stack->used > 0) {
        p = paper_stack_pop(stack);
        assert(p->colour == colour);
        for (int i = 0; i < p->num_refs; i++) {
            Common_paper_t *p2 = p->refs[i];
            if (p2->included && p2->colour != colour) {
                assert(p2->colour == 0);
                p2->colour = colour;
                paper_stack_push(stack, p2);
            }
        }
        for (int i = 0; i < p->num_cites; i++) {
            Common_paper_t *p2 = p->cites[i];
            if (p2->included && p2->colour != colour) {
                assert(p2->colour == 0);
                p2->colour = colour;
                paper_stack_push(stack, p2);
            }
        }
    }
}

// works out connected class for each paper (the colour after a flood fill painting algorigth)
// only includes papers that have their "included" flag set
void Common_recompute_colours(int num_papers, Common_paper_t *papers, int verbose) {
    // clear colour
    for (int i = 0; i < num_papers; i++) {
        papers[i].colour = 0;
    }

    // assign colour
    int cur_colour = 1;
    paper_stack_t *paper_stack = paper_stack_new();
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *paper = &papers[i];
        if (paper->included && paper->colour == 0) {
            paper_paint(paper, cur_colour++, paper_stack);
        }
    }
    paper_stack_free(paper_stack);

    // compute and assign num_with_my_colour for each paper
    int *num_with_col = m_new0(int, cur_colour);
    for (int i = 0; i < num_papers; i++) {
        num_with_col[papers[i].colour] += 1;
    }
    for (int i = 0; i < num_papers; i++) {
        papers[i].num_with_my_colour = num_with_col[papers[i].colour];
    }

    if (verbose) {
        // compute histogram
        int hist_max = 100;
        int hist_num = 0;
        int *hist_s = m_new(int, hist_max);
        int *hist_n = m_new(int, hist_max);
        for (int colour = 1; colour < cur_colour; colour++) {
            int n = num_with_col[colour];

            int i;
            for (i = 0; i < hist_num; i++) {
                if (hist_s[i] == n) {
                    break;
                }
            }
            if (i == hist_num && hist_num < hist_max) {
                hist_num += 1;
                hist_s[i] = n;
                hist_n[i] = 0;
            }
            hist_n[i] += 1;
        }

        printf("%d colours, %d unique sizes\n", cur_colour - 1, hist_num);
        for (int i = 0; i < hist_num; i++) {
            printf("size %d occured %d times\n", hist_s[i], hist_n[i]);
        }
    }

    m_free(num_with_col);
}

// for tred

static void compute_tred_refs_mark(int p_top_index, Common_paper_t *p_cur, Common_paper_t *follow_back_paper, int follow_back_ref) {
    if (p_cur->tred_visit_index != p_top_index) {
        // haven't visited this paper yet
        p_cur->tred_visit_index = p_top_index;
        p_cur->tred_follow_back_paper = follow_back_paper;
        p_cur->tred_follow_back_ref = follow_back_ref;
        // visit all refs
        for (int i = 0; i < p_cur->num_refs; i++) {
            if (p_cur->refs_tred_computed[i] != 0) { // only follow refs that are in the transitively reduced graph
                Common_paper_t *p_ref = p_cur->refs[i];
                if (p_ref->index < p_cur->index) { // allow only past references
                    compute_tred_refs_mark(p_top_index, p_ref, p_cur, i);
                }
            }
        }
    }
}

/* Transitively reduce the graph */
void Common_compute_tred(int num_papers, Common_paper_t *papers) {
    // reset the visit id and tred computed number
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *p = &papers[i];
        p->tred_visit_index = 0;
        // reset the tred_computed value
        for (int j = 0; j < p->num_refs; j++) {
            p->refs_tred_computed[j] = 0;
        }
        p->tred_follow_back_paper = NULL;
        p->tred_follow_back_ref = 0;
    }

    // transitively reduce
    for (int i = 0; i < num_papers; i++) {
        Common_paper_t *p = &papers[i];

        // clear the follow back pointer for this paper
        p->tred_follow_back_paper = NULL;
        p->tred_follow_back_ref = 0;

        // iterate all refs, from largest index to smallest index (youngest to oldest)
        for (int j = p->num_refs - 1; j >= 0; j--) {
            Common_paper_t *p_past = p->refs[j];

            // only allow references to the past
            if (p_past->index >= p->index) {
                p->refs_tred_computed[j] = 1;
                continue;
            }

            if (p_past->tred_visit_index == p->index) {
                // have already visited this paper
                // follow this path; increase weight of ref path
                Common_paper_t *p2 = p_past->tred_follow_back_paper;
                int ref = p_past->tred_follow_back_ref;
                while (p2 != NULL) {
                    p2->refs_tred_computed[ref] += 1;
                    ref = p2->tred_follow_back_ref;
                    p2 = p2->tred_follow_back_paper;
                }
                continue;
            }

            // haven't visited this paper yet
            // mark link as belonging to tred graph and mark past
            p->refs_tred_computed[j] = 1;
            compute_tred_refs_mark(p->index, p_past, p, j);
        }
    }
}
