// $Id$
// Author: John Wu <John.Wu at ACM.org>
// Copyright 2006-2009 the Regents of the University of California
//
// This file contains the implementation of the class ibis::fuzz, a
// unbinned version of interval-equality encoded index.
//
// In fuzzy clustering/classification, there are extensive use of interval
// equality condition.  Hence the funny name.
//
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifiers longer than 256 characters
#endif
#include "irelic.h"
#include "part.h"

#include <cmath>	// std::fabs
#include <sstream>	// std::ostringstream

////////////////////////////////////////////////////////////////////////
ibis::fuzz::fuzz(const ibis::column *c, const char *f)
    : ibis::relic(c, f) {
    if (c == 0) return; // nothing to do
    if (cbits.empty() || cbits.size()+1 != cbounds.size()) {
	if (fname != 0)
	    readCoarse(f);
	else
	    coarsen();
    }
    if (ibis::gVerbose > 4) {
	ibis::util::logger lg;
	print(lg.buffer());
    }
} // ibis::fuzz::fuzz

/// Reconstruct from content of fileManager::storage.
/**
   The leading portion of the index file is the same as ibis::relic, which
   allows the constructor of the base class to work properly.  The content
   following the last bitvector in ibis::relic is as follows, @sa
   ibis::fuzz::writeCoarse.

   nc      (uint32_t)                   -- number of coarse bins.
   cbounds (uint32_t[nc+1])             -- boundaries of the coarse bins.
   coffsets(int32_t[nc-ceil(nc/2)+2])   -- starting positions.
   cbits   (bitvector[nc-ceil(nc/2)+1]) -- bitvectors.
 */
ibis::fuzz::fuzz(const ibis::column* c, ibis::fileManager::storage* st,
		 uint32_t start) : ibis::relic(c, st, start) {
    if (st->size() <= static_cast<uint32_t>(offset32.back()))
	return; // no coarse bin

    start = offset32.back();
    uint32_t nc = *(reinterpret_cast<uint32_t*>(st->begin()+start));
    if (nc == 0 ||
	st->size() <= start + (sizeof(int32_t)+sizeof(uint32_t))*(nc+1))
	return;

    const uint32_t ncb = nc - (nc+1)/2 + 1;
    start += sizeof(uint32_t);
    if (start+sizeof(uint32_t)*(nc+1) < st->size()) {
	array_t<uint32_t> tmp(st, start, nc+1);
	cbounds.swap(tmp);
    }
    start += sizeof(uint32_t) * (nc+1);
    if (start+sizeof(int32_t)*(ncb+1) < st->size()) {
	array_t<int32_t> tmp(st, start, ncb+1);
	coffsets.swap(tmp);
	if (coffsets.back() > static_cast<int32_t>(st->size())) {
	    coffsets.swap(tmp);
	    array_t<uint32_t> tmp2;
	    cbounds.swap(tmp2);
	    return;
	}
    }
    else {
	array_t<uint32_t> tmp2;
	cbounds.swap(tmp2);
	return;
    }

    cbits.resize(ncb);
    for (unsigned i = 0; i < ncb; ++ i)
	cbits[i] = 0;

    if (st->isFileMap()) {
#if defined(FASTBIT_READ_BITVECTOR0)
	array_t<ibis::bitvector::word_t>
	    a0(st, coffsets[0], (coffsets[1] - coffsets[0])
	       / sizeof(ibis::bitvector::word_t));
	cbits[0] = new ibis::bitvector(a0);
	cbits[0]->sloppySize(nrows);
#endif
    }
    else { // all bytes in memory already
	for (unsigned i = 0; i < ncb; ++ i) {
	    if (coffsets[i+1] > coffsets[i]) {
		array_t<ibis::bitvector::word_t>
		    a(st, coffsets[i], (coffsets[i+1]-coffsets[i])
		      / sizeof(ibis::bitvector::word_t));
		cbits[i] = new ibis::bitvector(a);
		cbits[i]->sloppySize(nrows);
	    }
	}
    }

    if (ibis::gVerbose > 4) {
	ibis::util::logger lg;
	print(lg.buffer());
    }
} // ibis::fuzz::fuzz

long ibis::fuzz::append(const char* dt, const char* df, uint32_t nnew) {
    long ret = ibis::relic::append(dt, df, nnew);
    if (ret <= 0 || static_cast<uint32_t>(ret) != nnew)
	return ret;

    if (nrows == col->partition()->nRows())
	coarsen();
    return ret;
} // ibis::fuzz::append

// fill the offsets array, and divide the bitmaps into groups according to
// the sizes (bytes) of the bitmaps
void ibis::fuzz::coarsen() {
    const uint32_t nbits = bits.size();
    if (offset32.size() != nbits+1) {
	offset32.resize(nbits+1);
	offset32[0] = 0;
	for (unsigned i = 0; i < nbits; ++ i)
	    offset32[i+1] = offset32[i] + (bits[i] ? bits[i]->bytes() : 0U);
    }
    if (vals.size() < 32) return; // don't construct the coarse level
    if (cbits.size() > 0 && cbits.size()+1 == coffsets.size()) return;

    unsigned ncoarse = 0;
    if (col != 0) { // user specified value
	const char* spec = col->indexSpec();
	if (spec != 0 && *spec != 0 && strstr(spec, "ncoarse=") != 0) {
	    // number of coarse bins specified explicitly
	    const char* tmp = 8+strstr(spec, "ncoarse=");
	    unsigned j = atoi(tmp);
	    if (j > 4)
		ncoarse = j;
	}
    }
    // default size based on the size of fine level index sf: sf(w-1)/N/sqrt(2)
    if (ncoarse < 5U && offset32.back() > offset32[0]+nrows/31U) {
	ncoarse = sizeof(ibis::bitvector::word_t);
	const int wm1 = ncoarse*8-1;
	const long sf = (offset32.back()-offset32[0]) / ncoarse;
	ncoarse = static_cast<unsigned>(wm1*sf/(sqrt(2.0)*nrows));
	const unsigned ncmax = (unsigned) sqrt(2.0 * vals.size());
	if (ncoarse < ncmax) {
	    const double obj1 = (sf+(ncoarse+1-ceil(0.5*ncoarse))*nrows/wm1)
		*(sf*0.5/ncoarse+2.0*nrows/wm1);
	    const double obj2 = (sf+(ncoarse+2-ceil(0.5*ncoarse+0.5))*nrows/wm1)
		*(sf*0.5/(ncoarse+1.0)+2.0*nrows/wm1);
	    ncoarse += (obj2 < obj1);
	}
	else {
	    ncoarse = ncmax;
	}
    }
    if (ncoarse < 5 || ncoarse >= vals.size()) return;

    const uint32_t nc2 = (ncoarse + 1) / 2;
    const uint32_t ncb = ncoarse - nc2 + 1; // # of coarse level bitmaps
    // partition the fine level bitmaps into groups with nearly equal
    // number of bytes
    cbounds.resize(ncoarse+1);
    cbounds[0] = 0;
    for (unsigned i = 1; i < ncoarse; ++ i) {
	int32_t target = offset32[cbounds[i-1]] +
	    (offset32.back() - offset32[cbounds[i-1]]) / (ncoarse - i + 1);
	cbounds[i] = offset32.find(target);
	if (cbounds[i] > cbounds[i-1]+1 &&
	    offset32[cbounds[i]]-target > target-offset32[cbounds[i]-1])
	    -- (cbounds[i]);
	else if (cbounds[i] <= cbounds[i-1])
	    cbounds[i] = cbounds[i-1]+1;
    }
    cbounds[ncoarse] = nbits; // end with the last fine level bitmap
    for (unsigned i = ncoarse-1; i > 0 && cbounds[i+1] < cbounds[i]; -- i)
	cbounds[i] = cbounds[i+1] - 1;
    if (ibis::gVerbose > 2) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::fuzz::coarsen will divide " << bits.size()
		    << " bitmaps into " << ncoarse << " groups\n";
	for (unsigned i = 0; i < cbounds.size(); ++ i)
	    lg.buffer() << cbounds[i] << " ";
    }
    // fill cbits
    for (unsigned i = 0; i < cbits.size(); ++ i) {
	delete cbits[i];
	cbits[i] = 0;
    }
    cbits.resize(ncb);
    cbits[0] = new ibis::bitvector();
    sumBins(0, cbounds[nc2], *(cbits[0]));
    for (unsigned i = 1; i < ncb; ++ i) {
	ibis::bitvector front, back;
	sumBins(cbounds[i-1], cbounds[i], front);
	sumBins(cbounds[i-1+nc2], cbounds[i+nc2], back);
	cbits[i] = new ibis::bitvector(*(cbits[i-1]));
	*(cbits[i]) -= front;
	*(cbits[i]) |= back;
    }

    // fill coffsets
    coffsets.resize(ncb+1);
    coffsets[0] = 0;
    for (unsigned i = 0; i < ncb; ++ i) {
	cbits[i]->compress();
	coffsets[i+1] = coffsets[i] + cbits[i]->bytes();
    }
} // ibis::fuzz::coarsen

void ibis::fuzz::activateCoarse() const {
    const uint32_t nobs = cbits.size();
    bool missing = false; // any bits[i] missing (is 0)?
    ibis::column::mutexLock lock(col, "fuzz::activateCoarse");
    for (uint32_t i = 0; i < nobs && ! missing; ++ i)
	missing = (cbits[i] == 0);
    if (missing == false) return;

    if (coffsets.size() <= nobs || coffsets[0] <= offset32.back()) {
	col->logWarning("fuzz::activateCoarse", "no records of coffsets, "
			"can not regenerate the bitvectors");
    }
    else if (str) { // using a ibis::fileManager::storage as back store
	LOGGER(ibis::gVerbose > 8)
	    << "ibis::column[" << col->name()
	    << "]::fuzz::activateCoarse(all) retrieving data from "
	    "ibis::fileManager::storage(0x" << str << ")";

	for (uint32_t i = 0; i < nobs; ++i) {
	    if (cbits[i] == 0 && coffsets[i+1] > coffsets[i]) {
#if defined(DEBUG)
		LOGGER(ibis::gVerbose >= 0)
		    << "fuzz::activateCoarse -- activating bitvector "
		    << i << " from a raw storage ("
		    << static_cast<const void*>(str->begin())
		    << "), coffsets[" << i << "]= " << coffsets[i]
		    << ", coffsets[" << i+1 << "]= " << coffsets[i+1];
#endif
		array_t<ibis::bitvector::word_t>
		    a(str, coffsets[i], (coffsets[i+1]-coffsets[i]) /
		      sizeof(ibis::bitvector::word_t));
		cbits[i] = new ibis::bitvector(a);
		cbits[i]->sloppySize(nrows);
	    }
	}
    }
    else if (fname) { // using the named file directly
	int fdes = UnixOpen(fname, OPEN_READONLY);
	if (fdes >= 0) {
	    LOGGER(ibis::gVerbose > 8)
		<< "ibis::column[" << col->name()
		<< "]::fuzz::activateCoarse(all) retrieving data from "
		"file \"" << fname << "\"";

#if defined(_WIN32) && defined(_MSC_VER)
	    (void)_setmode(fdes, _O_BINARY);
#endif
	    uint32_t i = 0;
	    while (i < nobs) {
		// skip to next empty bit vector
		while (i < nobs && cbits[i] != 0)
		    ++ i;
		// the last bitvector to activate. can not be larger
		// than j
		uint32_t aj = (i<nobs ? i + 1 : nobs);
		while (aj < nobs && cbits[aj] == 0)
		    ++ aj;
		if (coffsets[aj] > coffsets[i]) {
		    const uint32_t start = coffsets[i];
		    ibis::fileManager::storage *a0 = new
			ibis::fileManager::storage(fdes, start,
						   coffsets[aj]);
		    while (i < aj) {
			if (coffsets[i+1] > coffsets[i]) {
			    array_t<ibis::bitvector::word_t>
				a1(a0, coffsets[i]-start,
				   (coffsets[i+1]-coffsets[i])/
				   sizeof(ibis::bitvector::word_t));
			    bits[i] = new ibis::bitvector(a1);
			    bits[i]->sloppySize(nrows);
#if defined(DEBUG)
			    LOGGER(ibis::gVerbose >= 0)
				<< "fuzz::activateCoarse -- "
				"activating bitvector " << i
				<< "by reading file " << fname
				<< "coffsets[" << i << "]= " << coffsets[i]
				<< ", coffsets[" << i+1 << "]= "
				<< coffsets[i+1];
#endif
			}
			++ i;
		    }
		}
		i = aj; // always advance i
	    }
	    UnixClose(fdes);
	}
	else {
	    col->logWarning("fuzz::activateCoarse", "failed to open file \"%s\""
			    " ... %s", fname, strerror(errno));
	}
    }
    else {
	col->logWarning("fuzz::activateCoarse", "can not regenerate "
			"bitvectors because neither str or fname is "
			"specified");
    }
} // ibis::fuzz::activateCoarse

void ibis::fuzz::activateCoarse(uint32_t i) const {
    if (i >= bits.size()) return;	// index out of range
    ibis::column::mutexLock lock(col, "fuzz::activateCoarse");
    if (cbits[i] != 0) return;	// already active
    if (coffsets.size() <= cbits.size() || coffsets[0] <= offset32.back()) {
	col->logWarning("fuzz::activateCoarse", "no records of offsets, "
			"can not regenerate bitvector %lu",
			static_cast<long unsigned>(i));
    }
    else if (coffsets[i+1] <= coffsets[i]) {
	return;
    }
    if (str) { // using a ibis::fileManager::storage as back store
	LOGGER(ibis::gVerbose > 8)
	    << "ibis::column[" << col->name()
	    << "]::fuzz::activateCoarse(" << i
	    << ") retrieving data from ibis::fileManager::storage(0x"
	    << str << ")";

	array_t<ibis::bitvector::word_t>
	    a(str, coffsets[i], (coffsets[i+1]-coffsets[i]) /
	      sizeof(ibis::bitvector::word_t));
	cbits[i] = new ibis::bitvector(a);
	cbits[i]->sloppySize(nrows);
#if defined(DEBUG)
	LOGGER(ibis::gVerbose >= 0)
	    << "fuzz::activateCoarse(" << i
	    << ") constructed a bitvector from range ["
	    << coffsets[i] << ", " << coffsets[i+1] << ") of a storage at "
	    << static_cast<const void*>(str->begin());
#endif
    }
    else if (fname) { // using the named file directly
	int fdes = UnixOpen(fname, OPEN_READONLY);
	if (fdes >= 0) {
	    LOGGER(ibis::gVerbose > 8)
		<< "ibis::column[" << col->name()
		<< "]::fuzz::activateCoarse(" << i
		<< ") retrieving data from file \"" << fname << "\"";

#if defined(_WIN32) && defined(_MSC_VER)
	    (void)_setmode(fdes, _O_BINARY);
#endif
	    array_t<ibis::bitvector::word_t> a0(fdes, coffsets[i],
						coffsets[i+1]);
	    cbits[i] = new ibis::bitvector(a0);
	    cbits[i]->sloppySize(nrows);
	    UnixClose(fdes);
#if defined(DEBUG)
	    LOGGER(ibis::gVerbose >= 0)
		<< "fuzz::activateCoarse(" << i
		<< ") constructed a bitvector from range [" << coffsets[i]
		<< ", "  << coffsets[i+1] << ") of file " << fname;
#endif
	}
	else {
	    col->logWarning("activateCoarse",
			    "failed to open file \"%s\" ... %s",
			    fname, strerror(errno));
	}
    }
    else {
	col->logWarning("fuzz::activateCoarse", "can not regenerate "
			"bitvector %lu because neither str or fname is "
			"specified", static_cast<long unsigned>(i));
    }
} // ibis::fuzz::activateCoarse

void ibis::fuzz::activateCoarse(uint32_t i, uint32_t j) const {
    if (j > cbits.size())
	j = cbits.size();
    if (i >= j) // empty range
	return;
    ibis::column::mutexLock lock(col, "fuzz::activateCoarse");

    while (i < j && cbits[i] != 0) ++ i;
    if (i >= j) return; // requested bitvectors active

    if (coffsets.size() <= cbits.size() || coffsets[0] <= offset32.back()) {
	col->logWarning("fuzz::activateCoarse", "no records of offsets, "
			"can not regenerate bitvectors %lu:%lu",
			static_cast<long unsigned>(i),
			static_cast<long unsigned>(j));
    }
    else if (str) { // using an ibis::fileManager::storage as back store
	LOGGER(ibis::gVerbose > 8)
	    << "ibis::column[" << col->name()
	    << "]::fuzz::activateCoarse(" << i << ", " << j
	    << ") retrieving data from ibis::fileManager::storage(0x"
	    << str << ")";

	while (i < j) {
#if defined(DEBUG)
	    LOGGER(ibis::gVerbose >= 0)
		    << "DEBUG -- fuzz::activateCoarse "
		    << "constructing bitvector " << i << " from range ["
		    << coffsets[i] << ", " << coffsets[i+1]
		    << ") of a storage at "
		    << static_cast<const void*>(str->begin());
#endif
	    if (cbits[i] == 0 && coffsets[i+1] > coffsets[i]) {
		array_t<ibis::bitvector::word_t>
		    a(str, coffsets[i], (coffsets[i+1]-coffsets[i]) /
		      sizeof(ibis::bitvector::word_t));
		cbits[i] = new ibis::bitvector(a);
		cbits[i]->sloppySize(nrows);
	    }
	    ++ i;
	}
    }
    else if (fname) { // using the named file directly
	if (coffsets[j] > coffsets[i]) {
	    int fdes = UnixOpen(fname, OPEN_READONLY);
	    if (fdes >= 0) {
		LOGGER(ibis::gVerbose > 8)
		    << "ibis::column[" << col->name()
		    << "]::fuzz::activateCoarse(" << i << ", " << j
		    << ") retrieving data from file \"" << fname << "\"";

#if defined(_WIN32) && defined(_MSC_VER)
		(void)_setmode(fdes, _O_BINARY);
#endif
		while (i < j) {
		    // skip to next empty bit vector
		    while (i < j && cbits[i] != 0)
			++ i;
		    // the last bitvector to activate. can not be larger
		    // than j
		    uint32_t aj = (i<j ? i + 1 : j);
		    while (aj < j && cbits[aj] == 0)
			++ aj;
		    if (coffsets[aj] > coffsets[i]) {
			const uint32_t start = coffsets[i];
			ibis::fileManager::storage *a0 = new
			    ibis::fileManager::storage(fdes, start,
						       coffsets[aj]);
			while (i < aj) {
			    if (coffsets[i+1] > coffsets[i]) {
				array_t<ibis::bitvector::word_t>
				    a1(a0, coffsets[i]-start,
				       (coffsets[i+1]-coffsets[i])/
				       sizeof(ibis::bitvector::word_t));
				cbits[i] = new ibis::bitvector(a1);
				cbits[i]->sloppySize(nrows);
#if defined(DEBUG)
				LOGGER(ibis::gVerbose >= 0)
				    << "fuzz::activateCoarse(" << i
				    << ") constructed a bitvector from range ["
				    << coffsets[i] << ", " << coffsets[i+1]
				    << ") of file " << fname;
#endif
			    }
			    ++ i;
			}
		    }
		    i = aj; // always advance i
		}
		UnixClose(fdes);
	    }
	    else {
		col->logWarning("fuzz::activateCoarse",
				"failed to open file \"%s\" ... %s",
				fname, strerror(errno));
	    }
	}
    }
    else {
	col->logWarning("fuzz::activateCoarse", "can not regenerate "
			"bitvector %lu because neither str or fname is "
			"specified", static_cast<long unsigned>(i));
    }
} // ibis::fuzz::activateCoarse

uint32_t ibis::fuzz::estimate(const ibis::qContinuousRange& expr) const {
    ibis::bitvector bv;
    long ierr = evaluate(expr, bv);
    return static_cast<uint32_t>(ierr > 0 ? ierr : 0);
} // ibis::fuzz::estimate

long ibis::fuzz::coarseEstimate(uint32_t lo, uint32_t hi) const {
    long cost;
    const uint32_t mid = cbounds.size() / 2;
    if (lo >= cbounds.size() || lo >= hi) {
	cost = 0;
    }
    else if (hi > mid) {
	cost = coffsets[hi-mid+1] - coffsets[hi-mid];
	if (lo > hi-mid) {
	    if (lo >= mid)
		cost += coffsets[lo-mid+1] - coffsets[lo-mid];
	    else
		cost += coffsets[lo+1] - coffsets[lo];
	}
	else if (lo < hi-mid) {
	    cost += coffsets[lo+1] - coffsets[lo];
	}
    }
    else if (hi < mid) {
	cost = (coffsets[lo+1] - coffsets[lo])
	    + (coffsets[hi+1] - coffsets[hi]);
    }
    else { // hi == mid
	cost = coffsets[1] - coffsets[0];
	if (lo > 0) {
	    cost += coffsets[lo+1] - coffsets[lo];
	}
    }
    return cost;
} // ibis::fuzz::coarseEstimate

long ibis::fuzz::coarseEvaluate(uint32_t lo, uint32_t hi,
				ibis::bitvector &res) const {
    const uint32_t mid = cbounds.size() / 2;
    if (lo >= cbounds.size() || lo >= hi) {
	res.set(0, nrows);
    }
    else if (lo+1 == hi) { // two consecutive bitmaps used
	if (hi < cbits.size()) {
	    activateCoarse(lo, hi+1);
	    if (cbits[lo] != 0) {
		res.copy(*(cbits[lo]));
		if (cbits[hi] != 0)
		    res -= *(cbits[hi]);
	    }
	    else {
		res.set(0, nrows);
	    }
	}
	else {
	    activateCoarse(lo-mid, lo-mid+2);
	    if (cbits[hi-mid] != 0) {
		res.copy(*(cbits[hi-mid]));
		if (cbits[lo-mid] != 0)
		    res -= *(cbits[lo-mid]);
	    }
	    else {
		res.set(0, nrows);
	    }
	}
    }
    else if (hi > mid) {
	if (cbits[hi-mid] == 0)
	    activateCoarse(hi-mid);
	if (cbits[hi-mid] != 0)
	    res.copy(*(cbits[hi-mid]));
	else
	    res.set(0, nrows);
	if (lo > hi-mid) {
	    if (lo >= mid) {
		if (cbits[lo-mid] == 0)
		    activateCoarse(lo-mid);
		if (cbits[lo-mid] != 0)
		    res -= *(cbits[lo-mid]);
	    }
	    else {
		if (cbits[lo] == 0)
		    activateCoarse(lo);
		if (cbits[lo] != 0)
		    res &= *(cbits[lo]);
		else
		    res.set(0, nrows);
	    }
	}
	else if (lo < hi-mid) {
	    if (cbits[lo] == 0)
		activateCoarse(lo);
	    if (cbits[lo] != 0)
		res |= *(cbits[lo]);
	}
    }
    else if (hi < mid) {
	if (cbits[lo] == 0)
	    activateCoarse(lo);
	if (cbits[lo] != 0) {
	    res.copy(*(cbits[lo]));
	    if (cbits[hi] == 0)
		activateCoarse(hi);
	    if (cbits[hi] != 0)
		res -= *(cbits[hi]);
	}
	else {
	    res.set(0, nrows);
	}
    }
    else { // hi == mid
	if (cbits[0] == 0)
	    activateCoarse(0);
	if (cbits[0] != 0)
	    res.copy(*(cbits[0]));
	else
	    res.set(0, nrows);
	if (lo > 0) {
	    if (cbits[lo] == 0)
		activateCoarse(lo);
	    if (cbits[lo] != 0)
		res &= *(cbits[lo]);
	}
    }
    return res.size();
} // ibis::fuzz::coarseEvaluate

double ibis::fuzz::estimateCost(const ibis::qContinuousRange& expr) const {
    double res = static_cast<double>(col->elementSize() * nrows);
    if (bits.empty()) {
	return res;
    }

    // values in the range [hit0, hit1) satisfy the query
    uint32_t hit0, hit1;
    locate(expr, hit0, hit1);

    const uint32_t ncoarse = (cbounds.empty() ? 0U : cbounds.size()-1);
    const long fine = offset32[hit1] - offset32[hit0] <=
	(offset32.back() - offset32[hit1]) + (offset32[hit0] - offset32[0])
	? offset32[hit1] - offset32[hit0] :
	(offset32.back() - offset32[hit1]) + (offset32[hit0] - offset32[0]);
    if (hit1 <= hit0 || hit0 >= bits.size()) {
	res = 0.0;
	return res;
    }
    if (hit0 == 0 && hit1 >= bits.size()) {
	res = 0.0;
	return res;
    }

    if (hit0+1 == hit1) { // equality condition
	res = fine;
	return res;
    }
    if (hit0+3 >= hit1 || ncoarse == 0 || (cbits.size()+1) != coffsets.size()
	|| cbits.size() != (ncoarse-(ncoarse+1)/2+1)) {
	res = fine;
	return res;
    }

    // see whether the coarse bins could help
    const uint32_t c0 = cbounds.find(hit0);
    const uint32_t c1 = cbounds.find(hit1);
    if (c0 >= c1) { // within the same coarse bin
	// complement
	long tmp = coarseEstimate(c1-1, c1)
	    + (offset32[hit0] - offset32[cbounds[c1-1]])
	    + (offset32[cbounds[c1]] - offset32[hit1]);
	if (tmp/100 >= fine/99)
	    res = fine;
	else
	    res = tmp;
    }
    else {// general case: need to evaluate 5 options
	// option 2: [direct | - | direct]
	long cost = (offset32[cbounds[c0]] - offset32[hit0])
	    + coarseEstimate(c0, c1-1)
	    + (offset32[hit1] - offset32[cbounds[c1-1]]);
	// option 3: [complement | - | direct]
	long tmp;
	if (c0 > 0) {
	    tmp = (offset32[hit0] - offset32[cbounds[c0-1]])
		+ coarseEstimate(c0-1, c1-1)
		+ (offset32[hit1] - offset32[cbounds[c1-1]]);
	    if (tmp < cost)
		cost = tmp;
	}
	// option 4: [direct | - | complement]
	tmp = (offset32[cbounds[c0]] - offset32[hit0])
	    + coarseEstimate(c0, c1)
	    + (offset32[cbounds[c1]] - offset32[hit1]);
	if (tmp < cost)
	    cost = tmp;
	// option 5: [complement | - | complement]
	if (c0 > 0) {
	    tmp = (offset32[hit0] - offset32[cbounds[c0-1]])
		+ coarseEstimate(c0-1, c1)
		+ (offset32[cbounds[c1]] - offset32[hit1]);
	    if (tmp < cost)
		cost = tmp;
	}
	// option 1: fine level only
	if (cost/100 >= fine/99) // slightly prefer 1
	    res = fine;
	else
	    res = cost;
    }
    return res;
} // ibis::fuzz::estimateCost

// Compute the hits as a @c bitvector.
long ibis::fuzz::evaluate(const ibis::qContinuousRange& expr,
			  ibis::bitvector& lower) const {
    if (bits.empty()) { // empty index
	lower.set(0, nrows);
	return 0L;
    }

    // values in the range [hit0, hit1) satisfy the query
    uint32_t hit0, hit1;
    locate(expr, hit0, hit1);
    if (hit1 <= hit0 || hit0 >= bits.size()) {
	lower.set(0, nrows);
	return 0L;
    }
    if (hit0 == 0 && hit1 >= bits.size()) {
	col->getNullMask(lower);
	return lower.cnt();
    }

    if (hit0+1 == hit1) { // equality condition
	if (bits[hit0] == 0)
	    activate(hit0);
	if (bits[hit0] != 0)
	    lower.copy(*(bits[hit0]));
	else
	    lower.set(0, nrows);
	return lower.cnt();
    }
    const uint32_t ncoarse = (cbounds.empty() ? 0U : cbounds.size()-1);
    if (hit0+3 >= hit1 || ncoarse == 0 || (cbits.size()+1) != coffsets.size()
	|| cbits.size() != (ncoarse-(ncoarse+1)/2+1)) {
	// no more than three bitmaps involved, or don't know the sizes
	sumBins(hit0, hit1, lower);
	return lower.cnt();
    }

    // see whether the coarse bins could help
    const uint32_t c0 = cbounds.find(hit0);
    const uint32_t c1 = cbounds.find(hit1);
    if (ibis::gVerbose > 4) {
	ibis::util::logger lg;
	lg.buffer() << "ibis::fuzz::evaluate(" << expr << ") hit0=" << hit0
		    << ", hit1=" << hit1;
	if (c0 < cbounds.size())
	    lg.buffer() << ", cbounds[" << c0 << "]=" << cbounds[c0];
	else
	    lg.buffer() << ", cbounds[" << cbounds.size()-1 << "]="
			<< cbounds.back();
	if (c1 < cbounds.size())
	    lg.buffer() << ", cbounds[" << c1 << "]=" << cbounds[c1] << "\n";
	else
	    lg.buffer() << ", c1=" << c1 << ", bits.size()=" << bits.size()
			<< "\n";
    }
    if (c0 >= c1) { // within the same coarse bin
	long tmp = coarseEstimate(c1-1, c1)
	    + (offset32[hit0] - offset32[cbounds[c1-1]])
	    + (offset32[cbounds[c1]] - offset32[hit1]);
	if (offset32[hit1]-offset32[hit0] <= static_cast<long>(0.99*tmp)) {
	    sumBins(hit0, hit1, lower);
	}
	else {
	    coarseEvaluate(c1-1, c1, lower);
	    if (hit0 > cbounds[c1-1]) {
		ibis::bitvector bv;
		sumBins(cbounds[c1-1], hit0, bv);
		lower -= bv;
	    }
	    if (cbounds[c1] > hit1) {
		ibis::bitvector bv;
		sumBins(hit1, cbounds[c1], bv);
		lower -= bv;
	    }
	}
    }
    else {// general case: need to evaluate 5 options
	unsigned option = 2; // option 2 [direct | - | direct]
	long cost = (offset32[cbounds[c0]] - offset32[hit0])
	    + coarseEstimate(c0, c1-1)
	    + (offset32[hit1] - offset32[cbounds[c1-1]]);
	long tmp;
	if (c0 > 0) {	// option 3: [complement | - | direct]
	    tmp = (offset32[hit0] - offset32[cbounds[c0-1]])
		+ coarseEstimate(c0-1, c1-1)
		+ (offset32[hit1] - offset32[cbounds[c1-1]]);
	    if (tmp < cost) {
		cost = tmp;
		option = 3;
	    }
	}
	// option 4: [direct | - | complement]
	tmp = (offset32[cbounds[c0]] - offset32[hit0])
	    + coarseEstimate((c0>0 ? c0-1 : 0), c1)
	    + (offset32[cbounds[c1]] - offset32[hit1]);
	if (tmp < cost) {
	    cost = tmp;
	    option = 4;
	}
	if (c0 > 0) { // option 5: [complement | - | complement]
	    tmp = (offset32[hit0] - offset32[cbounds[c0-1]])
		+ coarseEstimate(c0-1, c1)
		+ (offset32[cbounds[c1]] - offset32[hit1]);
	    if (tmp < cost) {
		cost = tmp;
		option = 5;
	    }
	}
	// option 0 and 1: fine level only
	tmp = (offset32[hit1] - offset32[hit0] <=
	       (offset32.back()-offset32[hit1])+(offset32[hit0]-offset32[0]) ?
	       offset32[hit1] - offset32[hit0] :
	       (offset32.back()-offset32[hit1])+(offset32[hit0]-offset32[0]));
	if (cost > static_cast<long>(0.99*tmp)) { // slightly prefer 0/1
	    cost = tmp;
	    option = 1;
	}
	switch (option) {
	default:
	case 1: // use fine level only
	    LOGGER(ibis::gVerbose > 7)
		<< "ibis::fuzz[" << col->name() << "]::evaluate(" << expr
		<< ") using only fine level bit vectors [" << hit0
		<< ", " << hit1 << ")";
	    sumBins(hit0, hit1, lower);
	    break;
	case 2: // direct | - | direct
	    LOGGER(ibis::gVerbose > 7)
		<< "ibis::fuzz[" << col->name() << "]::evaluate(" << expr
		<< ") using coarse bit vectors [" << c0 << ", " << c1-1
		<< ") plus fine bit vectors [" << hit0 << ", "
		<< cbounds[c0] << ") plus [" << cbounds[c1-1] << ", "
		<< hit1 << ")";
	    coarseEvaluate(c0, c1-1, lower);
	    if (hit0 < cbounds[c0])
		addBins(hit0, cbounds[c0], lower); // left edge bin
	    if (cbounds[c1-1] < hit1)
		addBins(cbounds[c1-1], hit1, lower); // right edge bin
	    break;
	case 3: // complement | - | direct
	    LOGGER(ibis::gVerbose > 7)
		<< "ibis::fuzz[" << col->name() << "]::evaluate(" << expr
		<< ") using coarse bit vectors [" << c0-1 << ", " << c1-1
		<< ") minus fine bit vectors [" << cbounds[c0-1] << ", "
		<< hit0 << ") plus [" << cbounds[c1-1] << ", "
		<< hit1 << ")";
	    coarseEvaluate(c0-1, c1-1, lower);
	    if (cbounds[c0-1] < hit0) { // left edge bin, complement
		ibis::bitvector bv;
		sumBins(cbounds[c0-1], hit0, bv);
		lower -= bv;
	    }
	    if (cbounds[c1-1] < hit1)
		addBins(cbounds[c1-1], hit1, lower); // right edge bin
	    break;
	case 4: // direct | - | complement
	    LOGGER(ibis::gVerbose > 7)
		<< "ibis::fuzz[" << col->name() << "]::evaluate(" << expr
		<< ") using coarse bit vectors [" << c0 << ", " << c1
		<< ") plus fine bit vectors [" << hit0 << ", "
		<< cbounds[c0] << ") minus [" << hit1 << ", "
		<< cbounds[c1] << ")";
	    coarseEvaluate(c0, c1, lower);
	    if (hit0 < cbounds[c0])
		addBins(hit0, cbounds[c0], lower); // left edge bin
	    if (cbounds[c1] > hit1) { // right edge bin
		ibis::bitvector bv;
		sumBins(hit1, cbounds[c1], bv);
		lower -= bv;
	    }
	    break;
	case 5: // complement | - | complement
	    LOGGER(ibis::gVerbose > 7)
		<< "ibis::fuzz[" << col->name() << "]::evaluate(" << expr
		<< ") using coarse bit vectors [" << c0-1 << ", " << c1
		<< ") minus fine bit vectors [" << cbounds[c0-1] << ", "
		<< hit0 << ") minus [" << hit1 << ", "
		<< cbounds[c1] << ")";
	    coarseEvaluate(c0-1, c1, lower);
	    if (hit0 > cbounds[c0-1]) { // left edge bin
		ibis::bitvector bv;
		sumBins(cbounds[c0-1], hit0, bv);
		lower -= bv;
	    }
	    if (cbounds[c1] > hit1) { // right edge bin
		ibis::bitvector bv;
		sumBins(hit1, cbounds[c1], bv);
		lower -= bv;
	    }
	}
    }
    return lower.cnt();
} // ibis::fuzz::evaluate

// the argument is the name of the directory, the file name is
// column::name() + ".idx"
int ibis::fuzz::write(const char* dt) const {
    if (vals.empty()) return -1;

    std::string fnm;
    indexFileName(dt, fnm);
    if (fname != 0 && fnm.compare(fname) == 0)
	return 0;
    if (fname != 0 || str != 0)
	activate(); // activate all bitvectors

    int fdes = UnixOpen(fnm.c_str(), OPEN_WRITENEW, OPEN_FILEMODE);
    if (fdes < 0) {
	ibis::fileManager::instance().flushFile(fnm.c_str());
	fdes = UnixOpen(fnm.c_str(), OPEN_WRITENEW, OPEN_FILEMODE);
	if (fdes < 0) {
	    col->logWarning("fuzz::write", "unable to open \"%s\" for write",
			    fnm.c_str());
	    return -2;
	}
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int32_t ierr = 0;
    const uint32_t nobs = vals.size();

    array_t<int32_t> offs(nobs+1);
    char header[] = "#IBIS\7\0\0";
    header[5] = (char)ibis::index::FUZZ;
    header[6] = (char)sizeof(int32_t);
    ierr = UnixWrite(fdes, header, 8);
    if (ierr < 8) {
	LOGGER(ibis::gVerbose > 0)
	    << "ibis::column[" << col->partition()->name() << "."
	    << col->name() << "]::fuzz::write(" << fnm
	    << ") failed to write the 8-byte header, ierr = " << ierr;
	return -3;
    }
    ierr = ibis::relic::write(fdes); // write the bulk of the index file
    if (ierr < 0) {
	(void)UnixClose(fdes);
	remove(fnm.c_str());
	return ierr;
    }
    ierr = writeCoarse(fdes); // write the coarse level bins
#if _POSIX_FSYNC+0 > 0 && defined(FASTBIT_SYNC_WRITE)
    (void) UnixFlush(fdes); // write to disk
#endif
    (void) UnixClose(fdes);

    if (ibis::gVerbose > 5)
	col->logMessage("fuzz::write", "wrote %lu bitmap%s to %s",
			static_cast<long unsigned>(nobs),
			(nobs>1?"s":""), fnm.c_str());
    return ierr;
} // ibis::fuzz::write

// This function intended to be called after calling ibis::relic::write,
// however, it does not check for this fact!
int ibis::fuzz::writeCoarse(int fdes) const {
    if (cbounds.empty() || cbits.empty() || nrows == 0)
	return -4;

    int32_t ierr;
    const uint32_t nc = cbounds.size()-1;
    const uint32_t nb = cbits.size();
    array_t<int32_t> offs(nb+1);
    ierr = UnixWrite(fdes, &nc, sizeof(nc));
    ierr = UnixWrite(fdes, cbounds.begin(), sizeof(uint32_t)*(nc+1));
    ierr = UnixSeek(fdes, sizeof(int32_t)*(nb+1), SEEK_CUR);
    for (unsigned i = 0; i < nb; ++ i) {
	offs[i] = UnixSeek(fdes, 0, SEEK_CUR);
	if (cbits[i] != 0)
	    cbits[i]->write(fdes);
    }
    offs[nb] = UnixSeek(fdes, 0, SEEK_CUR);
    ierr -= sizeof(int32_t) * (nb+1);
    UnixSeek(fdes, ierr, SEEK_SET);
    ierr = UnixWrite(fdes, offs.begin(), sizeof(int32_t)*(nb+1));
    ierr = UnixSeek(fdes, offs.back(), SEEK_SET);
    return 0;
} // ibis::fuzz::writeCoarse

// read the index contained in the file f
int ibis::fuzz::read(const char* f) {
    std::string fnm;
    indexFileName(f, fnm);

    int fdes = UnixOpen(fnm.c_str(), OPEN_READONLY);
    if (fdes < 0) return -1;

    char header[8];
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif
    if (8 != UnixRead(fdes, static_cast<void*>(header), 8)) {
	UnixClose(fdes);
	return -2;
    }

    if (false == (header[0] == '#' && header[1] == 'I' &&
		  header[2] == 'B' && header[3] == 'I' &&
		  header[4] == 'S' &&
		  header[5] == static_cast<char>(FUZZ) &&
		  header[6] == static_cast<char>(sizeof(int32_t)) &&
		  header[7] == static_cast<char>(0))) {
	UnixClose(fdes);
	return -3;
    }

    uint32_t dim[3];
    uint32_t begin, end;
    clear(); // clear the current content
    fname = ibis::util::strnewdup(fnm.c_str());

    long ierr = UnixRead(fdes, static_cast<void*>(dim), 3*sizeof(uint32_t));
    if (ierr < static_cast<int>(3*sizeof(uint32_t))) {
	UnixClose(fdes);
	return -4;
    }
    nrows = dim[0];
    bool trymmap = false;
#if defined(HAVE_FILE_MAP)
    trymmap = (dim[2] > ibis::fileManager::pageSize());
#endif
    // read vals
    begin = 8*((3*sizeof(uint32_t) + 15) / 8);
    end = begin + dim[2] * sizeof(double);
    if (trymmap) {
	// try to use memory map to enable reading only the needed values
	array_t<double> dbl(fname, begin, end);
	vals.swap(dbl);
    }
    else { // read all values into memory
	array_t<double> dbl(fdes, begin, end);
	vals.swap(dbl);
    }
    // read the offsets
    begin = end;
    end += sizeof(int32_t) * (dim[1] + 1);
    if (trymmap) {
	array_t<int32_t> tmp(fname, begin, end);
	offset32.swap(tmp);
    }
    else {
	array_t<int32_t> tmp(fdes, begin, end);
	offset32.swap(tmp);
    }
    ibis::fileManager::instance().recordPages(0, end);
#if defined(DEBUG) || defined(_DEBUG)
    if (ibis::gVerbose > 5) {
	unsigned nprt = (ibis::gVerbose < 30 ? (1 << ibis::gVerbose) : dim[1]);
	if (nprt > dim[1])
	    nprt = dim[1];
	ibis::util::logger lg;
	lg.buffer() << "DEBUG -- ibis::fuzz::read(" << f
		    << ") got nobs = " << dim[1] << ", card = " << dim[2]
		    << ", the offsets of the bit vectors are\n";
	for (unsigned i = 0; i < nprt; ++ i)
	    lg.buffer() << offset32[i] << " ";
	if (nprt < dim[1])
	    lg.buffer() << "... (skipping " << dim[1]-nprt << ") ... ";
	lg.buffer() << offset32[dim[1]] << "\n";
    }
#endif

    bits.resize(dim[1]);
    for (uint32_t i = 0; i < dim[1]; ++i)
	bits[i] = 0;
#if defined(FASTBIT_READ_BITVECTOR0)
    // read the first bitvector
    if (offset32[1] > offset32[0]) {
	array_t<ibis::bitvector::word_t> a0(fdes, offset32[0], offset32[1]);
	bits[0] = new ibis::bitvector(a0);
	bits[0]->sloppySize(nrows);
    }
    else {
	bits[0] = new ibis::bitvector;
	bits[0]->set(0, nrows);
    }
#endif

    // reading the coarse bins
    ierr = UnixSeek(fdes, offset32.back(), SEEK_SET);
    if (ierr == offset32.back()) {
	uint32_t nc;
	ierr = UnixRead(fdes, &nc, sizeof(nc));
	begin = offset32.back() + sizeof(nc);
	end = begin + sizeof(uint32_t)*(nc+1);
	if (ierr > 0 && nc > 0) {
	    array_t<uint32_t> tmp(fdes, begin, end);
	    cbounds.swap(tmp);
	}
	begin = end;
	end += sizeof(int32_t) * (nc+2-(nc+1)/2);
	if (cbounds.size() == nc+1) {
	    array_t<int32_t> tmp(fdes, begin, end);
	    coffsets.swap(tmp);
	}

	for (unsigned i = 0; i < cbits.size(); ++ i)
	    delete cbits[i];
	cbits.resize(nc+1-(nc+1)/2);
	for (unsigned i = 0; i < nc+1-(nc+1)/2; ++ i)
	    cbits[i] = 0;
    }
    (void) UnixClose(fdes);
    str = 0;
    if (ibis::gVerbose > 7)
	col->logMessage("readIndex", "finished reading '%s' header from %s",
			name(), fnm.c_str());
    return 0;
} // ibis::fuzz::read

// Reading information about the coarse bins.  To be used after calling
// ibis::relic::read, which happens in the constructor.
int ibis::fuzz::readCoarse(const char* fn) {
    std::string fnm;
    indexFileName(fn, fnm);

    int fdes = UnixOpen(fnm.c_str(), OPEN_READONLY);
    if (fdes < 0) return -4;
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    long ierr = UnixSeek(fdes, offset32.back(), SEEK_SET);
    if (ierr == offset32.back()) {
	uint32_t nc, begin, end;
	ierr = UnixRead(fdes, &nc, sizeof(nc));
	if (ierr <= 0 || static_cast<uint32_t>(ierr) != sizeof(nc)) {
	    UnixClose(fdes);
	    return -5;
	}

	begin = offset32.back() + sizeof(nc);
	end = begin + sizeof(uint32_t)*(nc+1);
	if (ierr > 0 && nc > 0) {
	    array_t<uint32_t> tmp(fdes, begin, end);
	    cbounds.swap(tmp);
	}
	const uint32_t ncb = nc+1-(nc+1)/2;
	begin = end;
	end += sizeof(int32_t) * (ncb+1);
	if (cbounds.size() == nc+1) {
	    array_t<int32_t> tmp(fdes, begin, end);
	    coffsets.swap(tmp);
	}

	for (unsigned i = 0; i < cbits.size(); ++ i)
	    delete cbits[i];
	cbits.resize(ncb);
	for (unsigned i = 0; i < ncb; ++ i)
	    cbits[i] = 0;
    }
    (void) UnixClose(fdes);

    if (ibis::gVerbose > 7)
	col->logMessage("readIndex", "finished reading '%s' coarse bin info "
			"from %s", name(), fnm.c_str());
    return 0;
} // ibis::fuzz::readCoarse

// attempt to reconstruct an index from a piece of consecutive memory
int ibis::fuzz::read(ibis::fileManager::storage* st) {
    if (st == 0) return -1;
    clear();

    nrows = *(reinterpret_cast<uint32_t*>(st->begin()+8));
    uint32_t pos = 8 + sizeof(uint32_t);
    const uint32_t nobs = *(reinterpret_cast<uint32_t*>(st->begin()+pos));
    pos += sizeof(uint32_t);
    const uint32_t card = *(reinterpret_cast<uint32_t*>(st->begin()+pos));
    pos += sizeof(uint32_t) + 7;
    array_t<int32_t> offs(st, 8*(pos/8) + sizeof(double)*card, nobs+1);
    array_t<double> dbl(st, 8*(pos/8), card);
    offset32.copy(offs);
    vals.swap(dbl);

    for (uint32_t i = 0; i < bits.size(); ++ i)
	delete bits[i];
    bits.resize(nobs);
    for (uint32_t i = 0; i < nobs; ++i)
	bits[i] = 0;
    if (st->isFileMap()) { // only restore the first bitvector
#if defined(FASTBIT_READ_BITVECTOR0)
	if (offs[1] > offs[0]) {
	    array_t<ibis::bitvector::word_t>
		a0(st, offs[0], (offs[1]-offs[0])/
		   sizeof(ibis::bitvector::word_t));
	    bits[0] = new ibis::bitvector(a0);
	    bits[0]->sloppySize(nrows);
	}
	else {
	    bits[0] = new ibis::bitvector;
	    bits[0]->adjustSize(0, nrows);
	}
#endif
	str = st;

	if (str->size() > static_cast<uint32_t>(offset32.back())) {
	    uint32_t nc =
		*(reinterpret_cast<uint32_t*>(str->begin() + offset32.back()));
	    if (nc > 0 && str->size() > static_cast<uint32_t>(offset32.back()) +
		(sizeof(int32_t)+sizeof(uint32_t))*(nc+1)) {
		uint32_t start;
		start = offset32.back() + sizeof(uint32_t);
		array_t<uint32_t> btmp(str, start, nc+1);
		cbounds.swap(btmp);

		start += sizeof(uint32_t)*(nc+1);
		array_t<int32_t> otmp(str, start, nc+1);

		cbits.resize(nc);
		for (unsigned i = 0; i < nc; ++ i)
		    cbits[i] = 0;
	    }
	}
    }
    else { // regenerate all the bitvectors
	for (uint32_t i = 0; i < nobs; ++i) {
	    if (offs[i+1] > offs[i]) {
		array_t<ibis::bitvector::word_t>
		    a(st, offs[i], (offs[i+1]-offs[i])/
		      sizeof(ibis::bitvector::word_t));
		ibis::bitvector* btmp = new ibis::bitvector(a);
		btmp->sloppySize(nrows);
		bits[i] = btmp;
	    }
	    else if (ibis::gVerbose > 0) {
		col->logWarning("fuzz::read", "bitvector %lu is invalid "
				"(offsets[%lu]=%lu, offsets[%lu]=%lu)",
				static_cast<long unsigned>(i),
				static_cast<long unsigned>(i),
				static_cast<long unsigned>(offs[i]),
				static_cast<long unsigned>(i+1),
				static_cast<long unsigned>(offs[i+1]));
	    }
	}

	if (str->size() > static_cast<uint32_t>(offset32.back())) {
	    uint32_t nc =
		*(reinterpret_cast<uint32_t*>(str->begin() + offset32.back()));
	    const uint32_t ncb = nc + 1 - (nc+1) / 2;
	    if (nc > 0 && str->size() > offset32.back() +
		(sizeof(int32_t)+sizeof(uint32_t))*(nc+1)) {
		uint32_t start;
		start = offset32.back() + sizeof(uint32_t);
		array_t<uint32_t> btmp(str, start, nc+1);
		cbounds.swap(btmp);

		start += sizeof(uint32_t)*(ncb+1);
		array_t<int32_t> otmp(str, start, ncb+1);

		cbits.resize(ncb);
		for (unsigned i = 0; i < ncb; ++ i) {
		    if (coffsets[i+1] > coffsets[i]) {
			array_t<ibis::bitvector::word_t>
			    a(st, coffsets[i], (coffsets[i+1]-coffsets[i])
			      / sizeof(ibis::bitvector::word_t));
			cbits[i] = new ibis::bitvector(a);
			cbits[i]->sloppySize(nrows);
		    }
		    else {
			cbits[i] = 0;
		    }
		}
	    }
	}
	str = 0;
    }
    return 0;
} // ibis::fuzz::read

void ibis::fuzz::clear() {
    clearCoarse();
    ibis::relic::clear();
} // ibis::fuzz::clear

void ibis::fuzz::clearCoarse() {
    const unsigned nb = cbits.size();
    for (unsigned i = 0; i < nb; ++ i)
	delete cbits[i];

    cbits.clear();
    cbounds.clear();
    coffsets.clear();
} // ibis::fuzz::clearCoarse

// the printing function
void ibis::fuzz::print(std::ostream& out) const {
    if (vals.size() != bits.size() || bits.empty())
	return;

    const uint32_t nc = (cbounds.empty() ? 0U : cbounds.size()-1);
    const uint32_t ncb = nc+1 - (nc+1)/2;
    out << "the interval-equality encoded bitmap index for "
	<< col->partition()->name() << '.'
	<< col->name() << " contains " << nc << " coarse bin"
	<< (nc>1 ? "s" : "") << " and " << bits.size()
	<< " fine bit vectors for " << nrows << " objects\n";
    uint32_t nprt = (ibis::gVerbose < 30 ? 1 << ibis::gVerbose : bits.size());
    uint32_t omitted = 0;
    uint32_t end;
    if (nc > 0 && cbits.size() == ncb && coffsets.size() == ncb+1) {
	// has coarse bins
	for (unsigned j = 0; j < nc; ++ j) {
	    out << "Coarse bin " << j << ", [" << cbounds[j] << ", "
		<< cbounds[j+1] << ")";
	    if (j < ncb && cbits[j] != 0)
		out << "\t{[" << cbounds[j] << ", " << cbounds[j+(nc+1)/2]
		    << ")\t" << cbits[j]->cnt() << "\t" << cbits[j]->bytes()
		    << "}\n";
	    else
		out << "\n";
	    end = (cbounds[j+1] <= cbounds[j]+nprt ?
		   cbounds[j+1] : cbounds[j]+nprt);
	    for (unsigned i = cbounds[j]; i < end; ++ i) {
		if (bits[i]) {
		    out << "\t" << i << ":\t";
		    out.precision(12);
		    out << vals[i] << "\t" << bits[i]->cnt()
			<< "\t" << bits[i]->bytes() << "\n";
		}
		else {
		    ++ omitted;
		}
	    }
	    if (cbounds[j+1] > end && nprt > 0) {
		out << "\t...\n";
		omitted += (cbounds[j+1] - end);
	    }
	}
	if (nprt > 0 && omitted > 0)
	    out << "\tfine level bitmaps omitted: " << omitted << "\n";
    }
    else { // no coarse bins
	const uint32_t nobs = bits.size();
	uint32_t skip = 0;
	if (ibis::gVerbose <= 0) {
	    skip = nobs;
	}
	else if ((nobs >> 2*ibis::gVerbose) > 2) {
	    skip = static_cast<uint32_t>
		(ibis::util::compactValue
		 (static_cast<double>(nobs >> (1+2*ibis::gVerbose)),
		  static_cast<double>(nobs >> (2*ibis::gVerbose))));
	    if (skip < 1)
		skip = 1;
	}
	if (skip < 1)
	    skip = 1;
	if (skip > 1) {
	    out << " (printing 1 out of every " << skip << ")\n";
	}

	for (uint32_t i=0; i<nobs; i += skip) {
	    if (bits[i]) {
		out << i << ":\t";
		out.precision(12);
		out << vals[i] << "\t" << bits[i]->cnt()
		    << "\t" << bits[i]->bytes() << "\n";
	    }
	    else if (ibis::gVerbose > 7) {
		out << i << ":\t";
		out.precision(12);
		out << vals[i] << " ... \n";
	    }
	}
	if ((nobs-1) % skip) {
	    if (bits[nobs-1]) {
		out << nobs-1 << ":\t";
		out << vals[nobs-1] << "\t" << bits[nobs-1]->cnt()
		    << "\t" << bits[nobs-1]->bytes() << "\n";
	    }
	    else if (ibis::gVerbose > 7) {
		out << nobs-1 << ":\t";
		out << vals[nobs-1] << " ... \n";
	    }
	}
    }
    out << "\n";
} // ibis::fuzz::print
