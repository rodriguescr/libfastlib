// $Id$
// Author: John Wu <John.Wu at ACM.org>
// Copyright 2000-2008 the Regents of the University of California
//
// This file contains the implementation of the class ibis::bak2
//
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
#endif
#include "ibin.h"
#include "part.h"
#include "column.h"
#include "resource.h"

#include <iterator>	// std::ostream_iterator

// construct a bitmap index from current data
ibis::bak2::bak2(const ibis::column* c, const char* f) : ibis::bin() {
    if (c == 0) return;  // nothing can be done
    col = c;

    try {
	if (f) { // f is not null
	    read(f);
	}   
	if (nobs == 0) {
	    bakMap bmap;
	    mapValues(f, bmap);
	    construct(bmap);
	    optionalUnpack(bits, col->indexSpec());

	    if (ibis::gVerbose > 4) {
		ibis::util::logger lg(4);
		print(lg.buffer());
	    }
	}
    }
    catch (...) {
	clear();
	throw;
    }
} // constructor

// read from a file
int ibis::bak2::read(const char* f) {
    int ierr = -1;
    try {
	std::string fnm;
	indexFileName(f, fnm);
	if (ibis::index::isIndex(fnm.c_str(), ibis::index::BAK2)) {
	    ierr = ibis::bin::read(f);
	}
    }
    catch (...) {
	clear();
	throw;
    }
    return ierr;
} // ibis::bak2::read

// locates the first bin that is just to the right of val or covers val
// return the smallest i such that maxval[i] >= val
uint32_t ibis::bak2::locate(const double& val) const {
#ifdef DEBUG
    ibis::util::logMessage("ibis::bak2::locate", "searching for %g in an "
			   "array of %lu double(s) in the range of [%g, %g]",
			   val, static_cast<long unsigned>(minval.size()),
			   minval[0], maxval.back());
#endif
    // check the extreme cases -- use negative tests to capture abnormal
    // numbers
    if (minval.empty()) return 0;
    if (! (val > maxval[0])) {
	return 0;
    }
    else if (! (val <= maxval[nobs-1])) {
	return nobs;
    }

    // the normal cases -- two different search strategies
    if (nobs >= 8) { // binary search
	uint32_t i0 = 0, i1 = nobs, it = nobs/2;
	while (i0 < it) { // maxval[i1] >= val
	    if (val <= maxval[it])
		i1 = it;
	    else
		i0 = it;
	    it = (i0 + i1) / 2;
	}
#ifdef DEBUG
	ibis::util::logMessage("locate", "%g in [%g, %g) ==> %lu", val,
			       maxval[i0], maxval[i1],
			       static_cast<long unsigned>(i1));
#endif
	return i1;
    }
    else { // do linear search
	for (uint32_t i = 0; i < nobs; ++i) {
	    if (val <= maxval[i]) {
#ifdef DEBUG
		if (i > 0)
		    ibis::util::logMessage("locate", "%g in [%g, %g) ==> %lu",
					   val, maxval[i-1], maxval[i],
					   static_cast<long unsigned>(i));
		else
		    ibis::util::logMessage("locate", "%g in (..., %g) ==> 0",
					   val, maxval[i]);
#endif
		return i;
	    }
	}
    }
    return nobs;
} // ibis::bak2::locate

// This function reads the data file and records the locations of the values
// in ibis::bak2::bakMap
void ibis::bak2::mapValues(const char* f, ibis::bak2::bakMap& bmap) const {
    horometer timer;
    if (ibis::gVerbose > 4)
	timer.start();

    const unsigned prec = parsePrec(); // the precision of mapped value

    uint32_t nev = col->partition()->nRows();
    std::string fnm; // name of the data file
    dataFileName(f, fnm);

    ibis::bitvector mask;
    {   // limit the scope of some variables
	array_t<ibis::bitvector::word_t> arr;
	std::string mname(fnm);   // name of mask file
	mname += ".msk";
	int i = ibis::fileManager::instance().getFile(mname.c_str(), arr);
	if (i == 0)
	    mask.copy(arr); // convert arr to a bitvector
	else
	    mask.set(1, nev); // default mask
    }

    // need to use different types of array_t for different columns
    switch (col->type()) {
    case ibis::TEXT:
    case ibis::UINT: {// unsigned int
	array_t<uint32_t> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    bmap.clear();
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1 = new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::INT: {// signed int
	array_t<int32_t> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1 = new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::ULONG: {// unsigned long int
	array_t<uint64_t> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    bmap.clear();
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1 = new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::LONG: {// signed long int
	array_t<int64_t> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1 = new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::FLOAT: {// (4-byte) floating-point values
	array_t<float> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1 = new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::DOUBLE: {// (8-byte) floating-point values
	array_t<double> val;
	ibis::fileManager::instance().getFile(fnm.c_str(), val);
	if (val.size() <= 0) {
	    col->logWarning("bak2::mapValues", "unable to read %s",
			    fnm.c_str());
	}
	else {
	    nev = val.size();
	    if (nev > mask.size())
		mask.adjustSize(nev, nev);
	    ibis::bitvector::indexSet iset = mask.firstIndexSet();
	    uint32_t nind = iset.nIndices();
	    const ibis::bitvector::word_t *iix = iset.indices();
	    while (nind) {
		if (iset.isRange()) { // a range
		    uint32_t k = (iix[1] < nev ? iix[1] : nev);
		    for (uint32_t i = *iix; i < k; ++i) {
			double key = ibis::util::coarsen(val[i], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[i] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(i, 1);
			    if (grn.min0 > val[i]) grn.min0 = val[i];
			    if (grn.max0 < val[i]) grn.max0 = val[i];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(i, 1);
			    if (grn.min1 > val[i]) grn.min1 = val[i];
			    if (grn.max1 < val[i]) grn.max1 = val[i];
			}
		    }
		}
		else if (*iix+ibis::bitvector::bitsPerLiteral() < nev) {
		    // a list of indices
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			double key = ibis::util::coarsen(val[k], prec);
			ibis::bak2::grain& grn = bmap[key];
			if (val[k] < key) {
			    if (grn.loc0 == 0)
				grn.loc0 = new ibis::bitvector;
			    grn.loc0->setBit(k, 1);
			    if (grn.min0 > val[k]) grn.min0 = val[k];
			    if (grn.max0 < val[k]) grn.max0 = val[k];
			}
			else {
			    if (grn.loc1 == 0)
				grn.loc1 = new ibis::bitvector;
			    grn.loc1->setBit(k, 1);
			    if (grn.min1 > val[k]) grn.min1 = val[k];
			    if (grn.max1 < val[k]) grn.max1 = val[k];
			}
		    }
		}
		else {
		    for (uint32_t i = 0; i < nind; ++i) {
			uint32_t k = iix[i];
			if (k < nev) {
			    double key = ibis::util::coarsen(val[k], prec);
			    ibis::bak2::grain& grn = bmap[key];
			    if (val[k] < key) {
				if (grn.loc0 == 0)
				    grn.loc0 = new ibis::bitvector;
				grn.loc0->setBit(k, 1);
				if (grn.min0 > val[k]) grn.min0 = val[k];
				if (grn.max0 < val[k]) grn.max0 = val[k];
			    }
			    else {
				if (grn.loc1 == 0)
				    grn.loc1= new ibis::bitvector;
				grn.loc1->setBit(k, 1);
				if (grn.min1 > val[k]) grn.min1 = val[k];
				if (grn.max1 < val[k]) grn.max1 = val[k];
			    }
			}
		    }
		}
		++iset;
		nind = iset.nIndices();
		if (*iix >= nev) nind = 0;
	    } // while (nind)
	}
	break;}
    case ibis::CATEGORY: // no need for a separate index
	col->logWarning("bak2::mapValues", "no need for binning -- should "
			"have a basic bitmap index already");
	return;
    default:
	col->logWarning("bak2::mapValues", "unable to create bins for "
			"this type (%s) of column",
			ibis::TYPESTRING[(int)col->type()]);
	return;
    }

    // make sure all bit vectors are the same size
    for (ibis::bak2::bakMap::iterator it = bmap.begin();
	 it != bmap.end(); ++ it) {
	if ((*it).second.loc0)
	    (*it).second.loc0->adjustSize(0, nev);
	if ((*it).second.loc1)
	    (*it).second.loc1->adjustSize(0, nev);
	//	(*it).second.loc->compress();
    }

    // write out the current content
    if (ibis::gVerbose > 2) {
	if (ibis::gVerbose > 4) {
	    timer.stop();
	    col->logMessage("bak2::mapValues", "mapped %lu values to %lu "
			    "%u-digit number%s in %g sec(elapsed)",
			    static_cast<long unsigned>(nev),
			    static_cast<long unsigned>(bmap.size()),
			    prec, (bmap.size() > 1 ? "s" : ""),
			    timer.realTime());
	}
	else {
	    col->logMessage("bak2::mapValues", "mapped %lu values to %lu "
			    "%u-digit number%s",
			    static_cast<long unsigned>(nev),
			    static_cast<long unsigned>(bmap.size()), prec,
			    (bmap.size() > 1 ? "s" : ""));
	}
	if (ibis::gVerbose > 6) {
	    ibis::util::logger lg(6);
	    printMap(lg.buffer(), bmap);
	}
    }
} // ibis::bak2::mapValues

void ibis::bak2::printMap(std::ostream& out,
			  const ibis::bak2::bakMap& bmap) const {
    out << "bak2::printMap(" << bmap.size()
	<< (bmap.size() > 1 ? " entries" : " entry")
	<< " [key, min_, max_, count_, min^, max^, count^]"
	<< std::endl;
    uint32_t prt = (ibis::gVerbose > 30 ? bmap.size() : (1 << ibis::gVerbose));
    if (prt < 5) prt = 5;
    if (prt+1 >= bmap.size()) { // print all
	for (ibis::bak2::bakMap::const_iterator it = bmap.begin();
	     it != bmap.end(); ++ it) {
	    out << (*it).first << ",\t";
	    if ((*it).second.loc0)
		out << (*it).second.min0 << ",\t"
		    << (*it).second.max0 << ",\t"
		    << (*it).second.loc0->cnt();
	    else
		out << ",\t,\t";
	    if ((*it).second.loc1)
		out << ",\t" << (*it).second.min1
		    << ",\t" << (*it).second.max1
		    << ",\t" << (*it).second.loc1->cnt() << "\n";
	    else
		out << ",\t,\t,\t\n";
	}
    }
    else { // print some
	ibis::bak2::bakMap::const_iterator it = bmap.begin();
	for (uint32_t i = 0; i < prt; ++i, ++it) {
	    out << (*it).first << ",\t";
	    if ((*it).second.loc0)
		out << (*it).second.min0 << ",\t"
		    << (*it).second.max0 << ",\t"
		    << (*it).second.loc0->cnt();
	    else
		out << ",\t,\t";
	    if ((*it).second.loc1)
		out << ",\t" << (*it).second.min1
		    << ",\t" << (*it).second.max1
		    << ",\t" << (*it).second.loc1->cnt() << "\n";
	    else
		out << ",\t,\t,\t\n";
	}
	prt =  bmap.size() - prt - 1;
	it = bmap.end();
	-- it;
	out << "...\n" << prt << (prt > 1 ? " entries" : " entry")
	    << " omitted\n...\n";
	out << (*it).first << ",\t";
	if ((*it).second.loc0)
	    out << (*it).second.min0 << ",\t"
		<< (*it).second.max0 << ",\t"
		<< (*it).second.loc0->cnt();
	else
	    out << ",\t,\t";
	if ((*it).second.loc1)
	    out << ",\t" << (*it).second.min1
		<< ",\t" << (*it).second.max1
		<< ",\t" << (*it).second.loc1->cnt() << "\n";
	else
	    out << ",\t,\t,\t\n";
    }
    out << std::endl;
} //ibis::bak2::printMap

// write the index to the named directory or file
int ibis::bak2::write(const char* dt) const {
    if (nobs <= 0) return -1;

    array_t<int32_t> offs(nobs+1);
    std::string fnm;
    indexFileName(dt, fnm);
    if (fname != 0 && fnm.compare(fname) == 0)
	return 0;
    if (fname != 0 || str != 0)
	activate();

    int fdes = UnixOpen(fnm.c_str(), OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	col->logWarning("bak2::write", "unable to open \"%s\" for write",
			fnm.c_str());
	return -2;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    char header[] = "#IBIS\0\0\0";
    header[5] = (char)ibis::index::BAK2;
    header[6] = (char) sizeof(int32_t);
    int ierr = UnixWrite(fdes, header, 8);
    if (ierr < 8) {
	LOGGER(ibis::gVerbose > 0)
	    << "ibis::column[" << col->partition()->name() << "."
	    << col->name() << "]::bak2::write(" << fnm << ") failed to write "
	    << "the 8-byte header, ierr = " << ierr;
	return -3;
    }
    ierr = UnixWrite(fdes, &nrows, sizeof(nobs));
    ierr = UnixWrite(fdes, &nobs, sizeof(nobs));
    ierr = UnixSeek(fdes,
		    ((sizeof(int32_t)*(nobs+1)+sizeof(uint32_t)*2+15)/8)*8,
		    SEEK_SET);
    ierr = UnixWrite(fdes, bounds.begin(), sizeof(double)*nobs);
    ierr = UnixWrite(fdes, maxval.begin(), sizeof(double)*nobs);
    ierr = UnixWrite(fdes, minval.begin(), sizeof(double)*nobs);
    for (uint32_t i = 0; i < nobs; ++i) {
	offs[i] = UnixSeek(fdes, 0, SEEK_CUR);
	bits[i]->write(fdes);
    }
    offs[nobs] = UnixSeek(fdes, 0, SEEK_CUR);
    ierr = UnixSeek(fdes, 8+2*sizeof(uint32_t), SEEK_SET);
    ierr = UnixWrite(fdes, offs.begin(), sizeof(int32_t)*(nobs+1));
#if _POSIX_FSYNC+0 > 0
    (void) fsync(fdes); // write to disk
#endif
    (void) UnixClose(fdes);

    if (ibis::gVerbose > 5)
	col->logMessage("bak2::write", "wrote to file %s (%lu bitmap(s) "
			"for %lu object(s)", fnm.c_str(),
			static_cast<long unsigned>(nobs),
			static_cast<long unsigned>(nrows));
    return 0;
} // ibis::bak2::write

// covert the hash structure in bakMap into the array structure in ibis::bin
// NOTE: the pointers to bitvectors in bakMap is copied! Those pointers
// should NOT be deleted when freeing the bakMap!
void ibis::bak2::construct(ibis::bak2::bakMap& bmap) {
    // clear the existing content
    clear();

    // count the number of bitvectors
    nobs = 0;
    for (ibis::bak2::bakMap::const_iterator ir = bmap.begin();
	 ir != bmap.end(); ++ ir) {
	nobs += ((*ir).second.loc0 != 0);
	nobs += ((*ir).second.loc1 != 0);
    }
    // initialize the arrays
    bits.resize(nobs);
    bounds.resize(nobs);
    minval.resize(nobs);
    maxval.resize(nobs);

    // copy the values
    ibis::bak2::bakMap::iterator it = bmap.begin();
    for (uint32_t i = 0; i < nobs; ++it) {
	if ((*it).second.loc0) {
	    bits[i] = (*it).second.loc0;
	    if (i > 0)
		bounds[i] = ibis::util::compactValue
		    (maxval[i-1], (*it).second.min0);
	    else
		bounds[i] = ibis::util::compactValue
		    (-DBL_MAX, (*it).second.min0);
	    if (nrows == 0)
		nrows = (*it).second.loc0->size();
	    minval[i] = (*it).second.min0;
	    maxval[i] = (*it).second.max0;
	    (*it).second.loc0 = 0;
	    ++ i;
	}
	if ((*it).second.loc1) {
	    bits[i] = (*it).second.loc1;
	    bounds[i] = (*it).second.min1;
	    minval[i] = (*it).second.min1;
	    maxval[i] = (*it).second.max1;
	    if (nrows == 0)
		nrows = (*it).second.loc1->size();
	    (*it).second.loc1 = 0;
	    ++ i;
	}
    }
} // ibis::bak2::construct
 
void ibis::bak2::binBoundaries(std::vector<double>& ret) const {
    const uint32_t sz = bounds.size();
    ret.resize(sz);
    for (uint32_t i = 0; i < sz; ++ i)
	ret[i] = bounds[i];
} // ibis::bak2::binBoundaries()

void ibis::bak2::binWeights(std::vector<uint32_t>& ret) const {
    activate(); // make sure all bitvectors are available
    ret.resize(nobs);
    for (uint32_t i=0; i<nobs; ++i)
	if (bits[i])
	    ret[i] = bits[i]->cnt();
	else
	    ret[i] = 0;
} // ibis::bak2::bak2Weights()

// the printing function
void ibis::bak2::print(std::ostream& out) const {
    if (nrows == 0) return;

    // activate(); -- active may invoke ioLock which causes problems
    out << "index (equality encoding on reduced precision values) for "
	<< col->partition()->name() << '.'
	<< col->name() << " contains " << nobs << " bitvectors for "
	<< nrows << " objects \n";
    if (ibis::gVerbose > 0) {
	uint32_t prt = (ibis::gVerbose > 30 ? nobs : (1 << ibis::gVerbose));
	if (prt < 5) prt = 5;
	if (prt+prt+1 >= nobs) { // print all
	    for (uint32_t i = 0; i < nobs; ++ i) {
		if (bits[i]) {
		    out << bounds[i] << "\t" << minval[i] << "\t"
			<< maxval[i] << "\t" << bits[i]->cnt() << "\n";
		    if (bits[i]->size() != nrows)
			out << "ERROR: bits[" << i << "]->size("
			    << bits[i]->size()
			    << ") differs from nrows ("
			    << nrows << ")\n";
		}
		else {
		    out << bounds[i] << "\t" << minval[i] << "\t"
			<< maxval[i] << "\n";
		}
	    }
	}
	else {
	    for (uint32_t i = 0; i < prt; ++ i) {
		if (bits[i]) {
		    out << bounds[i] << "\t" << minval[i] << "\t"
			<< maxval[i] << "\t" << bits[i]->cnt() << "\n";
		    if (bits[i]->size() != nrows)
			out << "ERROR: bits[" << i << "]->size("
			    << bits[i]->size()
			    << ") differs from nrows ("
			    << nrows << ")\n";
		}
		else {
		    out << bounds[i] << "\t" << minval[i] << "\t"
			<< maxval[i] << "\n";
		}
	    }
	    prt = nobs - prt - 1;
	    out << "...\n" << prt << (prt > 1 ? " entries" : " entry")
		<< " omitted\n...\n";
	    if (bits.back()) {
		out << bounds.back() << "\t" << minval.back() << "\t"
		    << maxval.back() << "\t" << bits.back()->cnt() << "\n";
		if (bits.back()->size() != nrows)
		    out << "ERROR: bits[" << nobs-1 << "]->size("
			<< bits.back()->size()
			<< ") differs from nrows ("
			<< nrows << ")\n";
	    }
	    else {
		out << bounds.back() << "\t" << minval.back() << "\t"
		    << maxval.back() << "\n";
	    }
	}
    }
    out << std::endl;
} // ibis::bak2::print()

// this function simply recreates the index using the current data in dt
// directory
long ibis::bak2::append(const char* dt, const char* df, uint32_t nnew) {
    if (nnew == 0)
	return 0;

    clear(); // clear the current content and rebuild index in dt
    bakMap bmap;
    mapValues(dt, bmap);
    construct(bmap);
    optionalUnpack(bits, col->indexSpec());
    write(dt); // record the new index

    if (ibis::gVerbose > 2) {
	ibis::util::logger lg(2);
	print(lg.buffer());
    }
    return nnew;
} // ibis::bak2::append

// expand range condition -- rely on the fact that the only operators used
// are LT, LE and EQ
int ibis::bak2::expandRange(ibis::qContinuousRange& rng) const {
    uint32_t cand0, cand1;
    double left, right;
    int ret = 0;
    ibis::bin::locate(rng, cand0, cand1);
    if (rng.leftOperator() == ibis::qExpr::OP_LT) {
	if (cand0 < minval.size() && rng.leftBound() >= minval[cand0]) {
	    // reduce left bound
	    ++ ret;
	    right = minval[cand0];
	    if (cand0 > 0)
		left = maxval[cand0-1];
	    else
		left = -DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.leftOperator() == ibis::qExpr::OP_LE) {
	if (cand0 < minval.size() && rng.leftBound() > minval[cand0]) {
	    // reduce left bound
	    ++ ret;
	    right = minval[cand0];
	    if (cand0 > 0)
		left = maxval[cand0-1];
	    else
		left = -DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.leftOperator() == ibis::qExpr::OP_EQ) {
	if (cand0 < minval.size() && minval[cand0] < maxval[cand1] &&
	    rng.leftBound() >= minval[cand0] &&
	    rng.leftBound() <= maxval[cand0]) {
	    // change equality condition to a two-sided range condition
	    ++ ret;
	    right = minval[cand0];
	    if (cand0 > 0)
		left = maxval[cand0-1];
	    else
		left = -DBL_MAX;
	    rng.leftOperator() = ibis::qExpr::OP_LE;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	    left = maxval[cand0];
	    if (cand0+1 < minval.size())
		right = minval[cand0+1];
	    else
		right = DBL_MAX;
	    rng.rightOperator() = ibis::qExpr::OP_LE;
	    rng.rightBound() = ibis::util::compactValue(left, right);
	}
    }

    if (rng.rightOperator() == ibis::qExpr::OP_LT) {
	if (cand1 > 0 && rng.rightBound() <= maxval[cand1-1]) {
	    // increase right bound
	    ++ ret;
	    left = maxval[cand1-1];
	    if (cand1 < minval.size())
		right = minval[cand1];
	    else
		right = DBL_MAX;
	    rng.rightBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.rightOperator() == ibis::qExpr::OP_LE) {
	if (cand1 > 0 && rng.rightBound() < maxval[cand1-1]) {
	    // increase right bound
	    ++ ret;
	    left = maxval[cand1-1];
	    if (cand1 < minval.size())
		right = minval[cand1];
	    else
		right = DBL_MAX;
	    rng.rightBound() = ibis::util::compactValue(left, right);
	}
    }
    return ret;
} // ibis::bak2::expandRange

// contract range condition -- rely on the fact that the only operators used
// are LT, LE and EQ
int ibis::bak2::contractRange(ibis::qContinuousRange& rng) const {
    uint32_t cand0, cand1;
    double left, right;
    int ret = 0;
    ibis::bin::locate(rng, cand0, cand1);
    if (rng.leftOperator() == ibis::qExpr::OP_LT) {
	if (cand0 < minval.size() && rng.leftBound() <= maxval[cand0]) {
	    // increase left bound
	    ++ ret;
	    left = maxval[cand0];
	    if (cand0+1 < minval.size())
		right = minval[cand0+1];
	    else
		right = DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.leftOperator() == ibis::qExpr::OP_LE) {
	if (cand0 < minval.size() && rng.leftBound() < maxval[cand0]) {
	    // increase left bound
	    ++ ret;
	    left = maxval[cand0];
	    if (cand0+1 < minval.size())
		right = minval[cand0+1];
	    else
		right = DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.leftOperator() == ibis::qExpr::OP_EQ) {
	if (cand0 < minval.size() && minval[cand0] < maxval[cand0] &&
	    rng.leftBound() >= minval[cand0] &&
	    rng.leftBound() <= maxval[cand0]) {
	    // make an equality with empty results
	    ++ ret;
	    right = minval[cand0];
	    if (cand0 > 0)
		left = maxval[cand0-1];
	    else
		left = -DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }

    if (rng.rightOperator() == ibis::qExpr::OP_LT) {
	if (cand1 > 0 && rng.rightBound() > minval[cand1-1]) {
	    // decrease right bound
	    ++ ret;
	    right = minval[cand1-1];
	    if (cand1 > 1)
		left = maxval[cand1-2];
	    else
		left = -DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    else if (rng.rightOperator() == ibis::qExpr::OP_LE) {
	if (cand1 > 0 && rng.rightBound() >= minval[cand1-1]) {
	    // decrease right bound
	    ++ ret;
	    right = minval[cand1-1];
	    if (cand1 > 1)
		left = maxval[cand1-2];
	    else
		left = -DBL_MAX;
	    rng.leftBound() = ibis::util::compactValue(left, right);
	}
    }
    return ret;
} // ibis::bak2::contractRange