/* stacks - Simple array-based stacks and supporting functions (uses 'lists'). */

/* See documentation for lists2.h.

   $Id: stacks.h,v 1.1.1.1 2004-06-03 22:43:11 acs Exp $
   Written by Adam Siepel, Spring 2001, Summer 2002
   Copyright 2001, 2002, Adam Siepel, University of California
*/

#ifndef STACKS_H
#define STACKS_H

#include "lists.h"

typedef List Stack;

/* Test whether stack is empty.
   Returns 1 if empty, 0 otherwise */
int stk_empty(Stack *s);

/* Pop object from stack.
   Object is removed.
   Returns address of object at top of stack, or NULL if stack is
   empty.  If object is an int, you will need to access it with
   *((int*)lst_get(...)); if object is a Node* and you seek the
   attribute called "data", you will need to do
   ((Node*)lst_get(...))->data. */
void* stk_pop(Stack* s);

/* Peek at object at top of stack.
   Object is not removed.
   Returns address of object at top of stack, or NULL if stack is
   empty.  See stk_pop for details. */
void* stk_peek(Stack* s);

/* Pop integer from stack.
   Integer is removed.
   Returns integer from top of stack or 0 if stack is empty (use stk_empty).

   Warning: return value will be ambiguous when using numeric data
   containing zeroes.  Use stk_empty to tell when stack is empty. */
int stk_pop_int(Stack *s);

/* Pop double from stack.
   Double is removed.
   Returns double from top of stack or 0 if stack is empty (use stk_empty).

   Warning: return value will be ambiguous when using numeric data
   containing zeroes.  Use stk_empty to tell when stack is empty. */
double stk_pop_dbl(Stack *s);

/* Pop pointer from stack.
   Pointer is removed.
   Returns pointer from top of stack or NULL if stack is empty. */
void* stk_pop_ptr(Stack *s);

/* Peek at integer at top of stack.
   Integer is not removed.
   Returns integer from top of stack or 0 if stack is empty (use stk_empty).

   Warning: return value will be ambiguous when using numeric data
   containing zeroes.  Use stk_empty to tell when stack is empty. */
int stk_peek_int(Stack *s);

/* Peek at double at top of stack.
   Double is not removed.
   Returns double from top of stack or 0 if stack is empty (use stk_empty).

   Warning: return value will be ambiguous when using numeric data
   containing zeroes.  Use stk_empty to tell when stack is empty. */
double stk_peek_dbl(Stack *s);

/* Peek at pointer at top of stack.
   Pointer is not removed.
   Returns pointer from top of stack or NULL if stack is empty. */
void* stk_peek_ptr(Stack *s);

/* Push object on stack */
void stk_push(Stack *s, void *o);

/* Push integer on stack */
void stk_push_int(Stack *s, int i);

/* Push double on stack */
void stk_push_dbl(Stack *s, double d);

/* Push pointer on stack */
void stk_push_ptr(Stack *s, void *ptr);

/* Obtain size of stack (number of elements).
   Returns number of elements */
int stk_size(Stack *s);

/* Create new stack of objects.
   Returns newly allocated Stack object, with specified starting
   size. */
Stack* stk_new(int nelements,	/* Starting number of elements */
	      int elementsz);	/* Size of each element (bytes) */


/* Create new stack of integers. 
   Like stk_new, but with element size fixed at sizeof(int).
   Returns newly allocated Stack object, with specified starting size. */
Stack* stk_new_int(int nelements); /* Starting number of elements */

/* Create new stack of doubles. 
   Like stk_new, but with element size fixed at sizeof(double).
   Returns newly allocated Stack object, with specified starting size. */
Stack* stk_new_dbl(int nelements); /* Starting number of elements */

/* Create new stack of pointers.
   Like stk_new, but with element size fixed at sizeof(void*).
   Returns newly allocated Stack object, with specified starting size. */
Stack* stk_new_ptr(int nelements);

/* Clear contents of stack.
   Contents will be cleared but memory will remain allocated. */   
void stk_clear(Stack *s);

/* Free memory associated with stack.
   Stack object itself will be freed also. */
void stk_free(Stack *s);

/***************************************************************************/
/* Inline functions; note: these are redefined in stacks.c for             */
/* situations in which inlining is not available                           */
/***************************************************************************/

extern inline
int stk_empty(Stack *s) { return lst_empty(s); }

extern inline
void* stk_pop(Stack* s) {
  if (stk_empty(s)) return NULL;
  return lst_arr_get(s, --s->ridx);
}

extern inline
void* stk_peek(Stack* s) {
  if (stk_empty(s)) return NULL;
  return lst_arr_get(s, s->ridx-1);
}

extern inline
int stk_pop_int(Stack *s) {
  int *ptr = (int*)stk_pop(s);
  return (ptr == NULL ? 0 : *ptr);
}

extern inline
double stk_pop_dbl(Stack *s) {
  double *ptr = (double*)stk_pop(s);
  return (ptr == NULL ? 0 : *ptr);
}

extern inline
void* stk_pop_ptr(Stack *s) {
  void **ptr = (void**)stk_pop(s);
  return (ptr == NULL ? NULL : *ptr);
}

extern inline
int stk_peek_int(Stack *s) {
  int *ptr = (int*)stk_peek(s);
  return (ptr == NULL ? 0 : *ptr);
}

extern inline
double stk_peek_dbl(Stack *s) {
  double *ptr = (double*)stk_peek(s);
  return (ptr == NULL ? 0 : *ptr);
}

extern inline
void* stk_peek_ptr(Stack *s) {
  void **ptr = (void**)stk_peek(s);
  return (ptr == NULL ? NULL : *ptr);
}

extern inline
void stk_push(Stack *s, void *o) { lst_push(s, o); }

extern inline
void stk_push_int(Stack *s, int i) { lst_push_int(s, i); }

extern inline
void stk_push_dbl(Stack *s, double d) { lst_push_dbl(s, d); }

extern inline
void stk_push_ptr(Stack *s, void *ptr) { lst_push_ptr(s, ptr); }

extern inline
int stk_size(Stack *s) { return lst_size(s); }

extern inline
Stack* stk_new(int nelements, /* Starting number of elements */
	      int elementsz)	    /* Size of each element (bytes) */
{ return lst_new(nelements, elementsz); }

extern inline
Stack* stk_new_int(int nelements) /* Starting number of elements */
{ return lst_new(nelements, sizeof(int)); }

extern inline
Stack* stk_new_dbl(int nelements) /* Starting number of elements */
{ return lst_new(nelements, sizeof(double)); }

extern inline
Stack* stk_new_ptr(int nelements) 
{ return lst_new(nelements, sizeof(void*)); }

extern inline
void stk_clear(Stack *s) { lst_clear(s); }

extern inline
void stk_free(Stack *s) { lst_free(s); }

#endif
