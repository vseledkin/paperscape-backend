#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include "xiwilib.h"
#include "Common.h"
#include "Layout.h"
#include "Force.h"
#include "quadtree.h"

void Force_compute_attractive_link_force(Force_params_t *param, bool do_tred, Layout_t *layout) {
    for (int i = 0; i < layout->num_nodes; i++) {
        Layout_node_t *n1 = &layout->nodes[i];
        for (int j = 0; j < n1->num_links; j++) {
            Layout_node_t *n2 = n1->links[j].node;
            double weight = n1->links[j].weight;

            double dx = n1->x - n2->x;
            double dy = n1->y - n2->y;
            double r = sqrt(dx*dx + dy*dy);
            double rest_len = 1.5 * (n1->radius + n2->radius);

            double fac = param->link_strength;

            if (param->use_ref_freq) {
                fac *= 0.65 * weight;
            }

            /*
            // these things we can only do if the nodes are papers
            if (layout->child_layout == NULL) {
                if (do_tred) {
                    //fac *= n1->paper->refs_tred_computed[j];
                }

                // loosen the force between papers in different categories
                if (n1->paper->kind != n2->paper->kind) {
                    fac *= 0.5;
                }

                // loosen the force between papers of different age
                fac *= 1.01 - 0.5 * fabs(n1->paper->age - n2->paper->age); // trying out the 0.5* factor; not tested yet
            }
            */

            // normalise refs so each paper has 1 unit for all references (doesn't really produce a good graph)
            //fac /= n1->num_links;

            if (r > 1e-2) {
                fac *= (r - rest_len) / r;
                double fx = dx * fac;
                double fy = dy * fac;

                n1->fx -= fx;
                n1->fy -= fy;
                n2->fx += fx;
                n2->fy += fy;
            }
        }
    }
}

// q1 is a leaf against which we check q2
static void quad_tree_forces_leaf_vs_node(Force_params_t *param, quad_tree_node_t *q1, quad_tree_node_t *q2) {
    if (q2 == NULL) {
        // q2 is empty node
    } else {
        // q2 is leaf or internal node

        // compute distance from q1 to centroid of q2
        double dx = q1->x - q2->x;
        double dy = q1->y - q2->y;
        double rsq = dx * dx + dy * dy;
        if (rsq < 1e-6) {
            // minimum distance cut-off
            rsq = 1e-6;
        }

        if (q2->num_items == 1) {
            // q2 is leaf node
            double fac;
            if (param->do_close_repulsion) {
                double rad_sum_sq = param->close_repulsion_c * pow(param->close_repulsion_d + q1->radius + q2->radius, 2);
                if (rsq < rad_sum_sq) {
                    // layout-nodes overlap, use stronger repulsive force
                    fac = param->close_repulsion_a * fmin(param->close_repulsion_b, (exp(4.0 * (rad_sum_sq - rsq)) - 1.0)) / rsq
                        + q1->mass * q2->mass / rad_sum_sq;
                } else {
                    // normal anti-gravity repulsive force
                    if (rsq > param->anti_gravity_falloff_rsq) { rsq *= rsq * param->anti_gravity_falloff_rsq_inv; }
                    fac = q1->mass * q2->mass / rsq;
                }
            } else {
                // normal anti-gravity repulsive force
                if (rsq > param->anti_gravity_falloff_rsq) { rsq *= rsq * param->anti_gravity_falloff_rsq_inv; }
                fac = q1->mass * q2->mass / rsq;
            }
            double fx = dx * fac;
            double fy = dy * fac;
            /*
            q1->fx += fx;
            q1->fy += fy;
            q2->fx -= fx;
            q2->fy -= fy;
            */
            ((Layout_node_t*)q1->item)->fx += fx;
            ((Layout_node_t*)q1->item)->fy += fy;

        } else {
            // q2 is internal node
            if (q2->side_length * q2->side_length < 0.45 * rsq) {
                // q1 and the cell q2 are "well separated"
                // approximate force by centroid of q2
                if (rsq > param->anti_gravity_falloff_rsq) { rsq *= rsq * param->anti_gravity_falloff_rsq_inv; }
                double fac = q1->mass * q2->mass / rsq;
                double fx = dx * fac;
                double fy = dy * fac;
                /*
                q1->fx += fx;
                q1->fy += fy;
                q2->fx -= fx;
                q2->fy -= fy;
                */
                ((Layout_node_t*)q1->item)->fx += fx;
                ((Layout_node_t*)q1->item)->fy += fy;

            } else {
                // q1 and q2 are not "well separated"
                // descend into children of q2
                quad_tree_forces_leaf_vs_node(param, q1, q2->q0);
                quad_tree_forces_leaf_vs_node(param, q1, q2->q1);
                quad_tree_forces_leaf_vs_node(param, q1, q2->q2);
                quad_tree_forces_leaf_vs_node(param, q1, q2->q3);
            }
        }
    }
}

static void quad_tree_forces_ascend(Force_params_t *param, quad_tree_node_t *q) {
    assert(q->num_items == 1); // must be a leaf node
    for (quad_tree_node_t *q2 = q; q2->parent != NULL; q2 = q2->parent) {
        quad_tree_node_t *parent = q2->parent;
        assert(parent->num_items > 1); // all parents should be internal nodes
        if (parent->q0 != q2) { quad_tree_forces_leaf_vs_node(param, q, parent->q0); }
        if (parent->q1 != q2) { quad_tree_forces_leaf_vs_node(param, q, parent->q1); }
        if (parent->q2 != q2) { quad_tree_forces_leaf_vs_node(param, q, parent->q2); }
        if (parent->q3 != q2) { quad_tree_forces_leaf_vs_node(param, q, parent->q3); }
    }
}

static void quad_tree_forces_descend(Force_params_t *param, quad_tree_node_t *q) {
    if (q->num_items == 1) {
        quad_tree_forces_ascend(param, q);
    } else {
        if (q->q0 != NULL) { quad_tree_forces_descend(param, q->q0); }
        if (q->q1 != NULL) { quad_tree_forces_descend(param, q->q1); }
        if (q->q2 != NULL) { quad_tree_forces_descend(param, q->q2); }
        if (q->q3 != NULL) { quad_tree_forces_descend(param, q->q3); }
    }
}

/*
static void quad_tree_node_forces_propagate(quad_tree_node_t *q, double fx, double fy) {
    if (q == NULL) {
    } else {
        fx *= q->mass;
        fy *= q->mass;
        fx += q->fx;
        fy += q->fy;

        if (q->num_items == 1) {
            ((Layout_node_t*)q->item)->fx += fx;
            ((Layout_node_t*)q->item)->fy += fy;
        } else {
            fx /= q->mass;
            fy /= q->mass;
            quad_tree_node_forces_propagate(q->q0, fx, fy);
            quad_tree_node_forces_propagate(q->q1, fx, fy);
            quad_tree_node_forces_propagate(q->q2, fx, fy);
            quad_tree_node_forces_propagate(q->q3, fx, fy);
        }
    }
}
*/

#include <pthread.h>

typedef struct _multi_env_t {
    Force_params_t *param;
    quad_tree_node_t *q;
} multi_env_t;

static void *multi_do(void *env_in) {
    multi_env_t *env = env_in;
    if (env->q != NULL) {
        quad_tree_forces_descend(env->param, env->q);
    }
    return NULL;
}

// descending then ascending is almost twice as fast (for large graphs) as
// just naively iterating through all the leaves, possibly due to cache effects
void Force_quad_tree_forces(Force_params_t *param, quad_tree_t *qt) {
    if (qt->root != NULL) {
        if (qt->root->num_items == 1 || 0) {
            // without threading
            quad_tree_forces_descend(param, qt->root);
        } else {
            // with threading
            multi_env_t me1 = {param, qt->root->q0};
            multi_env_t me2 = {param, qt->root->q1};
            //multi_env_t me3 = {param, qt->root->q2};
            //multi_env_t me4 = {param, qt->root->q3};
            pthread_t pt1, pt2;//, pt3;//, pt4;
            pthread_create(&pt1, NULL, multi_do, &me1);
            pthread_create(&pt2, NULL, multi_do, &me2);
            //pthread_create(&pt3, NULL, multi_do, &me3);
            //pthread_create(&pt4, NULL, multi_do, &me4);
            if (qt->root->q2 != NULL) {
                quad_tree_forces_descend(param, qt->root->q2);
            }
            if (qt->root->q3 != NULL) {
                quad_tree_forces_descend(param, qt->root->q3);
            }
            pthread_join(pt1, NULL);
            pthread_join(pt2, NULL);
            //pthread_join(pt3, NULL);
            //pthread_join(pt4, NULL);
        }
        //quad_tree_node_forces_propagate(qt->root, 0, 0);
    }
}

void Force_quad_tree_apply_if(Force_params_t *param, quad_tree_t *qt, bool (*f)(Layout_node_t*)) {
    if (qt->root != NULL) {
        for (quad_tree_pool_t *qtp = qt->quad_tree_pool; qtp != NULL; qtp = qtp->next) {
            for (int i = 0; i < qtp->num_nodes_used; i++) {
                quad_tree_node_t *q = &qtp->nodes[i];
                if (q->num_items == 1 && f((Layout_node_t*)q->item)) {
                    //quad_tree_forces_leaf_vs_node(param, q, qt->root);
                    quad_tree_forces_ascend(param, q);
                }
            }
        }
        //quad_tree_node_forces_propagate(qt->root, 0, 0);
    }
}
