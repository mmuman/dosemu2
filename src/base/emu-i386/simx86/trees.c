/***************************************************************************
 *
 * All modifications in this file to the original code are
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 *
 *
 *  SIMX86 a Intel 80x86 cpu emulator
 *  Copyright (C) 1997,2001 Alberto Vignani, FIAT Research Center
 *				a.vignani@crf.it
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Additional copyright notes:
 *
 * 1. The kernel-level vm86 handling was taken out of the Linux kernel
 *  (linux/arch/i386/kernel/vm86.c). This code originally was written by
 *  Linus Torvalds with later enhancements by Lutz Molgedey and Hans Lermen.
 *
 * 2. The tree handling routines were adapted from libavl:
 *  libavl - manipulates AVL trees.
 *  Copyright (C) 1998, 1999 Free Software Foundation, Inc.
 *  The author may be contacted at <pfaffben@pilot.msu.edu> on the
 *  Internet, or as Ben Pfaff, 12167 Airport Rd, DeWitt MI 48820, USA
 *  through more mundane means.
 *
 ***************************************************************************/

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include "emu86.h"
#include "dlmalloc.h"
#include "codegen-arch.h"

IMeta	*InstrMeta;
int	CurrIMeta = -1;

/* Tree structure to store collected code sequences */
avltr_tree CollectTree;
avltr_traverser Traverser;
int ninodes = 0;

int NodesCleaned = 0;
int NodesParsed = 0;
int NodesExecd = 0;
int CleanFreq = 8;
int CreationIndex = 0;

#if PROFILE
int MaxDepth = 0;
int MaxNodes = 0;
int MaxNodeSize = 0;
int TotalNodesParsed = 0;
int TotalNodesExecd = 0;
int NodesFound = 0;
int NodesFastFound = 0;
int NodesNotFound = 0;
int TreeCleanups = 0;
#endif

#ifdef X86_JIT

#define FINDTREE_CACHE_HASH_MASK 0xfff
static TNode *findtree_cache[FINDTREE_CACHE_HASH_MASK+1];

TNode *TNodePool;
int NodeLimit = 10000;

#define RANGE_IN_RANGE(al,ah,l,h)	({int _l2=(al);\
	int _h2=(ah); ((_h2 >= (l)) && (_l2 < (h))); })
#define ADDR_IN_RANGE(a,l,h)		({typeof(a) _a2=(a);	\
	((_a2 >= (l)) && (_a2 < (h))); })

/////////////////////////////////////////////////////////////////////////////

#define NEXTNODE(g)	({__typeof__(g) _g = (g)->link[1]; \
			  if ((g)->rtag == PLUS) \
			    while (_g->link[0]!=NULL) _g=_g->link[0]; \
			  _g; })

static inline TNode *Tmalloc(void)
{
  TNode *G  = TNodePool->link[0];
  TNode *G1 = G->link[0];
  if (G1==TNodePool) leavedos_main(0x4c4c); // return NULL;
  TNodePool->link[0] = G1; G->link[0]=NULL;
  memset(G, 0, sizeof(TNode));	// "bug covering"
  return G;
}

static inline void Tfree(TNode *G)
{
  G->key = G->alive = 0;
  G->addr = NULL;
  G->link[0] = TNodePool->link[0];
  TNodePool->link[0] = G;
}

/////////////////////////////////////////////////////////////////////////////

static inline void datacopy(TNode *nd, TNode *ns)
{
  char *s = (char *)&(ns->key);
  char *d = (char *)&(nd->key);
  int l = sizeof(TNode)-offsetof(TNode,key);
  memcpy(d,s,l);
}

static TNode *avltr_probe (const int key, int *found)
{
  avltr_tree *tree = &CollectTree;
  TNode *t;
  TNode *s, *p, *q, *r;
  int k = 1;

  t = &tree->root;
  s = p = t->link[0];

  if (s == NULL) {
      tree->count++;
      ninodes = tree->count;
      q = t->link[0] = Tmalloc();
      q->link[0] = NULL;
      q->link[1] = t;
      q->rtag = MINUS;
      q->bal = 0;
      return q;
  }

  for (;;) {
      int diff = (key - p->key);

      if (diff < 0) {
	  p->cache = 0;
	  q = p->link[0];
	  if (q == NULL) {
	      q = Tmalloc();
	      p->link[0] = q;
	      q->link[0] = NULL;
	      q->link[1] = p;
	      q->rtag = MINUS;
	      break;
	  }
      }
      else if (diff > 0) {
	  p->cache = 1;
	  q = p->link[1];
	  if (p->rtag == MINUS) {
	      q = Tmalloc();
	      q->link[1] = p->link[1];
	      q->rtag = p->rtag;
	      p->link[1] = q;
	      p->rtag = PLUS;
	      q->link[0] = NULL;
	      break;
	  }
      }
      else {	/* found */
        *found = 1;
	return p;
      }

      if (q->bal != 0) t = p, s = q;
      p = q;
      k++;
/**/ if (k>=AVL_MAX_HEIGHT) leavedos_main(0x777);
#if PROFILE
      if (debug_level('e')) if (k>MaxDepth) MaxDepth=k;
#endif
  }

  tree->count++;
  ninodes = tree->count;
#if PROFILE
  if (debug_level('e')) if (ninodes > MaxNodes) MaxNodes = ninodes;
#endif
  q->bal = 0;

  r = p = s->link[(int) s->cache];
  while (p != q) {
      p->bal = p->cache * 2 - 1;
      p = p->link[(int) p->cache];
  }

  if (s->cache == 0) {
      if (s->bal == 0) {
	  s->bal = -1;
	  return q;
      }
      else if (s->bal == +1) {
	  s->bal = 0;
	  return q;
      }

      if (r->bal == -1)	{
	  p = r;
	  if (r->rtag == MINUS) {
	      s->link[0] = NULL;
	      r->link[1] = s;
	      r->rtag = PLUS;
	  }
	  else {
	      s->link[0] = r->link[1];
	      r->link[1] = s;
	  }
	  s->bal = r->bal = 0;
      }
      else {
	  p = r->link[1];
	  r->link[1] = p->link[0];
	  p->link[0] = r;
	  s->link[0] = p->link[1];
	  p->link[1] = s;
	  if (p->bal == -1) s->bal = 1, r->bal = 0;
	    else if (p->bal == 0) s->bal = r->bal = 0;
	      else s->bal = 0, r->bal = -1;
	  p->bal = 0;
	  p->rtag = PLUS;
	  if (s->link[0] == s) s->link[0] = NULL;
	  if (r->link[1] == NULL) {
	      r->link[1] = p;
	      r->rtag = MINUS;
	  }
      }
  }
  else {
      if (s->bal == 0) {
	  s->bal = 1;
	  return q;
      }
      else if (s->bal == -1) {
	  s->bal = 0;
	  return q;
      }

      if (r->bal == +1)	{
	  p = r;
	  if (r->link[0] == NULL) {
	      s->rtag = MINUS;
	      r->link[0] = s;
	  }
	  else {
	      s->link[1] = r->link[0];
	      s->rtag = PLUS;
	      r->link[0] = s;
	  }
	  s->bal = r->bal = 0;
      }
      else {
	  p = r->link[0];
	  r->link[0] = p->link[1];
	  p->link[1] = r;
	  s->link[1] = p->link[0];
	  p->link[0] = s;
	  if (p->bal == +1) s->bal = -1, r->bal = 0;
	    else if (p->bal == 0) s->bal = r->bal = 0;
	      else s->bal = 0, r->bal = 1;
	  p->rtag = PLUS;
	  if (s->link[1] == NULL) {
	      s->link[1] = p;
	      s->rtag = MINUS;
	  }
	  if (r->link[0] == r) r->link[0] = NULL;
	  p->bal = 0;
      }
  }

  if (t != &tree->root && s == t->link[1]) t->link[1] = p;
    else t->link[0] = p;

  return q;
}


void avltr_delete (const int key)
{
  avltr_tree *tree = &CollectTree;
  TNode *pa[AVL_MAX_HEIGHT];		/* Stack P: Nodes. */
  unsigned char a[AVL_MAX_HEIGHT];	/* Stack P: Bits. */
  int k = 1;				/* Stack P: Pointer. */
  TNode *p;

  a[0] = 0;
  pa[0] = &tree->root;
  p = tree->root.link[0];
  if (p == NULL) return;

  for (;;) {
      int diff = (key - p->key);

      if (diff==0) break;
      pa[k] = p;
      if (diff < 0) {
	  if (p->link[0] == NULL) return;
	  p = p->link[0]; a[k] = 0;
      }
      else if (diff > 0) {
	  if (p->rtag != PLUS) return;
	  p = p->link[1]; a[k] = 1;
      }
      k++;
/**/ if (k>=AVL_MAX_HEIGHT) leavedos_main(0x777);
  }
#if !defined(SINGLESTEP)&&!defined(SINGLEBLOCK)
  if (debug_level('e')>2)
	e_printf("Found node to delete at %p(%08x)\n",p,p->key);
#endif
  tree->count--;
  ninodes = tree->count;

  {
    TNode *t = p;
    TNode **q = &pa[k - 1]->link[(int) a[k - 1]];

    if (t->rtag == MINUS) {
	if (t->link[0] != NULL) {
	    TNode *const x = t->link[0];

	    *q = x;
	    (*q)->bal = 0;
	    if (x->rtag == MINUS) {
		if (a[k - 1] == 1) x->link[1] = t->link[1];
		  else x->link[1] = pa[k - 1];
	    }
	}
	else {
	    *q = t->link[a[k - 1]];
	    if (a[k - 1] == 0) pa[k - 1]->link[0] = NULL;
	      else pa[k - 1]->rtag = MINUS;
	}
    }
    else {
	TNode *r = t->link[1];
	if (r->link[0] == NULL) {
	    r->link[0] = t->link[0];
	    r->bal = t->bal;
	    if (r->link[0] != NULL) {
		TNode *s = r->link[0];
		while (s->rtag == PLUS) s = s->link[1];
		s->link[1] = r;
	    }
	    *q = r;
	    a[k] = 1;
	    pa[k++] = r;
	}
	else {
	    TNode *s = r->link[0];

	    a[k] = 1;
	    pa[k++] = t;

	    a[k] = 0;
	    pa[k++] = r;

	    while (s->link[0] != NULL) {
		r = s;
		s = r->link[0];
		a[k] = 0;
		pa[k++] = r;
	    }

	    if (t->mblock) dlfree(t->mblock);
/* e_printf("<03 node exchange %p->%p>\n",s,t); */
	    datacopy(t, s);
/**/	    if (t->addr==NULL) leavedos_main(0x8130);
	    /* keep the node reference to itself */
	    t->mblock->bkptr = t;
	    s->addr = NULL;
	    s->mblock = NULL;
	    memset(&s->clink, 0, sizeof(linkdesc));
	    s->key = 0;

	    if (s->rtag == PLUS) r->link[0] = s->link[1];
	      else r->link[0] = NULL;
	    p = s;
	}
    }
  }

/**/ if (Traverser.p==p) Traverser.init=0;
#if !defined(SINGLESTEP)&&!defined(SINGLEBLOCK)
  if (debug_level('e')>2) e_printf("Remove node %p\n",p);
#endif
#ifdef DEBUG_LINKER
	if (p->clink.nrefs) {
	    dbug_printf("Cannot delete - nrefs=%d\n",p->clink.nrefs);
	    leavedos_main(0x9140);
	}
	if (p->clink.bkr.next) {
	    dbug_printf("Cannot delete - bkr busy\n");
	    leavedos_main(0x9141);
	}
	if (p->clink.t_ref || p->clink.nt_ref) {
	    dbug_printf("Cannot delete - ref busy\n");
	    leavedos_main(0x9142);
	}
#endif
  if (p->mblock) dlfree(p->mblock);
  Tfree(p);

  while (--k) {
      TNode *const s = pa[k];

      if (a[k] == 0) {
	  TNode *const r = s->link[1];

	  if (s->bal == -1) {
	      s->bal = 0;
	      continue;
	  }
	  else if (s->bal == 0) {
	      s->bal = +1;
	      break;
	  }

	  if (s->rtag == MINUS || r->bal == 0) {
	      s->link[1] = r->link[0];
	      r->link[0] = s;
	      r->bal = -1;
	      pa[k - 1]->link[(int) a[k - 1]] = r;
	      break;
	  }
	  else if (r->bal == +1) {
	      if (r->link[0] != NULL) {
		  s->rtag = PLUS;
		  s->link[1] = r->link[0];
	      }
	      else
		s->rtag = MINUS;
	      r->link[0] = s;
	      s->bal = r->bal = 0;
	      pa[k - 1]->link[a[k - 1]] = r;
	  }
	  else {
	      p = r->link[0];
	      if (p->rtag == PLUS) r->link[0] = p->link[1];
	        else r->link[0] = NULL;
	      p->link[1] = r;
	      p->rtag = PLUS;
	      if (p->link[0] == NULL) {
		  s->link[1] = p;
		  s->rtag = MINUS;
	      }
	      else {
		  s->link[1] = p->link[0];
		  s->rtag = PLUS;
	      }
	      p->link[0] = s;
	      if (p->bal == +1)	s->bal = -1, r->bal = 0;
	        else if (p->bal == 0) s->bal = r->bal = 0;
		  else s->bal = 0, r->bal = +1;
	      p->bal = 0;
	      pa[k - 1]->link[(int) a[k - 1]] = p;
	      if (a[k - 1] == 1) pa[k - 1]->rtag = PLUS;
	  }
      }
      else {
	  TNode *const r = s->link[0];

	  if (s->bal == +1) {
	      s->bal = 0;
	      continue;
	  }
	  else if (s->bal == 0) {
	      s->bal = -1;
	      break;
	  }

	  if (s->link[0] == NULL || r->bal == 0) {
	      s->link[0] = r->link[1];
	      r->link[1] = s;
	      r->bal = +1;
	      pa[k - 1]->link[(int) a[k - 1]] = r;
	      break;
	  }
	  else if (r->bal == -1) {
	      if (r->rtag == PLUS) s->link[0] = r->link[1];
	        else s->link[0] = NULL;
	      r->link[1] = s;
	      r->rtag = PLUS;
	      s->bal = r->bal = 0;
	      pa[k - 1]->link[a[k - 1]] = r;
	  }
	  else {
	      p = r->link[1];
	      if (p->link[0] != NULL) {
		  r->rtag = PLUS;
		  r->link[1] = p->link[0];
	      }
	      else
		r->rtag = MINUS;
	      p->link[0] = r;
	      if (p->rtag == MINUS) s->link[0] = NULL;
	        else s->link[0] = p->link[1];
	      p->link[1] = s;
	      p->rtag = PLUS;
	      if (p->bal == -1)	s->bal = +1, r->bal = 0;
	        else if (p->bal == 0) s->bal = r->bal = 0;
		  else s->bal = 0, r->bal = -1;
	      p->bal = 0;
	      if (a[k - 1] == 1)
		pa[k - 1]->rtag = PLUS;
	      pa[k - 1]->link[(int) a[k - 1]] = p;
	  }
      }
  }
}

#endif	// X86_JIT

/////////////////////////////////////////////////////////////////////////////

static void avltr_init(void)
{
#ifdef X86_JIT
 if (!config.cpusim) {
  int i;
  TNode *G;

  CollectTree.root.link[0] = NULL;
  CollectTree.root.link[1] = &CollectTree.root;
  CollectTree.root.rtag = PLUS;
  CollectTree.count = 0;
  Traverser.init = 0;
  Traverser.p = NULL;

  G = TNodePool;
  for (i=0; i<(NODES_IN_POOL-1); i++) {
	TNode *G1 = G; G++;
	G1->link[0] = G;
  }
  G->link[0] = TNodePool;

  InstrMeta = malloc(sizeof(IMeta) * MAXINODES);
  memset(InstrMeta, 0, sizeof(IMeta));
 }
#endif
  g_printf("avltr_init\n");
  CurrIMeta = -1;
  NodesCleaned = 0;
  ninodes = 0;
}


#ifdef X86_JIT

void avltr_destroy(void)
{
  avltr_tree *tree;
#if PROFILE
  hitimer_t t0 = 0;
#endif

  tree = &CollectTree;
  e_printf("--------------------------------------------------------------\n");
  e_printf("Destroy AVLtree with %d nodes\n",ninodes);
  e_printf("--------------------------------------------------------------\n");
#ifdef DEBUG_TREE
  DumpTree (tLog);
#endif
#if PROFILE
  if (debug_level('e')) t0 = GETTSC();
#endif

  mprot_end();
  if (tree->root.link[0] != &tree->root) {
      TNode *an[AVL_MAX_HEIGHT];	/* Stack A: nodes. */
      char ab[AVL_MAX_HEIGHT];		/* Stack A: bits. */
      int ap = 0;			/* Stack A: height. */
      TNode *p = tree->root.link[0];

      for (;;) {
	  while (p != NULL) {
	      ab[ap] = 0;
	      an[ap++] = p;
	      p = p->link[0];
	  }

	  for (;;) {
	      backref *B;
	      if (ap == 0) goto quit;

	      p = an[--ap];
	      if (ab[ap] == 0) {
		  ab[ap++] = 1;
		  if (p->rtag == MINUS) continue;
		  p = p->link[1];
		  break;
	      }
	      B = p->clink.bkr.next;
	      while (B) {
		  backref *B2 = B;
		  B = B->next;
		  free(B2);
	      }
	      if (p->mblock) dlfree(p->mblock);
	  }
      }
  }
quit:
  free(InstrMeta);
#if PROFILE
  if (debug_level('e')) {
    TreeCleanups++;
    CleanupTime += (GETTSC() - t0);
  }
#endif
}


/////////////////////////////////////////////////////////////////////////////

/*
 * Given addr with translated code (from e.g., a fault) find the
 * corresponding original PC. This is slow but it's only used for
 * DPMI exceptions.
 */
unsigned int FindPC(unsigned char *addr)
{
  TNode *G = &CollectTree.root;
  unsigned char *ahE;
  Addr2Pc *AP;
  unsigned int i;

  for (;;) {
      /* walk to next node */
      G = NEXTNODE(G);
      if (G == &CollectTree.root) break;
      if (!G->addr || !G->pmeta || G->alive<=0) continue;
      ahE = G->addr + G->len;
      if (!ADDR_IN_RANGE(addr,G->addr,ahE)) continue;
      e_printf("### FindPC: Found node %p->%p..%p", addr,G->addr,ahE);
      AP = G->pmeta;
      for (i=0; i<G->seqnum; i++) {
	  e_printf("     %08x:%p",(G->key+AP->dnpc),G->addr+AP->daddr);
	  if (addr < G->addr+AP->daddr) break;
	  AP++;
      }
      e_printf("\nFindPC: PC=%x\n", G->key+(AP-1)->dnpc);
      return G->key+(AP-1)->dnpc;
  }
  return 0;
}

/////////////////////////////////////////////////////////////////////////////

#ifdef DEBUG_LINKER

static void CheckLinks(void)
{
  TNode *G = &CollectTree.root;
  TNode *GL;
  unsigned char *p;
  linkdesc *L, *T;
  backref *B;
  int n, brt;

  for (;;) {
    /* walk to next node */
    G = NEXTNODE(G);
    if (G == &CollectTree.root) {
	e_printf("DEBUG: node link check ok\n");
	return;
    }
    if (G->key<=0) {
	error("Invalid key %08x\n",G->key);
	goto nquit;
    }
    if (G->alive <= 0) {
	e_printf("Node %p invalidated\n",G);
	continue;
    }
    if (debug_level('e')>5) e_printf("Node %p at %08x selfr=%p\n",G,G->key,
    	G->mblock->bkptr);
    if (G->mblock->bkptr != G) {
	error("bad selfref\n"); goto nquit;
    }
    L = &G->clink;
    if (L->t_type >= JMP_LINK) {
	if (L->t_ref) {
	    GL = *L->t_ref;
	    if (debug_level('e')>5)
		e_printf("  T: ref=%p link=%p\n",
		    GL,L->t_link.abs);
	    p = ((unsigned char *)L->t_link.abs) - 1;
	    if ((*p!=0xe9)&&(*p!=0xeb)) {
		error("bad t_link jmp\n"); goto nquit;
	    }
	    if (debug_level('e')>5)
		e_printf("  T: links to %p at %08x with jmp %08x\n",GL,GL->key,
		*L->t_link.abs);
	    T = &GL->clink;
	    B = T->bkr.next;
	    if ((B==NULL) || (T->nrefs < 1)) {
		error("bad backref B=%p n=%d\n",B,T->nrefs);
		goto nquit;
	    }
	    n = 0;
	    brt = 0;
	    while (B) {
		if (B->ref==&G->mblock->bkptr) {
		    n++;
		    brt += B->branch;
		    if (debug_level('e')>5) e_printf("  T: backref %d from %p\n",n,GL);
		}
		B = B->next;
	    }
	    if (n < 1 || n > 2 || (n == 2 && brt != 'N' + 'T')) {
		error("0 or >1 backrefs1 (%i)\n", n); goto nquit;
	    }
	}
	else {
	    p = ((unsigned char *)L->t_link.abs) - 1;
	    if (*p!=0xb8) {
		error("bad t_link jmp\n"); goto nquit;
	    }
	}
	if (L->nt_ref) {
	    GL = *L->nt_ref;
	    if (debug_level('e')>5)
		e_printf("  N: ref=%p link=%p\n",
		    GL,L->nt_link.abs);
	    p = ((unsigned char *)L->nt_link.abs) - 1;
	    if ((*p!=0xe9)&&(*p!=0xeb)) {
		error("bad nt_link jmp\n"); goto nquit;
	    }
	    if (debug_level('e')>5)
		e_printf("  N: links to %p at %08x with jmp %08x\n",GL,GL->key,
		*L->nt_link.abs);
	    T = &GL->clink;
	    B = T->bkr.next;
	    if ((B==NULL) || (T->nrefs < 1)) {
		error("bad backref B=%p n=%d\n",B,T->nrefs);
		goto nquit;
	    }
	    n = 0;
	    brt = 0;
	    while (B) {
		if (B->ref==&G->mblock->bkptr) {
		    n++;
		    brt += B->branch;
		    if (debug_level('e')>5) e_printf("  N: backref %d from %p\n",n,GL);
		}
		B = B->next;
	    }
	    if (n < 1 || n > 2 || (n == 2 && brt != 'N' + 'T')) {
		error("0 or >1 backrefs2 (%i)\n", n); goto nquit;
	    }
	}
	else if (L->nt_link.abs) {
	    p = ((unsigned char *)L->nt_link.abs) - 1;
	    if (*p!=0xb8) {
		error("bad nt_link jmp\n"); goto nquit;
	    }
	}
    }
  }
nquit:
  leavedos_main(0x9143);
}

#endif // DEBUG_LINKER

#ifdef DEBUG_TREE

void DumpTree (FILE *fd)
{
  TNode *G = &CollectTree.root;
  linkdesc *L;
  backref *B;
  int nn;

  if (fd==NULL) return;
  fprintf(fd,"\n== BOT ========= %6d nodes =============================\n",ninodes);
  nn = 0;

  while (nn < 10000) {		// sorry,only 4 digits available
    /* walk to next node */
    G = NEXTNODE(G);
    if (G == &CollectTree.root) {
	fprintf(fd,"\n== EOT ====================================================\n");
	fflush(fd);
	return;
    }
    fprintf(fd,"\n-----------------------------------------------------------\n");
    if (G->alive <= 0) {
	fprintf(fd,"%04d Node %p invalidated\n",nn,G);
	nn++;
	continue;
    }
    fprintf(fd,"%04d Node %p at %08x..%08x mblock=%p flags=%#x\n",
	nn,G,G->key,(G->seqbase+G->seqlen-1),G->mblock,G->flags);
    fprintf(fd,"     AVL (%p:%p),%d,%d,%d,%d\n",G->link[0],G->link[1],
		G->bal,G->cache,G->pad,G->rtag);
    fprintf(fd,"     source:     instr=%d, len=%#x\n",G->seqnum,G->seqlen);
    fprintf(fd,"     translated: len=%#x\n",G->len);
    L = &G->clink;
    fprintf(fd,"     LINK type=%d refs=%d\n",L->t_type,L->nrefs);
    if (L->t_type >= JMP_LINK) {
	fprintf(fd,"         T ref=%p patch=%08x at %p\n",L->t_ref,
		L->t_undo,L->t_link.abs);
	if (L->t_type>JMP_LINK) {
	    fprintf(fd,"         N ref=%p patch=%08x at %p\n",L->nt_ref,
		L->nt_undo,L->nt_link.abs);
	}
    }
    if (L->nrefs) {
	B = L->bkr.next;
	while (B) {
	    fprintf(fd,"         bkref %c -> %p\n",B->branch,B->ref);
	    B = B->next;
	}
    }
    if (G->addr && G->pmeta) {
	int i, j, k;
	unsigned char *p = G->addr;
	Addr2Pc *AP = G->pmeta;
	for (i=0; i<G->seqnum; i++) {
	    fprintf(fd,"     %08x:%p",(G->key+AP->dnpc),G->addr+AP->daddr);
	    k = 0;
	    for (j=0; j<(AP[1].daddr-AP->daddr); j++) {
		fprintf(fd," %02x",*p++); k++;
		if (k>=16) {
		    fprintf(fd,"\n                      "); k=0;
		}
	    }
	    fprintf(fd,"\n");
	    AP++;
	}
	fprintf(fd,"             :%p",G->addr+AP->daddr);
	k = 0;
	for (j=AP->daddr; j<G->len; j++) {
	    fprintf(fd," %02x",*p++); k++;
	    if (k>=16) {
		fprintf(fd,"\n                      "); k=0;
	    }
	}
	fprintf(fd,"\n");
    }
    fflush(fd);
    nn++;
  }
}

#endif // DEBUG_TREE

static int TraverseAndClean(void)
{
  int cnt = 0;
  TNode *G;
#if PROFILE
  hitimer_t t0 = 0;

  if (debug_level('e')) t0 = GETTSC();
#endif

  if (Traverser.init == 0) {
      Traverser.p = G = &CollectTree.root;
      Traverser.init = 1;
  }
  else
      G = Traverser.p;

  /* walk to next node */
  G = NEXTNODE(G);
  if (G == &CollectTree.root) {
      G = NEXTNODE(G);
      if (G == &CollectTree.root)
          return 0;
  }

  if ((G->addr != NULL) && (G->alive>0)) {
      G->alive -= AGENODE;
      if (G->alive <= 0) {
	if (debug_level('e')>2) e_printf("TraverseAndClean: node at %08x decayed\n",G->key);
	e_unmarkpage(G->seqbase, G->seqlen);
	NodeUnlinker(G);
      }
  }
  if ((G->addr == NULL) || (G->alive<=0)) {
      if (debug_level('e')>2) e_printf("Delete node %08x\n",G->key);
      avltr_delete(G->key);
      cnt++;
  }
  else {
      if (debug_level('e')>3)
	e_printf("TraverseAndClean: node at %08x of %d life=%d\n",
		G->key,ninodes,G->alive);
      Traverser.p = G;
  }
#if PROFILE
  if (debug_level('e')) CleanupTime += (GETTSC() - t0);
#endif
  return cnt;
}

/*
 * Add a node to the collector tree.
 * The code is linearly stored in the CodeBuf and its associated structures
 * are in the InstrMeta array. We allocate a buffer and copy the code, then
 * we copy the sequence data from the head element of InstrMeta. In this
 * process we lose all the correspondences between original code and compiled
 * code addresses. At the end, we reset both CodeBuf and InstrMeta to prepare
 * for a new sequence.
 */
TNode *Move2Tree(IMeta *I0, CodeBuf *GenCodeBuf)
{
  TNode *nG = NULL;
#if PROFILE
  hitimer_t t0 = 0;
  if (debug_level('e')) t0 = GETTSC();
#endif
  int key;
  int len, found, nap;
  IMeta *I;
  int i, apl=0;
  Addr2Pc *ap;
  CodeBuf *mallmb;
  void **cp;

  /* try to keep a limit to the number of nodes in the tree. 3000-4000
   * nodes are probably enough before performance starts to suffer */
  if (ninodes > NodeLimit) {
	for (i=0; i<CreationIndex; i++) TraverseAndClean();
  }

  key = I0->npc;

  found = 0;
  nG = avltr_probe(key, &found);
/**/ if (nG==NULL) leavedos_main(0x8201);

  if (found) {
	if (debug_level('e')>2) {
		e_printf("Equal keys: replace node %p at %08x\n",
			nG,key);
	}
	/* ->REPLACE the code of the node found with the latest
	   compiled version */
	NodeUnlinker(nG);
	if (nG->mblock) dlfree(nG->mblock);
  }
  else {
#if !defined(SINGLESTEP)&&!defined(SINGLEBLOCK)
	if (debug_level('e')>2) {
		e_printf("New TNode %d at=%p key=%08x\n",
			ninodes,nG,key);
		if (debug_level('e')>3)
			e_printf("Header: len=%d n_ops=%d PC=%08x\n",
				I0->totlen, I0->ncount, I0->npc);
	}
#endif
	nG->key = key;
  }

  /* transfer info from first node of the Meta list to our new node */
  nG->seqbase = I0->seqbase;
  nG->seqlen = I0->seqlen;
  nG->seqnum = I0->ncount;
#if PROFILE
  if (debug_level('e')) if (nG->len > MaxNodeSize) MaxNodeSize = nG->len;
#endif
  nG->len = len = I0->totlen;
  nG->flags = I0->flags;
  nG->alive = NODELIFE(nG);
  findtree_cache[key&FINDTREE_CACHE_HASH_MASK] = nG;

  /* allocate the extra memory used by the node. This includes the
   * translated code plus the table of correspondences between source
   * and translated addresses.
   * The first longword of the memory block is special; it stores a
   * back-pointer to the node. This because nodes can be moved in
   * memory when rebalancing the AVL tree, while we need absolute and
   * constant memory references.
   * The second longword is equal to its own address. Guess why.
   * After that come the offset table, then the code.
   */
  nap = nG->seqnum+1;
  mallmb = GenCodeBuf;
  nG->mblock = GenCodeBuf;
  nG->mblock->bkptr = nG;
  cp = &nG->mblock->selfptr;
  *cp = cp;
  nG->pmeta = mallmb->meta;
  if (nG->pmeta==NULL) leavedos_main(0x504d45);
  nG->addr = (unsigned char *)&mallmb->meta[nap];

  /* setup structures for inter-node linking */
  nG->clink.t_type  = I0->clink.t_type;
  nG->clink.unlinked_jmp_targets = 0;
  if (I0->clink.t_type >= JMP_LINK) {
    nG->clink.t_link.abs  = (unsigned int *)(nG->addr + I0->clink.t_link.rel);
    nG->clink.t_target = *nG->clink.t_link.abs;
    nG->clink.unlinked_jmp_targets |= TARGET_T;
  }
  else
    nG->clink.t_link.abs  = I0->clink.t_link.abs;
  if (I0->clink.t_type > JMP_LINK) {
    nG->clink.nt_link.abs = (unsigned int *)(nG->addr + I0->clink.nt_link.rel);
    nG->clink.nt_target = *nG->clink.nt_link.abs;
    nG->clink.unlinked_jmp_targets |= TARGET_NT;
  }
  else
    nG->clink.nt_link.abs = I0->clink.nt_link.abs;
  if ((debug_level('e')>3) && nG->clink.t_type)
	dbug_printf("Link %d: %p:%08x\n",nG->clink.t_type,
		nG->clink.nt_link.abs,
		(nG->clink.t_type>JMP_LINK? *nG->clink.nt_link.abs:0));

  /* setup source/xlated instruction offsets */
  ap = nG->pmeta;
  I = I0;
  for (i=0; i<nG->seqnum; i++) {
	ap->daddr = I->daddr;
	apl = ap->daddr + I->len;
	ap->dnpc  = I->npc - I0->npc;
	if (debug_level('e')>8)
	    e_printf("Pmeta %03d: %p(%04x):%08x(%04x)\n",i,
		nG->addr+ap->daddr,ap->daddr,I->npc,ap->dnpc);
	ap++, I++;
  }
  ap->daddr = apl;
  if (debug_level('e')>8) e_printf("Pmeta %03d:         (%04x)\n",i,apl);

#ifdef DEBUG_LINKER
  CheckLinks();
#endif
  CurrIMeta = -1;
  memset(&InstrMeta[0],0,sizeof(IMeta));
#if PROFILE
  if (debug_level('e')) AddTime += (GETTSC() - t0);
#endif
  return nG;
}


TNode *FindTree(int key)
{
  TNode *I;
  static int tccount=0;
#if PROFILE
  hitimer_t t0 = 0;
#endif

  if (TheCPU.sigprof_pending) {
	CollectStat();
	TheCPU.sigprof_pending = 0;
  }

  /* fast path: using cache indexed by low 12 bits of PC:
     ~99.99% success rate */
  I = findtree_cache[key&FINDTREE_CACHE_HASH_MASK];
  if (I && (I->alive>0) && (I->key==key)) {
	if (debug_level('e')) {
	    if (debug_level('e')>4)
		e_printf("Found key %08x via cache\n", key);
#if PROFILE
	    NodesFastFound++;
#endif
	}
	I->alive = NODELIFE(I);
	return I;
  }
  if (!e_querymark(key, 1))
	return NULL;

#if PROFILE
  if (debug_level('e')) t0 = GETTSC();
#endif
  I = CollectTree.root.link[0];
  if (I == NULL) return NULL;	/* always NULL the first time! */

  for (;;) {
      int diff = (key - I->key);

      if (diff < 0) {
	  I = I->link[0];
	  if (I == NULL) goto endsrch;
      }
      else if (diff > 0) {
	  if (I->rtag == MINUS) goto endsrch;
	  I = I->link[1];
      }
      else break;
  }

  if (I && I->addr && (I->alive>0)) {
	if (debug_level('e')>3) e_printf("Found key %08x\n",key);
	I->alive = NODELIFE(I);
	findtree_cache[key&FINDTREE_CACHE_HASH_MASK] = I;
#if PROFILE
	if (debug_level('e')) {
	    NodesFound++;
	    SearchTime += (GETTSC() - t0);
	}
#endif
	return I;
  }

endsrch:
#if PROFILE
  if (debug_level('e')) SearchTime += (GETTSC() - t0);
#endif
  if ((ninodes>500) && (((++tccount) >= CleanFreq) || NodesCleaned)) {
	while (NodesCleaned > 0) {
	    (void)TraverseAndClean();
	    if (NodesCleaned) NodesCleaned--;
	}
	tccount=0;
  }

  if (debug_level('e')) {
    if (debug_level('e')>4) e_printf("Not found key %08x\n",key);
#if PROFILE
    NodesNotFound++;
#endif
  }
  return NULL;
}


/////////////////////////////////////////////////////////////////////////////
/*
 * We come here:
 *   a) from a fault on a protected memory page. A page is protected
 *	when code has been found on it. In this case, len is zero.
 *   b) from a disk read, no matter if the pages were protected or not.
 *	When we read something from disk we must mark as dirty anything
 *	present on the memory we are going to overwrite. In this case,
 *	len is greater than 0.
 * Too bad the smallest memory unit is a 4k page; DOS programs used to
 * be quite small. It is not unusual to find code and data/stack on the
 * same 4k page.
 *
 */

static void BreakNode(TNode *G, unsigned char *eip)
{
  Addr2Pc *A = G->pmeta;
  int ebase;
  unsigned char *p;
  int i;

  if (eip==0) {
	dbug_printf("Cannot break node %08x, eip=%p\n",G->key,eip);
	leavedos_main(0x7691);
  }

  ebase = eip - G->addr;
  for (i=0; i<G->seqnum; i++) {
    if (A->daddr >= ebase) {		// found following instr
	p = G->addr + A->daddr;		// translated IP of following instr
	memcpy(p, TailCode, TAILSIZE);
	*((int *)(p+TAILFIX)) = G->key + A->dnpc;
	if (debug_level('e')>1)
		e_printf("============ Force node closing at %08x(%p)\n",
			 (G->key+A->dnpc),p);
	return;
    }
    A++;
  }
  e_printf("============ Node %08x break failed\n",G->key);
}

static TNode *DoDelNode(TNode *G)
{
  if (Traverser.p == G)
    Traverser.p = NEXTNODE(G);
  avltr_delete(G->key);
  return CollectTree.root.link[0];
}

int InvalidateNodeRange(int al, int len, unsigned char *eip)
{
  TNode *G;
  int ah;
  int cleaned = 0;
#if PROFILE
  hitimer_t t0 = 0;

  if (debug_level('e')) t0 = GETTSC();
#endif
  ah = al + len;
  if (debug_level('e')>1) dbug_printf("Invalidate area %08x..%08x\n",al,ah);

  G = CollectTree.root.link[0];
  if (G == NULL) goto quit;
  /* find nearest (lesser than) node */
  for (;;) {
      if (G == NULL) goto quit;
      if (G->key > al) {
	/* no need to check for dead node here as the left-most
	 * node will not be overlapped by anything from left */
	if (G->link[0]==NULL) break;
	G = G->link[0];
      }
      else if (G->key < al) {
        TNode *G2;
	G2 = G->link[1];
	if (G2 == &CollectTree.root || G2->key > al) {
	  if (G->alive <= 0) {
	    /* remove dead node as it may be overlapped by good one */
	    G = DoDelNode(G);
	    continue;
	  }
	  break;
	} else G = G2;
      }
      else {
	if (G->alive <= 0) {
	  G = DoDelNode(G);
	  continue;
	}
	break;
      }
  }
  if (debug_level('e')>1) e_printf("Invalidate from node %08x on\n",G->key);

  /* walk tree in ascending, hopefully sorted, address order */
  for (;;) {
      if (G == &CollectTree.root || G->key >= ah)
        break;
      if (G->addr && (G->alive>0)) {
	int ahG = G->seqbase + G->seqlen;
	if (RANGE_IN_RANGE(G->seqbase,ahG,al,ah)) {
	    unsigned char *ahE = G->addr + G->len;
	    if (debug_level('e')>1)
		dbug_printf("Invalidated node %p at %08x\n",G,G->key);
	    G->alive = 0;
	    e_unmarkpage(G->seqbase, G->seqlen);
	    NodeUnlinker(G);
	    cleaned++;
	    NodesCleaned++;
	    /* if the current eip is in *any* chunk of code that is deleted
	        (not just the one written to)
	       then we need to break the node immediately to go back to
	       the interpreter; otherwise the remaining chunk (that does
	       not officially exist anymore) that the SIGSEGV or patched
	       call returns to may write to the current unprotected page.
	    */
	    if (eip && ADDR_IN_RANGE(eip,G->addr,ahE)) {
		if (debug_level('e')>1)
		    e_printf("### Node self hit %p->%p..%p\n",
			     eip,G->addr,ahE);
		BreakNode(G, eip);
	    }
	}
      }
      G = NEXTNODE(G);
  }
quit:
  if (debug_level('e') && e_querymark(al, len))
    error("simx86: InvalidateNodeRange did not clear all code for %#08x, len=%x\n",
	  al, len);
#if PROFILE
  if (debug_level('e')) CleanupTime += (GETTSC() - t0);
#endif
  return cleaned;
}


#endif // X86_JIT


/////////////////////////////////////////////////////////////////////////////
static void do_invalidate(unsigned data, int cnt)
{
	cnt = PAGE_ALIGN(data + cnt) - (data & _PAGE_MASK);
	data &= _PAGE_MASK;
#ifdef X86_JIT
	/* e_querymprotrange prevents coming here for sim */
	assert (!config.cpusim);
	InvalidateNodeRange(data, cnt, 0);
#endif
}

void e_invalidate(unsigned data, int cnt)
{
	if (!IS_EMU_JIT())
		return;
	/* nothing to invalidate if there are no page protections */
	if (!e_querymprotrange(data, cnt))
		return;
#ifdef X86_JIT
	/* e_querymprotrange prevents coming here for sim */
	assert(!config.cpusim);
	if (!e_querymark(data, cnt))
		return;
	// no need to invalidate the whole page here,
	// as the page does not need to be unprotected
	InvalidateNodeRange(data, cnt, 0);
#endif
}

void e_invalidate_pa(unsigned pa, int cnt)
{
    dosaddr_t addr = physaddr_to_dosaddr(pa, cnt);
    if (addr == (dosaddr_t)-1)
	return;
    e_invalidate(addr, cnt);
}

void e_invalidate_full_pa(unsigned pa, int cnt)
{
    dosaddr_t addr = physaddr_to_dosaddr(pa, cnt);
    if (addr == (dosaddr_t)-1)
	return;
    e_invalidate_full(addr, cnt);
}

/* invalidate and unprotect even if we hit only data.
 * Needed if we are about to destroy the page protection by other means.
 * Otherwise use e_invalidate() */
void e_invalidate_full(unsigned data, int cnt)
{
	if (!IS_EMU_JIT())
		return;
	/* nothing to invalidate if there are no page protections */
	if (!e_querymprotrange(data, cnt))
		return;
	do_invalidate(data, cnt);
}

int e_invalidate_page_full(unsigned data)
{
	int cnt = PAGE_SIZE;
	if (!IS_EMU_JIT())
		return 0;
	data &= _PAGE_MASK;
	/* nothing to invalidate if there are no page protections */
	if (!e_querymprotrange(data, cnt))
		return 0;
	do_invalidate(data, cnt);
	return 1;
}

/////////////////////////////////////////////////////////////////////////////


#ifdef X86_JIT

static void CleanIMeta(void)
{
#if PROFILE
	hitimer_t t0 = 0;

	if (debug_level('e')) t0 = GETTSC();
#endif
	memset(InstrMeta,0,sizeof(IMeta));
#if PROFILE
	if (debug_level('e')) CleanupTime += (GETTSC() - t0);
#endif
}

/////////////////////////////////////////////////////////////////////////////


int NewIMeta(int npc, int *rc)
{
#if PROFILE
	hitimer_t t0 = 0;

	if (debug_level('e')) t0 = GETTSC();
#endif
	if (CurrIMeta >= 0) {
		// add new opcode metadata
		IMeta *I,*I0;

		if (CurrIMeta>=MAXINODES) {
			*rc = -1; goto quit;
		}
		I  = &InstrMeta[CurrIMeta];
		if (CurrIMeta==0) {		// no open code sequences
			if (debug_level('e')>2) e_printf("============ Opening sequence at %08x\n",npc);
			I0 = I;
		}
		else {
			I0 = &InstrMeta[0];
		}

		I0->ncount += 1;
		I->npc = npc;

		if (CurrIMeta>0) {
			/* F_INHI (pop ss/mov ss) only applies to the last
			   instruction in the sequence and not twice in
			   a row */
			if ((I->flags & F_INHI) && !(I0->flags & F_INHI))
				I0->flags |= F_INHI;
			else
				I0->flags &= ~F_INHI;
			I0->flags |= I->flags & ~F_INHI;
		}
		if (debug_level('e')>4) {
			e_printf("Metadata %03d PC=%08x flags=%x(%x) ng=%d\n",
				CurrIMeta,I->npc,I->flags,I0->flags,I->ngen);
		}
#if PROFILE
		if (debug_level('e')) AddTime += (GETTSC() - t0);
#endif
		CurrIMeta++;
		*rc = 1; I++;
		I->ngen = 0;
		I->flags = 0;
		return CurrIMeta;
	}
	*rc = 0;
quit:
#if PROFILE
	if (debug_level('e')) AddTime += (GETTSC() - t0);
#endif
	return -1;
}

#endif	// X86_JIT

/////////////////////////////////////////////////////////////////////////////

#ifdef SHOW_STAT
#define CST_SIZE	4096
static struct {
	long long a;
	int b,c,m,d,s;
} xCST[CST_SIZE];
#else
#define CST_SIZE	4
static int xCST[CST_SIZE];
#endif

static int cstx = 0;
static int xCS1 = 0;

void CollectStat (void)
{
	int i, m = 0;
#ifdef SHOW_STAT
	int csm = config.CPUSpeedInMhz*1000;
//	xCST[cstx].a = TheCPU.EMUtime;
	xCST[cstx].s = TheCPU.sigprof_pending;
	xCST[cstx].b = ninodes;
	xCST[cstx].c = NodesParsed;
	xCST[cstx].d = NodesExecd;
	for (i=cstx-3; i<=cstx; i++) {
		if (i<0) { if (!xCS1) i=0; else i=CST_SIZE+i; }
		m += xCST[i].c;
	}
	m >>= 2;
	xCST[cstx].m = CLEAN_SPEED(FastLog2(m));
	i = cstx;
	if (debug_level('e')>1)
		e_printf("SIGPROF %04d %8d %8d(%3d) %8d %d\n",i,
			xCST[i].b,xCST[i].c,xCST[i].m,xCST[i].d,xCST[i].s);
	cstx++;
	if (cstx==CST_SIZE) {
	    if (!xCS1) xCS1=1;
	    for (i=0; i<cstx; i++) {
		dbug_printf("%04d %16Ld %8d %8d(%3d) %8d %d\n",i,(xCST[i].a/csm),
		    xCST[i].b,xCST[i].c,xCST[i].m,xCST[i].d,xCST[i].s);
	    }
	    cstx=0;
	}
#else
	xCST[cstx++] = NodesParsed;
	if (cstx==CST_SIZE) {
	    if (!xCS1) xCS1=1;
	    cstx=0;
	}
	if (xCS1) {
	    for (i=0; i<CST_SIZE; i++) m += xCST[i];	/* moving average */
	    m /= CST_SIZE;
	    m = FastLog2(m);	/* take leftmost bit index */
	    CreationIndex = CLEAN_SPEED(m);
	    CleanFreq = (8-m); if (CleanFreq<1) CleanFreq=1;
	}
	if (debug_level('e')>1)
		e_printf("SIGPROF %d n=%8d p=%8d x=%8d ix=%3d cln=%2d\n",
			TheCPU.sigprof_pending,
			ninodes,NodesParsed,NodesExecd,CreationIndex,
			CleanFreq);
#endif
	NodesParsed = NodesExecd = 0;
}


/////////////////////////////////////////////////////////////////////////////

void InitTrees(void)
{
	g_printf("InitTrees\n");
#ifdef X86_JIT
	if (!config.cpusim)
	    TNodePool = calloc(NODES_IN_POOL, sizeof(TNode));
#endif

	avltr_init();

#ifdef X86_JIT
	if (!config.cpusim && debug_level('e')>1) {
	    e_printf("Root tree node at %p\n",&CollectTree.root);
	    e_printf("TNode pool at %p\n",TNodePool);
	}
#endif
	NodesParsed = NodesExecd = 0;
	CleanFreq = 8;
	cstx = xCS1 = 0;
	CreationIndex = 0;
#if PROFILE
	if (debug_level('e')) {
	    MaxDepth = MaxNodes = MaxNodeSize = 0;
	    TotalNodesParsed = TotalNodesExecd = 0;
	    NodesFound = NodesFastFound = NodesNotFound = 0;
	    TreeCleanups = 0;
	}
#endif
}

void EndGen(void)
{
#ifdef SHOW_STAT
	int i;
	int csm = config.CPUSpeedInMhz*1000;
#endif
#ifdef X86_JIT
	if (!config.cpusim)
	    CleanIMeta();
#endif
	CurrIMeta = -1;
#ifdef X86_JIT
	if (!config.cpusim) {
	    avltr_destroy();
	    free(TNodePool); TNodePool=NULL;
	}
#endif
#ifdef SHOW_STAT
	for (i=0; i<cstx; i++) {
	    dbug_printf("%04d %16Ld %8d %8d(%3d) %8d %d\n",i,(xCST[i].a/csm),
		xCST[i].b,xCST[i].c,xCST[i].m,xCST[i].d,xCST[i].s);
	}
#endif
}

/////////////////////////////////////////////////////////////////////////////

