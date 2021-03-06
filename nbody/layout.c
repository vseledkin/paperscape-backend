#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "util/xiwilib.h"
#include "common.h"
#include "layout.h"

static void layout_combine_duplicate_links(layout_t *layout) {
    // combine duplicate links
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *node = &layout->nodes[i];
        assert(node != NULL);
        for (int j = 0; j < node->num_links; j++) {
            layout_node_t *node2 = LAYOUT_LINK_GET_NODE(&node->links[j]);
            float weight2 = LAYOUT_LINK_GET_WEIGHT(&node->links[j]);
            assert(node2 != NULL);
            assert(node2 != node); // shouldn't be any nodes linking to themselves
            for (int k = 0; k < node2->num_links; k++) {
                if (LAYOUT_LINK_GET_NODE(&node2->links[k]) == node) {
                    // a duplicate link (node -> node2, and node2 -> node)
                    weight2 += LAYOUT_LINK_GET_WEIGHT(&node2->links[k]);
                    memmove(&node2->links[k], &node2->links[k + 1], (node2->num_links - k - 1) * sizeof(layout_link_t));
                    node2->num_links -= 1;
                    break;
                }
            }

            // store the updated weight
            LAYOUT_LINK_SET_WEIGHT(&node->links[j], weight2);
        }
    }

    // count number of links layout
    layout->num_links = 0;
    for (int i = 0; i < layout->num_nodes; i++) {
        layout->num_links += layout->nodes[i].num_links;
    }
}

layout_t *layout_build_from_papers(int num_papers, paper_t **papers, bool age_weaken, double factor_ref_link, double factor_other_link) {
    // allocate memory for the nodes
    int num_nodes = num_papers;
    layout_node_t *nodes = m_new(layout_node_t, num_nodes);

    // assign each paper to a node
    for (int i = 0; i < num_papers; i++) {
        papers[i]->layout_node = &nodes[i];
    }

    // build the nodes
    for (int i = 0; i < num_papers; i++) {
        paper_t *paper = papers[i];
        layout_node_t *node = &nodes[i];
        node->flags = LAYOUT_NODE_IS_FINEST;
        node->parent = NULL;
        node->paper = paper;
        node->mass = paper->mass;
        node->radius = paper->radius;
        node->x = 0;
        node->y = 0;
        node->fx = 0;
        node->fy = 0;
        //num_total_links += paper->num_refs + paper->num_fake_links;
    }

    // count number of links we need, only include valid links
    int num_total_links = 0;
    for (int i = 0; i < num_nodes; i++) {
        layout_node_t *node = &nodes[i];
        node->num_links = 0;
        for (int j = 0; j < node->paper->num_refs; j++) {
            if (node->paper->refs[j]->layout_node != NULL) {
                node->num_links++;
            }
        }
        for (int j = 0; j < node->paper->num_fake_links; j++) {
            if (node->paper->fake_links[j]->layout_node != NULL) {
                node->num_links++;
            }
        }
        num_total_links += node->num_links;
    }

    // build the links
    layout_link_t *all_links = m_new(layout_link_t, num_total_links);
    layout_link_t *links = all_links;
    for (int i = 0; i < num_papers; i++) {
        paper_t *paper = papers[i];
        layout_node_t *node = &nodes[i];
        node->links = links;

        // make layout links from the paper's refs
        int k = 0;
        for (int j = 0; j < paper->num_refs; j++) {
            if (node->paper->refs[j]->layout_node == NULL) continue;

            // compute the weight of the link
            int ref_freq = paper->refs_ref_freq[j];
            //double weight = ref_freq; // ref_freq standard
            double weight = factor_ref_link * ref_freq * ref_freq; // ref_freq squared
            if (age_weaken) {
                //weight *= 1.0 - 0.5 * fabs(paper->age - paper->refs[j]->age);
                weight *= 0.4 + 0.6 * exp(-pow(1e-7 * paper->id - 1e-7 * paper->refs[j]->id, 2));
            }
            if (paper->refs_other_weight != NULL) {
                //weight = factor_ref_link * weight + factor_other_link * paper->refs_other_weight[j];
                weight += factor_other_link * paper->refs_other_weight[j];
            }

            // set the weight and linked node
            //node->links[j].weight = weight;
            //node->links[j].node = paper->refs[j]->layout_node;
            //assert(node->links[j].node != NULL);
            LAYOUT_LINK_SET_WEIGHT(&node->links[k], weight);
            LAYOUT_LINK_SET_NODE(&node->links[k], paper->refs[j]->layout_node);
            assert(LAYOUT_LINK_GET_NODE(&node->links[k]) != NULL);
            k++;
        }

        // make layout links from the fake links
        for (int j = 0; j < paper->num_fake_links; j++) {
            if (node->paper->fake_links[j]->layout_node == NULL) continue;

            //node->links[paper->num_refs + j].weight = 0.25; // what to use for fake link weight??
            //node->links[paper->num_refs + j].node = paper->fake_links[j]->layout_node;
            //assert(node->links[paper->num_refs + j].node != NULL);
            LAYOUT_LINK_SET_WEIGHT(&node->links[k], 0.25); // what to use for fake link weight??
            LAYOUT_LINK_SET_NODE(&node->links[k], paper->fake_links[j]->layout_node);
            assert(LAYOUT_LINK_GET_NODE(&node->links[k]) != NULL);
            k++;
        }
        
        assert(node->num_links == k);
        links += node->num_links;
    }
    assert(all_links + num_total_links == links);

    // make layout object
    layout_t *layout = m_new(layout_t, 1);
    layout->parent_layout = NULL;
    layout->child_layout = NULL;
    layout->num_nodes = num_nodes;
    layout->nodes = nodes;
    layout->num_links = num_total_links;
    layout->links = all_links;

    // combine duplicate links
    layout_combine_duplicate_links(layout);

    return layout;
}

// count unique links in the 2 given arrays of links
static size_t count_links(layout_node_t *node, unsigned int num_links2, layout_link_t *links2, unsigned int num_links3, layout_link_t *links3) {
    size_t nl = 0;
    for (int i = 0; i < num_links2 + num_links3; ++i) {
        layout_link_t *link_to_add;
        if (i < num_links2) {
            link_to_add = &links2[i];
        } else {
            link_to_add = &links3[i - num_links2];
        }
        layout_node_t *link_to_add_node_parent = LAYOUT_LINK_GET_NODE(link_to_add)->parent;

        if (link_to_add_node_parent == node) {
            // a link to itself, don't include
            continue;
        }

        // look to see if link already exists
        bool found = false;
        for (int j = 0; j < i; ++j) {
            layout_node_t *l;
            if (j < num_links2) {
                l = LAYOUT_LINK_GET_NODE(&links2[j]);
            } else {
                l = LAYOUT_LINK_GET_NODE(&links3[j - num_links2]);
            }
            if (l->parent == link_to_add_node_parent) {
                found = true;
                break;
            }
        }

        // link does not exist, count it as a new one
        if (!found) {
            nl += 1;
        }
    }
    return nl;
}

// adds links2 to links in node, combining weights if destination already exists in links
static void add_links(layout_node_t *node, unsigned int num_links2, layout_link_t *links2) {
    for (int i = 0; i < num_links2; i++) {
        layout_link_t *link_to_add = &links2[i];
        layout_node_t *link_to_add_node_parent = LAYOUT_LINK_GET_NODE(link_to_add)->parent;
        float link_to_add_weight = LAYOUT_LINK_GET_WEIGHT(link_to_add);
        if (link_to_add_node_parent == node) {
            // a link to itself, don't include
            continue;
        }

        // look to see if link already exists
        bool found = false;
        for (int j = 0; j < node->num_links; j++) {
            if (LAYOUT_LINK_GET_NODE(&node->links[j]) == link_to_add_node_parent) {
                // combine weights
                LAYOUT_LINK_SET_WEIGHT(&node->links[j], LAYOUT_LINK_GET_WEIGHT(&node->links[j]) + link_to_add_weight);
                found = true;
                break;
            }
        }

        // link does not exist, make a new one
        if (!found) {
            LAYOUT_LINK_SET_WEIGHT(&node->links[node->num_links], link_to_add_weight);
            LAYOUT_LINK_SET_NODE(&node->links[node->num_links], link_to_add_node_parent);
            node->num_links += 1;
        }
    }
}

typedef struct _node_weight_t {
    layout_node_t *node;
    float weight;
} node_weight_t;

static int node_weight_cmp(const void *nw1_in, const void *nw2_in) {
    node_weight_t *nw1 = (node_weight_t*)nw1_in;
    node_weight_t *nw2 = (node_weight_t*)nw2_in;
    // largest weight first
    if (nw1->weight < nw2->weight) {
        return 1;
    } else if (nw1->weight > nw2->weight) {
        return -1;
    // if equal weights, smallest mass first
    } else if (nw1->node->mass < nw2->node->mass) {
        return -1;
    } else if (nw1->node->mass > nw2->node->mass) {
        return 1;
    } else {
        return 0;
    }
}

layout_t *layout_build_reduced_from_layout(layout_t *layout) {
    // clear the parents, and count number of links
    int num_nodes_with_links = 0;
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *node = &layout->nodes[i];
        node->parent = NULL;
        if (node->num_links > 0) {
            num_nodes_with_links += 1;
        }
    }

    //
    node_weight_t *nodes_with_links = m_new(node_weight_t, num_nodes_with_links);
    for (int i = 0, j = 0; i < layout->num_nodes; i++) {
        layout_node_t *node = &layout->nodes[i];
        if (node->num_links > 0) {
            float max_weight = 0;
            for (int i = 0; i < node->num_links; i++) {
                float w = LAYOUT_LINK_GET_WEIGHT(&node->links[i]);
                if (w > max_weight) {
                    max_weight = w;
                }
            }
            nodes_with_links[j].node = node;
            nodes_with_links[j].weight = max_weight;
            j += 1;
        }
    }
    qsort(nodes_with_links, num_nodes_with_links, sizeof(node_weight_t), node_weight_cmp);

    // allocate nodes for new layout
    int num_nodes2 = 0;
    layout_node_t *nodes2 = m_new(layout_node_t, layout->num_nodes);

    // where possible, combine 2 nodes into a new single node
    for (int i = 0; i < num_nodes_with_links; i++) {
        layout_node_t *node = nodes_with_links[i].node;
        if (node->parent != NULL) {
            // node already combined
            continue;
        }

        // find the link with the largest weight
        layout_node_t *max_link_node = NULL;
        float max_link_weight;
        for (int i = 0; i < node->num_links; i++) {
            layout_link_t *link = &node->links[i];
            layout_node_t *ln = LAYOUT_LINK_GET_NODE(link);
            float lw = LAYOUT_LINK_GET_WEIGHT(link);
            if (ln->parent == NULL && (max_link_node == NULL || lw > max_link_weight)) {
                max_link_node = ln;
                max_link_weight = lw;
            }
        }

        if (max_link_node == NULL) {
            // no available link
            continue;
        }

        // combine node with link->node into node2
        layout_node_t *node2 = &nodes2[num_nodes2++];
        node2->flags = 0;
        node2->parent = NULL;
        node2->child1 = node;
        node2->child2 = max_link_node;
        node2->num_links = 0;
        node2->links = NULL;
        node2->mass = node->mass + max_link_node->mass;
        node2->radius = sqrt(node->radius*node->radius + max_link_node->radius*max_link_node->radius);
        node2->x = 0;
        node2->y = 0;
        node2->fx = 0;
        node2->fy = 0;
        node->parent = node2;
        max_link_node->parent = node2;
    }
    m_free(nodes_with_links);

    // put left over nodes into single new node
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *node = &layout->nodes[i];
        if (node->parent != NULL) {
            // node already combined
            continue;
        }

        // put node into node2
        layout_node_t *node2 = &nodes2[num_nodes2++];
        node2->flags = 0;
        node2->parent = NULL;
        node2->child1 = node;
        node2->child2 = NULL;
        node2->num_links = 0;
        node2->links = NULL;
        node2->mass = node->mass;
        node2->radius = node->radius;
        node2->x = 0;
        node2->y = 0;
        node2->fx = 0;
        node2->fy = 0;
        node->parent = node2;
    }

    // sanity checks; takes ages since it's O(N^2)
    /*
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *node = &layout->nodes[i];
        assert(node->parent != NULL);
        bool found_parent = false;
        int num_parents = 0;
        for (int j = 0; j < num_nodes2; j++) {
            if (nodes2[j].child1 != NULL) {
                assert(nodes2[j].child1 != nodes2[j].child2);
            }
            if (node->parent == &nodes2[j]) {
                assert(nodes2[j].child1 == node || nodes2[j].child2 == node);
                found_parent = true;
            }
            if (nodes2[j].child1 == node) {
                num_parents += 1;
            }
            if (nodes2[j].child2 == node) {
                num_parents += 1;
            }
        }
        assert(found_parent);
        assert(num_parents == 1);
    }
    */

    // count number of links needed for new, reduced layout
    size_t total_links = 0;
    for (int i = 0; i < num_nodes2; i++) {
        layout_node_t *node2 = &nodes2[i];
        total_links += count_links(node2,
            node2->child1->num_links, node2->child1->links,
            node2->child2 == NULL ? 0 : node2->child2->num_links,
            node2->child2 == NULL ? NULL : node2->child2->links);
    }

    // allocate a big array for the new links
    layout_link_t *new_links = m_new(layout_link_t, total_links);
    layout_link_t *new_links_cur = new_links;

    // make links for new, reduced layout
    for (int i = 0; i < num_nodes2; i++) {
        layout_node_t *node2 = &nodes2[i];
        node2->links = new_links_cur;
        node2->num_links = 0;
        add_links(node2, node2->child1->num_links, node2->child1->links);
        if (node2->child2 != NULL) {
            add_links(node2, node2->child2->num_links, node2->child2->links);
        }
        // use up some links from the big array
        new_links_cur += node2->num_links;
    }

    // check we didn't overflow the big array of links
    assert(new_links_cur <= new_links + total_links);

    // make layout object
    layout_t *layout2 = m_new(layout_t, 1);
    layout2->parent_layout = NULL;
    layout2->child_layout = layout;
    layout2->num_nodes = num_nodes2;
    layout2->nodes = nodes2;
    layout2->num_links = 0;
    layout2->links = NULL;
    layout->parent_layout = layout2;

    // combine duplicate links
    layout_combine_duplicate_links(layout2);

    return layout2;
}

static void layout_node_propagate_position_to_children(layout_t *layout, layout_node_t *node) {
    if (layout->child_layout != NULL) {
        node->child1->x = node->x;
        node->child1->y = node->y;
        layout_node_propagate_position_to_children(layout->child_layout, node->child1);
        if (node->child2 != NULL) {
            node->child2->x = node->x;
            node->child2->y = node->y;
            layout_node_propagate_position_to_children(layout->child_layout, node->child2);
        }
    }
}

void layout_propagate_positions_to_children(layout_t *layout) {
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_propagate_position_to_children(layout, &layout->nodes[i]);
    }
}

void layout_print(layout_t *l) {
    double mass = 0;
    double radius = 0;
    for (int i = 0; i < l->num_nodes; i++) {
        mass += l->nodes[i].mass;
        radius += l->nodes[i].radius*l->nodes[i].radius;
    }
    bool finest = l->child_layout == NULL;
    printf("layout has %d nodes, %d links, %lg total mass, %lg total radius", l->num_nodes, l->num_links, mass, sqrt(radius));
    if (finest) {
        printf("\n");
    } else {
        printf("; ratio to child: %f nodes, %f links\n", 1.0 * l->num_nodes / l->child_layout->num_nodes, 1.0 * l->num_links / l->child_layout->num_links);
    }
    /*
    for (int i = 0; i < l->num_nodes; i++) {
        layout_node_t *n = &l->nodes[i];
        printf("  %d, mass %g, parent (", i, n->mass);
        if (n->parent != NULL) {
            printf("%ld", n->parent - l->parent_layout->nodes);
        }
        printf(") children (");
        if (finest) {
            printf("%u", n->paper->id);
        } else {
            printf("%ld", n->child1 - l->child_layout->nodes);
            if (n->child2 != NULL) {
                printf(",%ld", n->child2 - l->child_layout->nodes);
            }
        }
        printf(") linked to (");
        for (int j = 0; j < n->num_links; j++) {
            printf("%ldw%g,", n->links[j].node - l->nodes, n->links[j].weight);
        }
        printf(")\n");
    }
    */
}

layout_node_t *layout_get_node_by_id(layout_t *layout, unsigned int id) {
    assert(layout->child_layout == NULL);
    int lo = 0;
    int hi = layout->num_nodes - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (id == layout->nodes[mid].paper->id) {
            return &layout->nodes[mid];
        } else if (id < layout->nodes[mid].paper->id) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

layout_node_t *layout_get_node_at(layout_t *layout, double x, double y) {
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *n = &layout->nodes[i];
        double dx = n->x - x;
        double dy = n->y - y;
        double r = dx*dx + dy*dy;
        if (r < n->radius*n->radius) {
            return n;
        }
    }
    return NULL;
}

void layout_node_compute_best_start_position(layout_node_t *n) {
    // compute initial position for a node as the average of all its links
    double x = 0;
    double y = 0;
    double weight = 0;

    // average x- and y-pos of links
    for (int i = 0; i < n->num_links; i++) {
        layout_link_t *l = &n->links[i];
        layout_node_t *ln = LAYOUT_LINK_GET_NODE(l);
        double lw = LAYOUT_LINK_GET_WEIGHT(l);
        if (ln->flags & LAYOUT_NODE_POS_VALID) {
            x += lw * ln->x;
            y += lw * ln->y;
            weight += lw;
        }
    }

    if (weight == 0) {
        n->x = 100.0 * (-0.5 + 1.0 * random() / RAND_MAX);
        n->y = 100.0 * (-0.5 + 1.0 * random() / RAND_MAX);
    } else {
        // add some random element to average, mainly so we don't put it at the same pos when there is only one link
        n->x = x / weight + (-0.5 + 1.0 * random() / RAND_MAX);
        n->y = y / weight + (-0.5 + 1.0 * random() / RAND_MAX);
    }
}

void layout_rotate_all(layout_t *layout, double angle) {
    double s_angle = sin(angle);
    double c_angle = cos(angle);
    for (int i = 0; i < layout->num_nodes; i++) {
        layout_node_t *n = &layout->nodes[i];
        double x = n->x;
        double y = n->y;
        n->x = c_angle * x - s_angle * y;
        n->y = s_angle * x + c_angle * y;
    }
}

// when we export layout positions/radius we want to use integers for performance reasons
// therefore we have a multiplicative factor to include a bit of the fraction
// all export/import code must go through these 2 functions

static const double export_import_double_conversion_factor = 20.0;

void layout_node_export_quantities(layout_node_t *l, int *x_out, int *y_out, int *r_out) {
    *x_out = round(l->x * export_import_double_conversion_factor);
    *y_out = round(l->y * export_import_double_conversion_factor);
    *r_out = round(l->radius * export_import_double_conversion_factor);
}

void layout_node_import_quantities(layout_node_t *l, int x_in, int y_in) {
    l->x = (double)x_in / export_import_double_conversion_factor;
    l->y = (double)y_in / export_import_double_conversion_factor;
    // radius is not imported
}

// iterate backwards through layout chain to recompute mass and radius
void layout_recompute_mass_radius(layout_t *layout) {
    // get finest layout
    int num_layouts = 1;
    while(layout->child_layout != NULL) {
        layout = layout->child_layout;
        num_layouts++;
    }
    
    // recompute mass and radius, starting from finest
    // layout
    for(int i = 0; i < num_layouts; i++) {
        for (int j = 0; j < layout->num_nodes; j++) {
            layout_node_t *n = &layout->nodes[j];
            assert(n != NULL);
            if (n->flags & LAYOUT_NODE_IS_FINEST) {
                // finest layout node
                assert(n->paper != NULL);
                n->mass = n->paper->mass;
                n->radius = n->paper->radius;
            } else {
                float mass = 0, rad2 = 0;
                // a coarse layout node
                if (n->child1 != NULL) {
                    mass += n->child1->mass;
                    rad2 += n->child1->radius * n->child1->radius;
                }
                if (n->child2 != NULL) {
                    mass += n->child2->mass;
                    rad2 += n->child2->radius * n->child2->radius;
                }
                n->mass = mass;
                n->radius = sqrt(rad2);
            }
        }
        if (layout->parent_layout != NULL) {
            layout = layout->parent_layout;
        } else {
            break;
        }
    }
}
