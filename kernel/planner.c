/*
 * Copyright (c) 2000 Matteo Frigo
 * Copyright (c) 2000 Steven G. Johnson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/* $Id: planner.c,v 1.73 2002-09-12 19:46:56 stevenj Exp $ */
#include "ifftw.h"
#include <string.h> /* strlen */

#define IMPATIENCE(flags) ((flags) & IMPATIENCE_MASK)
#define MODULO_EQV(flags) ((flags) & EQV_MASK)
#define BLESSEDP(s) ((s)->flags & BLESSING)

#define MAXNAM 64  /* maximum length of registrar's name.
		      Used for reading wisdom.  There is no point
		      in doing this right */
		      
/*
  slvdesc management:
*/
static uint hash_regnam(const char *s)
{
     uint h = 0xDEADBEEFul;
     do {
	  h = h * 17 + *s;
     } while (*s++);
     return h;
}

static void register_solver(planner *ego, solver *s)
{
     slvdesc *n;
     if (s) { /* add s to solver list */
	  X(solver_use)(s);
	  n = (slvdesc *) fftw_malloc(sizeof(slvdesc), SLVDESCS);
	  n->slv = s;
	  n->reg_nam = ego->cur_reg_nam;
	  n->reg_id = ego->cur_reg_id++;

	  A(strlen(n->reg_nam) < MAXNAM);
	  n->nam_hash = hash_regnam(n->reg_nam);

	  /* cons! onto solvers list */
	  n->cdr = ego->solvers;
	  ego->solvers = n;
     }
}

static slvdesc *slvdesc_lookup(planner *ego, char *nam, int id)
{
     slvdesc *sp;
     uint h = hash_regnam(nam); /* used to avoid strcmp in the common case */
     for (sp = ego->solvers; sp; sp = sp->cdr)
	  if (sp->reg_id == id && sp->nam_hash == h 
	      && !strcmp(sp->reg_nam, nam))
	       break;
     return sp;
}

/*
  md5-related stuff:
*/
static uint sig_to_hash_index(planner *ego, md5uint *s)
{
     return s[0] % ego->hashsiz;
}

static void md5hash(md5 *m, const problem *p, uint flags, uint nthr)
{
     X(md5begin)(m);
     X(md5uint)(m, sizeof(R)); /* so we don't mix different precisions */
     X(md5uint)(m, MODULO_EQV(flags));
     X(md5uint)(m, nthr);
     p->adt->hash(p, m);
     X(md5end)(m);
}

static int md5eq(md5uint *a, md5uint *b)
{
     return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void sigcpy(md5uint *a, md5uint *b)
{
     b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; b[3] = a[3];
}

/*
  memoization routines :
*/

/*
   liber scriptus proferetur
   in quo totum continetur
   unde mundus iudicetur
*/
struct solution_s {
     md5uint s[4];
     slvdesc *sp;
     unsigned short flags;
     short state;
};

/* states: */
enum { H_EMPTY, H_VALID, H_DELETED };


/* maintain invariant lb(cnt) <= hashsz < ub(cnt) */
static uint ub(uint cnt) { return 3U * (cnt + 10U); }
static uint lb(uint cnt) { return ub(cnt) / 2U; }

static solution *hlookup(planner *ego, md5uint *s)
{
     uint h, g;

     ++ego->access;
     h = sig_to_hash_index(ego, s);

     for (g = h; ; g = (g + 1) % ego->hashsiz) {
	  solution *l = ego->sols + g;
	  switch (l->state) {
	      case H_EMPTY: return 0;
	      case H_VALID: if (md5eq(s, l->s)) { ++ego->hit; return l; }
	  }
	  A((g + 1) % ego->hashsiz != h);
     }
}


static void hinsert0(planner *ego, md5uint *s, uint flags, slvdesc *sp,
		     solution *l)
{
     if (!l) { 	 
	  /* search for nonfull slot */
	  uint g, h = sig_to_hash_index(ego, s); 
	  for (g = h; ; g = (g + 1) % ego->hashsiz) {
	       l = ego->sols + g;
	       if (l->state != H_VALID) break;
	       A((g + 1) % ego->hashsiz != h);
	  }
     }

     /* fill slot */
     l->state = H_VALID;
     l->flags = flags;
     l->sp = sp;
     sigcpy(s, l->s);
}

static void rehash(planner *ego)
{
     uint osiz = ego->hashsiz, nsiz, bl, bu;

     bl = lb(ego->cnt); bu = ub(ego->cnt);
     if (bl <= osiz && osiz < bu)
	  return;  /* nothing to do */

     nsiz = (bl + bu + 1) / 2;

     if (nsiz != osiz) {
	  solution *osol = ego->sols;
	  solution *nsol =
	       (solution *)fftw_malloc(nsiz * sizeof(solution), HASHT);
	  uint h;

	  ++ego->nrehash;

	  /* init new table */
	  for (h = 0; h < nsiz; ++h)
	       nsol[h].state = H_EMPTY;

	  /* install new table */
	  ego->hashsiz = nsiz;
	  ego->sols = nsol;

	  /* copy table */
	  for (h = 0; h < osiz; ++h) {
	       solution *l = osol + h;
	       if (l->state == H_VALID)
		    hinsert0(ego, l->s, (uint)l->flags, l->sp, 0);
	  }

	  if (osol)
	       X(free)(osol);
     }
}


static void hinsert(planner *ego, md5uint *s, uint flags, slvdesc *sp)
{
     solution *l;

     if ((l = hlookup(ego, s))) {
	  /* overwrite old solution */
	  if (IMPATIENCE(flags) > IMPATIENCE(((uint)l->flags)))
	       return; /* don't overwrite less impatient solution */

	  flags |= l->flags & BLESSING; /* ne me perdas illa die */
     } else {
	  ++ego->cnt;
	  rehash(ego);
     }
     hinsert0(ego, s, flags, sp, l);
}


static void insert(planner *ego, problem *p, slvdesc *sp)
{
     md5 m;
     md5hash(&m, p, ego->flags, ego->nthr);
     hinsert(ego, m.s, ego->flags, sp);
}

static solution *lookup(planner *ego, problem *p)
{
     md5 m;
     solution *l;

     md5hash(&m, p, ego->flags, ego->nthr);
     l = hlookup(ego, m.s);
     return  (l && (IMPATIENCE(ego->flags) >= IMPATIENCE(((uint)l->flags)))) 
	  ? l : 0;
}

/*
  routines to make plans:
*/
static plan *slv_mkplan(planner *ego, problem *p, solver *s)
{
     uint flags = ego->flags;
     uint nthr = ego->nthr;
     plan *pln;
     pln = s->adt->mkplan(s, p, ego);
     ego->flags = flags;
     ego->nthr = nthr;
     return pln;
}

static plan *mkplan(planner *ego, problem *p)
{
     solution *sol;
     plan *pln;
     slvdesc *sp = 0;

     ++ego->nprob;
     sp = (sol = lookup(ego, p)) ? sol->sp : 0;
     ego->inferior_mkplan(ego, p, &pln, &sp);
     insert(ego, p, sp);

     if (pln)
	  ego->hook(pln, p, 1);
     return pln;
}

/* destroy hash table entries.  If FORGET_EVERYTHING, destroy the whole
   table.  If FORGET_ACCURSED, then destroy entries that are not blessed. */
static void forget(planner *ego, amnesia a)
{
     uint h;

     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->sols + h;
	  if (l->state == H_VALID) {
	       if (a == FORGET_EVERYTHING ||
		   (a == FORGET_ACCURSED && !BLESSEDP(l))) {
		    /* confutatis maledictis
		       flammis acribus addictis */
		    l->state = H_DELETED;
		    --ego->cnt;
	       }
	  }
     }
     /* nil inultum remanebit */

     rehash(ego);
}

static void htab_destroy(planner *ego)
{
     forget(ego, FORGET_EVERYTHING);
     X(free)(ego->sols);
     ego->cnt = 0;
}

/* FIXME: what sort of version information should we write? */
#define WISDOM_PREAMBLE PACKAGE "-" VERSION "-wisdom "

/* tantus labor non sit cassus */
static void exprt(planner *ego, printer *p)
{
     uint h;

     p->print(p, "(" WISDOM_PREAMBLE "%(");
     for (h = 0; h < ego->hashsiz; ++h) {
	  solution *l = ego->sols + h;
	  if (l->state == H_VALID && BLESSEDP(l) && l->sp) {
	       /* qui salvandos salvas gratis
		  salva me fons pietatis */
	       p->print(p, "(%s %d #x%x #x%M #x%M #x%M #x%M)\n",
			l->sp->reg_nam, l->sp->reg_id, (uint)l->flags,
			l->s[0], l->s[1], l->s[2], l->s[3]);
	  }
     }
     p->print(p, "%))\n");
}

static int imprt(planner *ego, scanner *sc)
{
     char buf[MAXNAM + 1];
     md5uint sig[4];
     uint flags;
     int reg_id;
     slvdesc *sp;

     if (!sc->scan(sc, "(" WISDOM_PREAMBLE))
	  goto bad;


     while (1) {
	  if (sc->scan(sc, ")"))
	       break;

	  if (!sc->scan(sc, "(%*s %d #x%x #x%M #x%M #x%M #x%M)",
			MAXNAM, buf, &reg_id, &flags, 
			sig + 0, sig + 1, sig + 2, sig + 3))
	       goto bad;

	  sp = slvdesc_lookup(ego, buf, reg_id);
	  if (!sp)
	       goto bad; /* TODO: panic? */

	  /* inter oves locum praesta */
	  hinsert(ego, sig, flags, sp);
     }
     return 1;

 bad:
     return 0;
}

static void hooknil(plan *pln, const problem *p, int optimalp)
{
     UNUSED(pln);
     UNUSED(p);
     UNUSED(optimalp);
     /* do nothing */
}

/*
 * create a planner
 */
planner *X(mkplanner)(size_t sz,
		      void (*infmkplan)(planner *ego, problem *p,
					plan **, slvdesc **),
                      void (*destroy) (planner *),
		      uint flags)
{
     static const planner_adt padt = {
	  register_solver,
	  mkplan, forget, exprt, imprt, slv_mkplan
     };

     planner *p = (planner *) fftw_malloc(sz, PLANNERS);

     p->adt = &padt;
     p->inferior_mkplan = infmkplan;
     p->destroy = destroy;
     p->nplan = p->nprob = p->hit = p->access = 0;
     p->hook = hooknil;
     p->cur_reg_nam = 0;
     p->solvers = 0;
     p->sols = 0;
     p->hashsiz = 0;
     p->cnt = 0;
     p->nrehash = 0;
     p->score = BAD;            /* unused, but we initialize it anyway */
     p->flags = flags;
     p->nthr = 1;
     rehash(p);			/* so that hashsiz > 0 */

     return p;
}

void X(planner_destroy)(planner *ego)
{
     slvdesc *l, *l0;

     /* destroy local state, if any */
     if (ego->destroy)
          ego->destroy(ego);

     /* destroy hash table */
     htab_destroy(ego);

     /* destroy all solvers */
     for (l = ego->solvers; l; l = l0) {
          l0 = l->cdr;
          X(solver_destroy)(l->slv);
          X(free)(l);
     }

     X(free)(ego);
}

/* set planner hook */
void X(planner_set_hook)(planner *p,
			 void (*hook)(plan *, const problem *, int))
{
     p->hook = hook;
}

void X(evaluate_plan)(planner *ego, plan *pln, const problem *p)
{
     if (!(ego->flags & IMPATIENT) || pln->pcost == 0.0) {
	  ego->nplan++;
	  if (ego->flags & ESTIMATE) {
	       /* heuristic */
	       pln->pcost = 0
		    + pln->ops.add
		    + pln->ops.mul
		    + 2 * pln->ops.fma
		    + pln->ops.other;
	  } else {
	       pln->pcost = X(measure_execution_time)(pln, p);
	  }
     }
     ego->hook(pln, p, 0);
}

/*
 * Debugging code:
 */
#ifdef FFTW_DEBUG

void X(planner_dump)(planner *ego, int verbose)
{
     uint valid = 0, deleted = 0, empty = 0;
     uint h;
     UNUSED(verbose); /* historical */

     for (h = 0; h < ego->hashsiz; ++h) 
	  switch (ego->sols[h].state) {
	      case H_EMPTY: ++empty; break;
	      case H_DELETED: ++deleted; break;
	      case H_VALID: ++valid; break;
	      default: A(0);
	  }

     D("nplan = %u\n", ego->nplan);
     D("nprob = %u\n", ego->nprob);
     D("access = %u\n", ego->access);
     D("hit = %u\n", ego->hit);
     D("nrehash = %d\n", ego->nrehash);
     D("hashsiz = %d\n", ego->hashsiz);
     D("empty = %d\n", empty);
     D("deleted = %d\n", deleted);
     D("valid = %d\n", valid);
     A(ego->cnt == valid);
}
#endif
