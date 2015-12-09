/* Copyright (c) 2015 Michael R. Tirado -- The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 *
 * ----------------------------------------------------------------------------
 *	vllist.h
 * ----------------------------------------------------------------------------
 *
 * wraps a (void *) in a linked list node and uses various macros to iterate
 * and manipulate the list. note that vll_head is always a double pointer to
 * ensure that changes made will be present across function calls, and so that
 * we can set to NULL when list is empty.
 *
 *
 * 		!!	some things you should know	!!
 *
 * before adding, you must initialize list(head) pointer to NULL
 *
 * vllist_allocnode calls malloc, you should always check for NULL return.
 *
 *
 * vll_iter needs to be it's own separate variable, not a member of list(head).
 * in other words, don't pass something like head->tail as vll_iter, since
 * vll_iter is modified by these macros. instead, pass it using a temporary
 * variable unless you're a fan of undefined behavior.
 *
 * eject will always rewind iterator to iter->prev, so if you eject on a
 * reverse loop: (vllist_while_rev), you should not call vllist_prev!
 * because i don't want to add the overhead to track and correct this. and
 * not doing so keeps reverse traversal for searches branch free.
 *
 * TODO  cool features like swap, sort callback maybe? reverse? other ideas?
 *
 */


#ifndef VLLIST_H__
#define VLLIST_H__
#include <malloc.h>


/* vllist_node
 * wrap (void *) as a list node
 */
struct vllist_node
{
	/* only head node has valid tail pointer */
	struct  vllist_node *tail;
	struct  vllist_node *next;
	struct  vllist_node *prev;
	void   *v;
};

/*
 * vllist_allocnode()
 * allocate a new node
 */
#define vllist_allocnode() (malloc(sizeof(struct vllist_node)))


/*
 * vllist_print_node(vllist_node *)
 * optional (requires stdio.h)
 */
/*#define vllist_printnode(node)					\
	printf("[node %p] data(%p) prev(%p) next(%p) tail(%p)\n",	\
			node,						\
			node->v,					\
			(void *)node->prev,				\
			(void *)node->next,				\
			(void *)node->tail);				*/



/*
 * vllist_addhead(vllist_node **, vllist_node *, void *)
 * add node to beginning of list.
 */
#define vllist_addhead(head, node, data)				\
if (node) {								\
	node->prev = NULL;						\
	node->next = *head;						\
	node->v    = data;						\
	if (*head) {							\
		(*head)->prev = node;					\
		node->tail = (*head)->tail;				\
		(*head)->tail = NULL;					\
	}								\
	else {	/* empty list */					\
		node->tail = node;					\
	}								\
	*head = node;							\
}



/*
 * vllist_addtail(vllist_node **, vllist_node *, void *)
 * add node to end of list.
 */
#define vllist_addtail(head, node, data)				\
if (*head && node) {							\
	node->prev = (*head)->tail;					\
	node->next = NULL;						\
	node->v    = data;						\
	node->tail = NULL;						\
	(*head)->tail->next = node;					\
	(*head)->tail = node;						\
}									\
else if (node) { /* empty list */					\
	node->prev = NULL;						\
	node->next = NULL;						\
	node->v    = data;						\
	node->tail = node;						\
	*head = node;							\
}



/*
 * vllist_addafter(vllist_node **, vllist_node *, vllist_node *, void *)
 * add new node after iterators location.
 */
#define vllist_addafter(head, iter, node, data)				\
if (*head && node) {							\
	if (iter->next == NULL)	/* at tail */				\
		(*head)->tail = node;					\
	else								\
		iter->next->prev = node;				\
	node->next = iter->next;					\
	node->prev = iter;						\
	node->v = data;							\
	node->tail = NULL;						\
	iter->next = node;						\
}



/*
 * vllist_addbefore(vllist_node **, vllist_node *, vllist_node *, void *)
 * add new node before iterators location.
 */
#define vllist_addbefore(head, iter, node, data)			\
if (node) {								\
	node->next = iter;						\
	if (iter->prev) {						\
		node->tail = NULL;					\
		iter->prev->next = node;				\
	}								\
	else { /* at head */						\
		node->tail = iter->tail;				\
		iter->tail = NULL;					\
		*head = node;						\
	}								\
	node->prev = iter->prev;					\
	node->v = data;							\
	iter->prev = node;						\
}



/*
 * vllist_eject(vllist_node **, vllist_node *)
 * remove node, and rewind.
 * -!- iter will be set to NULL if head is ejected -!-
 *  this is corrected when next is called.
 */
#define vllist_eject(head, iter)					\
if (*head && iter) {							\
	struct vllist_node *vll_tmp = iter;				\
	int vll_count = 0;						\
	if (iter->next) {						\
		iter->next->prev = iter->prev;				\
		++vll_count;						\
	}								\
	if (iter->prev) {						\
		iter->prev->next = iter->next;				\
		++vll_count;						\
	}								\
	else if (iter->next) { /* ejecting head */			\
		iter->next->tail = iter->tail;				\
		*head = iter->next;					\
									\
	}								\
	/* free node */ 						\
	if (vll_count != 0)						\
	{	/* tail? */						\
		if (iter->next == NULL)					\
			(*head)->tail = iter->prev;			\
		iter = iter->prev;					\
		free(vll_tmp);						\
	}								\
	else { /* list is now empty */					\
		free(*head);						\
		*head = NULL;						\
		iter = NULL;						\
	}								\
}



/*
 * vllist_next(vllist_node **, vllist_node *)
 * since eject/prev rewind, next will reset to head if we had
 * just ejected or traversed past head.
 */
#define vllist_next(head, iter)						\
if (iter)								\
	iter = iter->next;						\
else									\
	iter = *head;



/*
 * vllist_prev(vllist_node *)
 * rewind one position
 */
#define vllist_prev(iter)						\
if (iter) {								\
	iter = iter->prev;						\
}



/*
 * vllist_while_fwd(vllist_node **, vllist_node *)
 * initializes iterator to head, and will loop until NULL
 */
#define vllist_while_fwd(head, iter)					\
iter = *head;								\
while (iter)



/*
 * vllist_while_rev(vllist_node **, vllist_node *)
 * initializes iterator to tail, and will loop until NULL
 * if you eject in this loop, be sure to NOT call vllist_prev,
 * as eject will rewind for us.
 */
#define vllist_while_rev(head, iter)					\
if (*head)								\
	iter = (*head)->tail;						\
else									\
	iter = NULL;							\
while (iter)



/*
 * vllist_destroy(vllist_node **)
 * destroy entire list,
 */
#define vllist_destroy(head)						\
while(*head)								\
{									\
	struct vllist_node *vll_tmp = *head;				\
	*head = vll_tmp->next;						\
	free(vll_tmp);							\
}									\



#if 0
/* example use */
int main()
{
	/* is set to 0 if emptied, and must always be initialized to null */
	struct vllist_node *list = NULL; /* the actual list */
	struct vllist_node *iter = NULL; /* traverses list  */
	struct vllist_node *node = NULL; /* newly allocated node after add  */
	void *item; /* node data */

	node = vllist_allocnode();
	if (node == NULL) {
		printf("errah\n");
		return -1;
	}

	vllist_addhead(&list, node, item);
	vllist_addtail(&list, node, item);
	vllist_addbefore(&list, iter, node, item);
	vllist_addafter(&list, iter, node, item);

	vllist_eject(&list, iter); /* rewinds iter, sets to NULL if at head */

	vllist_while_fwd(&list, iter)
	{
		/* do stuff here with iter->v */
		vllist_next(&list, iter);
	}

	vllist_while_rev(&list, iter)
	{
		if (did_not_eject)
			vllist_prev(iter);
	}

	vllist_destroy(&list);

	return 0;
}
#endif



#endif /* VLLIST_H__ */
