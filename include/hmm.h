/* $Id: hmm.h,v 1.1.1.1 2004-06-03 22:43:11 acs Exp $
   Written by Adam Siepel, 2002
   Copyright 2002, Adam Siepel, University of California */

/* Library of functions relating to hidden Markov models.  Includes
 * simple reading and writing routines, as well as implementations of
 * the Viterbi algorithm, the forward algorithm, and the backward
 * algorithm.  Also includes a function to compute posterior
 * probabilities. */

#ifndef HMM_H
#define HMM_H

#include <gsl/gsl_matrix.h>
#include <markov_matrix.h>
#include <misc.h>
#include <lists.h>

#define MAXSTATES 1000
#define BEGIN_STATE -99
#define END_STATE -98

#define BEGIN_TRANSITIONS_TAG "BEGIN_TRANSITIONS:"
#define END_TRANSITIONS_TAG "END_TRANSITIONS:"
#define TRANSITION_MATRIX_TAG "TRANSITION_MATRIX:"
#define EQ_FREQS_TAG "EQUILIBRIUM_FREQUENCIES:"

typedef enum {VITERBI, FORWARD, BACKWARD} hmm_mode;

/* NOTE: eventually need to be able to support an "adjacency list"
 * rather than an "adjacency matrix" representation of an HMM, for
 * better efficiency when there are many states and they are not fully
 * connected  */
typedef struct {
  int nstates;
  MarkovMatrix *transition_matrix;
  gsl_matrix *transition_score_matrix; /* entries are logs of entries
                                        * in transition matrix */
  gsl_vector *begin_transitions, *end_transitions, 
    *begin_transition_scores, *end_transition_scores, *eq_freqs;
  List **predecessors, **successors;
  List *begin_successors, *end_predecessors;
} HMM;



HMM* hmm_new(MarkovMatrix *mm, gsl_vector *eq_freqs,
             gsl_vector *begin_transitions, 
             gsl_vector *end_transitions);
HMM *hmm_new_nstates(int nstates, int begin, int end);
void hmm_free(HMM *hmm);
double hmm_get_transition_score(HMM *hmm, int from_state, int to_state);
HMM* hmm_new_from_file(FILE *F);
void hmm_print(FILE *F, HMM *hmm);
void hmm_viterbi(HMM *hmm, double **emission_scores, int seqlen, int *path);
double hmm_forward(HMM *hmm, double **emission_scores, int seqlen, 
                   double **forward_scores);
double hmm_backward(HMM *hmm, double **emission_scores, int seqlen,
                    double **backward_scores);
void hmm_posterior_probs(HMM *hmm, double **emission_scores, int seqlen,
                         double **posterior_probs);
void hmm_do_dp_forward(HMM *hmm, double **emission_scores, int seqlen, 
                       hmm_mode mode, double **full_scores, int **backptr);
void hmm_do_dp_backward(HMM *hmm, double **emission_scores, int seqlen, 
                        double **full_scores);
double hmm_max_or_sum(HMM *hmm, double **full_scores, double **emission_scores,
                      int **backptr, int i, int j, hmm_mode mode);

void hmm_dump_matrices(HMM *hmm, double **emission_scores, int seqlen,
                       double **full_scores, int **backptr);

void hmm_train_from_counts(HMM *hmm, gsl_matrix *trans_counts, 
                           gsl_matrix *trans_pseudocounts,
                           gsl_vector *state_counts,
                           gsl_vector *state_pseudocounts,
                           gsl_vector *beg_counts, 
                           gsl_vector *beg_pseudocounts);
void hmm_train_from_paths(HMM *hmm, int **path, int npaths,
                          gsl_matrix *trans_pseudocounts, 
                          gsl_vector *state_pseudocounts,int use_begin,
                          gsl_vector *beg_pseudocounts);
void hmm_train_update_counts(gsl_matrix *trans_counts, gsl_vector *state_counts, 
                             gsl_vector *beg_counts,
                             int *path, int len, int nstates);
HMM *hmm_create_trivial();
double hmm_path_likelihood(HMM *hmm, double **emission_scores, int seqlen, 
                           int *path);
double hmm_score_subset(HMM *hmm, double **emission_scores, List *states,
                        int begidx, int len);
double hmm_log_odds_subset(HMM *hmm, double **emission_scores, 
                           List *test_states, List *null_states,
                           int begidx, int len);
void hmm_cross_product(HMM *dest, HMM *src1, HMM *src2);
void hmm_reset(HMM *hmm);
HMM *hmm_reverse_compl(HMM *hmm, List *pivot_states, int *mapping);
void hmm_renormalize(HMM *hmm);

#endif