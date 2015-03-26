/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 2008-2015 MonetDB B.V.
 */

/*
 * Post-optimization. After the join path has been constructed
 * we could search for common subpaths. This heuristic is to
 * remove any pair which is used more than once.
 * Inner paths are often foreign key walks.
 * The heuristics is sufficient for the code produced by SQL frontend.
 * The alternative is to search for all possible subpaths and materialize them.
 * For example, using recursion for all common paths.
 */
#include "monetdb_config.h"
#include "joinpath.h"
//#include "cluster.h"

/*
 * The join path optimizer takes a join sequence and
 * attempts to minimize the intermediate result.
 * The choice depends on a good estimate of intermediate
 * results using properties.
 * For the time being, we use a simplistic model, based
 * on the assumption that most joins are foreign key joins anyway.
 *
 * We use a sample based approach for sizeable  tables.
 * The model is derived from the select statement. However, we did not succeed.
 * The code is now commented for future improvement.
 *
 * Final conclusion from this exercise is:
 * The difference between the join input size and the join output size is not
 * the correct (or unique) metric which should be used to decide which order
 * should be used in the joinPath.
 * A SMALL_OPERAND is preferrable set to those cases where the table
 * fits in the cache. This depends on the cache size and operand type.
 * For the time being we limit ourself to a default of 1Kelements
 */
/*#define SAMPLE_THRESHOLD_lOG 17*/

#define SMALL_OPERAND	1024

static BUN
ALGjoinCost(Client cntxt, BAT *l, BAT *r, int flag)
{
	BUN lc, rc;
	BUN cost=0;
#if 0
	BUN lsize,rsize;
	BAT *lsample, *rsample, *j; 
#endif

	(void) flag;
	(void) cntxt;
	lc = BATcount(l);
	rc = BATcount(r);
#if 0	
	/* The sampling method */
	if(flag < 2 && ( lc > 100000 || rc > 100000)){
		lsize= MIN(lc/100, (1<<SAMPLE_THRESHOLD_lOG)/3);
		lsample= BATsample(l,lsize);
		BBPreclaim(lsample);
		rsize= MIN(rc/100, (1<<SAMPLE_THRESHOLD_lOG)/3);
		rsample= BATsample(r,rsize);
		BBPreclaim(rsample);
		j= BATjoin(l,r, MAX(lsize,rsize));
		lsize= BATcount(j);
		BBPreclaim(j);
		return lsize;
	}
#endif

	/* first use logical properties to estimate upper bound of result size */
	if (l->tkey && r->hkey)
		cost = MIN(lc,rc);
	else
	if (l->tkey)
		cost = rc;
	else
	if (r->hkey)
		cost = lc;
	else
	if (lc * rc >= BUN_MAX)
		cost = BUN_MAX;
	else
		cost = lc * rc;

	/* then use physical properties to rank costs */
	if (BATtdense(l) && BAThdense(r))
		/* densefetchjoin -> sequential access */
		cost /= 7;
	else
	if (BATtordered(l) && BAThdense(r))
		/* orderedfetchjoin > sequential access */
		cost /= 6;
	else
	if (BATtdense(l) && BAThordered(r) && flag != 0 /* no leftjoin */)
		/* (reversed-) orderedfetchjoin -> sequential access */
		cost /= 6;
	else
	if (BAThdense(r) && rc <= SMALL_OPERAND)
		/* fetchjoin with random access in L1 */
		cost /= 5;
	else
	if (BATtdense(l) && lc <= SMALL_OPERAND && flag != 0 /* no leftjoin */)
		/* (reversed-) fetchjoin with random access in L1 */
		cost /= 5;
	else
	if (BATtordered(l) && BAThordered(r))
		/* mergejoin > sequential access */
		cost /= 4;
	else
	if (BAThordered(r) && rc <= SMALL_OPERAND)
		/* binary-lookup-join with random access in L1 */
		cost /= 3;
	else
	if (BATtordered(l) && lc <= SMALL_OPERAND && flag != 0 /* no leftjoin */)
		/* (reversed-) binary-lookup-join with random access in L1 */
		cost /= 3;
	else
	if ((BAThordered(r) && lc <= SMALL_OPERAND) || (BATtordered(l) && rc <= SMALL_OPERAND))
		/* sortmergejoin with sorting in L1 */
		cost /= 3;
	else
	if (rc <= SMALL_OPERAND)
		/* hashjoin with hashtable in L1 */
		cost /= 3;
	else
	if (lc <= SMALL_OPERAND && flag != 0 /* no leftjoin */)
		/* (reversed-) hashjoin with hashtable in L1 */
		cost /= 3;
	else
	if (BAThdense(r))
		/* fetchjoin with random access beyond L1 */
		cost /= 2;
	else
	if (BATtdense(l) && flag != 0 /* no leftjoin */)
		/* (reversed-) fetchjoin with random access beyond L1 */
		cost /= 2;
	else
		/* hashjoin with hashtable larger than L1 */
		/* sortmergejoin with sorting beyond L1 */
		cost /= 1;

	ALGODEBUG
		fprintf(stderr,"#batjoin cost ?"BUNFMT"\n",cost);
	return cost;
}

/*
 * The joinChain assumes a list of OID columns ending in a projection column.
 * It is built from leftfetchjoin operations, which allows for easy chaining.
 * No intermediates are needed and not multistep cost-based evaluation
 */

#define MAXCHAINDEPTH 256

static BAT *
ALGjoinChain(Client cntxt, int top, BAT **joins)
{
	BAT *bn = NULL;
	oid lo, hi, *o, oc;
	BATiter iter[MAXCHAINDEPTH];
	int i, pcol= top -1;
	size_t cnt=0, cap=0, empty=0, offset[MAXCHAINDEPTH];
	const void *v;

#ifdef _DEBUG_JOINPATH_
	mnstr_printf(cntxt->fdout,"#joinchain \n");
#else
	(void) cntxt;
#endif
	for ( i =0; i< top ; i++){
		if( (cnt  = BATcount(joins[i]) ) > cap)
			cap = BATcount(joins[i]);
		empty += cnt == 0;
		iter[i] = bat_iterator(joins[i]);
		offset[i] = BUNfirst(joins[i])-joins[i]->hseqbase;
	}

	bn = BATnew( TYPE_void, joins[pcol]->ttype, cap, TRANSIENT);
	if( bn == NULL){
		GDKerror("joinChain" MAL_MALLOC_FAIL);
		return NULL;
	}
	if ( empty)
		return bn;
	/* be optimistic, inherit the properties  */
	bn->T->nonil = joins[pcol]->T->nil;
	bn->T->nonil = joins[pcol]->T->nonil;
	bn->tsorted = joins[pcol]->tsorted;
	bn->trevsorted = joins[pcol]->trevsorted;
	bn->tkey = joins[pcol]->tkey;

	o = (oid *) BUNtail(iter[0], BUNfirst(joins[0]));
	for( lo = 0, hi = lo + BATcount(joins[0]); lo < hi && o; o++, lo++)
	{
		oc = *o;
		for(i = 1; i < pcol; i++){
			v = BUNtail(iter[i], oc + offset[i]);
			oc = *(oid*) v;
			if( oc == oid_nil)
				goto bunins_failed;
		}
		// update the join result and keep track of properties
		v = BUNtail(iter[pcol], oc + offset[pcol]);
		bunfastapp(bn,v);
		cnt++;
		// perform inline property mgmt?
		// Nonils can not be changed by inclusion of values.
		// Nils is indicative, not a must that they occur
		// sorting can be determined by the oids of the last fetch
		bunins_failed:
		;
	}
    BATsetcount(bn, cnt);
    BATseqbase(bn, joins[0]->H->seq);

	// release the chain 
	for ( i =0; i< top ; i++)
		BBPunfix(joins[i]->batCacheid);
		
	BATderiveProps(bn, FALSE);

	if (bn && !(bn->batDirty&2)) BATsetaccess(bn, BAT_READ);

	return bn;
}

static BAT *
ALGjoinPathBody(Client cntxt, int top, BAT **joins, int flag)
{
	BAT *b = NULL;
	BUN estimate, e = 0;
	int i, j, k;
	int *postpone= (int*) GDKzalloc(sizeof(int) *top);
	int postponed=0;

	if(postpone == NULL){
		GDKerror("joinPathBody" MAL_MALLOC_FAIL);
		return NULL;
	}

	/* solve the join by pairing the smallest first */
	while (top > 1) {
		j = 0;
		estimate = ALGjoinCost(cntxt,joins[0],joins[1],flag);
		ALGODEBUG
			fprintf(stderr,"#joinPath estimate join(%d,%d) %d cnt="BUNFMT" %s\n", joins[0]->batCacheid, 
				joins[1]->batCacheid,(int)estimate, BATcount(joins[0]), postpone[0]?"postpone":"");
		for (i = 1; i < top - 1; i++) {
			e = ALGjoinCost(cntxt,joins[i], joins[i + 1],flag);
			ALGODEBUG
				fprintf(stderr,"#joinPath estimate join(%d,%d) %d cnt="BUNFMT" %s\n", joins[i]->batCacheid, 
					joins[i+1]->batCacheid,(int)e,BATcount(joins[i]),  postpone[i]?"postpone":"");
			if (e < estimate &&  ( !(postpone[i] && postpone[i+1]) || postponed<top)) {
				estimate = e;
				j = i;
			}
		}
		/*
		 * BEWARE. you may not use a size estimation, because it
		 * may fire a BATproperty check in a few cases.
		 * In case a join fails, we may try another order first before
		 * abandoning the task. It can handle cases where a Cartesian product emerges.
		 *
		 * A left-join sequence only requires the result to be sorted
		 * against the first operand. For all others operand pairs, the cheapest join suffice.
		 */

		switch(flag){
		case 0:
			if ( j == 0) {
				b = BATleftjoin(joins[j], joins[j + 1], BATcount(joins[j]));
				break;
			}
		case 1:
			b = BATjoin(joins[j], joins[j + 1], (BATcount(joins[j]) < BATcount(joins[j + 1])? BATcount(joins[j]):BATcount(joins[ j + 1])));
			break;
		case 3:
			b = BATproject(joins[j], joins[j + 1]);
			break;
		}
		if (b==NULL){
			if ( postpone[j] && postpone[j+1]){
				for( --top; top>=0; top--)
					BBPunfix(joins[top]->batCacheid);
				GDKfree(postpone);
				return NULL;
			}
			postpone[j] = TRUE;
			postpone[j+1] = TRUE;
			postponed = 0;
			for( k=0; k<top; k++)
				postponed += postpone[k]== TRUE;
			if ( postponed == top){
				for( --top; top>=0; top--)
					BBPunfix(joins[top]->batCacheid);
				GDKfree(postpone);
				return NULL;
			}
			/* clear the GDKerrors and retry */
			if( cntxt->errbuf )
				cntxt->errbuf[0]=0;
			continue;
		} else {
			/* reset the postponed joins */
			for( k=0; k<top; k++)
				postpone[k]=FALSE;
			if (!(b->batDirty&2)) BATsetaccess(b, BAT_READ);
			postponed = 0;
		}
		ALGODEBUG{
			if (b ) {
				fprintf(stderr, "#joinPath %d:= join(%d,%d)"
				" arguments %d (cnt= "BUNFMT") against (cnt "BUNFMT") cost "BUNFMT"\n", 
					b->batCacheid, joins[j]->batCacheid, joins[j + 1]->batCacheid,
					j, BATcount(joins[j]),  BATcount(joins[j+1]), e);
			}
		}

		if ( b == 0 ){
			for( --top; top>=0; top--)
				BBPunfix(joins[top]->batCacheid);
			GDKfree(postpone);
			return 0;
		}
		BBPunfix(joins[j]->batCacheid);
		BBPunfix(joins[j+1]->batCacheid);
		joins[j] = b;
		top--;
		for (i = j + 1; i < top; i++)
			joins[i] = joins[i + 1];
	}
	GDKfree(postpone);
	b = joins[0];
	if (b && !(b->batDirty&2)) BATsetaccess(b, BAT_READ);
	return b;
}

str
ALGjoinPath(Client cntxt, MalBlkPtr mb, MalStkPtr stk, InstrPtr pci)
{
	int i,top=0, chain = 1;
	bat *bid;
	bat *r = getArgReference_bat(stk, pci, 0);
	BAT *b, **joins = (BAT**)GDKmalloc(pci->argc*sizeof(BAT*)); 
	int error = 0;
	str joinPathRef = putName("joinPath",8);
	str leftjoinPathRef = putName("leftjoinPath",12);

	if ( joins == NULL)
		throw(MAL, "algebra.joinPath", MAL_MALLOC_FAIL);
	(void)mb;
	for (i = pci->retc; i < pci->argc; i++) {
		bid = getArgReference_bat(stk, pci, i);
		b = BATdescriptor(*bid);
		if (  b && top ) {
			if ( !(joins[top-1]->ttype == b->htype) &&
			     !(joins[top-1]->ttype == TYPE_void && b->htype == TYPE_oid) &&
			     !(joins[top-1]->ttype == TYPE_oid && b->htype == TYPE_void) ) {
				b= NULL;
				error = 1;
			}
		}
		if ( b == NULL) {
			for( --top; top>=0; top--)
				BBPunfix(joins[top]->batCacheid);
			GDKfree(joins);
			throw(MAL, "algebra.joinPath", "%s", error? SEMANTIC_TYPE_MISMATCH: INTERNAL_BAT_ACCESS);
		}
		joins[top++] = b;
	}
	/* detect easy chain joins */
	for ( i =1; i< top ; i++)
		if( BATcount(joins[i-1])> BATcount(joins[i]) )
			chain = 0;
	chain = 0; // disabled for the moment, because it is not robust yet

	ALGODEBUG{
		char *ps;
		ps = instruction2str(mb, 0, pci, 0);
		fprintf(stderr,"#joinpath [%s] %s\n", (ps ? ps : ""), chain?"chain":"diverse");
		GDKfree(ps);
	}
	if ( getFunctionId(pci) == joinPathRef)
		b= ALGjoinPathBody(cntxt,top,joins, 1);
	else
	if ( getFunctionId(pci) == leftjoinPathRef)
		b= ALGjoinPathBody(cntxt,top,joins, 0); 
	else{
		if( chain && top < MAXCHAINDEPTH)
			b= ALGjoinChain(cntxt,top,joins); 
		else
			b= ALGjoinPathBody(cntxt,top,joins, 3); 
	}

	GDKfree(joins);
	if ( b)
		BBPkeepref( *r = b->batCacheid);
	else
		throw(MAL, "algebra.joinPath", INTERNAL_OBJ_CREATE);
	return MAL_SUCCEED;
}
