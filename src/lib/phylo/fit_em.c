/* $Id: fit_em.c,v 1.1.1.1 2004-06-03 22:43:12 acs Exp $
   Written by Adam Siepel, 2002-2004
   Copyright 2002-2004, Adam Siepel, University of California */

/* Functions for fitting tree models by EM */

#include <fit_em.h>
#include <tree_model.h>
#include <stringsplus.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <numerical_opt.h>
#include <tree_likelihoods.h>
#include <time.h>
#include <sys/time.h>
#include <sufficient_stats.h>
#include <matrix.h>
#include <sys/types.h>
#include <unistd.h>
#include <gsl/gsl_complex_math.h>
#include <math.h>
#include <dgamma.h>

#define DERIV_EPSILON 1e-5      
                                /* used for numerical est. of derivatives */

/* internal functions */
double tm_partial_ll_wrapper(gsl_vector *params, void *data);
double tm_partial_ll_wrapper_fast(gsl_vector *params, void *data);
void tm_log_em(FILE *logf, int header_only, double val, gsl_vector *params);
void compute_grad_em_approx(gsl_vector *grad, gsl_vector *params, void *data, 
                            gsl_vector *lb, gsl_vector *ub);
void compute_grad_em_exact(gsl_vector *grad, gsl_vector *params, void *data, 
                           gsl_vector *lb, gsl_vector *ub);
void get_neighbors(int *neighbors, int state, int order, int alph_size);

/* fit a tree model using EM */
int tm_fit_em(TreeModel *mod, MSA *msa, gsl_vector *params, int cat, 
              opt_precision_type precision, FILE *logf) {
  double ll, improvement;
  gsl_vector *lower_bounds, *upper_bounds;
  int retval = 0, it, i, home_stretch = 0, nratecats;
  double lastll = NEGINFTY, alpha, rK0, freqK0, branchlen_scale;
  struct timeval start_time, end_time, post_prob_start, post_prob_end;
  TreeModel *proj_mod = NULL;
  char tmp_mod_fname[STR_SHORT_LEN];
  FILE *F;
  void (*grad_func)(gsl_vector*, gsl_vector*, void*, gsl_vector*, 
                    gsl_vector*);
  gsl_matrix *H = gsl_matrix_alloc(params->size, params->size);
  opt_precision_type bfgs_prec = OPT_LOW_PREC;
                                /* will be adjusted as necessary */

  /* obtain sufficient statistics for MSA, if necessary */
  if (msa->ss == NULL) {
    assert(msa->seqs != NULL);
    ss_from_msas(msa, mod->order+1, 0, NULL, NULL, NULL, -1);
  }

  if (mod->backgd_freqs == NULL) { 
    mod->backgd_freqs = gsl_vector_alloc(mod->rate_matrix->size);
    if (mod->subst_mod == JC69 || mod->subst_mod == K80)
      gsl_vector_set_all(mod->backgd_freqs, 1.0/mod->backgd_freqs->size);
    else
      msa_get_base_freqs_tuples(msa, mod->backgd_freqs, mod->order + 1, cat);
  }

  if (mod->tree == NULL) {      /* weight matrix */
    mod->lnL = tl_compute_log_likelihood(mod, msa, NULL, cat, NULL) * 
      log(2);
    return 0;
  }

  /* if using an expensive model, set up filename for temporary files */
  if (mod->order >= 2) {
    char tmpstr[20];
    gethostname(tmpstr, 20);
    sprintf(tmp_mod_fname, "fit_em.%s.%d.mod", tmpstr, getpid());
  }

  if (logf != NULL)
    gettimeofday(&start_time, NULL);

  /* package with mod any data needed to compute likelihoods */
  mod->msa = msa;               
  mod->tree_posteriors = tl_new_tree_posteriors(mod, msa, 0, 0, 0, 1, 0, 
                                                mod->empirical_rates ? 1 : 0);
  mod->category = cat;
  mod->max_samples = -1;

  /* most params have lower bound of zero and no upper bound */
  lower_bounds = gsl_vector_calloc(params->size);
  upper_bounds = NULL;

  /* however, in this case we don't want the eq freqs to go to zero */
  if (mod->estimate_backgd) {
    int offset = tm_get_nbranchlenparams(mod);
    for (i = 0; i < mod->backgd_freqs->size; i++)
      gsl_vector_set(lower_bounds, i + offset, 0.001);
  }

  /* in the case of rate variation, start by ignoring then reinstate
     when close to convergence */
  nratecats = mod->nratecats;
  if (nratecats > 1) {
    alpha = mod->alpha;
    mod->alpha = -nratecats;    /* code to ignore rate variation
                                   temporarily */
    mod->nratecats = 1;
    freqK0 = mod->freqK[0];
    rK0 = mod->rK[0];
    mod->freqK[0] = mod->rK[0] = 1;
  }

  if (logf != NULL) tm_log_em(logf, 1, 0, params);
  grad_func = (proj_mod == NULL && 
               mod->estimate_branchlens == TM_BRANCHLENS_ALL && 
               mod->subst_mod != JC69 && mod->subst_mod != F81 &&
               !mod->estimate_backgd ? compute_grad_em_approx : NULL);
                                /* functions for analytical gradients
                                   don't yet know about estimating
                                   scale or backgd freqs, also require
                                   diagonalization  */

  gsl_matrix_set_identity(H);

  for (it = 1; ; it++) {
    double tmp;

    tm_unpack_params(mod, params, -1);
    
    /* if appropriate, dump intermediate version of model for inspection */
    if (mod->order >= 2) {
      F = fopen(tmp_mod_fname, "w+"); tm_print(F, mod); fclose(F);
    }

    /* obtain posterior probabilities and likelihood */
    if (logf != NULL) 
      gettimeofday(&post_prob_start, NULL);

    ll = tl_compute_log_likelihood(mod, msa, NULL, cat, mod->tree_posteriors) 
      * log(2); 

    if (logf != NULL) {
      gettimeofday(&post_prob_end, NULL);
      fprintf(logf, "\nTime to collect posterior probabilities: %.4f sec.\n", 
              post_prob_end.tv_sec - post_prob_start.tv_sec + 
              (post_prob_end.tv_usec - post_prob_start.tv_usec)/1.0e6);      
      tm_log_em(logf, 0, ll, params);
    }

    improvement = fabs((lastll - ll)/ll);
    lastll = ll;

    /* check convergence */
    if (improvement < TM_EM_CONV(precision) &&
        bfgs_prec == precision && mod->nratecats == nratecats) 
                                /* don't exit unless precision is
                                   already at its max and rate
                                   variation has been reintroduced (if
                                   necessary) */
      break;

    /* adjust inner optimization strategy as necessary */
    if (improvement < TM_EM_CONV(OPT_CRUDE_PREC)) {
      /* change gradient function first (if necessary), and use
         slightly better precision with BFGS; then on a subsequent
         pass maximize BFGS precision.  A big jump in likelihood
         usually occurs when the gradient function is first changed. */
      if (grad_func == compute_grad_em_approx) {
        if (logf != NULL) fprintf(logf, "Switching to exact gradients.\n");
        grad_func = compute_grad_em_exact;
        if (bfgs_prec == OPT_LOW_PREC && bfgs_prec != precision) 
          bfgs_prec = OPT_MED_PREC;
      }
      else {
        home_stretch = 1;
        if (bfgs_prec != precision) {
          if (logf != NULL) 
            fprintf(logf, "Switching to higher precision with BFGS.\n");
          bfgs_prec = precision;
        }
      }
    }
    /* NOTE: medium precision case could possibly be improved
       by switching to high precision for BFGS approx when improvement <
       2e-5.  Could save some iterations in the outer loop (collecting
       post probs is expensive).  Will require some testing and tuning to
       get it right.  Probably try with U2S, R2, U2. */
    

    opt_bfgs(tm_partial_ll_wrapper, params, (void*)mod, &tmp, lower_bounds,
             upper_bounds, logf, grad_func, bfgs_prec, H); 

    /* in case of empirical rate variation, also reestimate mixing
       proportions (rate weights).  The m.l.e. for these is a simple
       function of the posterior probs. of the rate categories and the
       rate constants (the maximization step of EM decomposes into two
       separate problems) */
    if (mod->nratecats > 1 && mod->empirical_rates) {
      int offset = tm_get_nbranchlenparams(mod) + tm_get_neqfreqparams(mod);
      double sum = 0;
      for (i = 0; i < mod->nratecats; i++) 
        sum += mod->tree_posteriors->rcat_expected_nsites[i];
      for (i = 0; i < mod->nratecats; i++) 
        gsl_vector_set(params, offset + i, 
                       mod->tree_posteriors->rcat_expected_nsites[i] / sum);
      /* NOTE: currently, the rate weights are part of the parameter
         vector optimized by BFGS, but are given partial derivatives
         of zero.  This is mathematically okay, but computationally
         inefficient, because it causes a lot of zeroes to have to be
         propagated through the manipulations of the gradient vector
         and Hessian.  One simple workaround would be to put these
         parameters at the end of the vector, rather than in the
         middle, and to fool BFGS into ignoring them by reducing the
         dimension of the vector before calling opt_bfgs (would then
         change the vector back when opt_bfgs returned) */
    }

    if (mod->nratecats != nratecats && 
        improvement < TM_EM_CONV(OPT_CRUDE_PREC) && home_stretch) {
      if (logf != NULL) fprintf(logf, "Introducing rate variation.\n");
      mod->nratecats = nratecats;
      mod->alpha  = alpha;
      mod->rK[0] = rK0;
      mod->freqK[0] = freqK0;
    }
  }

  mod->lnL = ll;

  /* take care of final scaling of rate matrix and branch lengths */
  branchlen_scale = 1;
  if (mod->subst_mod != JC69 && mod->subst_mod != F81)
    branchlen_scale *= tm_scale_rate_matrix(mod); 
  if (mod->estimate_branchlens == TM_SCALE_ONLY) { 
    branchlen_scale *= mod->scale;
    mod->scale = 1;
  }
  if (mod->nratecats > 1 && mod->empirical_rates) { 
    double rate_scale = 0;
    for (i = 0; i < mod->nratecats; i++) /* scale for expected rate */
      rate_scale += mod->rK[i] * mod->freqK[i];
    branchlen_scale *= rate_scale;
  }
  if (branchlen_scale != 1)
    tm_scale(mod, branchlen_scale, 0); 

  /* close log */
  if (logf != NULL) {
    gettimeofday(&end_time, NULL);
    fprintf(logf, "\nNumber of iterations: %d\nTotal time: %.4f sec.\n", it, 
            end_time.tv_sec - start_time.tv_sec + 
            (end_time.tv_usec - start_time.tv_usec)/1.0e6);
  }

  gsl_vector_free(lower_bounds);
  tl_free_tree_posteriors(mod, msa, mod->tree_posteriors);
  mod->tree_posteriors = NULL;

  return retval;
}

double tm_partial_ll_wrapper(gsl_vector *params, void *data) {
  TreeModel *mod = (TreeModel*)data;
  TreePosteriors *post = mod->tree_posteriors;
  tm_unpack_params(mod, params, -1);
  return -tl_compute_partial_ll_suff_stats(mod, post) * log(2);
}

/* Print a line to a log file that describes the state of the
   optimization procedure on a given iteration.  The value of the
   function is output, along with the values of all parameters.  If
   "header_only == 1", an appropriate header is printed. */
void tm_log_em(FILE *logf, int header_only, double val, gsl_vector *params) {
  int i;
  char tmp[30];
  if (header_only) {
    fprintf(logf, "%15s ", "f(x)");
    for (i = 0; i < params->size; i++) {
      sprintf(tmp, "x_%d", i);
      fprintf(logf, "%15s ", tmp);
    }
    fprintf(logf, "\n");
  }
  else {
    fprintf(logf, "%15.6f ", val);
    for (i = 0; i < params->size; i++) 
      fprintf(logf, "%15.6f ", gsl_vector_get(params, i));
    fprintf(logf, "\n");
  }
  fflush(logf);
}

/* (used in compute_grad_em_approx) given model info and a state number,
   obtain the "neighbors" of the state -- that is, states
   corresponding to all character tuples that differ from it by no
   more than one character */
void get_neighbors(int *neighbors, int state, int order, int alph_size) {
  int place, j, k;
  for (place = 0; place <= order; place++) {
    int state_digit = (state % int_pow(alph_size, place+1)) / 
      int_pow(alph_size, place);   
    int refval = state - state_digit * int_pow(alph_size, place);
    for (j = 0, k = 0; j < alph_size; j++) {
      if (j == state_digit) continue;
      neighbors[place * (alph_size-1) + k] = refval + j *
        int_pow(alph_size, place);
      k++;
    }
  }        

  neighbors[(order+1) * (alph_size-1)] = state;  
                                /* every state is a neighbor of itself */
}

/* Compute gradient for tree model using (approximate) analytical rate
   matrix derivs.  NOTE: the tree model is assumed to be up to date
   wrt the parameters, including the eigenvalues and eigenvectors, and
   the exponentiated matrices associated with each edge (okay if
   tm_unpack_params called since last parameter update) */
void compute_grad_em_approx(gsl_vector *grad, gsl_vector *params, void *data, 
                          gsl_vector *lb, gsl_vector *ub) {

  TreeModel *mod = (TreeModel*)data;
  MarkovMatrix *P, *Q = mod->rate_matrix;
  int alph_size = strlen(mod->rate_matrix->states);
  int nstates = mod->rate_matrix->size;
  int ndigits = mod->order + 1;
  int nneighbors = ((alph_size-1) * ndigits + 1);
  int neighbors[nstates][nneighbors];
  int mark_col[nstates];
  int i, j, k, l, m, rcat, params_idx, node, lidx, lidx2, orig_size, assigned;
  TreeNode *n;
  List *traversal;
  double t;
  double freqK[mod->nratecats], rK_tweak[mod->nratecats];

  List *erows = lst_new_int(4), *ecols = lst_new_int(4), 
    *distinct_rows = lst_new_int(2), *distinct_cols = lst_new_int(4);

  static double **q = NULL, **q2 = NULL, **q3 = NULL, 
    **dq = NULL, **dqq = NULL, **qdq = NULL, **dqq2 = NULL, **qdqq = NULL, 
    **q2dq = NULL, **dqq3 = NULL, **qdqq2 = NULL, **q2dqq = NULL, 
    **q3dq = NULL;
  static gsl_complex *diag = NULL;

  if (diag == NULL) 
    diag = (gsl_complex*)smalloc(nstates * sizeof(gsl_complex));

  /* init memory (first time only) */
  if (q == NULL) {
    q = (double**)smalloc(nstates * sizeof(double*));
    q2 = (double**)smalloc(nstates * sizeof(double*));
    q3 = (double**)smalloc(nstates * sizeof(double*));
    dq = (double**)smalloc(nstates * sizeof(double*));
    dqq = (double**)smalloc(nstates * sizeof(double*));
    qdq = (double**)smalloc(nstates * sizeof(double*));
    dqq2 = (double**)smalloc(nstates * sizeof(double*));
    qdqq = (double**)smalloc(nstates * sizeof(double*));
    q2dq = (double**)smalloc(nstates * sizeof(double*));
    dqq3 = (double**)smalloc(nstates * sizeof(double*));
    qdqq2 = (double**)smalloc(nstates * sizeof(double*));
    q2dqq = (double**)smalloc(nstates * sizeof(double*));
    q3dq = (double**)smalloc(nstates * sizeof(double*));

    for (i = 0; i < nstates; i++) {
      q[i] = (double*)smalloc(nstates * sizeof(double));
      q2[i] = (double*)smalloc(nstates * sizeof(double));
      q3[i] = (double*)smalloc(nstates * sizeof(double));
      dq[i] = (double*)smalloc(nstates * sizeof(double));
      dqq[i] = (double*)smalloc(nstates * sizeof(double));
      qdq[i] = (double*)smalloc(nstates * sizeof(double));
      dqq2[i] = (double*)smalloc(nstates * sizeof(double));
      qdqq[i] = (double*)smalloc(nstates * sizeof(double));
      q2dq[i] = (double*)smalloc(nstates * sizeof(double));
      q3dq[i] = (double*)smalloc(nstates * sizeof(double));
      q2dqq[i] = (double*)smalloc(nstates * sizeof(double));
      qdqq2[i] = (double*)smalloc(nstates * sizeof(double));
      dqq3[i] = (double*)smalloc(nstates * sizeof(double));
    }
  }
  
  /* set Q, zero Q^2 and Q^3 */
  for (i = 0; i < nstates; i++) {
    for (j = 0; j < nstates; j++) {
        q[i][j] = mm_get(mod->rate_matrix, i, j); /* just for
                                                     convenience
                                                     below */
        q2[i][j] = q3[i][j] = 0;
    }
  }

  /* obtain the neighbors of each state, used throughout to improve
     efficiency of multiplications */
  for (i = 0; i < nstates; i++) 
    get_neighbors(neighbors[i], i, mod->order, alph_size);

  /* compute Q^2 */
  for (i = 0; i < nstates; i++) {
    for (j = 0; j < nstates; j++) 
      for (k = 0; k < nneighbors; k++) 
        q2[i][j] += q[i][neighbors[i][k]] * q[neighbors[i][k]][j];
  }
  /* you can do this a bit more efficiently, but I don't think it's
     worth the trouble */

  /* compute Q^3 */
  for (i = 0; i < nstates; i++) {
    for (j = 0; j < nstates; j++) 
      for (k = 0; k < nneighbors; k++) 
        q3[i][j] += q[i][neighbors[i][k]] * q2[neighbors[i][k]][j];
  }

  gsl_vector_set_zero(grad);

  /* compute partial derivs for branch length params */
  traversal = tr_preorder(mod->tree); /* branch-length parameters
                                         correspond to pre-order
                                         traversal of tree */
  params_idx = 0;
  assigned = 0;
  for (j = 0; j < lst_size(traversal); j++) {
    double unrooted_factor;
    int grad_idx;

    n = lst_get_ptr(traversal, j);
    if (n == mod->tree || n->id == mod->root_leaf_id) continue;

    /* if the tree is unrooted, then the the branches descending from
       the (virtual) root are governed by a single parameter, and
       there is a hidden factor of 1/2 to consider in the
       derivative */
    if (tm_is_reversible(mod->subst_mod) && 
        (n == mod->tree->lchild || n == mod->tree->rchild)) {
      grad_idx = 0;
      unrooted_factor = 0.5;
      if (!assigned) { params_idx++; assigned = 1; }
    }
    else {
      unrooted_factor = 1.0;
      grad_idx = params_idx++;
    }

    /* NOTE: I think the contortions above are provably equivalent to
       the simpler strategy of ignoring one of the two branches from
       the root and treating the other as an ordinary branch, because
       with a reversible process, the expected numbers of
       substitutions on the two branches have to be equal, and 2 * 1/2
       = 1.  This was how I had originally implemented things,
       without understanding the subtleties ... */
 
    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      P = mod->P[n->id][rcat];
      t = n->dparent * mod->rK[rcat]; /* the factor of 1/2 is taken
                                         care of here, in the def. of
                                         n->dparent */

      /* main diagonal of matrix of eigenvalues * exponentials of
         eigenvalues for branch length t*/
      for (i = 0; i < nstates; i++)
        diag[i] = gsl_complex_mul_real(gsl_complex_mul(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), gsl_vector_complex_get(Q->evals, i)), mod->rK[rcat] * unrooted_factor);

      /* save time by only using complex numbers in the inner loop if
         necessary (each complex mult equivalent to four real mults and
         two real adds) */
      if (tm_is_reversible(mod->subst_mod)) {
        for (k = 0; k < nstates; k++) {
          for (l = 0; l < nstates; l++) {
            double p = mm_get(P, k, l);
            double dp_dt = 0;
            double dp_dt_div_p;

            for (i = 0; i < nstates; i++) 
              dp_dt += GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix, k, i)) * 
                GSL_REAL(diag[i]) * 
                GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix_inv, i, l));

            /* have to handle case of p == 0 carefully -- want contrib
               to derivative to be zero if dp_dt == 0 or
               expected_nsubst_tot == 0 (as will normally be the case)
               and want to avoid a true inf value */
            if (p == 0) {
              if (dp_dt == 0) dp_dt_div_p = 0;
              else if (dp_dt < 0) dp_dt_div_p = NEGINFTY;
              else dp_dt_div_p = INFTY;
            }
            else dp_dt_div_p = dp_dt / p;
          
            gsl_vector_set(grad, grad_idx, gsl_vector_get(grad, grad_idx) +
                           dp_dt_div_p * 
                           mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]); 

          }
        }
      }
      else {                      /* non-reversible model -- need to
                                     allow for complex numbers */
        for (k = 0; k < nstates; k++) {
          for (l = 0; l < nstates; l++) {
            double p = mm_get(P, k, l);
            double partial_p_div_p;
            gsl_complex partial_p; 
            GSL_SET_REAL(&partial_p, 0); GSL_SET_IMAG(&partial_p, 0);

            for (i = 0; i < nstates; i++) 
              partial_p = gsl_complex_add(partial_p, gsl_complex_mul(gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix, k, i), diag[i]), gsl_matrix_complex_get(Q->evec_matrix_inv, i, l)));
          
            assert(fabs(GSL_IMAG(partial_p)) <= TM_IMAG_EPS);

            /* see comments for real case (above) */
            if (p == 0) {
              if (GSL_REAL(partial_p) == 0) partial_p_div_p = 0;
              else if (GSL_REAL(partial_p) < 0) partial_p_div_p = NEGINFTY;
              else partial_p_div_p = INFTY;
            }
            else partial_p_div_p = GSL_REAL(partial_p) / p;

            gsl_vector_set(grad, grad_idx, gsl_vector_get(grad, grad_idx) +
                           partial_p_div_p * 
                           mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]);
          }
        }
      }
    }
  }

  /* compute partial deriv for alpha (if dgamma) */
  if (mod->nratecats > 1 && !mod->empirical_rates) {
    /* for numerical est. of derivatives of rate consts wrt alpha */
    DiscreteGamma(freqK, rK_tweak, mod->alpha + DERIV_EPSILON, 
                  mod->alpha + DERIV_EPSILON, mod->nratecats, 0);

    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      double dr_da = (rK_tweak[rcat] - mod->rK[rcat]) / DERIV_EPSILON;

      for (j = 0; j < mod->tree->nnodes; j++) {
        n = lst_get_ptr(mod->tree->nodes, j);
        if (n->parent == NULL || n->id == mod->root_leaf_id) continue;
        t = n->dparent * mod->rK[rcat];
        P = mod->P[n->id][rcat];

        for (i = 0; i < nstates; i++)
          diag[i] = gsl_complex_mul_real(gsl_complex_mul(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), gsl_vector_complex_get(Q->evals, i)), n->dparent * dr_da);

        /* only use complex numbers if necessary (as above) */
        if (tm_is_reversible(mod->subst_mod)) {
          for (k = 0; k < nstates; k++) {
            for (l = 0; l < nstates; l++) {
              double p = mm_get(P, k, l);
              double dp_da = 0; 
              double dp_da_div_p;

              for (i = 0; i < nstates; i++) 
                dp_da += 
                  GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix, k, i)) * 
                  GSL_REAL(diag[i]) * 
                  GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix_inv, i, l));
 
              if (p == 0) {
                if (dp_da == 0) dp_da_div_p = 0;
                else if (dp_da < 0) dp_da_div_p = NEGINFTY;
                else dp_da_div_p = INFTY;
              }
              else dp_da_div_p = dp_da / p;

              gsl_vector_set(grad, params_idx, 
                             gsl_vector_get(grad, params_idx) +
                             dp_da_div_p * 
                             mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]); 
            }
          }
        }
        else {                  /* non-reversible model -- use complex
                                   numbers */
          for (k = 0; k < nstates; k++) {
            for (l = 0; l < nstates; l++) {
              double p = mm_get(P, k, l);
              gsl_complex dp_da; 
              double dp_da_div_p;
              GSL_SET_REAL(&dp_da, 0); GSL_SET_IMAG(&dp_da, 0);
              for (i = 0; i < nstates; i++) 
                dp_da = gsl_complex_add(dp_da, gsl_complex_mul(gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix, k, i), diag[i]), gsl_matrix_complex_get(Q->evec_matrix_inv, i, l)));

              assert(fabs(GSL_IMAG(dp_da)) <= TM_IMAG_EPS);

              if (p == 0) {
                if (GSL_REAL(dp_da) == 0) dp_da_div_p = 0;
                else if (GSL_REAL(dp_da) < 0) dp_da_div_p = NEGINFTY;
                else dp_da_div_p = INFTY;
              }
              else dp_da_div_p = GSL_REAL(dp_da) / p;

              gsl_vector_set(grad, params_idx, gsl_vector_get(grad, params_idx) +
                             dp_da_div_p * 
                             mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]); 
            }
          }
        }
      }
    }
    params_idx++;
  }
  else if (mod->empirical_rates && (mod->nratecats > 1 || mod->alpha < 0)) {
                                /* empirical rates -- gradient is zero
                                   wrt rate weights (they're already
                                   incorporated into the post
                                   probs) */
    int nrc = mod->nratecats > 1 ? mod->nratecats : -mod->alpha;
    for (; nrc >= 1; nrc--) gsl_vector_set(grad, params_idx++, 0);
  }
  else if (mod->alpha < 0)      /* dgamma temporarily disabled -- grad
                                   for alpha is zero */
    gsl_vector_set(grad, params_idx++, 0);


  /* compute partial derivs for rate matrix params */

  assert(mod->subst_mod != JC69 && mod->subst_mod != K80 && 
         /* mod->subst_mod != HKY2 &&  */mod->subst_mod != UNDEF_MOD); 
                                /* FIXME: temporary */

  for (; params_idx < params->size; params_idx++) {

    /* zero all matrices */
    for (i = 0; i < nstates; i++)
      for (j = 0; j < nstates; j++)
       dq[i][j] = dqq[i][j] = qdq[i][j] = dqq2[i][j] = 
         qdqq[i][j] = q2dq[i][j] = dqq3[i][j] = 
         qdqq2[i][j] = q2dqq[i][j] = q3dq[i][j] = 0;

    /* element coords (rows/col pairs) at which current param appears in Q */
    lst_cpy(erows, mod->rate_matrix_param_row[params_idx]);
    lst_cpy(ecols, mod->rate_matrix_param_col[params_idx]);
    assert(lst_size(erows) == lst_size(ecols));

    /* set up dQ, the partial deriv of Q wrt the current param */
    for (i = 0; i < nstates; i++) mark_col[i] = 0;
    lst_clear(distinct_rows);
    lst_clear(distinct_cols);
    for (i = 0, orig_size = lst_size(erows); i < orig_size; i++) {
      l = lst_get_int(erows, i); 
      m = lst_get_int(ecols, i);

      assert(dq[l][m] == 0);    /* row/col pairs should be unique */

      dq[l][m] = tm_is_reversible(mod->subst_mod) ? 
        gsl_vector_get(mod->backgd_freqs, m) : 1;
                                /* FIXME: may need to generalize */
      
      if (dq[l][m] == 0) continue; 
      /* possible if reversible with zero eq freq */

      /* keep track of distinct rows and cols with non-zero entries */
      /* also add diagonal elements to 'rows' and 'cols' lists, as
         necessary */
      if (dq[l][l] == 0) {      /* new row */
        lst_push_int(distinct_rows, l);
        lst_push_int(erows, l);
        lst_push_int(ecols, l);
      }
      if (!mark_col[m]) {       /* new col */
        lst_push_int(distinct_cols, m); 
        mark_col[m] = 1; 
      }
      if (!mark_col[l]) {       /* row also col, because of diag elment */
        lst_push_int(distinct_cols, l); 
        mark_col[l] = 1; 
      }

      dq[l][l] -= dq[l][m];     /* note that a param can appear
                                   multiple times in a row */
    }

    /* compute (dQ)Q */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      i = lst_get_int(erows, lidx);
      k = lst_get_int(ecols, lidx);
      for (j = 0; j < nstates; j++)
        dqq[i][j] += dq[i][k] * q[k][j];
    }

    /* compute Q(dQ) */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      k = lst_get_int(erows, lidx);
      j = lst_get_int(ecols, lidx);
      for (i = 0; i < nstates; i++)
        qdq[i][j] += q[i][k] * dq[k][j];
    }

    /* compute (dQ)Q^2 */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      i = lst_get_int(erows, lidx);
      k = lst_get_int(ecols, lidx);
      for (j = 0; j < nstates; j++)
        dqq2[i][j] += dq[i][k] * q2[k][j];
    }

    /* compute Q(dQ)Q */
    for (lidx = 0; lidx < lst_size(distinct_rows); lidx++) {
      k = lst_get_int(distinct_rows, lidx);
      for (lidx2 = 0; lidx2 < nneighbors; lidx2++) {
        i = neighbors[k][lidx2];
        for (j = 0; j < nstates; j++)
          qdqq[i][j] += q[i][k] * dqq[k][j];
      }
    }

    /* compute Q^2(dQ) */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      k = lst_get_int(erows, lidx);
      j = lst_get_int(ecols, lidx);
      for (i = 0; i < nstates; i++)
        q2dq[i][j] += q2[i][k] * dq[k][j];
    }

    /* compute (dQ)Q^3 */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      i = lst_get_int(erows, lidx);
      k = lst_get_int(ecols, lidx);
      for (j = 0; j < nstates; j++)
        dqq3[i][j] += dq[i][k] * q3[k][j];
    }

    /* compute Q(dQ)Q^2 */
    for (lidx = 0; lidx < lst_size(distinct_rows); lidx++) {
      k = lst_get_int(distinct_rows, lidx);
      for (lidx2 = 0; lidx2 < nneighbors; lidx2++) {
        i = neighbors[k][lidx2];
        for (j = 0; j < nstates; j++)
          qdqq2[i][j] += q[i][k] * dqq2[k][j];
      }
    }

    /* compute Q^2(dQ)Q */
    for (lidx = 0; lidx < lst_size(distinct_cols); lidx++) {
      k = lst_get_int(distinct_cols, lidx);
      for (lidx2 = 0; lidx2 < nneighbors; lidx2++) {
        j = neighbors[k][lidx2];
        for (i = 0; i < nstates; i++)
          q2dqq[i][j] += q2dq[i][k] * q[k][j];
      }
    }

    /* compute Q^3(dQ) */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      k = lst_get_int(erows, lidx);
      j = lst_get_int(ecols, lidx);
      for (i = 0; i < nstates; i++)
        q3dq[i][j] += q3[i][k] * dq[k][j];
    }

    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      for (node = 0; node < mod->tree->nnodes; node++) {
        double taylor2, taylor3, taylor4;

        if (node == mod->tree->id || node == mod->root_leaf_id)
          continue; 

        n = lst_get_ptr(mod->tree->nodes, node);
        t = n->dparent * mod->rK[rcat];
        P = mod->P[n->id][rcat];

        /* this allows for fewer mults in intensive loop below */
        taylor2 = t*t/2;
        taylor3 = t*t*t/6;
        taylor4 = t*t*t*t/24;

        for (i = 0; i < nstates; i++) {
          for (j = 0; j < nstates; j++) {
            double partial_p = t * dq[i][j] + 
              taylor2 * (dqq[i][j] + qdq[i][j]) +
              taylor3 * (dqq2[i][j] + qdqq[i][j] + q2dq[i][j]) +
              taylor4 * (dqq3[i][j] + qdqq2[i][j] + q2dqq[i][j] + 
                         q3dq[i][j]);
            double p = mm_get(P, i, j);
            double partial_p_div_p;

            /* handle case of p == 0 carefully, as described above */
            if (p == 0) {
              if (partial_p == 0) partial_p_div_p = 0;
              else if (partial_p < 0) partial_p_div_p = NEGINFTY;
              else partial_p_div_p = INFTY;
            }
            else partial_p_div_p = partial_p / p;

            gsl_vector_set(grad, params_idx, gsl_vector_get(grad, params_idx) + 
                           partial_p_div_p * 
                           mod->tree_posteriors->expected_nsubst_tot[rcat][i][j][node]);
          }
        }
      }
    }
  }
  gsl_vector_scale(grad, -1);
  lst_free(erows); lst_free(ecols); lst_free(distinct_rows); 
  lst_free(distinct_cols);
}

/* Like above, but using the approach outlined by Schadt and Lange for
   computing the partial derivatives wrt rate matrix parameters.
   Slower, but gives exact results */
void compute_grad_em_exact(gsl_vector *grad, gsl_vector *params, void *data, 
                           gsl_vector *lb, gsl_vector *ub) {

  TreeModel *mod = (TreeModel*)data;
/*   int alph_size = strlen(mod->rate_matrix->states); */
  int nstates = mod->rate_matrix->size;
  int i, j, k, l, m, rcat, params_idx, node, lidx, orig_size, assigned;
  TreeNode *n;
  MarkovMatrix *P, *Q;
  List *traversal;
  List *erows = lst_new_int(4), *ecols = lst_new_int(4), 
    *distinct_rows = lst_new_int(2);
  double t;
  double freqK[mod->nratecats], rK_tweak[mod->nratecats];

  static double **dq = NULL;
  static gsl_complex **f = NULL, **tmpmat = NULL, **sinv_dq_s = NULL;
  static gsl_complex *diag = NULL;

  if (diag == NULL) 
    diag = (gsl_complex*)smalloc(nstates * sizeof(gsl_complex));

  Q = mod->rate_matrix;

  /* init memory (first time only) */
  if (dq == NULL) {
    dq = (double**)smalloc(nstates * sizeof(double*));
    f = (gsl_complex**)smalloc(nstates * sizeof(gsl_complex*));
    tmpmat = (gsl_complex**)smalloc(nstates * sizeof(gsl_complex*));
    sinv_dq_s = (gsl_complex**)smalloc(nstates * sizeof(gsl_complex*));

    for (i = 0; i < nstates; i++) {
      dq[i] = (double*)smalloc(nstates * sizeof(double));
      f[i] = (gsl_complex*)smalloc(nstates * sizeof(gsl_complex));
      tmpmat[i] = (gsl_complex*)smalloc(nstates * sizeof(gsl_complex));
      sinv_dq_s[i] = (gsl_complex*)smalloc(nstates * sizeof(gsl_complex));
    }
  }
  
  gsl_vector_set_zero(grad);

  /* compute partial derivs for branch length params */
  traversal = tr_preorder(mod->tree); /* branch-length parameters
                                         correspond to pre-order
                                         traversal of tree */
  params_idx = 0;
  assigned = 0;
  for (j = 0; j < lst_size(traversal); j++) {
    double unrooted_factor;
    int grad_idx;

    n = lst_get_ptr(traversal, j);
    if (n == mod->tree || n->id == mod->root_leaf_id) continue;

    /* if the tree is unrooted, then the the branches descending from
       the (virtual) root are governed by a single parameter, and
       there is a hidden factor of 1/2 to consider in the
       derivative */
    if (tm_is_reversible(mod->subst_mod) && 
        (n == mod->tree->lchild || n == mod->tree->rchild)) {
      grad_idx = 0;
      unrooted_factor = 0.5;
      if (!assigned) { params_idx++; assigned = 1; }
    }
    else {
      unrooted_factor = 1.0;
      grad_idx = params_idx++;
    }

    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      P = mod->P[n->id][rcat];
      t = n->dparent * mod->rK[rcat]; /* the factor of 1/2 is taken
                                         care of here, in the def. of
                                         n->dparent */

      /* main diagonal of matrix of eigenvalues * exponentials of
         eigenvalues for branch length t*/
      for (i = 0; i < nstates; i++)
        diag[i] = gsl_complex_mul_real(gsl_complex_mul(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), gsl_vector_complex_get(Q->evals, i)), mod->rK[rcat] * unrooted_factor);

      /* save time by only using complex numbers in the inner loop if
         necessary (each complex mult equivalent to four real mults and
         two real adds) */
      if (tm_is_reversible(mod->subst_mod)) {
        for (k = 0; k < nstates; k++) {
          for (l = 0; l < nstates; l++) {
            double p = mm_get(P, k, l);
            double dp_dt = 0;
            double dp_dt_div_p;

            for (i = 0; i < nstates; i++) 
              dp_dt += GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix, k, i)) * 
                GSL_REAL(diag[i]) * 
                GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix_inv, i, l));

            /* have to handle case of p == 0 carefully -- want contrib
               to derivative to be zero if dp_dt == 0 or
               expected_nsubst_tot == 0 (as will normally be the case)
               and want to avoid a true inf value */
            if (p == 0) {
              if (dp_dt == 0) dp_dt_div_p = 0;
              else if (dp_dt < 0) dp_dt_div_p = NEGINFTY;
              else dp_dt_div_p = INFTY;
            }
            else dp_dt_div_p = dp_dt / p;
          
            gsl_vector_set(grad, grad_idx, gsl_vector_get(grad, grad_idx) +
                           dp_dt_div_p *
                           mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]);

          }
        }
      }
      else {                      /* non-reversible model -- need to
                                     allow for complex numbers */
        for (k = 0; k < nstates; k++) {
          for (l = 0; l < nstates; l++) {
            double p = mm_get(P, k, l);
            double dp_dt_div_p;
            gsl_complex dp_dt; 
            GSL_SET_REAL(&dp_dt, 0); GSL_SET_IMAG(&dp_dt, 0);

            for (i = 0; i < nstates; i++) 
              dp_dt = gsl_complex_add(dp_dt, gsl_complex_mul(gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix, k, i), diag[i]), gsl_matrix_complex_get(Q->evec_matrix_inv, i, l)));

            /* see comments for real case (above) */
            if (p == 0) {
              if (GSL_REAL(dp_dt) == 0) dp_dt_div_p = 0;
              else if (GSL_REAL(dp_dt) < 0) dp_dt_div_p = NEGINFTY;
              else dp_dt_div_p = INFTY;
            }
            else dp_dt_div_p = GSL_REAL(dp_dt) / p;

            assert(fabs(GSL_IMAG(dp_dt)) <= TM_IMAG_EPS);
            gsl_vector_set(grad, grad_idx, gsl_vector_get(grad, grad_idx) +
                           dp_dt_div_p *
                           mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]);
          }
        }
      }
    }
  }

  /* compute partial deriv for alpha (if dgamma) */
  if (mod->nratecats > 1 && !mod->empirical_rates) {
    /* for numerical est. of derivatives of rate consts wrt alpha */
    DiscreteGamma(freqK, rK_tweak, mod->alpha + DERIV_EPSILON, 
                  mod->alpha + DERIV_EPSILON, mod->nratecats, 0);

    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      double dr_da = (rK_tweak[rcat] - mod->rK[rcat]) / DERIV_EPSILON;

      for (j = 0; j < mod->tree->nnodes; j++) {
        n = lst_get_ptr(mod->tree->nodes, j);
        if (n->parent == NULL || n->id == mod->root_leaf_id) continue;
        t = n->dparent * mod->rK[rcat];
        P = mod->P[n->id][rcat];

        for (i = 0; i < nstates; i++)
          diag[i] = gsl_complex_mul_real(gsl_complex_mul(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), gsl_vector_complex_get(Q->evals, i)), n->dparent * dr_da);

        /* only use complex numbers if necessary (as above) */
        if (tm_is_reversible(mod->subst_mod)) {
          for (k = 0; k < nstates; k++) {
            for (l = 0; l < nstates; l++) {
              double p = mm_get(P, k, l);
              double dp_da = 0; 
              double dp_da_div_p;

              for (i = 0; i < nstates; i++) 
                dp_da += 
                  GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix, k, i)) * 
                  GSL_REAL(diag[i]) * 
                  GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix_inv, i, l));
 
              if (p == 0) {
                if (dp_da == 0) dp_da_div_p = 0;
                else if (dp_da < 0) dp_da_div_p = NEGINFTY;
                else dp_da_div_p = INFTY;
              }
              else dp_da_div_p = dp_da / p;

              gsl_vector_set(grad, params_idx, 
                             gsl_vector_get(grad, params_idx) +
                             dp_da_div_p * 
                             mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]); 
            }
          }
        }
        else {                  /* non-reversible model -- use complex
                                   numbers */
          for (k = 0; k < nstates; k++) {
            for (l = 0; l < nstates; l++) {
              double p = mm_get(P, k, l);
              gsl_complex dp_da; 
              double dp_da_div_p;
              GSL_SET_REAL(&dp_da, 0); GSL_SET_IMAG(&dp_da, 0);
              for (i = 0; i < nstates; i++) 
                dp_da = gsl_complex_add(dp_da, gsl_complex_mul(gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix, k, i), diag[i]), gsl_matrix_complex_get(Q->evec_matrix_inv, i, l)));

              assert(fabs(GSL_IMAG(dp_da)) <= TM_IMAG_EPS);

              if (p == 0) {
                if (GSL_REAL(dp_da) == 0) dp_da_div_p = 0;
                else if (GSL_REAL(dp_da) < 0) dp_da_div_p = NEGINFTY;
                else dp_da_div_p = INFTY;
              }
              else dp_da_div_p = GSL_REAL(dp_da) / p;

              gsl_vector_set(grad, params_idx, gsl_vector_get(grad, params_idx) +
                             dp_da_div_p * 
                             mod->tree_posteriors->expected_nsubst_tot[rcat][k][l][n->id]); 
            }
          }
        }
      }
    }
    params_idx++;
  }
  else if (mod->empirical_rates && (mod->nratecats > 1 || mod->alpha < 0)) {
                                /* empirical rates -- gradient is zero
                                   wrt rate weights (they're already
                                   incorporated into the post
                                   probs) */
    int nrc = mod->nratecats > 1 ? mod->nratecats : -mod->alpha;
    for (; nrc >= 1; nrc--) gsl_vector_set(grad, params_idx++, 0);
  }
  else if (mod->alpha < 0)      /* dgamma temporarily disabled -- grad
                                   for alpha is zero */
    gsl_vector_set(grad, params_idx++, 0);

  /* compute partial derivs for rate matrix params */

  assert(mod->subst_mod != JC69 && mod->subst_mod != K80 && 
         mod->subst_mod != UNDEF_MOD); 
                                /* FIXME: temporary */

  for (; params_idx < params->size; params_idx++) {

    for (i = 0; i < nstates; i++) {
      for (j = 0; j < nstates; j++) {
        dq[i][j] = 0;
        GSL_SET_REAL(&tmpmat[i][j], 0); GSL_SET_IMAG(&tmpmat[i][j], 0);
        GSL_SET_REAL(&sinv_dq_s[i][j], 0); GSL_SET_IMAG(&sinv_dq_s[i][j], 0);
      }
    }

    /* element coords (rows/col pairs) at which current param appears in Q */
    lst_cpy(erows, mod->rate_matrix_param_row[params_idx]);
    lst_cpy(ecols, mod->rate_matrix_param_col[params_idx]);
    assert(lst_size(erows) == lst_size(ecols));

    /* set up dQ, the partial deriv of Q wrt the current param */
    lst_clear(distinct_rows);
    for (i = 0, orig_size = lst_size(erows); i < orig_size; i++) {
      l = lst_get_int(erows, i); 
      m = lst_get_int(ecols, i);

      assert(dq[l][m] == 0);    /* row/col pairs should be unique */

      dq[l][m] = tm_is_reversible(mod->subst_mod) ? 
        gsl_vector_get(mod->backgd_freqs, m) : 1;
                                /* FIXME: may need to generalize */
      
      if (dq[l][m] == 0) continue; 
      /* possible if reversible with zero eq freq */

      /* keep track of distinct rows and cols with non-zero entries */
      /* also add diagonal elements to 'rows' and 'cols' lists, as
         necessary */
      if (dq[l][l] == 0) {      /* new row */
        lst_push_int(distinct_rows, l);
        lst_push_int(erows, l);
        lst_push_int(ecols, l);
      }

      dq[l][l] -= dq[l][m];     /* note that a param can appear
                                   multiple times in a row */
    }

    /* compute S^-1 dQ S */
    for (lidx = 0; lidx < lst_size(erows); lidx++) {
      i = lst_get_int(erows, lidx);
      k = lst_get_int(ecols, lidx);
      for (j = 0; j < nstates; j++)
        tmpmat[i][j] = gsl_complex_add(tmpmat[i][j], gsl_complex_mul_real(gsl_matrix_complex_get(Q->evec_matrix, k, j), dq[i][k]));
    }

    for (lidx = 0; lidx < lst_size(distinct_rows); lidx++) {
      k = lst_get_int(distinct_rows, lidx);
      for (i = 0; i < nstates; i++) {
        for (j = 0; j < nstates; j++) {
          sinv_dq_s[i][j] =
            gsl_complex_add(sinv_dq_s[i][j], gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix_inv, i, k), tmpmat[k][j]));
        }
      }
    }

    for (rcat = 0; rcat < mod->nratecats; rcat++) {
      for (node = 0; node < mod->tree->nnodes; node++) {
        if (node == mod->tree->id || node == mod->root_leaf_id)
          continue; 

        n = lst_get_ptr(mod->tree->nodes, node);
        t = n->dparent * mod->rK[rcat];
        P = mod->P[n->id][rcat];

        /* as above, it's worth it to have separate versions of the
           computations below for the real and complex cases */

        if (tm_is_reversible(mod->subst_mod)) { /* real case */
          /* build the matrix F */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              if (GSL_REAL(gsl_vector_complex_get(Q->evals, i)) ==
                  GSL_REAL(gsl_vector_complex_get(Q->evals, j)))
                GSL_SET_REAL(&f[i][j], exp(GSL_REAL(gsl_vector_complex_get(Q->evals, i)) * t) * t);
              else
                GSL_SET_REAL(&f[i][j], 
                             (exp(GSL_REAL(gsl_vector_complex_get(Q->evals, i)) * t) 
                              - exp(GSL_REAL(gsl_vector_complex_get(Q->evals, j)) * t)) /
                             (GSL_REAL(gsl_vector_complex_get(Q->evals, i)) - 
                              GSL_REAL(gsl_vector_complex_get(Q->evals, j))));
          
            }
          }

          /* compute (F o S^-1 dQ S) S^-1 */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              GSL_SET_REAL(&tmpmat[i][j], 0);
              for (k = 0; k < nstates; k++) 
                GSL_SET_REAL(&tmpmat[i][j], 
                             GSL_REAL(tmpmat[i][j]) + GSL_REAL(f[i][k]) *
                             GSL_REAL(sinv_dq_s[i][k]) *
                             GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix_inv, k, j)));
            }
          }

          /* compute S (F o S^-1 dQ S) S^-1; simultaneously compute
             gradient elements */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              double partial_p = 0;
              double p = mm_get(P, i, j);
              double partial_p_div_p;

              for (k = 0; k < nstates; k++) 
                partial_p += GSL_REAL(gsl_matrix_complex_get(Q->evec_matrix, i, k)) * 
                  GSL_REAL(tmpmat[k][j]);

              /* handle case of p == 0 carefully, as described above */
              if (p == 0) {
                if (partial_p == 0) partial_p_div_p = 0;
                else if (partial_p < 0) partial_p_div_p = NEGINFTY;
                else partial_p_div_p = INFTY;
              }
              else partial_p_div_p = partial_p / p;

              gsl_vector_set(grad, params_idx, gsl_vector_get(grad, params_idx) + 
                             partial_p_div_p *
                             mod->tree_posteriors->expected_nsubst_tot[rcat][i][j][node]);
            }
          }
        }
        else {                    /* complex case */
          /* build the matrix F */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              if (GSL_COMPLEX_EQ(gsl_vector_complex_get(Q->evals, i),
                                 gsl_vector_complex_get(Q->evals, j)))
                f[i][j] = gsl_complex_mul_real(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), t);
              else
                f[i][j] = gsl_complex_div(gsl_complex_sub(gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, i), t)), gsl_complex_exp(gsl_complex_mul_real(gsl_vector_complex_get(Q->evals, j), t))), gsl_complex_sub(gsl_vector_complex_get(Q->evals, i), gsl_vector_complex_get(Q->evals, j)));
          
            }
          }

          /* compute (F o S^-1 dQ S) S^-1 */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              GSL_SET_REAL(&tmpmat[i][j], 0);
              GSL_SET_IMAG(&tmpmat[i][j], 0);
              for (k = 0; k < nstates; k++) 
                tmpmat[i][j] = gsl_complex_add(tmpmat[i][j], gsl_complex_mul(f[i][k], gsl_complex_mul(sinv_dq_s[i][k], gsl_matrix_complex_get(Q->evec_matrix_inv, k, j))));
            }
          }

          /* compute S (F o S^-1 dQ S) S^-1; simultaneously compute
             gradient elements */
          for (i = 0; i < nstates; i++) {
            for (j = 0; j < nstates; j++) {
              gsl_complex partial_p; 
              double p = mm_get(P, i, j);
              double partial_p_div_p;
              GSL_SET_REAL(&partial_p, 0); GSL_SET_IMAG(&partial_p, 0);

              for (k = 0; k < nstates; k++) 
                partial_p = gsl_complex_add(partial_p, gsl_complex_mul(gsl_matrix_complex_get(Q->evec_matrix, i, k), tmpmat[k][j]));

              assert(fabs(GSL_IMAG(partial_p)) <= TM_IMAG_EPS);

              /* handle case of p == 0 carefully, as described above */
              if (p == 0) {
                if (GSL_REAL(partial_p) == 0) partial_p_div_p = 0;
                else if (GSL_REAL(partial_p) < 0) partial_p_div_p = NEGINFTY;
                else partial_p_div_p = INFTY;
              }
              else partial_p_div_p = GSL_REAL(partial_p) / p;

              gsl_vector_set(grad, params_idx, gsl_vector_get(grad, params_idx) + 
                             partial_p_div_p * 
                             mod->tree_posteriors->expected_nsubst_tot[rcat][i][j][node]);
            }
          }
        }
      }
    }
  }
  gsl_vector_scale(grad, -1);
  lst_free(erows); lst_free(ecols); lst_free(distinct_rows); 
}
