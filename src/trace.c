/* impl.c.trace: GENERIC TRACER IMPLEMENTATION
 *
 * $HopeName: MMsrc!trace.c(trunk.63) $
 * Copyright (C) 1997 The Harlequin Group Limited.  All rights reserved.
 *
 * .sources: design.mps.tracer.
 * .design: design.mps.trace.
 *
 * NOTES
 *
 * .exact.legal: Exact references should either point outside the arena
 * (to non-managed address space) or to an allocated segment.  Exact
 * references that are to addresses which the arena has reserved
 * but hasn't allocated memory to are illegal (the exact reference
 * couldn't possibly refer to a real object).  Depending on the
 * future semantics of PoolDestroy we might need to adjust our
 * strategy here.  See mail.dsm.1996-02-14.18-18(0) for a strategy
 * of coping gracefully with PoolDestroy.  We check that this is the
 * case in the fixer.  It may be sensible to make this check CRITICAL
 * in certain configurations.
 *
 * .fix.fixed.all:
 * ss->fixedSummary is accumulated (in the fixer) for all the pointers
 * whether or not they are genuine references.  We could accumulate fewer
 * pointers here; if a pointer fails the SegOfAddr test then we
 * know it isn't a reference, so we needn't accumulate it into the
 * fixed summary.  The design allows this, but it breaks a useful
 * post-condition on scanning.  See .scan.post-condition.  (if
 * the accumulation of ss->fixedSummary was moved the accuracy
 * of ss->fixedSummary would vary according to the "width" of the
 * white summary).
 */

#include "mpm.h"

SRCID(trace, "$HopeName: MMsrc!trace.c(trunk.63) $");

/* Types
 *
 * These types only used internally to this trace module.
 */

enum {TraceAccountingPhaseRootScan,
      TraceAccountingPhaseSegScan,
      TraceAccountingPhaseSingleScan};
typedef int TraceAccountingPhase;


/* ScanStateCheck -- check consistency of a ScanState object */

Bool ScanStateCheck(ScanState ss)
{
  TraceId ti;
  RefSet white;
  CHECKS(ScanState, ss);
  CHECKL(FUNCHECK(ss->fix));
  CHECKU(Arena, ss->arena);
  CHECKL(TraceSetCheck(ss->traces));
  CHECKL(TraceSetSuper(ss->arena->busyTraces, ss->traces));
  white = RefSetEMPTY;
  for(ti = 0; ti < TRACE_MAX; ++ti)
    if(TraceSetIsMember(ss->traces, ti))
      white = RefSetUnion(white, ss->arena->trace[ti].white);
  CHECKL(ss->white == white);
  CHECKL(ss->zoneShift == ss->arena->zoneShift);
  CHECKL(RankCheck(ss->rank));
  CHECKL(BoolCheck(ss->wasMarked));
  return TRUE;
}

static void ScanStateInit(ScanState ss, TraceSet ts, Arena arena,
                          Rank rank, RefSet white)
{
  TraceId ti;

  /* we are initing it, so we can't check ss */
  AVERT(Arena, arena);
  AVER(RankCheck(rank));
  /* white is arbitrary and can't be checked */

  ss->fix = TraceFix;
  for(ti = 0; ti < TRACE_MAX; ++ti) {
    if(TraceSetIsMember(ts, ti) &&
       ArenaTrace(arena, ti)->emergency) {
      ss->fix = TraceFixEmergency;
    }
  }
  ss->rank = rank;
  ss->traces = ts;
  ss->zoneShift = arena->zoneShift;
  ss->unfixedSummary = RefSetEMPTY;
  ss->fixedSummary = RefSetEMPTY;
  ss->arena = arena;
  ss->wasMarked = TRUE;
  ss->white = white;
  ss->fixRefCount = (Count)0;
  ss->segRefCount = (Count)0;
  ss->whiteSegRefCount = (Count)0;
  ss->nailCount = (Count)0;
  ss->snapCount = (Count)0;
  ss->forwardCount = (Count)0;
  ss->copiedSize = (Size)0;
  ss->scannedSize = (Size)0;
  ss->sig = ScanStateSig;

  AVERT(ScanState, ss);
}

static void ScanStateFinish(ScanState ss)
{
  AVERT(ScanState, ss);
  ss->sig = SigInvalid;
}


/* TraceIdCheck -- check that a TraceId is valid */

Bool TraceIdCheck(TraceId ti)
{
  CHECKL(ti == TraceIdNONE || ti < TRACE_MAX);
  UNUSED(ti); /* impl.c.mpm.check.unused */
  return TRUE;
}


/* TraceSetCheck -- check that a TraceSet is valid */

Bool TraceSetCheck(TraceSet ts)
{
  CHECKL(ts < (1uL << TRACE_MAX));
  UNUSED(ts); /* impl.c.mpm.check.unused */
  return TRUE;
}


/* TraceCheck -- check consistency of Trace object */

Bool TraceCheck(Trace trace)
{
  CHECKS(Trace, trace);
  CHECKU(Arena, trace->arena);
  CHECKL(TraceIdCheck(trace->ti));
  CHECKL(trace == &trace->arena->trace[trace->ti]);
  CHECKL(TraceSetIsMember(trace->arena->busyTraces, trace->ti));
  /* Can't check trace->white -- not in O(1) anyway. */
  CHECKL(RefSetSub(trace->mayMove, trace->white));
  /* Use trace->state to check more invariants. */
  switch(trace->state) {
    case TraceINIT:
    /* @@@@ What can be checked here? */
    break;

    case TraceUNFLIPPED:
    CHECKL(!TraceSetIsMember(trace->arena->flippedTraces, trace->ti));
    /* @@@@ Assert that mutator is grey for trace. */
    break;

    case TraceFLIPPED:
    CHECKL(TraceSetIsMember(trace->arena->flippedTraces, trace->ti));
    /* @@@@ Assert that mutator is black for trace. */
    break;

    case TraceRECLAIM:
    CHECKL(TraceSetIsMember(trace->arena->flippedTraces, trace->ti));
    /* @@@@ Assert that grey set is empty for trace. */
    break;

    case TraceFINISHED:
    CHECKL(TraceSetIsMember(trace->arena->flippedTraces, trace->ti));
    /* @@@@ Assert that grey and white sets is empty for trace. */
    break;

    default:
    NOTREACHED;
  }
  CHECKL(BoolCheck(trace->emergency));
  return TRUE;
}

static void TraceUpdateCounts(Trace trace, ScanState ss,
                              TraceAccountingPhase phase)
{
  AVERT(Trace, trace);
  AVERT(ScanState, ss);

  switch(phase) {
  case TraceAccountingPhaseRootScan:
    trace->rootScanSize += ss->scannedSize;
    trace->rootCopiedSize += ss->copiedSize;
    break;

  case TraceAccountingPhaseSegScan:
    trace->segScanSize += ss->scannedSize;
    trace->segCopiedSize += ss->copiedSize;
    break;

  case TraceAccountingPhaseSingleScan:
    trace->singleScanSize += ss->scannedSize;
    trace->singleCopiedSize += ss->copiedSize;
    break;

  default:
    NOTREACHED;
  }
  trace->fixRefCount += ss->fixRefCount;
  trace->segRefCount += ss->segRefCount;
  trace->whiteSegRefCount += ss->whiteSegRefCount;
  trace->nailCount += ss->nailCount;
  trace->snapCount += ss->snapCount;
  trace->forwardCount += ss->forwardCount;
}


/* TraceAddWhite -- add a segment to the white set of a trace */

Res TraceAddWhite(Trace trace, Seg seg)
{
  Res res;
  Pool pool;

  AVERT(Trace, trace);
  AVERT(Seg, seg);
  AVER(!TraceSetIsMember(SegWhite(seg), trace->ti)); /* .start.black */

  pool = SegPool(seg);
  AVERT(Pool, pool);

  /* Give the pool the opportunity to turn the segment white. */
  /* If it fails, unwind. */
  res = PoolWhiten(pool, trace, seg);
  if(res != ResOK)
    return res;

  /* Add the segment to the approximation of the white set the */
  /* pool made it white. */
  if(TraceSetIsMember(SegWhite(seg), trace->ti)) {
    trace->white = RefSetUnion(trace->white,
                               RefSetOfSeg(trace->arena, seg));
    trace->condemned += SegSize(seg);
    /* if the pool is a moving GC, then condemned objects may move */
    if(pool->class->attr & AttrMOVINGGC) {
      trace->mayMove = RefSetUnion(trace->mayMove, 
                                   RefSetOfSeg(PoolArena(pool), seg));
    }
  }

  return ResOK;
}


/* TraceCondemnRefSet -- condemn a set of objects
 *
 * TraceCondemnRefSet is passed a trace in state TraceINIT,
 * and a set of objects to condemn.
 *
 * @@@@ For efficiency, we ought to find the condemned set and
 * the foundation in one search of the segment ring.  This hasn't
 * been done because some pools still use TraceAddWhite for the
 * condemned set.
 *
 * @@@@ This function would be more efficient if there were a 
 * cheaper way to select the segments in a particular zone set.
 */

Res TraceCondemnRefSet(Trace trace, RefSet condemnedSet)
{
  Seg seg;
  Arena arena;
  Res res;

  AVERT(Trace, trace);
  AVER(condemnedSet != RefSetEMPTY);
  AVER(trace->state == TraceINIT);
  AVER(trace->white == RefSetEMPTY);

  arena = trace->arena;

  if(SegFirst(&seg, arena)) {
    Addr base;
    do {
      base = SegBase(seg);
      /* Segment should be black now. */
      AVER(!TraceSetIsMember(SegGrey(seg), trace->ti));
      AVER(!TraceSetIsMember(SegWhite(seg), trace->ti));

      /* A segment can only be white if it is GC-able. */
      /* This is indicated by the pool having the GC attribute */
      /* We only condemn segments that fall entirely within */
      /* the requested zone set.  Otherwise, we would bloat the */
      /* foundation to no gain.  Note that this doesn't exclude */
      /* any segments from which the condemned set was derived, */
      if((SegPool(seg)->class->attr & AttrGC) != 0 &&
         RefSetSuper(condemnedSet, RefSetOfSeg(arena, seg))) {
        res = TraceAddWhite(trace, seg);
	if(res != ResOK)
	  return res;
      }
    } while(SegNext(&seg, arena, base));
  }

  /* The trace's white set must be a subset of the condemned set */
  AVER(RefSetSuper(condemnedSet, trace->white));

  return ResOK;
}

/* TraceStart -- condemn a set of objects and start collection
 *
 * TraceStart should be passed a trace with state TraceINIT, i.e.
 * recently returned from TraceCreate.
 *
 * .start.black: All segments are black w.r.t. a newly allocated trace.
 * However, if TraceStart initialized segments to black when it
 * calculated the grey set then this condition could be relaxed, making
 * it easy to destroy traces half-way through.
 */

Res TraceStart(Trace trace)
{
  Ring ring, node;
  Arena arena;
  Seg seg;
  Res res;

  AVERT(Trace, trace);
  AVER(trace->state == TraceINIT);

  arena = trace->arena;

  /* If there is nothing white then there can be nothing grey, */
  /* so everything is black and we can finish the trace immediately. */
  if(trace->white == RefSetEMPTY) {
    arena->flippedTraces = TraceSetAdd(arena->flippedTraces, trace->ti);
    trace->state = TraceFINISHED;
    trace->rate = (Size)1;
    return ResOK;
  }

  /* Turn everything else grey. */

  /* @@@@ Instead of iterating over all the segments, we could */
  /* iterate over all pools which are scannable and thence over */
  /* all their segments.  This might be better if the minority */
  /* of segments are scannable.  Perhaps we should choose */
  /* dynamically which method to use. */

  if(SegFirst(&seg, arena)) {
    Addr base;
    do {
      base = SegBase(seg);
      /* Segment should be either black or white by now. */
      AVER(!TraceSetIsMember(SegGrey(seg), trace->ti));

      /* A segment can only be grey if it contains some references. */
      /* This is indicated by the rankSet begin non-empty.  Such */
      /* segments may only belong to scannable pools. */
      if(SegRankSet(seg) != RankSetEMPTY) {
        /* Segments with ranks may only belong to scannable pools. */
        AVER((SegPool(seg)->class->attr & AttrSCAN) != 0);

        /* Turn the segment grey if there might be a reference in it */
        /* to the white set.  This is done by seeing if the summary */
        /* of references in the segment intersects with the approximation */
        /* to the white set. */
        if(RefSetInter(SegSummary(seg), trace->white) != RefSetEMPTY) {
          PoolGrey(SegPool(seg), trace, seg);
	  if(TraceSetIsMember(SegGrey(seg), trace->ti))
	    trace->foundation += SegSize(seg);
        }
      }
    } while(SegNext(&seg, arena, base));
  }

  ring = ArenaRootRing(arena);
  node = RingNext(ring);
  while(node != ring) {
    Ring next = RingNext(node);
    Root root = RING_ELT(Root, arenaRing, node);

    if(RefSetInter(root->summary, trace->white) != RefSetEMPTY)
      RootGrey(root, trace);

    node = next;
  }

  /* Calculate the rate of working.  Assumes that half the condemned */
  /* set will survive, and calculates a rate of work which will */
  /* finish the collection by the time that a megabyte has been */
  /* allocated.  The 4096 is the number of bytes scanned by each */
  /* TraceStep (approximately) and should be replaced by a parameter. */
  /* This is a temporary measure for change.dylan.honeybee.170466. */
  {
    double surviving = trace->condemned / 2;
    double scan = trace->foundation + surviving;
    /* double reclaim = trace->condemned - surviving; */
    double alloc = 1024*1024L; /* reclaim / 2; */
    /* if(alloc > 0) */
      trace->rate = 1 + (Size)(scan * ARENA_POLL_MAX / (4096 * alloc));
    /* else */
      /* trace->rate = 1 + (Size)(scan / 4096); */
  }

  trace->state = TraceUNFLIPPED;

  /* All traces must flip at beginning at the moment. */
  res = TraceFlip(trace);
  if(res != ResOK)
    return res;

  return ResOK;
}


/* TraceCreate -- create a Trace object
 *
 * Allocates and initializes a new Trace object with a TraceId
 * which is not currently active.
 *
 * Returns ResLIMIT if there aren't any available trace IDs.
 *
 * Trace objects are allocated directly from a small array in the
 * arena structure which is indexed by the TraceId.  This is so
 * that it's always possible to start a trace (provided there's
 * a free TraceId) even if there's no available memory.
 *
 * This code is written to be adaptable to allocating Trace
 * objects dynamically.
 */

Res TraceCreate(Trace *traceReturn, Arena arena)
{
  TraceId ti;
  Trace trace;

  AVER(TRACE_MAX == 1);         /* .single-collection */

  AVER(traceReturn != NULL);
  AVERT(Arena, arena);

  /* Find a free trace ID */
  for(ti = 0; ti < TRACE_MAX; ++ti)
    if(!TraceSetIsMember(arena->busyTraces, ti))
      goto found;

  return ResLIMIT;              /* no trace IDs available */

found:
  trace = ArenaTrace(arena, ti);
  AVER(trace->sig == SigInvalid);       /* design.mps.arena.trace.invalid */
  arena->busyTraces = TraceSetAdd(arena->busyTraces, ti);

  trace->arena = arena;
  trace->white = RefSetEMPTY;
  trace->mayMove = RefSetEMPTY;
  trace->ti = ti;
  trace->state = TraceINIT;
  trace->emergency = FALSE;
  trace->condemned = (Size)0;   /* nothing condemned yet */
  trace->foundation = (Size)0;  /* nothing grey yet */
  trace->rate = (Size)0;        /* no scanning to be done yet */
  trace->rootScanCount = (Count)0;
  trace->rootScanSize = (Size)0;
  trace->rootCopiedSize = (Size)0;
  trace->segScanCount = (Count)0;
  trace->segScanSize = (Size)0;
  trace->segCopiedSize = (Size)0;
  trace->singleScanCount = (Count)0;
  trace->singleScanSize = (Size)0;
  trace->singleCopiedSize = (Size)0;
  trace->fixRefCount = (Count)0;
  trace->segRefCount = (Count)0;
  trace->whiteSegRefCount = (Count)0;
  trace->nailCount = (Count)0;
  trace->snapCount = (Count)0;
  trace->forwardCount = (Count)0;
  trace->faultCount = (Count)0;
  trace->reclaimCount = (Count)0;
  trace->reclaimSize = (Size)0;
  trace->sig = TraceSig;
  AVERT(Trace, trace);

  *traceReturn = trace;
  return ResOK;
}


/* TraceDestroy -- destroy a trace object
 *
 * Finish and deallocate a Trace object, freeing up a TraceId.
 *
 * This code does not allow a Trace to be destroyed while it is
 * active.  It would be possible to allow this, but the colours
 * of segments etc. would need to be reset to black.
 */

void TraceDestroy(Trace trace)
{
  AVERT(Trace, trace);

  AVER(trace->state == TraceFINISHED);

  trace->sig = SigInvalid;
  trace->arena->busyTraces =
    TraceSetDel(trace->arena->busyTraces, trace->ti);
  trace->arena->flippedTraces =
    TraceSetDel(trace->arena->flippedTraces, trace->ti);
  EVENT_P(TraceDestroy, trace);
}


/* TraceFlipBuffers -- flip all buffers in the arena */

static void TraceFlipBuffers(Arena arena)
{
  Ring poolRing, poolNode, bufferRing, bufferNode;

  AVERT(Arena, arena);

  poolRing = ArenaPoolRing(arena);
  poolNode = RingNext(poolRing);
  while(poolNode != poolRing) {
    Ring poolNext = RingNext(poolNode);
    Pool pool = RING_ELT(Pool, arenaRing, poolNode);

    AVERT(Pool, pool);

    bufferRing = &pool->bufferRing;
    bufferNode = RingNext(bufferRing);
    while(bufferNode != bufferRing) {
      Ring bufferNext = RingNext(bufferNode);
      Buffer buffer = RING_ELT(Buffer, poolRing, bufferNode);

      AVERT(Buffer, buffer);

      BufferFlip(buffer);

      bufferNode = bufferNext;
    }

    poolNode = poolNext;
  }
}


/* TraceFlip -- blacken the mutator */

Res TraceFlip(Trace trace)
{
  Ring ring;
  Ring node, nextNode;
  Arena arena;
  ScanStateStruct ss;
  Rank rank;
  Res res;

  AVERT(Trace, trace);

  arena = trace->arena;
  ShieldSuspend(arena);

  AVER(trace->state == TraceUNFLIPPED);
  AVER(!TraceSetIsMember(arena->flippedTraces, trace->ti));

  EVENT_PP(TraceFlipBegin, trace, arena);

  TraceFlipBuffers(arena);

  /* Update location dependency structures. */
  /* mayMove is a conservative approximation of the refset of refs */
  /* which may move during this collection. */
  if(trace->mayMove != RefSetEMPTY) {
    LDAge(arena, trace->mayMove);
  }

  /* At the moment we must scan all roots, because we don't have */
  /* a mechanism for shielding them.  There can't be any weak or */
  /* final roots either, since we must protect these in order to */
  /* avoid scanning them too early, before the pool contents. */

  /* @@@@ This isn't correct if there are higher ranking roots than */
  /* data in pools. */

  ScanStateInit(&ss, TraceSetSingle(trace->ti),
                arena, RankAMBIG, trace->white);

  for(ss.rank = RankAMBIG; ss.rank <= RankEXACT; ++ss.rank) {
    ring = ArenaRootRing(arena);
    node = RingNext(ring);

    AVERT(ScanState, &ss);

    while(node != ring) {
      Ring next = RingNext(node);
      Root root = RING_ELT(Root, arenaRing, node);

      AVER(RootRank(root) <= RankEXACT); /* see above */

      if(RootRank(root) == ss.rank) {
        ScanStateSetSummary(&ss, RefSetEMPTY);
        res = RootScan(&ss, root);
        ++trace->rootScanCount;
        if(res != ResOK) {
          return res;
        }
      }

      node = next;
    }
  }
  TraceUpdateCounts(trace, &ss, TraceAccountingPhaseRootScan);

  ScanStateFinish(&ss);

  /* .flip.alloc: Allocation needs to become black now. While we flip */
  /* at the start, we can get away with always allocating black. This */
  /* needs to change when we flip later (i.e. have a read-barrier     */
  /* collector), so that we allocate grey or white before the flip    */
  /* and black afterwards. For instance, see                          */
  /* design.mps.poolams.invariant.alloc.                              */

  /* Now that the mutator is black we must prevent it from reading */
  /* grey objects so that it can't obtain white pointers.  This is */
  /* achieved by read protecting all segments containing objects */
  /* which are grey for any of the flipped traces. */
  for(rank = 0; rank < RankMAX; ++rank)
    RING_FOR(node, ArenaGreyRing(arena, rank), nextNode) {
      Seg seg = SegOfGreyRing(node);
      if(TraceSetInter(SegGrey(seg),
                       arena->flippedTraces) == TraceSetEMPTY &&
         TraceSetIsMember(SegGrey(seg), trace->ti))
        ShieldRaise(arena, seg, AccessREAD);
    }

  /* @@@@ When write barrier collection is implemented, this is where */
  /* write protection should be removed for all segments which are */
  /* no longer blacker than the mutator.  Possibly this can be done */
  /* lazily as they are touched. */

  /* Mark the trace as flipped. */
  trace->state = TraceFLIPPED;
  arena->flippedTraces = TraceSetAdd(arena->flippedTraces, trace->ti);

  EVENT_PP(TraceFlipEnd, trace, arena);

  ShieldResume(arena);

  return ResOK;
}


static void TraceReclaim(Trace trace)
{
  Arena arena;
  Seg seg;

  AVERT(Trace, trace);
  AVER(trace->state == TraceRECLAIM);


  EVENT_P(TraceReclaim, trace);
  arena = trace->arena;
  if(SegFirst(&seg, arena)) {
    Addr base;
    do {
      base = SegBase(seg);

      /* There shouldn't be any grey stuff left for this trace. */
      AVER_CRITICAL(!TraceSetIsMember(SegGrey(seg), trace->ti));

      if(TraceSetIsMember(SegWhite(seg), trace->ti)) {
        AVER_CRITICAL((SegPool(seg)->class->attr & AttrGC) != 0);

        ++trace->reclaimCount;
        PoolReclaim(SegPool(seg), trace, seg);

        /* If the segment still exists, it should no longer be white. */
        /* Note that the seg returned by this SegOfAddr may not be */
        /* the same as the one above, but in that case it's new and */
        /* still shouldn't be white for this trace. */

	/* The code from the class-specific reclaim methods to */
	/* unwhiten the segment could in fact be moved here.   */
        {
          Seg nonWhiteSeg = NULL;	/* prevents compiler warning */
	  AVER_CRITICAL(!(SegOfAddr(&nonWhiteSeg, arena, base) &&
		        TraceSetIsMember(SegWhite(nonWhiteSeg), trace->ti)));
        }
      }
    } while(SegNext(&seg, arena, base));
  }

  trace->state = TraceFINISHED;
}


/* traceFindGrey -- find a grey segment
 *
 * This function finds a segment which is grey for any of the traces
 * in ts and which does not have a higher rank than any other such
 * segment (i.e. a next segment to scan).
 *
 * This is equivalent to choosing a grey node from the grey set
 * of a partition.
 */

static Bool traceFindGrey(Seg *segReturn, Rank *rankReturn,
                          Arena arena, TraceId ti)
{
  Rank rank;
  Trace trace;
  Ring node, nextNode;

  AVER(segReturn != NULL);
  AVERT(Arena, arena);
  AVER(TraceIdCheck(ti));

  trace = ArenaTrace(arena, ti);

  for(rank = 0; rank < RankMAX; ++rank) {
    RING_FOR(node, ArenaGreyRing(arena, rank), nextNode) {
      Seg seg = SegOfGreyRing(node);
      AVERT(Seg, seg);
      AVER(SegGrey(seg) != TraceSetEMPTY);
      AVER(RankSetIsMember(SegRankSet(seg), rank));
      if(TraceSetIsMember(SegGrey(seg), ti)) {
        *segReturn = seg;
        *rankReturn = rank;
        return TRUE;
      }
    }
  }

  /* There are no grey segments for this trace. */

  return FALSE;
}


/* ScanStateSetSummary -- set the summary of scanned references
 *
 * This function sets unfixedSummary and fixedSummary such that
 * ScanStateSummary will return the summary passed.  Subsequently
 * fixed references are accumulated into this result.
 */

void ScanStateSetSummary(ScanState ss, RefSet summary)
{
  AVERT(ScanState, ss);

  ss->unfixedSummary = RefSetEMPTY;
  ss->fixedSummary = summary;
  AVER(ScanStateSummary(ss) == summary);
}

/* ScanStateSummary -- calculate the summary of scanned references
 *
 * The summary of the scanned references is the summary of the
 * unfixed references, minus the white set, plus the summary of the
 * fixed references.  This is because TraceFix is called for all
 * references in the white set, and accumulates a summary of
 * references after they have been fixed.
 */

RefSet ScanStateSummary(ScanState ss)
{
  AVERT(ScanState, ss);

  return TraceSetUnion(ss->fixedSummary,
                       TraceSetDiff(ss->unfixedSummary, ss->white));
}


/* TraceScan -- scan a segment to remove greyness
 *
 * @@@@ During scanning, the segment should be write-shielded to
 * prevent any other threads from updating it while fix is being
 * applied to it (because fix is not atomic).  At the moment, we
 * don't bother, because we know that all threads are suspended.
 */

static Res TraceScan(TraceSet ts, Rank rank,
                     Arena arena, Seg seg)
{
  Bool wasTotal;
  RefSet white;
  Res res;
  ScanStateStruct ss;
  TraceId ti;

  AVER(TraceSetCheck(ts));
  AVER(RankCheck(rank));
  AVERT(Seg, seg);

  /* The reason for scanning a segment is that it's grey. */
  AVER(TraceSetInter(ts, SegGrey(seg)) != TraceSetEMPTY);
  EVENT_UUPPP(TraceScan, ts, rank, arena, seg, &ss);

  white = RefSetEMPTY;
  for(ti = 0; ti < TRACE_MAX; ++ti)
    if(TraceSetIsMember(ts, ti))
      white = RefSetUnion(white, ArenaTrace(arena, ti)->white);

  /* only scan a segment if it refers to the white set */
  if(RefSetInter(white, SegSummary(seg)) == RefSetEMPTY) { /* blacken it */
    PoolBlacken(SegPool(seg), ts, seg);
    /* setup result code to return later */
    res = ResOK;
  } else {  /* scan it */
    ScanStateInit(&ss, ts, arena, rank, white);

    /* Expose the segment to make sure we can scan it. */
    ShieldExpose(arena, seg);

    res = PoolScan(&wasTotal, &ss, SegPool(seg), seg);
    /* Cover, regardless of result */
    ShieldCover(arena, seg);

    /* following is true whether or not scan was total */
    /* See design.mps.scan.summary.subset. */
    AVER(RefSetSub(ss.unfixedSummary, SegSummary(seg)));

    if(res != ResOK || !wasTotal) {
      /* scan was partial, so... */
      /* scanned summary should be ORed into segment summary. */
      SegSetSummary(seg, RefSetUnion(SegSummary(seg),
                                     ScanStateSummary(&ss)));
    } else {
      /* all objects on segment have been scanned, so... */
      /* scanned summary should replace the segment summary. */
      SegSetSummary(seg, ScanStateSummary(&ss));
    }

    for(ti = 0; ti < TRACE_MAX; ++ti)
      if(TraceSetIsMember(ts, ti)) {
        Trace trace = ArenaTrace(arena, ti);

        ++trace->segScanCount;
	TraceUpdateCounts(trace, &ss, TraceAccountingPhaseSegScan);
      }
    ScanStateFinish(&ss);
  }

  if(res == ResOK) {
    /* The segment is now black only if scan was successful. */
    /* Remove the greyness from it. */
    SegSetGrey(seg, TraceSetDiff(SegGrey(seg), ts));
  }

  return res;
}


void TraceAccess(Arena arena, Seg seg, AccessSet mode)
{
  Res res;
  TraceId ti;

  AVERT(Arena, arena);
  AVERT(Seg, seg);
  UNUSED(mode);

  /* If it's a read access, then the segment must be grey for a trace */
  /* which is flipped. */
  AVER((mode & SegSM(seg) & AccessREAD) == 0 ||
       TraceSetInter(SegGrey(seg), arena->flippedTraces) !=
       TraceSetEMPTY);

  /* If it's a write acess, then the segment must have a summary that */
  /* is smaller than the mutator's summary (which is assumed to be */
  /* RefSetUNIV). */
  AVER((mode & SegSM(seg) & AccessWRITE) == 0 ||
       SegSummary(seg) != RefSetUNIV);

  EVENT_PPU(TraceAccess, arena, seg, mode);

  if((mode & SegSM(seg) & AccessREAD) != 0) {     /* read barrier? */
    /* .scan.conservative: At the moment we scan at RankEXACT.  Really */
    /* we should be scanning at the "phase" of the trace, which is the */
    /* minimum rank of all grey segments. */
    /* design.mps.poolamc.access.multi @@@@ tag correct?? */

    /* Pick set of traces to scan for: */
    /* @@@@ Should just be flipped traces? */
    TraceSet traces = arena->busyTraces;
    res = TraceScan(traces, RankEXACT, arena, seg);
    if(res != ResOK) {
      /* enter emergency tracing mode */
      for(ti = 0; ti < TRACE_MAX; ++ti) {
        if(TraceSetIsMember(traces, ti)) {
	  ArenaTrace(arena, ti)->emergency = TRUE;
	}
      }
      res = TraceScan(traces, RankEXACT, arena, seg);
      AVER(res == ResOK);
    }

    /* The pool should've done the job of removing the greyness that */
    /* was causing the segment to be protected, so that the mutator */
    /* can go ahead and access it. */
    AVER(TraceSetInter(SegGrey(seg), arena->flippedTraces) == TraceSetEMPTY);

    for(ti = 0; ti < TRACE_MAX; ++ti)
      if(TraceSetIsMember(arena->busyTraces, ti))
        ++ArenaTrace(arena, ti)->faultCount;
  }

  /* The write barrier handling must come after the read barrier, */
  /* because the latter may set the summary and raise the write barrier. */

  if((mode & SegSM(seg) & AccessWRITE) != 0)      /* write barrier? */
    SegSetSummary(seg, RefSetUNIV);

  /* The segment must now be accessible. */
  AVER((mode & SegSM(seg)) == AccessSetEMPTY);
}


static Res TraceRun(Trace trace)
{
  Res res;
  Arena arena;
  Seg seg;
  Rank rank;

  AVERT(Trace, trace);
  AVER(trace->state == TraceFLIPPED);

  arena = trace->arena;

  if(traceFindGrey(&seg, &rank, arena, trace->ti)) {
    AVER((SegPool(seg)->class->attr & AttrSCAN) != 0);
    res = TraceScan(TraceSetSingle(trace->ti), rank,
                    arena, seg);
    if(res != ResOK)
      return res;
  } else
    trace->state = TraceRECLAIM;

  return ResOK;
}

/* TraceExpedite -- signals an emergency on the trace and */
/* moves it to the Finished state. */
static void TraceExpedite(Trace trace)
{
  AVERT(Trace, trace);

  /* check trace is not in INIT state.  If the trace was in the */
  /* INIT state, then TraceStep would not progress it so the loop */
  /* would never terminate (see .step.noprogress) */
  AVER(trace->state != TraceINIT);

  trace->emergency = TRUE;

  while(trace->state != TraceFINISHED) {
    Res res = TraceStep(trace);
    /* because we are using emergencyFix the trace shouldn't */
    /* raise any error conditions */
    AVER(res == ResOK);
  }
}


/* TraceStep -- progresses a trace by some small amount
 *
 */

Res TraceStep(Trace trace)
{
  Arena arena;
  Res res;

  AVERT(Trace, trace);

  arena = trace->arena;

  EVENT_PP(TraceStep, trace, arena);

  switch(trace->state) {
  case TraceUNFLIPPED:
    /* all traces are flipped in TraceStart at the moment */
    NOTREACHED;
    res = TraceFlip(trace);
    if(res != ResOK)
      return res;
    break;

  case TraceFLIPPED:
    res = TraceRun(trace);
    if(res != ResOK)
      return res;
    break;

  case TraceRECLAIM:
    TraceReclaim(trace);
    break;

  case TraceFINISHED:
  case TraceINIT:
    /* .step.noprogress: no progress in either of these two states. */
    /* @@@@ in fact, should we ever see a trace in the INIT state? */
    NOOP;
    break;

  default:
    NOTREACHED;
    break;
  }

  return ResOK;
}


/* TracePoll -- progresses a trace, without returning errors */

void TracePoll(Trace trace)
{
  Res res;

  AVERT(Trace, trace);

  res = TraceStep(trace);
  if(res != ResOK) {
    /* check res is expected failure code */
    AVER(res == ResMEMORY ||
         res == ResRESOURCE);
    TraceExpedite(trace);
    AVER(trace->state == TraceFINISHED);
  }
}


/* TraceGreyEstimate -- estimate amount of grey stuff
 *
 * This function returns an estimate of the total size (in bytes)
 * of objects which would need to be scanned in order to find
 * all references to a certain RefSet.
 *
 * @@@@ This currently assumes that it's everything in the world.
 * @@@@ Should factor in the size of the roots, especially if the stack
 * is currently very deep.
 */

Size TraceGreyEstimate(Arena arena, RefSet refSet)
{
  UNUSED(refSet);
  return ArenaCommitted(arena);
}


Res TraceFix(ScanState ss, Ref *refIO)
{
  Ref ref;
  Seg seg;
  Pool pool;

  /* See design.mps.trace.fix.noaver */
  AVERT_CRITICAL(ScanState, ss);
  AVER_CRITICAL(refIO != NULL);

  ref = *refIO;

  ++ss->fixRefCount;

  EVENT_PPAU(TraceFix, ss, refIO, ref, ss->rank);

  /* SegOfAddr is inlined, see design.mps.trace.fix.segofaddr */
  if(SEG_OF_ADDR(&seg, ss->arena, ref)) {
    ++ss->segRefCount;
    EVENT_P(TraceFixSeg, seg);
    if(TraceSetInter(SegWhite(seg), ss->traces) != TraceSetEMPTY) {
      Res res;

      ++ss->whiteSegRefCount;
      EVENT_0(TraceFixWhite);
      pool = SegPool(seg);
      /* Could move the rank switch here from the class-specific */
      /* fix methods. */
      res = PoolFix(pool, ss, seg, refIO);
      if(res != ResOK)
        return res;
    }
  } else {
    /* See .exact.legal */
    AVER(ss->rank < RankEXACT ||
	 !ArenaIsReservedAddr(ss->arena, ref));
  }


  /* See .fix.fixed.all */
  ss->fixedSummary = RefSetAdd(ss->arena, ss->fixedSummary, *refIO);

  return ResOK;
}


Res TraceFixEmergency(ScanState ss, Ref *refIO)
{
  Ref ref;
  Seg seg;
  Pool pool;

  AVERT(ScanState, ss);
  AVER(refIO != NULL);

  ref = *refIO;

  ++ss->fixRefCount;

  EVENT_PPAU(TraceFix, ss, refIO, ref, ss->rank);

  /* SegOfAddr is inlined, see design.mps.trace.fix.segofaddr */
  if(SEG_OF_ADDR(&seg, ss->arena, ref)) {
    ++ss->segRefCount;
    EVENT_P(TraceFixSeg, seg);
    if(TraceSetInter(SegWhite(seg), ss->traces) != TraceSetEMPTY) {
      ++ss->whiteSegRefCount;
      EVENT_0(TraceFixWhite);
      pool = SegPool(seg);
      PoolFixEmergency(pool, ss, seg, refIO);
    }
  } else {
    /* See .exact.legal */
    AVER(ss->rank < RankEXACT ||
	 !ArenaIsReservedAddr(ss->arena, ref));
  }

  /* See .fix.fixed.all */
  ss->fixedSummary = RefSetAdd(ss->arena, ss->fixedSummary, *refIO);

  return ResOK;
}


Res TraceScanSingleRef(TraceSet ts, Arena arena, 
                       Seg seg, Rank rank, Ref *refIO)
{
  RefSet summary;
  RefSet white;
  ScanStateStruct ss;
  TraceId ti;
  Res res;

  AVER(TraceSetCheck(ts));
  AVERT(Arena, arena);
  AVER(SegCheck(seg));
  AVER(RankCheck(rank));
  AVER(refIO != NULL);

  white = RefSetEMPTY;
  for(ti = 0; ti < TRACE_MAX; ++ti) {
    if(TraceSetIsMember(ts, ti)) {
      white = RefSetUnion(white, ArenaTrace(arena, ti)->white);
    }
  }

  if(RefSetInter(SegSummary(seg), white) == RefSetEMPTY) {
    return ResOK;
  }

  ScanStateInit(&ss, ts, arena, rank, white);
  ShieldExpose(arena, seg);

  TRACE_SCAN_BEGIN(&ss) {
    res = TRACE_FIX(&ss, refIO);
  } TRACE_SCAN_END(&ss);
  ss.scannedSize = sizeof *refIO;

  summary = SegSummary(seg);
  summary = RefSetAdd(arena, summary, *refIO);
  SegSetSummary(seg, summary);
  ShieldCover(arena, seg);

  for(ti = 0; ti < TRACE_MAX; ++ti) {
    if(TraceSetIsMember(ts, ti)) {
      TraceUpdateCounts(ArenaTrace(arena, ti), &ss,
                        TraceAccountingPhaseSingleScan);
    }
  }
  return res;
}


/* TraceScanArea -- scan contiguous area of references
 *
 * This is a convenience function for scanning the contiguous area
 * [base, limit).  i.e. it calls fix on all words from base up
 * to limit, inclusive of base and exclusive of limit.
 */

Res TraceScanArea(ScanState ss, Addr *base, Addr *limit)
{
  Res res;
  Addr *p;
  Ref ref;

  AVER(base != NULL);
  AVER(limit != NULL);
  AVER(base < limit);

  EVENT_PPP(TraceScanArea, ss, base, limit);

  TRACE_SCAN_BEGIN(ss) {
    p = base;
  loop:
    if(p >= limit) goto out;
    ref = *p++;
    if(!TRACE_FIX1(ss, ref))
      goto loop;
    res = TRACE_FIX2(ss, p-1);
    if(res == ResOK)
      goto loop;
    return res;
  out:
    AVER(p == limit);
  } TRACE_SCAN_END(ss);

  return ResOK;
}


/* TraceScanAreaTagged -- scan contiguous area of tagged references
 *
 * This is as TraceScanArea except words are only fixed if they are
 * tagged as Dylan references (i.e. bottom two bits are zero).
 * @@@@ This Dylan-specificness should be generalized in some way.
 */

Res TraceScanAreaTagged(ScanState ss, Addr *base, Addr *limit)
{
  return TraceScanAreaMasked(ss, base, limit, (Word)3);
}


/* TraceScanAreaMasked -- scan contiguous area of filtered references
 *
 * This is as TraceScanArea except words are only fixed if they
 * are zero when masked with a mask.
 */

Res TraceScanAreaMasked(ScanState ss, Addr *base, Addr *limit, Word mask)
{
  Res res;
  Addr *p;
  Ref ref;

  AVER(base != NULL);
  AVER(limit != NULL);
  AVER(base < limit);

  EVENT_PPP(TraceScanAreaTagged, ss, base, limit);

  TRACE_SCAN_BEGIN(ss) {
    p = base;
  loop:
    if(p >= limit) goto out;
    ref = *p++;
    if(((Word)ref & mask) != 0) goto loop;
    if(!TRACE_FIX1(ss, ref)) goto loop;
    res = TRACE_FIX2(ss, p-1);
    if(res == ResOK)
      goto loop;
    return res;
  out:
    AVER(p == limit);
  } TRACE_SCAN_END(ss);

  return ResOK;
}
