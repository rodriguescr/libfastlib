// File: $Id$
// Author: John Wu <John.Wu at ACM.org>
// Copyright 2006-2009 the Regents of the University of California
#include "idirekte.h"
#include "part.h"

/// Constructing a new ibis::direkte object from base data in a file.
ibis::direkte::direkte(const ibis::column* c, const char* f)
    : ibis::index(c) {
    if (c == 0)
	return;
    if (c->type() == ibis::FLOAT ||
	c->type() == ibis::DOUBLE ||
	c->type() == ibis::TEXT) {
	ibis::util::logMessage("Error", "ibis::direkte can only be used "
			       "for columns with integer values (current "
			       "column %s, type=%s)", c->name(),
			       ibis::TYPESTRING[(int)c->type()]);
	throw ibis::bad_alloc("wrong column type for ibis::direkte");
    }
//     if (c->lowerBound() < 0.0 || c->lowerBound() > 1.0) {
// 	ibis::util::logMessage("Error", "ibis::direkte can only be used "
// 			       " on integer attributes with minimal value "
// 			       "of 0 or 1, current minimal value is %g",
// 			       c->lowerBound());
// 	throw ibis::bad_alloc("unexpected minimal value for ibis::direkte");
//     }

    std::string dfname;
    dataFileName(f, dfname);
    if (c->type() == ibis::CATEGORY)
	dfname += ".int";

    int ierr = 0;
    switch (c->type()) {
    default: {
	ibis::util::logMessage("Error", "ibis::direkte can only be used "
			       "for columns with integer values (current "
			       "column %s, type=%s)", c->name(),
			       ibis::TYPESTRING[(int)c->type()]);
	throw ibis::bad_alloc("wrong column type for ibis::direkte");
	break;}
    case ibis::BYTE: {
	ierr = construct<signed char>(dfname.c_str());
	break;}
    case ibis::UBYTE: {
	ierr = construct<unsigned char>(dfname.c_str());
	break;}
    case ibis::SHORT: {
	ierr = construct<int16_t>(dfname.c_str());
	break;}
    case ibis::USHORT: {
	ierr = construct<uint16_t>(dfname.c_str());
	break;}
    case ibis::INT: {
	ierr = construct<int32_t>(dfname.c_str());
	break;}
    case ibis::UINT:
    case ibis::CATEGORY: {
	ierr = construct<uint32_t>(dfname.c_str());
	break;}
    case ibis::LONG: {
	ierr = construct<int64_t>(dfname.c_str());
	break;}
    case ibis::ULONG: {
	ierr = construct<uint64_t>(dfname.c_str());
	break;}
    }
    if (ierr < 0) {
	ibis::util::logMessage("Error", "ibis::direkte failed with error "
			       "code %d", ierr);
	throw ibis::bad_alloc("ibis::direkte construction failure");
    }
    if (ibis::gVerbose > 4) {
	ibis::util::logger lg(4);
	print(lg.buffer());
    }
} // ibis::direkte::direkte

template <typename T>
int ibis::direkte::construct(const char* dfname) {
    int ierr = 0;
    array_t<T> vals;
    if (ibis::gVerbose > 4)
	col->logMessage("direkte::construct", "starting to process file %s",
			dfname);
    ibis::bitvector mask;
    col->getNullMask(mask);
    nrows = col->partition()->nRows();
    ierr = ibis::fileManager::instance().getFile(dfname, vals);
    if (ierr == 0) { // got a pointer to the base data
	if (col->upperBound() > col->lowerBound()) {
	    const uint32_t nbits = (uint32_t)col->upperBound() + 1;
#ifdef RESERVE_SPACE_BEFORE_CREATING_INDEX
	    const uint32_t nset = (uint32_t)(nrows+nbits-1)/nbits;
#endif
	    bits.resize(nbits);
	    for (uint32_t i = 0; i < nbits; ++ i) {
		bits[i] = new ibis::bitvector();
#ifdef RESERVE_SPACE_BEFORE_CREATING_INDEX
		bits[i]->reserve(nbits, nset);
#endif
	    }
	    if (ibis::gVerbose > 6)
		col->logMessage("direkte::construct", "finished allocating "
				"%lu bitvectors",
				static_cast<long unsigned>(nbits));
	}
	if (vals.size() > nrows)
	    vals.resize(nrows);

	for (ibis::bitvector::indexSet iset = mask.firstIndexSet();
	     iset.nIndices() > 0; ++ iset) {
	    const ibis::bitvector::word_t *iis = iset.indices();
	    if (iset.isRange()) { // a range
		for (uint32_t j = *iis; j < iis[1]; ++ j) {
		    const uint32_t nbits = bits.size();
		    if (nbits <= static_cast<uint32_t>(vals[j])) {
			const uint32_t newsize = vals[j]+1;
			bits.resize(newsize);
			for (uint32_t i = nbits; i < newsize; ++ i)
			    bits[i] = new ibis::bitvector;
		    }
		    bits[vals[j]]->setBit(j, 1);
		}
	    }
	    else {
		for (uint32_t i = 0; i < iset.nIndices(); ++ i) {
		    const ibis::bitvector::word_t j = iis[i];
		    const uint32_t nbits = bits.size();
		    if (nbits <= static_cast<uint32_t>(vals[j])) {
			const uint32_t newsize = vals[j]+1;
			bits.resize(newsize);
			for (uint32_t i = nbits; i < newsize; ++ i)
			    bits[i] = new ibis::bitvector;
		    }
		    bits[vals[j]]->setBit(j, 1);
		}
	    }
	}
    }
    else { // failed to read or memory map the data file, try to read the
	   // values one at a time
	const unsigned elemsize = sizeof(T);
	uint32_t sz = ibis::util::getFileSize(dfname);
	if (sz == 0) {
	    ierr = -1; // no data file
	    return ierr;
	}

	if (ibis::gVerbose > 5)
	    col->logMessage("direkte::construct", "starting to construct "
			    "the index by reading the values from %s one "
			    "at a time", dfname);
	if (col->upperBound() > col->lowerBound()) {
	    const uint32_t nbits = (uint32_t)col->upperBound() + 1;
#ifdef RESERVE_SPACE_BEFORE_CREATING_INDEX
	    const uint32_t nset = (nrows + nbits - 1) / nbits;
#endif
	    bits.resize(nbits);
	    for (uint32_t i = 0; i < nbits; ++ i) {
		bits[i] = new ibis::bitvector();
#ifdef RESERVE_SPACE_BEFORE_CREATING_INDEX
		bits[i]->reserve(nbits, nset);
#endif
	    }
	}
	sz /= elemsize;
	if (sz > nrows)
	    sz = nrows;
	int fdes = UnixOpen(dfname, OPEN_READONLY);
	if (fdes < 0) {
	    ierr = -2; // failed to open file for reading
	    return ierr;
	}

	for (ibis::bitvector::indexSet iset = mask.firstIndexSet();
	     iset.nIndices() > 0; ++ iset) {
	    const ibis::bitvector::word_t *iis = iset.indices();
	    if (iset.isRange()) { // a range
		ierr = UnixSeek(fdes, *iis * elemsize, SEEK_SET);
		for (uint32_t j = *iis; j < iis[1]; ++ j) {
		    T val;
		    ierr = UnixRead(fdes, &val, elemsize);
		    if (ierr < static_cast<int>(elemsize)) {
			ierr = -3;
			break;
		    }

		    const uint32_t nbits = bits.size();
		    if (nbits <= static_cast<uint32_t>(val)) {
			const uint32_t newsize = val + 1;
			bits.resize(newsize);
			for (uint32_t i = nbits; i < newsize; ++ i)
			    bits[i] = new ibis::bitvector;
		    }
		    bits[val]->setBit(j, 1);
		}
	    }
	    else {
		for (uint32_t i = 0; i < iset.nIndices(); ++ i) {
		    const ibis::bitvector::word_t j = iis[i];
		    T val;
		    ierr = UnixSeek(fdes, j * elemsize, SEEK_SET);
		    if (ierr < 0 || static_cast<unsigned>(ierr) != j*elemsize) {
			ierr = -3;
			break;
		    }
		    ierr = UnixRead(fdes, &val, elemsize);
		    if (ierr < static_cast<int>(elemsize)) {
			ierr = -4;
			break;
		    }

		    const uint32_t nbits = bits.size();
		    if (nbits <= static_cast<uint32_t>(val)) {
			const uint32_t newsize = val + 1;
			bits.resize(newsize);
			for (uint32_t i = nbits; i < newsize; ++ i)
			    bits[i] = new ibis::bitvector;
		    }
		    bits[val]->setBit(j, 1);
		}
	    }

	    if (ierr < 0) break;
	}
	UnixClose(fdes);
    }

    // make sure all bitvectors are of the right size
    for (uint32_t i = 0; i < bits.size(); ++ i)
	bits[i]->adjustSize(0, nrows);
    return ierr;
} // ibis::direkte::construct

ibis::direkte::direkte(const ibis::column* c, ibis::fileManager::storage* st,
		       uint32_t offset) : ibis::index(c, st) {
    read(st);
} // ibis::direkte::direkte

void ibis::direkte::print(std::ostream& out) const {
    if (ibis::gVerbose < 0) return;
    const uint32_t nobs = bits.size();
    if (nobs > 0) {
	out << "The direct bitmap index for " << col->name() << " contains "
	    << nobs << " bit vector" << (nobs > 1 ? "s" : "") << "\n";
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
	    out << " (printing 1 out of every " << skip << ")";
	}
	out << "\n";

	for (uint32_t i=0; i<nobs; i += skip) {
	    if (bits[i]) {
		out << i << "\t" << bits[i]->cnt()
		    << "\t" << bits[i]->bytes() << "\n";
	    }
	}
	if ((nobs-1) % skip) {
	    if (bits[nobs-1]) {
		out << nobs-1 << "\t" << bits[nobs-1]->cnt()
		    << "\t" << bits[nobs-1]->bytes() << "\n";
	    }
	}
    }
    else {
	out << "The direct bitmap index for " << col->name()
	    << " is empty\n";
    }
    out << std::endl;
} // ibis::direkte::print

/// Write the direct bitmap index to a file.
int ibis::direkte::write(const char* dt) const {
    std::string fnm;
    indexFileName(dt, fnm);
    if (fname != 0 && fnm.compare(fname) == 0)
	return 0;
    if (fname != 0 || str != 0)
	activate();

    int fdes = UnixOpen(fnm.c_str(), OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	col->logWarning("direkte::write", "unable to open \"%s\" for write",
			fnm.c_str());
	return -2;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    int ierr = 0;
    const uint32_t nobs = bits.size();

    array_t<int32_t> offs(nobs+1);
    char header[] = "#IBIS\0\0\0";
    header[5] = (char)ibis::index::DIREKTE;
    header[6] = (char)sizeof(int32_t);
    ierr = UnixWrite(fdes, header, 8);
    if (ierr < 8) {
	LOGGER(ibis::gVerbose > 0)
	    << "ibis::column[" << col->partition()->name() << "."
	    << col->name() << "]::direkte::write(" << fnm
	    << ") failed to write the 8-byte header, ierr = " << ierr;
	return -3;
    }
    ierr = UnixWrite(fdes, &nrows, sizeof(uint32_t));
    ierr = UnixWrite(fdes, &nobs, sizeof(uint32_t));
    ierr = UnixSeek(fdes, sizeof(int32_t)*(nobs+1), SEEK_CUR);
    for (uint32_t i = 0; i < nobs; ++ i) {
	offs[i] = UnixSeek(fdes, 0, SEEK_CUR);
	if (bits[i] != 0) {
	    if (bits[i]->cnt() > 0)
		bits[i]->write(fdes);
	    if (bits[i]->size() != nrows)
		col->logWarning("direkte::write", "bits[%lu] has %lu bits, "
				"expected %lu", static_cast<long unsigned>(i),
				static_cast<long unsigned>(bits[i]->size()),
				static_cast<long unsigned>(nrows));
	}
#if defined(DEBUG)
	int id0 = -1;
	if (bits[i]) {
	    ibis::bitvector::indexSet ixs = bits[i]->firstIndexSet();
	    id0 = *(ixs.indices());
	}
	col->logMessage("direkte::write", "value:%lu count:%lu (%d)",
			static_cast<long unsigned>(i),
			static_cast<long unsigned>(bits[i] ? bits[i]->cnt()
						   : 0),
			id0);
#endif
    }
    offs[nobs] = UnixSeek(fdes, 0, SEEK_CUR);
    ierr = UnixSeek(fdes, 8+sizeof(uint32_t)*2, SEEK_SET);
    ierr = UnixWrite(fdes, offs.begin(), sizeof(int32_t)*(nobs+1));
#if _POSIX_FSYNC+0 > 0
    (void) fsync(fdes); // write to disk
#endif
    (void) UnixClose(fdes);

    if (ibis::gVerbose > 5)
	col->logMessage("direkte::write", "wrote %lu bitmap%s to %s",
			static_cast<long unsigned>(nobs),
			(nobs>1?"s":""), fnm.c_str());
    return 0;
} // ibis::direkte::write

int ibis::direkte::read(const char* f) {
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
		  header[5] == static_cast<char>(ibis::index::DIREKTE) &&
		  header[6] == static_cast<char>(sizeof(int32_t)) &&
		  header[7] == static_cast<char>(0))) {
	UnixClose(fdes);
	return -3;
    }

    uint32_t dim[2];
    uint32_t begin, end;
    ibis::index::clear(); // clear the current bit vectors
    fname = ibis::util::strnewdup(fnm.c_str());

    int ierr = UnixRead(fdes, static_cast<void*>(dim), 2*sizeof(uint32_t));
    if (ierr < static_cast<int>(2*sizeof(uint32_t))) {
	UnixClose(fdes);
	return -4;
    }
    nrows = dim[0];
    bool trymmap = false;
#if defined(HAS_FILE_MAP)
    trymmap = (dim[1] > ibis::fileManager::pageSize());
#endif
    // read offsets
    begin = 8 + 2*sizeof(uint32_t);
    end = 8 + 2*sizeof(uint32_t) + sizeof(int32_t) * (dim[1] + 1);
    if (trymmap) {
	array_t<int32_t> tmp(fnm.c_str(), begin, end);
	offsets.swap(tmp);
    }
    else {
	array_t<int32_t> tmp(fdes, begin, end);
	offsets.swap(tmp);
    }
    ibis::fileManager::instance().recordPages(0, end);
#if defined(DEBUG)
    if (ibis::gVerbose > 5) {
	unsigned nprt = (ibis::gVerbose < 30 ? (1 << ibis::gVerbose) : dim[1]);
	if (nprt > dim[1])
	    nprt = dim[1];
	ibis::util::logger lg(4);
	lg.buffer() << "DEBUG -- ibis::direkte::read(" << fnm
		    << ") got nobs = " << dim[1]
		    << ", the offsets of the bit vectors are\n";
	for (unsigned i = 0; i < nprt; ++ i)
	    lg.buffer() << offsets[i] << " ";
	if (nprt < dim[1])
	    lg.buffer() << "... (skipping " << dim[1]-nprt << ") ... ";
	lg.buffer() << offsets[dim[1]];
    }
#endif

    bits.resize(dim[1]);
    for (uint32_t i = 0; i < dim[1]; ++i)
	bits[i] = 0;
#if defined(ALWAY_READ_BITVECTOR0)
    // read the first bitvector
    if (offsets[1] > offsets[0]) {
	array_t<ibis::bitvector::word_t> a0(fdes, offsets[0], offsets[1]);
	bits[0] = new ibis::bitvector(a0);
#if defined(WAH_CHECK_SIZE)
	const uint32_t len = strlen(col->partition()->currentDataDir());
	if (0 == strncmp(f, col->partition()->currentDataDir(), len)
	    && bits[0]->size() != nrows) {
	    col->logWarning("readIndex", "the size (%lu) of the 1st "
			    "bitvector (from \"%s\") differs from "
			    "nRows (%lu)",
			    static_cast<long unsigned>(bits[0]->size()),
			    fnm.c_str(),
			    static_cast<long unsigned>(nrows));
	}
#else
	bits[0]->setSize(nrows);
#endif
    }
    else {
	bits[0] = new ibis::bitvector;
	bits[0]->set(0, nrows);
    }
#endif
    (void) UnixClose(fdes);
    str = 0;
    if (ibis::gVerbose > 7)
	col->logMessage("readIndex", "finished reading '%s' header from %s",
			name(), fnm.c_str());
    return 0;
} // ibis::direkte::read

// attempt to reconstruct an index from a piece of consecutive memory
int ibis::direkte::read(ibis::fileManager::storage* st) {
    if (st == 0) return -1;
    clear();

    nrows = *(reinterpret_cast<uint32_t*>(st->begin()+8));
    uint32_t pos = 8 + sizeof(uint32_t);
    const uint32_t nobs = *(reinterpret_cast<uint32_t*>(st->begin()+pos));
    pos += sizeof(uint32_t);
    array_t<int32_t> offs(st, pos, nobs+1);
    offsets.copy(offs);

    for (uint32_t i = 0; i < bits.size(); ++ i)
	delete bits[i];
    bits.resize(nobs);
    for (uint32_t i = 0; i < nobs; ++i)
	bits[i] = 0;
    if (st->isFileMap()) { // only restore the first bitvector
#if defined(ALWAY_READ_BITVECTOR0)
	if (offs[1] > offs[0]) {
	    array_t<ibis::bitvector::word_t>
		a0(st, offs[0], (offs[1]-offs[0])/
		   sizeof(ibis::bitvector::word_t));
	    bits[0] = new ibis::bitvector(a0);
#if defined(WAH_CHECK_SIZE)
	    if (bits[0]->size() != nrows) {
		col->logWarning("readIndex", "the length (%lu) of the 1st "
				"bitvector differs from nRows(%lu)",
				static_cast<long unsigned>(bits[0]->size()),
				static_cast<long unsigned>(nrows));
	    }
#else
	    bits[0]->setSize(nrows);
#endif
	}
	else {
	    bits[0] = new ibis::bitvector;
	    bits[0]->adjustSize(0, nrows);
	}
#endif
	str = st;
    }
    else { // regenerate all the bitvectors
	for (uint32_t i = 0; i < nobs; ++i) {
	    if (offs[i+1] > offs[i]) {
		array_t<ibis::bitvector::word_t>
		    a(st, offs[i], (offs[i+1]-offs[i])/
		      sizeof(ibis::bitvector::word_t));
		ibis::bitvector* btmp = new ibis::bitvector(a);
		bits[i] = btmp;
#if defined(WAH_CHECK_SIZE)
		if (btmp->size() != nrows) {
		    col->logWarning("readIndex", "the length (%lu) of the "
				    "%lu-th bitvector differs from "
				    "nRows(%lu)",
				    static_cast<long unsigned>(btmp->size()),
				    static_cast<long unsigned>(i),
				    static_cast<long unsigned>(nrows));
		}
#else
		btmp->setSize(nrows);
#endif
	    }
	}
	str = 0;
    }
    return 0;
} // ibis::direkte::read

// Convert to a range [ib, ie) such that bits[ib:ie-1] contains the solution
void ibis::direkte::locate(const ibis::qContinuousRange& expr,
			   uint32_t& ib, uint32_t& ie) const {
    ib = static_cast<uint32_t>(expr.leftBound()>0.0 ? expr.leftBound() : 0.0);
    ie = static_cast<uint32_t>(expr.rightBound()>0.0 ? expr.rightBound() : 0.0);

    switch (expr.leftOperator()) {
    case ibis::qExpr::OP_LT: {
	++ ib;
	switch (expr.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (expr.rightBound()>ie)
		++ ie;
	    break;}
	case ibis::qExpr::OP_LE: {
	    ++ ie;
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ib < ie+1)
		ib = ie + 1;
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (expr.rightBound() > ie)
		++ ie;
	    if (ib < ie)
		ib = ie;
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (expr.leftBound() < expr.rightBound() &&
		ie == expr.rightBound()) {
		ib = ie;
		++ ie;
	    }
	    else {
		ie = ib;
	    }
	    break;}
	default: {
	    ie = bits.size();
	    break;}
	}
	break;}
    case ibis::qExpr::OP_LE: {
	if (expr.leftBound() > ib)
	    ++ ib;
	switch (expr.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (expr.rightBound()>ie)
		++ ie;
	    break;}
	case ibis::qExpr::OP_LE: {
	    ++ ie;
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (ib < ie+1)
		ib = ie+1;
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (expr.rightBound() > ie)
		++ ie;
	    if (ib < ie)
		ib = ie;
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (expr.rightBound() >= expr.leftBound() &&
		ie == expr.rightBound()) {
		ib = ie;
		++ ie;
	    }
	    else {
		ie = ib;
	    }
	    break;}
	default: {
	    ie = bits.size();
	    break;}
	}
	break;}
    case ibis::qExpr::OP_GT: {
	++ ib;
	switch (expr.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (expr.rightBound() > ie)
		++ ie;
	    if (ib < ie)
		ie = ib;
	    ib = 0;
	    break;}
	case ibis::qExpr::OP_LE: {
	    ++ ie;
	    if (ib < ie)
		ie = ib;
	    ib = 0;
	    break;}
	case ibis::qExpr::OP_GT: {
	    uint32_t tmp = ie+1;
	    ie = ib;
	    ib = tmp;
	    break;}
	case ibis::qExpr::OP_GE: {
	    uint32_t tmp = (expr.rightBound()>ie ? ie+1 : ie);
	    ie = ib;
	    ib = tmp;
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (expr.rightBound() > expr.leftBound() &&
		expr.rightBound() == ie) {
		ib = ie;
		++ ie;
	    }
	    else {
		ie = ib;
	    }
	    break;}
	default: {
	    ie = ib;
	    ib = 0;
	    break;}
	}
	break;}
    case ibis::qExpr::OP_GE: {
	if (expr.leftBound() > ib)
	    ++ ib;
	switch (expr.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (expr.rightBound() > ie)
		++ ie;
	    if (ib < ie)
		ie = ib;
	    ib = 0;
	    break;}
	case ibis::qExpr::OP_LE: {
	    ++ ie;
	    if (ib < ie)
		ie = ib;
	    ib = 0;
	    break;}
	case ibis::qExpr::OP_GT: {
	    uint32_t tmp = ie+1;
	    ie = ib+1;
	    ib = tmp;
	    break;}
	case ibis::qExpr::OP_GE: {
	    uint32_t tmp = (expr.rightBound()<=ie ? ie : ie+1);
	    ie = ib+1;
	    ib = tmp;
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (expr.rightBound() >= expr.leftBound()) {
		ib = ie;
		++ ie;
	    }
	    else {
		ie = ib;
	    }  
	    break;}
	default: {
	    ie = ib + 1;
	    ib = 0;
	    break;}
	}
	break;}
    case ibis::qExpr::OP_EQ: {
	if (expr.leftBound() == ib) {
	    switch (expr.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		if (expr.leftBound() < expr.rightBound())
		    ie = ib+1;
		else
		    ie = ib;
		break;}
	    case ibis::qExpr::OP_LE: {
		if (expr.leftBound() <= expr.rightBound())
		    ie = ib+1;
		else
		    ie = ib;
		break;}
	    case ibis::qExpr::OP_GT: {
		if (expr.leftBound() > expr.rightBound())
		    ie = ib+1;
		else
		    ie = ib;
		break;}
	    case ibis::qExpr::OP_GE: {
		if (expr.leftBound() >= expr.rightBound())
		    ie = ib+1;
		else
		    ie = ib;
		break;}
	    case ibis::qExpr::OP_EQ: {
		if (expr.leftBound() == expr.rightBound())
		    ie = ib+1;
		else
		    ie = ib;
		break;}
	    default: {
		ie = ib+1;
		break;}
	    }
	}
	else {
	    ie = ib;
	}
	break;}
    default: {
	switch (expr.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    ib = 0;
	    if (expr.rightBound()>ie)
		++ ie;
	    break;}
	case ibis::qExpr::OP_LE: {
	    ib = 0;
	    ++ ie;
	    break;}
	case ibis::qExpr::OP_GT: {
	    ib = ie + 1;
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_GE: {
	    ib = (expr.rightBound() == ie ? ie : ie+1);
	    ie = bits.size();
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (expr.rightBound() == ie) {
		ib = ie;
		++ ie;
	    }
	    else {
		ie = ib;
	    }
	    break;}
	default: {
	    // nothing specified, match all
	    if (ibis::gVerbose > -1)
		col->logWarning("direkte::locate", "no operator specified "
				"in a qContinuousQuery object");
	    ib = 0;
	    ie = bits.size();
	    break;}
	}
	break;}
    }
} // ibis::direkte::locate

long ibis::direkte::evaluate(const ibis::qContinuousRange& expr,
			     ibis::bitvector& lower) const {
    uint32_t ib, ie;
    locate(expr, ib, ie);
    sumBins(ib, ie, lower);
    return lower.cnt();
} // ibis::direkte::evaluate

void ibis::direkte::estimate(const ibis::qContinuousRange& expr,
			     ibis::bitvector& lower,
			     ibis::bitvector& upper) const {
    upper.clear();
    uint32_t ib, ie;
    locate(expr, ib, ie);
    sumBins(ib, ie, lower);
} // ibis::direkte::estimate

uint32_t ibis::direkte::estimate(const ibis::qContinuousRange& expr) const {
    uint32_t ib, ie, cnt;
    locate(expr, ib, ie);
    activate(ib, ie);
    cnt = 0;
    for (uint32_t j = ib; j < ie; ++ j)
	if (bits[j])
	    cnt += bits[j]->cnt();
    return cnt;
} // ibis::direkte::estimate

long ibis::direkte::evaluate(const ibis::qDiscreteRange& expr,
			     ibis::bitvector& lower) const {
    const std::vector<double>& varr = expr.getValues();
    lower.set(0, nrows);
    for (unsigned i = 0; i < varr.size(); ++ i) {
	unsigned int tmp = static_cast<unsigned int>(varr[i]);
	if (tmp < bits.size()) {
	    if (bits[tmp] == 0)
		activate(tmp);
	    if (bits[tmp])
		lower |= *(bits[tmp]);
	}
    }
    return lower.cnt();
} // ibis::direkte::evaluate

void ibis::direkte::estimate(const ibis::qDiscreteRange& expr,
			     ibis::bitvector& lower,
			     ibis::bitvector& upper) const {
    const std::vector<double>& varr = expr.getValues();
    upper.clear();
    lower.set(0, nrows);
    for (unsigned i = 0; i < varr.size(); ++ i) {
	unsigned int tmp = static_cast<unsigned int>(varr[i]);
	if (tmp < bits.size()) {
	    if (bits[tmp] == 0)
		activate(tmp);
	    if (bits[tmp])
		lower |= *(bits[tmp]);
	}
    }
} // ibis::direkte::estimate

uint32_t ibis::direkte::estimate(const ibis::qDiscreteRange& expr) const {
    uint32_t res = 0;
    const std::vector<double>& varr = expr.getValues();
    for (unsigned i = 0; i < varr.size(); ++ i) {
	unsigned int tmp = static_cast<unsigned int>(varr[i]);
	if (tmp < bits.size()) {
	    if (bits[tmp] == 0)
		activate(tmp);
	    if (bits[tmp])
		res += bits[tmp]->cnt();
	}
    }
    return res;
} // ibis::direkte::estimate

double ibis::direkte::estimateCost(const ibis::qContinuousRange& expr) const {
    double cost = 0.0;
    uint32_t ib, ie;
    locate(expr, ib, ie);
    if (offsets.size() > bits.size()) {
	if (ib < ie && ie < offsets.size())
	    cost = offsets[ie] - offsets[ib];
	else if (ib < offsets.size())
	    cost = offsets.back() - offsets[ib];
    }
    return cost;
} // ibis::direkte::estimateCost

double ibis::direkte::estimateCost(const ibis::qDiscreteRange& expr) const {
    double cost = 0;
    const std::vector<double>& varr = expr.getValues();
    for (uint32_t j = 0; j < varr.size(); ++ j) {
	uint32_t ind = static_cast<uint32_t>(varr[j]);
	if (ind+1 < offsets.size() && ind < bits.size())
	    cost += offsets[ind+1] - offsets[ind];
    }
    return cost;
} // ibis::direkte::estimateCost

long ibis::direkte::append(const char*, const char*, uint32_t) {
    col->logWarning("ERROR", "direkte::append not implemented yet");
    return 0L;
} // ibis::direkte::append

double ibis::direkte::getSum() const {
    double ret = 0;
    activate(); // need all bitvectors
    for (unsigned j = 0; j < bits.size(); ++ j) {
	if (bits[j])
	    ret += j * bits[j]->cnt();
    }
    return ret;
} // ibis::direkte::getSum

void ibis::direkte::binBoundaries(std::vector<double>& bb) const {
    bb.resize(bits.size());
    for (uint32_t i = 0; i < bits.size(); ++ i)
	bb[i] = i;
} // ibis::direkte::binBoundaries

void ibis::direkte::binWeights(std::vector<uint32_t>& cnts) const {
    activate();
    cnts.resize(bits.size());
    for (uint32_t j = 0; j < bits.size(); ++ j) {
	if (bits[j])
	    cnts[j] = bits[j]->cnt();
	else
	    cnts[j] = 0;
    }
} // ibis::direkte::binWeights

long ibis::direkte::getCumulativeDistribution
(std::vector<double>& bds, std::vector<uint32_t>& cts) const {
    activate();
    bds.resize(bits.size());
    cts.resize(bits.size());
    uint32_t sum = 0;
    for (uint32_t j = 0; j < bits.size(); ++ j) {
	bds[j] = j;
	cts[j] = sum;
	if (bits[j])
	    sum += bits[j]->cnt();
    }
    return cts.size();
} // ibis::direkte::getCumulativeDistribution

long ibis::direkte::getDistribution
(std::vector<double>& bds, std::vector<uint32_t>& cts) const {
    activate();
    bds.reserve(bits.size());
    cts.reserve(bits.size());
    for (uint32_t j = 0; j < bits.size(); ++ j) {
	if (bits[j]) {
	    cts.push_back(bits[j]->cnt());
	    bds.push_back(j+1);
	}
    }
    bds.pop_back();
    return cts.size();
} // ibis::direkte::getDistribution
