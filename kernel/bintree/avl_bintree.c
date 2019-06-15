/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/kernel/bintree.h>

#define MAX_TREE_HEIGHT       32
#define ALLOWED_IMBALANCE      1

#define STACK_PUSH(r)   (stack[stack_size++] = (r))
#define STACK_TOP()     (stack[stack_size-1])
#define STACK_POP()     (stack[--stack_size])


static ALWAYS_INLINE bintree_node *
obj_to_bintree_node(void *obj, ptrdiff_t offset)
{
   return obj ? (bintree_node *)((char*)obj + offset) : NULL;
}

static ALWAYS_INLINE void *
bintree_node_to_obj(bintree_node *node, ptrdiff_t offset)
{
   return node ? (void *)((char*)node - offset) : NULL;
}

#define OBJTN(o) (obj_to_bintree_node((o), bintree_offset))
#define NTOBJ(n) (bintree_node_to_obj((n), bintree_offset))

#define LEFT_OF(obj) ( OBJTN((obj))->left_obj )
#define RIGHT_OF(obj) ( OBJTN((obj))->right_obj )
#define HEIGHT(obj) ((obj) ? OBJTN((obj))->height : -1)

static inline void
update_height(bintree_node *node, ptrdiff_t bintree_offset)
{
   node->height = (u16)MAX(HEIGHT(node->left_obj), HEIGHT(node->right_obj)) + 1;
}

#define UPDATE_HEIGHT(n) update_height((n), bintree_offset)


/*
 * rotate the left child of *obj_ref clock-wise
 *
 *         (n)                  (nl)
 *         /  \                 /  \
 *       (nl) (nr)   ==>    (nll)  (n)
 *       /  \                     /   \
 *    (nll) (nlr)               (nlr) (nr)
 */

void rotate_left_child(void **obj_ref, ptrdiff_t bintree_offset)
{
   ASSERT(obj_ref != NULL);
   ASSERT(*obj_ref != NULL);

   bintree_node *orig_node = OBJTN(*obj_ref);
   ASSERT(orig_node->left_obj != NULL);

   bintree_node *orig_left_child = OBJTN(orig_node->left_obj);
   *obj_ref = orig_node->left_obj;
   orig_node->left_obj = orig_left_child->right_obj;
   OBJTN(*obj_ref)->right_obj = NTOBJ(orig_node);

   UPDATE_HEIGHT(orig_node);
   UPDATE_HEIGHT(orig_left_child);
}

/*
 * rotate the right child of *obj_ref counterclock-wise (symmetric function)
 */

void rotate_right_child(void **obj_ref, ptrdiff_t bintree_offset)
{
   ASSERT(obj_ref != NULL);
   ASSERT(*obj_ref != NULL);

   bintree_node *orig_node = OBJTN(*obj_ref);
   ASSERT(orig_node->right_obj != NULL);

   bintree_node *orig_right_child = OBJTN(orig_node->right_obj);
   *obj_ref = orig_node->right_obj;
   orig_node->right_obj = orig_right_child->left_obj;
   OBJTN(*obj_ref)->left_obj = NTOBJ(orig_node);

   UPDATE_HEIGHT(orig_node);
   UPDATE_HEIGHT(orig_right_child);
}

#define ROTATE_CW_LEFT_CHILD(obj) (rotate_left_child((obj), bintree_offset))
#define ROTATE_CCW_RIGHT_CHILD(obj) (rotate_right_child((obj), bintree_offset))
#define BALANCE(obj) (balance((obj), bintree_offset))

static void balance(void **obj_ref, ptrdiff_t bintree_offset)
{
   ASSERT(obj_ref != NULL);

   if (*obj_ref == NULL)
      return;

   void *left_obj = LEFT_OF(*obj_ref);
   void *right_obj = RIGHT_OF(*obj_ref);

   int bf = HEIGHT(left_obj) - HEIGHT(right_obj);

   if (bf > ALLOWED_IMBALANCE) {

      if (HEIGHT(LEFT_OF(left_obj)) >= HEIGHT(RIGHT_OF(left_obj))) {
         ROTATE_CW_LEFT_CHILD(obj_ref);
      } else {
         ROTATE_CCW_RIGHT_CHILD(&LEFT_OF(*obj_ref));
         ROTATE_CW_LEFT_CHILD(obj_ref);
      }

   } else if (bf < -ALLOWED_IMBALANCE) {

      if (HEIGHT(RIGHT_OF(right_obj)) >= HEIGHT(LEFT_OF(right_obj))) {
         ROTATE_CCW_RIGHT_CHILD(obj_ref);
      } else {
         ROTATE_CW_LEFT_CHILD(&RIGHT_OF(*obj_ref));
         ROTATE_CCW_RIGHT_CHILD(obj_ref);
      }
   }

   UPDATE_HEIGHT(OBJTN(*obj_ref));
}

static void
bintree_remove_internal_aux(void **root_obj_ref,
                            void ***stack,
                            int stack_size,
                            ptrdiff_t bintree_offset)
{
   if (LEFT_OF(*root_obj_ref) && RIGHT_OF(*root_obj_ref)) {

      // not-leaf node

      void **left = &LEFT_OF(*root_obj_ref);
      void **right = &RIGHT_OF(*root_obj_ref);
      void **successor_ref = &RIGHT_OF(*root_obj_ref);

      int saved_stack_size = stack_size;

      while (LEFT_OF(*successor_ref)) {
         STACK_PUSH(successor_ref);
         successor_ref = &LEFT_OF(*successor_ref);
      }

      STACK_PUSH(successor_ref);

      // now *successor_ref is the smallest node at the right side of
      // *root_obj_ref and so it is its successor.

      // save *successor's right node (it has no left node!).
      void *successors_right = RIGHT_OF(*successor_ref); // may be NULL.

      // replace *root_obj_ref (to be deleted) with *successor_ref
      *root_obj_ref = *successor_ref;

      // now we have to replace *obj with its right child
      *successor_ref = successors_right;

      // balance the part of the tree up to the original value of 'obj'
      while (stack_size > saved_stack_size) {
         BALANCE(STACK_POP());
      }

      // restore root's original left and right links
      OBJTN(*root_obj_ref)->left_obj = *left;
      OBJTN(*root_obj_ref)->right_obj = *right;

   } else {

      // leaf node: replace with its left/right child.

      *root_obj_ref = LEFT_OF(*root_obj_ref)
                        ? LEFT_OF(*root_obj_ref)
                        : RIGHT_OF(*root_obj_ref);
   }

   while (stack_size > 0)
      BALANCE(STACK_POP());
}


#include <tilck/common/norec.h>

int
bintree_in_order_visit_internal(void *obj,
                                bintree_visit_cb visit_cb,
                                void *visit_cb_arg,
                                ptrdiff_t bintree_offset)
{
   int r;

   if (!obj)
      return 0;

   CREATE_SHADOW_STACK(MAX_TREE_HEIGHT, 1);
   SIMULATE_CALL1(obj);

   while (stack_size) {

      obj = LOAD_ARG_FROM_STACK(1, void *);

      void *left_obj = LEFT_OF(obj);
      void *right_obj = RIGHT_OF(obj);

      HANDLE_SIMULATED_RETURN();

      if (left_obj)
         SIMULATE_CALL1(left_obj);

      if ((r = visit_cb(obj, visit_cb_arg)))
         return r;

      if (right_obj)
         SIMULATE_CALL1(right_obj);

      SIMULATE_RETURN_NULL();
      NOREC_LOOP_END();
   }

   return 0;
}

int
bintree_in_rorder_visit_internal(void *obj,
                                 bintree_visit_cb visit_cb,
                                 void *visit_cb_arg,
                                 ptrdiff_t bintree_offset)
{
   int r;

   if (!obj)
      return 0;

   CREATE_SHADOW_STACK(MAX_TREE_HEIGHT, 1);
   SIMULATE_CALL1(obj);

   while (stack_size) {

      obj = LOAD_ARG_FROM_STACK(1, void *);

      void *left_obj = LEFT_OF(obj);
      void *right_obj = RIGHT_OF(obj);

      HANDLE_SIMULATED_RETURN();

      if (right_obj)
         SIMULATE_CALL1(right_obj);

      if ((r = visit_cb(obj, visit_cb_arg)))
         return r;

      if (left_obj)
         SIMULATE_CALL1(left_obj);

      SIMULATE_RETURN_NULL();
      NOREC_LOOP_END();
   }

   return 0;
}

void *
bintree_get_first_obj_internal(void *root_obj, ptrdiff_t bintree_offset)
{
   if (!root_obj)
      return NULL;

   while (LEFT_OF(root_obj) != NULL)
      root_obj = LEFT_OF(root_obj);

   return root_obj;
}

void *
bintree_get_last_obj_internal(void *root_obj, ptrdiff_t bintree_offset)
{
   if (!root_obj)
      return NULL;

   while (RIGHT_OF(root_obj) != NULL)
      root_obj = RIGHT_OF(root_obj);

   return root_obj;
}

static ALWAYS_INLINE sptr
bintree_insrem_int_cmp(const void *a, const void *b, ptrdiff_t field_off)
{
   const char *f1 = (const char *)a + field_off;
   const char *f2 = (const char *)b + field_off;
   return *(sptr *)f1 - *(sptr *)f2;
}

static ALWAYS_INLINE sptr
bintree_find_int_cmp(const void *obj, const sptr *valptr, ptrdiff_t field_off)
{
   sptr obj_field_val = *(sptr *)((const char *)obj + field_off);
   return obj_field_val - *valptr;
}

#define BINTREE_INT_FUNCS 0
#include "avl_find.c.h"
#include "avl_insert.c.h"
#include "avl_remove.c.h"
#undef BINTREE_INT_FUNCS
#define BINTREE_INT_FUNCS 1
#include "avl_find.c.h"
#include "avl_insert.c.h"
#include "avl_remove.c.h"